#include "pch.h"
#include "AbiTestFixture.h"
#include "DeepPathAbiHelpers.h"
#include "EncryptionTestHelpers.h"
#include "FileIdTestHelpers.h"
#include "FileTimeTestHelpers.h"

#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using LayerMountTestShared::CountDaclAcesForSid;
using LayerMountTestShared::EncryptedOrSkipped;
using LayerMountTestShared::EveryoneSid;
using LayerMountTestShared::ExtendedFormOf;
using LayerMountTestShared::ExtendedPathUnder;
using LayerMountTestShared::GetTimes;
using LayerMountTestShared::GrantInheritableReadAttributes;
using LayerMountTestShared::kDefaultHostCapabilities;
using LayerMountTestShared::kHostCapabilitiesWithoutAds;
using LayerMountTestShared::LinkOpen;
using LayerMountTestShared::MakeDeepLayerRoot;
using LayerMountTestShared::MakeFileTime;
using LayerMountTestShared::NtfsFileIdOf;
using LayerMountTestShared::StampTimes;

namespace LayerMountAbiTests {

namespace {

const std::wstring kRootViewPath = L"\\";

struct DeepLayerRoots {
    explicit DeepLayerRoots(const TempLayerEnv& env)
        : upper(MakeDeepLayerRoot(env.Root(), L"DeepUpper")),
          lower(MakeDeepLayerRoot(env.Root(), L"DeepLower")),
          work(MakeDeepLayerRoot(env.Root(), L"DeepWork")) {}

    std::wstring upper;
    std::wstring lower;
    std::wstring work;
};

LayerMountHolder CreateOnRoots(const TempLayerEnv& env, const DeepLayerRoots& roots,
                               UINT32 hostCapabilities) {
    ConfigBuilder builder(env, hostCapabilities);
    LM_CONFIG config = *builder.Ptr();
    const PCWSTR lowers[] = {roots.lower.c_str()};
    config.upperPath = roots.upper.c_str();
    config.workDirPath = roots.work.c_str();
    config.lowerPathCount = 1;
    config.lowerPaths = lowers;
    LM_HANDLE handle = nullptr;
    const HRESULT hr = ::LayerMountCreate(&config, &handle);
    Assert::AreEqual<HRESULT>(S_OK, hr,
        (L"LayerMountCreate starts the overlay on deep roots: " + LastErrorMessage(hr)).c_str());
    return LayerMountHolder(handle);
}

LayerMountHolder CreateOnDeepRoots(const TempLayerEnv& env, const DeepLayerRoots& roots) {
    return CreateOnRoots(env, roots, kDefaultHostCapabilities);
}

std::wstring Sha1HexOf(const std::wstring& text) {
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    Assert::IsTrue(size > 0, L"The path converts to UTF-8");
    std::string utf8(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                          utf8.data(), size, nullptr, nullptr);
    UCHAR digest[20] = {};
    Assert::IsTrue(BCRYPT_SUCCESS(::BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0,
                                               reinterpret_cast<PUCHAR>(utf8.data()),
                                               static_cast<ULONG>(utf8.size()),
                                               digest, sizeof(digest))),
        L"BCryptHash computes the SHA-1 of the path");
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring hex;
    for (const UCHAR byte : digest) {
        hex += kHex[byte >> 4];
        hex += kHex[byte & 0xF];
    }
    return hex;
}

std::wstring SidecarRecordPathOf(const std::wstring& upperPath, const std::wstring& entryPath) {
    std::wstring lowered = entryPath;
    ::CharLowerBuffW(lowered.data(), static_cast<DWORD>(lowered.size()));
    return ExtendedFormOf(upperPath + L"\\.overlay\\" + Sha1HexOf(lowered) + L".meta.json");
}

UINT64 IndexNumberThroughMount(LM_HANDLE mount, const std::wstring& mountPath) {
    OpenedFile opened;
    Assert::AreEqual<HRESULT>(S_OK,
        OpenOverlayFile(mount, mountPath.c_str(), GENERIC_READ, kNoCreateOptions, opened),
        (L"The open of " + mountPath + L" succeeds").c_str());
    Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));
    return opened.info.indexNumber;
}

UINT64 WriteSharedLowerFile(const DeepLayerRoots& roots) {
    const std::wstring lowerFile = ExtendedPathUnder(roots.lower, L"shared.txt");
    WriteText(lowerFile, "lower");
    return NtfsFileIdOf(lowerFile, LinkOpen::Itself);
}

DWORD UpperAttributesOf(const DeepLayerRoots& roots, const std::wstring& name) {
    const DWORD attributes = ::GetFileAttributesW(ExtendedPathUnder(roots.upper, name).c_str());
    Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES, attributes,
        (L"The copy-up makes " + name + L" in the deep upper root").c_str());
    return attributes;
}

FILETIME LastWriteTimeOf(const std::wstring& path) {
    FILETIME creation{}, access{}, write{};
    GetTimes(path, &creation, &access, &write);
    return write;
}

}

TEST_CLASS(AbiDeepLayerRootTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        LayerMountTestShared::AssertHostRefusesPlainPathsPastMaxPath();
    }

    TEST_METHOD(Create_DeepLayerRoots_ShowsTheRootAndReportsTheVolume) {
        TempLayerEnv env(0);
        const DeepLayerRoots roots(env);

        LayerMountHolder mount = CreateOnDeepRoots(env, roots);

        LM_VOLUME_INFO volume{};
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountGetVolumeInfo(mount.Get(), &volume),
            L"The volume read of the deep upper root succeeds");
        Assert::IsTrue(volume.totalSize > 0, L"The volume of the deep upper root has a size");

        LM_RESOLVED_PATH resolved{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountResolvePath(mount.Get(), kRootViewPath.c_str(), &resolved));
        Assert::AreEqual<UINT32>(FILE_ATTRIBUTE_DIRECTORY,
            resolved.attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT),
            L"The root resolves to the attributes of the deep upper root");

        OpenedFile root;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), kRootViewPath.c_str(), GENERIC_READ,
                            FILE_DIRECTORY_FILE, root),
            L"The open of the root succeeds");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(root.handle));
        Assert::IsTrue((root.info.fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
            L"The root opens as a directory");
    }

    TEST_METHOD(CreateFile_UnderDeepUpperRoot_WritesTheHostFileThatOpenReads) {
        TempLayerEnv env(0);
        const DeepLayerRoots roots(env);
        LayerMountHolder mount = CreateOnDeepRoots(env, roots);

        WriteThroughOverlay(mount.Get(), L"\\created.txt", "created");

        Assert::AreEqual<std::string>("created",
            ReadAllBytes(ExtendedPathUnder(roots.upper, L"created.txt")),
            L"The create writes the host file in the deep upper root");
        Assert::AreEqual<std::string>("created",
            ReadThroughMount(mount.Get(), L"\\created.txt", GENERIC_READ),
            L"The open reads the file that the create made");
    }

    TEST_METHOD(WriteFile_LowerFileUnderDeepLowerRoot_CopiesTheFileUpAndKeepsTheLower) {
        TempLayerEnv env(0);
        const DeepLayerRoots roots(env);
        WriteText(ExtendedPathUnder(roots.lower, L"shared.txt"), "lower");
        LayerMountHolder mount = CreateOnDeepRoots(env, roots);

        Assert::AreEqual<std::string>("lower",
            ReadThroughMount(mount.Get(), L"\\shared.txt", GENERIC_READ),
            L"The mount reads the file in the deep lower root");
        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), L"\\shared.txt", GENERIC_READ | GENERIC_WRITE,
                            kNoCreateOptions, opened),
            L"The write open of the lower file succeeds");
        UINT32 written = 0;
        LM_FILE_INFO info{};
        const HRESULT writeHr = WriteFromStart(opened.handle, "upper", 5, &written, &info);
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));

        Assert::AreEqual<HRESULT>(S_OK, writeHr);
        Assert::AreEqual<std::string>("upper",
            ReadAllBytes(ExtendedPathUnder(roots.upper, L"shared.txt")),
            L"The copy-up writes the file in the deep upper root");
        Assert::AreEqual<std::string>("lower",
            ReadAllBytes(ExtendedPathUnder(roots.lower, L"shared.txt")),
            L"The copy-up leaves the file in the deep lower root as it was");
        Assert::AreEqual<std::string>("upper",
            ReadThroughMount(mount.Get(), L"\\shared.txt", GENERIC_READ),
            L"The mount reads the copied-up file");
    }

    TEST_METHOD(RenameFile_UnderDeepUpperRoot_MovesTheHostFileUnderTheNewName) {
        TempLayerEnv env(0);
        const DeepLayerRoots roots(env);
        LayerMountHolder mount = CreateOnDeepRoots(env, roots);
        WriteThroughOverlay(mount.Get(), L"\\source.txt", "moved");

        constexpr BOOL replaceIfExists = FALSE;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountRenameFile(mount.Get(), L"\\source.txt", L"\\target.txt",
                                   replaceIfExists));

        Assert::IsFalse(HostEntryExists(ExtendedPathUnder(roots.upper, L"source.txt")),
            L"The rename removes the old host name from the deep upper root");
        Assert::AreEqual<std::string>("moved",
            ReadAllBytes(ExtendedPathUnder(roots.upper, L"target.txt")),
            L"The rename moves the host file in the deep upper root");
        AssertOpenFailsNotFound(mount.Get(), L"\\source.txt",
            L"The old name is gone from the mount");
        Assert::AreEqual<std::string>("moved",
            ReadThroughMount(mount.Get(), L"\\target.txt", GENERIC_READ),
            L"The new name opens the moved file");
    }

    TEST_METHOD(CopyUp_WithoutAdsUnderDeepUpperRoot_ReportsTheLowerFileId) {
        TempLayerEnv env(0);
        const DeepLayerRoots roots(env);
        const UINT64 lowerId = WriteSharedLowerFile(roots);
        LayerMountHolder mount =
            CreateOnRoots(env, roots, kHostCapabilitiesWithoutAds);

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountEnsureInUpperLayer(mount.Get(), L"\\shared.txt"),
            L"The copy-up writes the sidecar record under the deep upper root");

        Assert::IsTrue(HostEntryExists(ExtendedPathUnder(roots.upper, L"shared.txt")),
            L"The copy-up makes the file in the deep upper root");
        Assert::AreEqual(lowerId, IndexNumberThroughMount(mount.Get(), L"\\shared.txt"),
            L"The open reads the sidecar record and reports the file ID of the lower file");
    }

    TEST_METHOD(RenameFile_CopiedUpWithoutAdsUnderDeepUpperRoot_MovesTheRecordToTheNewName) {
        TempLayerEnv env(0);
        const DeepLayerRoots roots(env);
        const UINT64 lowerId = WriteSharedLowerFile(roots);
        LayerMountHolder mount =
            CreateOnRoots(env, roots, kHostCapabilitiesWithoutAds);
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountEnsureInUpperLayer(mount.Get(), L"\\shared.txt"));

        constexpr BOOL replaceIfExists = FALSE;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountRenameFile(mount.Get(), L"\\shared.txt", L"\\renamed.txt",
                                   replaceIfExists));

        Assert::IsFalse(HostEntryExists(
                SidecarRecordPathOf(roots.upper, roots.upper + L"\\shared.txt")),
            L"The rename moves the sidecar record away from the old name");
        Assert::IsTrue(HostEntryExists(
                SidecarRecordPathOf(roots.upper, roots.upper + L"\\renamed.txt")),
            L"The rename moves the sidecar record to the new name");
        Assert::AreEqual(lowerId, IndexNumberThroughMount(mount.Get(), L"\\renamed.txt"),
            L"The new name reports the file ID of the lower file");
    }

    TEST_METHOD(DeleteFile_CopiedUpWithoutAdsUnderDeepUpperRoot_RemovesTheRecord) {
        TempLayerEnv env(0);
        const DeepLayerRoots roots(env);
        WriteSharedLowerFile(roots);
        LayerMountHolder mount =
            CreateOnRoots(env, roots, kHostCapabilitiesWithoutAds);
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountEnsureInUpperLayer(mount.Get(), L"\\shared.txt"));
        const std::wstring record =
            SidecarRecordPathOf(roots.upper, roots.upper + L"\\shared.txt");
        Assert::IsTrue(HostEntryExists(record),
            L"The copy-up keys the sidecar record by the path of the upper file");

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountDeleteFile(mount.Get(), L"\\shared.txt"));

        Assert::IsFalse(HostEntryExists(record), L"The delete removes the sidecar record");
        AssertOpenFailsNotFound(mount.Get(), L"\\shared.txt",
            L"The deleted name is gone from the mount");
    }

    TEST_METHOD(CopyUp_WithoutAdsUnderExtendedDeepUpperRoot_KeysTheRecordByTheExtendedPath) {
        TempLayerEnv env(0);
        const DeepLayerRoots roots(env);
        const UINT64 lowerId = WriteSharedLowerFile(roots);
        DeepLayerRoots extendedRoots = roots;
        extendedRoots.upper = ExtendedFormOf(roots.upper);
        const std::wstring& extendedUpper = extendedRoots.upper;
        LayerMountHolder mount =
            CreateOnRoots(env, extendedRoots, kHostCapabilitiesWithoutAds);

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountEnsureInUpperLayer(mount.Get(), L"\\shared.txt"));

        Assert::IsTrue(HostEntryExists(
                SidecarRecordPathOf(extendedUpper, extendedUpper + L"\\shared.txt")),
            L"The sidecar key is the hash of the upper file path in the form the caller gave");
        Assert::AreEqual(lowerId, IndexNumberThroughMount(mount.Get(), L"\\shared.txt"),
            L"The open reads the sidecar record and reports the file ID of the lower file");
    }

    TEST_METHOD(WriteFile_EncryptedLowerFileUnderDeepRoots_CopiesTheFileUpEncrypted) {
        TempLayerEnv env(0);
        const DeepLayerRoots roots(env);
        const std::wstring lowerFile = ExtendedPathUnder(roots.lower, L"secret.bin");
        WriteText(lowerFile, "secret");
        if (!EncryptedOrSkipped(lowerFile)) {
            return;
        }
        LayerMountHolder mount = CreateOnDeepRoots(env, roots);

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), L"\\secret.bin", GENERIC_READ | GENERIC_WRITE,
                            kNoCreateOptions, opened),
            L"The write open copies the encrypted lower file up");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));

        const DWORD upperAttributes = UpperAttributesOf(roots, L"secret.bin");
        Assert::IsTrue((upperAttributes & FILE_ATTRIBUTE_ENCRYPTED) != 0,
            L"The copy in the deep upper root is encrypted");
        Assert::AreEqual<std::string>("secret",
            ReadThroughMount(mount.Get(), L"\\secret.bin", GENERIC_READ),
            L"The mount reads the copied-up file");
    }

    TEST_METHOD(EnsureInUpperLayer_EncryptedLowerDirectoryUnderDeepRoots_CopiesItUpEncrypted) {
        TempLayerEnv env(0);
        const DeepLayerRoots roots(env);
        const std::wstring lowerDirectory = ExtendedPathUnder(roots.lower, L"Foo");
        Assert::IsTrue(::CreateDirectoryW(lowerDirectory.c_str(), nullptr) != FALSE,
            L"Precondition: the test makes the lower Foo");
        if (!EncryptedOrSkipped(lowerDirectory)) {
            return;
        }
        LayerMountHolder mount = CreateOnDeepRoots(env, roots);

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountEnsureInUpperLayer(mount.Get(), L"\\Foo"),
            L"The copy-up of the encrypted lower directory succeeds");

        const DWORD upperAttributes = UpperAttributesOf(roots, L"Foo");
        Assert::IsTrue((upperAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
            L"The upper Foo is a directory");
        Assert::IsTrue((upperAttributes & FILE_ATTRIBUTE_ENCRYPTED) != 0,
            L"The upper Foo is encrypted like the lower Foo");
    }

    TEST_METHOD(EnsureInUpperLayer_LowerDirectoryUnderDeepRoots_CopiesItsAceToTheUpper) {
        TempLayerEnv env(0);
        const DeepLayerRoots roots(env);
        const std::wstring lowerDirectory = ExtendedPathUnder(roots.lower, L"Foo");
        WriteText(lowerDirectory + L"\\a.txt", "child");
        EveryoneSid everyone;
        Assert::IsNotNull(everyone.sid);
        GrantInheritableReadAttributes(lowerDirectory, everyone.sid);
        if (CountDaclAcesForSid(ExtendedFormOf(roots.upper), everyone.sid) != 0) {
            Logger::WriteMessage(L"[SKIP] The deep upper root already holds an ACE for Everyone, "
                                 L"so the upper Foo would inherit it without the copy-up");
            return;
        }
        LayerMountHolder mount = CreateOnDeepRoots(env, roots);

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountEnsureInUpperLayer(mount.Get(), L"\\Foo"),
            L"The copy-up of the lower directory succeeds");

        Assert::IsTrue(
            CountDaclAcesForSid(ExtendedPathUnder(roots.upper, L"Foo"), everyone.sid) > 0,
            L"The upper Foo carries the ACE for Everyone of the lower Foo");
    }

    TEST_METHOD(RenameFile_ReplaceThatFailsUnderDeepRoots_MovesTheTargetBack) {
        TempLayerEnv env(0);
        const DeepLayerRoots roots(env);
        LayerMountHolder mount = CreateOnDeepRoots(env, roots);
        WriteThroughOverlay(mount.Get(), L"\\source.txt", "source");
        WriteThroughOverlay(mount.Get(), L"\\target.txt", "target");
        const std::wstring stagingArea = ExtendedFormOf(StagingArea(roots.work));
        const FILETIME stamped = MakeFileTime(2001, 1, 1);
        StampTimes(stagingArea, stamped, stamped, stamped);
        const FILETIME beforeRename = LastWriteTimeOf(stagingArea);
        Assert::AreEqual(0L, ::CompareFileTime(&stamped, &beforeRename),
            L"Precondition: the staging area carries the stamped last-write time");

        const HANDLE sourceOpenWithoutShareDelete =
            ::CreateFileW(ExtendedPathUnder(roots.upper, L"source.txt").c_str(), GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
        Assert::IsTrue(sourceOpenWithoutShareDelete != INVALID_HANDLE_VALUE,
            L"The test holds the source open");
        constexpr BOOL replaceIfExists = TRUE;
        const HRESULT renameHr = ::LayerMountRenameFile(mount.Get(), L"\\source.txt",
                                                        L"\\target.txt", replaceIfExists);
        ::CloseHandle(sourceOpenWithoutShareDelete);

        Assert::IsTrue(FAILED(renameHr),
            L"The rename fails while the test holds the source open");
        const FILETIME afterRename = LastWriteTimeOf(stagingArea);
        Assert::AreNotEqual(0L, ::CompareFileTime(&stamped, &afterRename),
            L"The target moved through the staging area, so its last-write time changed");
        Assert::AreEqual<std::string>("target",
            ReadAllBytes(ExtendedPathUnder(roots.upper, L"target.txt")),
            L"The failed rename moves the target back to its name in the deep upper root");
        Assert::AreEqual<std::string>("target",
            ReadThroughMount(mount.Get(), L"\\target.txt", GENERIC_READ),
            L"The mount reads the target as it was");
        Assert::IsTrue(SortedNamesIn(stagingArea).empty(),
            L"The staging area holds nothing after the failed rename");
    }

    TEST_METHOD(RenameFile_LowerDirectoryUnderDeepRoots_MovesItsEntriesUnderTheNewName) {
        TempLayerEnv env(0);
        const DeepLayerRoots roots(env);
        WriteText(ExtendedPathUnder(roots.lower, L"source\\a.txt"), "a");
        WriteText(ExtendedPathUnder(roots.lower, L"source\\b.txt"), "b");
        LayerMountHolder mount = CreateOnDeepRoots(env, roots);

        constexpr BOOL replaceIfExists = FALSE;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountRenameFile(mount.Get(), L"\\source", L"\\target", replaceIfExists),
            L"The rename of the lower directory succeeds");

        AssertOpenFailsNotFound(mount.Get(), L"\\source",
            L"The old directory name is gone from the mount");
        for (const wchar_t* child : {L"target\\a.txt", L"target\\b.txt"}) {
            Assert::IsTrue(HostEntryExists(ExtendedPathUnder(roots.upper, child)),
                (L"The rename copies " + std::wstring(child) + L" into the deep upper root")
                    .c_str());
        }
        Assert::AreEqual<std::string>("a",
            ReadThroughMount(mount.Get(), L"\\target\\a.txt", GENERIC_READ),
            L"An entry of the renamed directory opens under the new name");
    }
};

}
