// Compression + encryption flag propagation on copy-up. Unit tests drive
// CopyUp directly (no mount). The single integration-style Metacopy test
// that needs a mount lives in
// LayerMount.IntegrationTests::CompressionEncryptionTests.
//
// Risk these cover:
//   - Modifying a compressed lower file must produce a compressed upper
//     file. Losing compression on copy-up can silently explode disk usage
//     by 2-10× for compressible data.
//   - Encrypted lower files must either carry their encryption state to
//     upper OR fail cleanly. Silently copying plaintext into upper is a
//     confidentiality break.

#include "pch.h"
#include "TestFixture.h"

#include "CopyUp.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"

#include <winioctl.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;

namespace LayerMountTests {

namespace {

// Mark a file as NTFS-compressed. Returns 0 on success or Win32 error.
// Compression state is a file-layout property set via DeviceIoControl;
// SetFileAttributes with FILE_ATTRIBUTE_COMPRESSED is explicitly rejected
// by Windows — the FSCTL is the only way.
DWORD SetNtfsCompression(const std::wstring& path, USHORT format) {
    HANDLE h = ::CreateFileW(path.c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return ::GetLastError();
    DWORD ret = 0;
    const BOOL ok = ::DeviceIoControl(h, FSCTL_SET_COMPRESSION,
                                        &format, sizeof(format),
                                        nullptr, 0, &ret, nullptr);
    const DWORD err = ok ? 0 : ::GetLastError();
    ::CloseHandle(h);
    return err;
}

bool HasAttr(const std::wstring& path, DWORD flag) {
    const DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & flag) != 0;
}

} // namespace

// ============================================================================
// CompressionPropagationTests — NTFS compression state through copy-up.
// ============================================================================

TEST_CLASS(CompressionPropagationTests) {
public:
    TEST_METHOD(CopyUp_CompressedLowerFile_UpperIsAlsoCompressed) {
        TempLayerEnvironment env(1);
        std::string payload;
        payload.reserve(128 * 1024);
        for (int i = 0; i < 128 * 1024; ++i) {
            payload.push_back(static_cast<char>('a' + (i % 16)));
        }
        env.WriteFile(env.Lower(0), L"z.bin", payload);

        const std::wstring lowerPath = env.Lower(0) + L"\\z.bin";
        const DWORD cmpRc = SetNtfsCompression(lowerPath, COMPRESSION_FORMAT_LZNT1);
        if (cmpRc != 0) {
            wchar_t msg[128];
            swprintf_s(msg, L"[SKIP] FSCTL_SET_COMPRESSION on lower failed "
                            L"with error %lu — volume may not support compression",
                       cmpRc);
            Logger::WriteMessage(msg);
            return;
        }
        Assert::IsTrue(HasAttr(lowerPath, FILE_ATTRIBUTE_COMPRESSED),
            L"Precondition: lower file must be marked compressed");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.CopyUpFile(L"z.bin")));

        const std::wstring upperPath = env.Upper() + L"\\z.bin";
        Assert::IsTrue(HasAttr(upperPath, FILE_ATTRIBUTE_COMPRESSED),
            L"KNOWN GAP: upper copy should be compressed to preserve storage "
            L"characteristics. If this assertion fails, copy-up needs to "
            L"issue FSCTL_SET_COMPRESSION on the work-dir temp before data "
            L"transfer (compression is a layout property, like sparse).");

        Assert::AreEqual(payload, env.ReadFile(env.Upper(), L"z.bin"));
    }

    TEST_METHOD(CopyUp_CompressedLowerDirectory_UpperDirIsAlsoCompressed) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"cmpdir");

        const std::wstring lowerDir = env.Lower(0) + L"\\cmpdir";
        const DWORD cmpRc = SetNtfsCompression(lowerDir, COMPRESSION_FORMAT_LZNT1);
        if (cmpRc != 0) {
            Logger::WriteMessage(L"[SKIP] Cannot compress directory on this volume");
            return;
        }
        Assert::IsTrue(HasAttr(lowerDir, FILE_ATTRIBUTE_COMPRESSED),
            L"Precondition: lower dir must carry compressed attribute");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.CopyUpDirectory(L"cmpdir")));

        const std::wstring upperDir = env.Upper() + L"\\cmpdir";
        Assert::IsTrue(HasAttr(upperDir, FILE_ATTRIBUTE_COMPRESSED),
            L"KNOWN GAP: upper directory should inherit compressed state so "
            L"new child files land compressed by default. Fix: CopyUpDirectory "
            L"must issue FSCTL_SET_COMPRESSION on the upper dir after create.");
    }
};

// ============================================================================
// EncryptionPropagationTests — NTFS EFS state through copy-up.
// ============================================================================

TEST_CLASS(EncryptionPropagationTests) {
public:
    TEST_METHOD(CopyUp_EncryptedLowerFile_UpperIsAlsoEncrypted) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"secret.bin", "top-secret-payload");

        const std::wstring lowerPath = env.Lower(0) + L"\\secret.bin";
        if (!::EncryptFileW(lowerPath.c_str())) {
            const DWORD err = ::GetLastError();
            wchar_t msg[160];
            swprintf_s(msg, L"[SKIP] EncryptFileW on lower failed (error %lu). "
                            L"EFS unavailable or user has no cert.", err);
            Logger::WriteMessage(msg);
            return;
        }
        Assert::IsTrue(HasAttr(lowerPath, FILE_ATTRIBUTE_ENCRYPTED),
            L"Precondition: lower must be encrypted");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        const NTSTATUS cuStatus = cu.CopyUpFile(L"secret.bin");
        const std::wstring upperPath = env.Upper() + L"\\secret.bin";

        Assert::IsTrue(NT_SUCCESS(cuStatus),
            L"Copy-up of encrypted lower content must succeed when EFS is available");
        Assert::IsTrue(HasAttr(upperPath, FILE_ATTRIBUTE_ENCRYPTED),
            L"Encrypted lower content must remain encrypted after copy-up");
    }

    TEST_METHOD(CopyUp_EncryptedLowerDirectory_UpperDirRemainsEncrypted) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"secret-dir");

        const std::wstring lowerDir = env.Lower(0) + L"\\secret-dir";
        if (!::EncryptFileW(lowerDir.c_str())) {
            const DWORD err = ::GetLastError();
            wchar_t msg[160];
            swprintf_s(msg, L"[SKIP] EncryptFileW on lower dir failed (error %lu). "
                            L"EFS unavailable or user has no cert.", err);
            Logger::WriteMessage(msg);
            return;
        }
        Assert::IsTrue(HasAttr(lowerDir, FILE_ATTRIBUTE_ENCRYPTED),
            L"Precondition: lower directory must be encrypted");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        const NTSTATUS cuStatus = cu.CopyUpDirectory(L"secret-dir");
        const std::wstring upperDir = env.Upper() + L"\\secret-dir";
        Assert::IsTrue(NT_SUCCESS(cuStatus),
            L"Copy-up of encrypted lower directory must succeed when EFS is available");
        Assert::IsTrue(HasAttr(upperDir, FILE_ATTRIBUTE_ENCRYPTED),
            L"Encrypted lower directory must remain encrypted after copy-up");

        env.WriteFile(env.Upper(), L"secret-dir\\child.txt", "encrypted-child");
        Assert::IsTrue(HasAttr(upperDir + L"\\child.txt", FILE_ATTRIBUTE_ENCRYPTED),
            L"Children created under an encrypted copied-up directory must inherit EFS");
    }
};

} // namespace LayerMountTests
