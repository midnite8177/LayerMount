#include "pch.h"
#include "TestFixture.h"

#include "MetadataADS.h"
#include "LayerMount.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;

namespace LayerMountTests {

namespace {

// While the object lives, a read of the second megabyte of the file fails.
class LockPastFirstMegabyte {
public:
    explicit LockPastFirstMegabyte(const std::wstring& path)
        : handle_(::CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, 0, nullptr)) {
        Assert::AreNotEqual<HANDLE>(INVALID_HANDLE_VALUE, handle_,
            L"LockPastFirstMegabyte: CreateFileW must succeed");
        region_.Offset = 1024 * 1024;
        Assert::IsTrue(::LockFileEx(handle_,
                                    LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                                    0, 1, 0, &region_) != FALSE,
            L"LockPastFirstMegabyte: LockFileEx must succeed");
    }
    ~LockPastFirstMegabyte() {
        ::UnlockFileEx(handle_, 0, 1, 0, &region_);
        ::CloseHandle(handle_);
    }
    LockPastFirstMegabyte(const LockPastFirstMegabyte&) = delete;
    LockPastFirstMegabyte& operator=(const LockPastFirstMegabyte&) = delete;

private:
    HANDLE handle_;
    OVERLAPPED region_{};
};

void WriteADS(const std::wstring& basePath, const std::wstring& streamName,
              const std::string& content) {
    const std::wstring adsPath = basePath + L":" + streamName;
    HANDLE h = ::CreateFileW(adsPath.c_str(),
                              GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Assert::AreNotEqual<HANDLE>(INVALID_HANDLE_VALUE, h,
        L"WriteADS: CreateFileW on :stream must succeed");
    DWORD w = 0;
    ::WriteFile(h, content.data(), static_cast<DWORD>(content.size()),
                &w, nullptr);
    ::CloseHandle(h);
}

bool ADSExists(const std::wstring& basePath, const std::wstring& streamName) {
    const std::wstring adsPath = basePath + L":" + streamName;
    HANDLE h = ::CreateFileW(adsPath.c_str(),
                              GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ::CloseHandle(h);
    return true;
}

bool IsCompressed(const std::wstring& path) {
    return HasAttribute(path, FILE_ATTRIBUTE_COMPRESSED);
}

bool IsSparse(const std::wstring& path) {
    return HasAttribute(path, FILE_ATTRIBUTE_SPARSE_FILE);
}

bool OpenAndIoctl(const std::wstring& path, DWORD controlCode,
                  const void* input, DWORD inputSize) {
    HANDLE h = ::CreateFileW(path.c_str(),
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD br = 0;
    const BOOL ok = ::DeviceIoControl(h, controlCode,
                                       const_cast<void*>(input), inputSize,
                                       nullptr, 0, &br, nullptr);
    ::CloseHandle(h);
    return ok != FALSE;
}

bool MakeSparseWithHole(const std::wstring& path, LONGLONG holeBytes) {
    FILE_SET_SPARSE_BUFFER sparse{TRUE};
    if (!OpenAndIoctl(path, FSCTL_SET_SPARSE, &sparse, sizeof(sparse))) {
        return false;
    }
    FILE_ZERO_DATA_INFORMATION hole{};
    hole.FileOffset.QuadPart      = 0;
    hole.BeyondFinalZero.QuadPart = holeBytes;
    return OpenAndIoctl(path, FSCTL_SET_ZERO_DATA, &hole, sizeof(hole));
}

} // namespace

TEST_CLASS(LazyMetacopyFidelityTests) {
public:
    TEST_METHOD(LazyCompletion_PreservesSourceTimestamps) {
        // 2 MiB: above the 1 MiB metacopy threshold.
        LayerMountTests::TempLayerEnvironment env(1);
        const std::string payload(2 * 1024 * 1024, 'Z');
        env.WriteFile(env.Lower(0), L"ts.bin", payload);

        const std::wstring srcPath = env.Lower(0) + L"\\ts.bin";
        const FILETIME srcCreation = MakeFileTime(2015, 6, 15);
        const FILETIME srcAccess   = MakeFileTime(2016, 6, 15);
        const FILETIME srcWrite    = MakeFileTime(2017, 6, 15);
        StampFile(srcPath, srcCreation, srcAccess, srcWrite);

        CopyUpRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpMetadataOnly(L"ts.bin")));

        const std::wstring upperPath = env.Upper() + L"\\ts.bin";

        // Pre-completion: the metacopy shell has the source timestamps.
        {
            FILETIME c{}, a{}, w{};
            GetTimes(upperPath, &c, &a, &w);
            Assert::IsTrue(FileTimesEqual(c, srcCreation),
                L"Metacopy shell must preserve source creation time");
            Assert::IsTrue(FileTimesEqual(w, srcWrite),
                L"Metacopy shell must preserve source write time");
        }

        // Trigger lazy completion.
        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CompleteLazyCopyUp(L"ts.bin")));

        FILETIME c{}, a{}, w{};
        GetTimes(upperPath, &c, &a, &w);

        // An untouched shell carries the source's times, so the fill
        // leaves them equal to the source. The write time is the one the
        // data write and the metadata write change.
        Assert::IsTrue(FileTimesEqual(w, srcWrite),
            L"Upper LastWriteTime must match the source after lazy "
            L"completion, matching the eager-path invariant");
        Assert::IsTrue(FileTimesEqual(c, srcCreation),
            L"Upper creation time must match the source after lazy "
            L"completion");
    }

    TEST_METHOD(LazyCompletion_KeepsShellTimestampsSetAfterStaging) {
        LayerMountTests::TempLayerEnvironment env(1);
        const std::string payload(2 * 1024 * 1024, 'Q');
        env.WriteFile(env.Lower(0), L"shell-ts.bin", payload);

        const std::wstring srcPath = env.Lower(0) + L"\\shell-ts.bin";
        StampFile(srcPath, MakeFileTime(2015, 6, 15), MakeFileTime(2016, 6, 15),
                  MakeFileTime(2017, 6, 15));

        CopyUpRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpMetadataOnly(L"shell-ts.bin")));

        const std::wstring upperPath = env.Upper() + L"\\shell-ts.bin";
        const FILETIME shellCreation = MakeFileTime(2020, 1, 10);
        const FILETIME shellAccess   = MakeFileTime(2021, 1, 10);
        const FILETIME shellWrite    = MakeFileTime(2022, 1, 10);
        StampFile(upperPath, shellCreation, shellAccess, shellWrite);

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CompleteLazyCopyUp(L"shell-ts.bin")));

        // A read of the file can update the access time, so the check
        // covers creation and write time.
        FILETIME c{}, a{}, w{};
        GetTimes(upperPath, &c, &a, &w);
        Assert::IsTrue(FileTimesEqual(w, shellWrite),
            L"Upper LastWriteTime must keep the time set on the shell after "
            L"lazy completion");
        Assert::IsTrue(FileTimesEqual(c, shellCreation),
            L"Upper creation time must keep the time set on the shell after "
            L"lazy completion");
    }

    TEST_METHOD(LazyCompletion_KeepsShellTimestampsWhenTheFillFails) {
        LayerMountTests::TempLayerEnvironment env(1);
        const std::string payload(2 * 1024 * 1024, 'F');
        env.WriteFile(env.Lower(0), L"fail-ts.bin", payload);

        CopyUpRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpMetadataOnly(L"fail-ts.bin")));

        const std::wstring upperPath = env.Upper() + L"\\fail-ts.bin";
        const FILETIME shellCreation = MakeFileTime(2020, 1, 10);
        const FILETIME shellAccess   = MakeFileTime(2021, 1, 10);
        const FILETIME shellWrite    = MakeFileTime(2022, 1, 10);
        StampFile(upperPath, shellCreation, shellAccess, shellWrite);

        NTSTATUS fillStatus = STATUS_SUCCESS;
        {
            LockPastFirstMegabyte lock(env.Lower(0) + L"\\fail-ts.bin");
            fillStatus = rig.copyUp.CompleteLazyCopyUp(L"fail-ts.bin");
        }

        Assert::IsFalse(NT_SUCCESS(fillStatus),
            L"The fill must fail when a read of the lower file fails");

        FILETIME c{}, a{}, w{};
        GetTimes(upperPath, &c, &a, &w);
        Assert::IsTrue(FileTimesEqual(w, shellWrite),
            L"Upper LastWriteTime must keep the time set on the shell after "
            L"a failed fill");
        Assert::IsTrue(FileTimesEqual(c, shellCreation),
            L"Upper creation time must keep the time set on the shell after "
            L"a failed fill");
    }

    TEST_METHOD(LazyCompletion_PreservesUserADS) {
        UNIT_SKIP_IF_NOT_NTFS();

        LayerMountTests::TempLayerEnvironment env(1);
        const std::string payload(2 * 1024 * 1024, 'L');
        env.WriteFile(env.Lower(0), L"ads.bin", payload);

        const std::wstring srcPath = env.Lower(0) + L"\\ads.bin";
        WriteADS(srcPath, L"Zone.Identifier", "[ZoneTransfer]\r\nZoneId=3\r\n");
        WriteADS(srcPath, L"custom",          "user-metadata-payload");

        Assert::IsTrue(ADSExists(srcPath, L"Zone.Identifier"),
            L"Preconditions: source ADS must be present");

        CopyUpRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpMetadataOnly(L"ads.bin")));
        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CompleteLazyCopyUp(L"ads.bin")));

        const std::wstring upperPath = env.Upper() + L"\\ads.bin";

        // The overlay's own :overlay stream should exist (metacopy cleared).
        LayerMountMetadata md = MetadataADS::ReadLayerMountMetadata(upperPath, nullptr);
        Assert::IsFalse(md.metacopy,
            L"metacopy flag must clear after successful lazy completion");

        // User ADS carried through the lazy path.
        Assert::IsTrue(ADSExists(upperPath, L"Zone.Identifier"),
            L"Zone.Identifier must survive lazy metacopy + completion");
        Assert::IsTrue(ADSExists(upperPath, L"custom"),
            L"Custom user ADS must survive lazy metacopy + completion");
    }

    TEST_METHOD(LazyCompletion_PreservesCompression) {
        UNIT_SKIP_IF_NOT_NTFS();

        LayerMountTests::TempLayerEnvironment env(1);
        // 2 MiB puts the file above the metacopy threshold.
        const std::string payload(2 * 1024 * 1024, 'c');
        env.WriteFile(env.Lower(0), L"cmp.bin", payload);

        const std::wstring srcPath = env.Lower(0) + L"\\cmp.bin";
        if (!EnableCompression(srcPath)) {
            Logger::WriteMessage(
                L"[SKIP] The volume refused FSCTL_SET_COMPRESSION on the source");
            return;
        }
        Assert::IsTrue(IsCompressed(srcPath),
            L"Preconditions: source file must report FILE_ATTRIBUTE_COMPRESSED");

        CopyUpRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpMetadataOnly(L"cmp.bin")));
        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CompleteLazyCopyUp(L"cmp.bin")));

        const std::wstring upperPath = env.Upper() + L"\\cmp.bin";
        Assert::IsTrue(IsCompressed(upperPath),
            L"Compressed lower file must remain compressed in upper after "
            L"metacopy + lazy completion");
    }

    TEST_METHOD(MetacopyFill_KeepsSparseAttributeWhenSourceIsSparse) {
        UNIT_SKIP_IF_NOT_NTFS();

        LayerMountTests::TempLayerEnvironment env(1);
        const std::string payload(2 * 1024 * 1024, 's');
        env.WriteFile(env.Lower(0), L"sparse.bin", payload);

        const std::wstring srcPath = env.Lower(0) + L"\\sparse.bin";
        if (!MakeSparseWithHole(srcPath, 1024 * 1024)) {
            Logger::WriteMessage(
                L"[SKIP] NTFS refused FSCTL_SET_SPARSE on source. "
                L"The volume does not support sparse files.");
            return;
        }
        Assert::IsTrue(IsSparse(srcPath),
            L"Preconditions: source file must report FILE_ATTRIBUTE_SPARSE_FILE");

        CopyUpRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpMetadataOnly(L"sparse.bin")));
        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CompleteLazyCopyUp(L"sparse.bin")));

        const std::wstring upperPath = env.Upper() + L"\\sparse.bin";
        Assert::IsTrue(IsSparse(upperPath),
            L"A filled shell of a sparse lower file stays sparse");
    }

    TEST_METHOD(MetacopyFill_KeepsHolesOfSparseSource) {
        UNIT_SKIP_IF_NOT_NTFS();

        LayerMountTests::TempLayerEnvironment env(1);
        const LONGLONG logical = 3LL * 1024 * 1024;
        const LONGLONG holeBytes = 2LL * 1024 * 1024;
        const std::string payload(static_cast<size_t>(logical), 's');
        env.WriteFile(env.Lower(0), L"sparse.bin", payload);

        const std::wstring srcPath = env.Lower(0) + L"\\sparse.bin";
        if (!MakeSparseWithHole(srcPath, holeBytes)) {
            Logger::WriteMessage(
                L"[SKIP] NTFS refused FSCTL_SET_SPARSE on source. "
                L"The volume does not support sparse files.");
            return;
        }
        Assert::IsTrue(LayerMountTests::AllocatedBytes(srcPath) < holeBytes,
            L"Precondition: the source allocates only the bytes past the hole");

        CopyUpRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpMetadataOnly(L"sparse.bin")));
        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CompleteLazyCopyUp(L"sparse.bin")));

        const std::wstring upperPath = env.Upper() + L"\\sparse.bin";
        Assert::IsTrue(IsSparse(upperPath),
            L"A filled shell of a sparse lower file stays sparse");
        Assert::AreEqual(logical, LayerMountTests::LogicalBytes(upperPath),
            L"The filled file has the logical size of the source");
        Assert::IsTrue(LayerMountTests::AllocatedBytes(upperPath) < holeBytes,
            L"The filled file allocates its data range only, not its hole");
        const DWORD tail = 1024 * 1024;
        Assert::AreEqual(std::string(tail, 's'),
                         LayerMountTests::ReadRange(upperPath, holeBytes, tail),
            L"The data past the hole matches the source");
        Assert::AreEqual(std::string(64 * 1024, '\0'),
                         LayerMountTests::ReadRange(upperPath, 0, 64 * 1024),
            L"The hole of the filled file reads as zeros");
    }

    TEST_METHOD(EagerCopyUp_PreservesTimestampsADSAndCompression) {
        UNIT_SKIP_IF_NOT_NTFS();

        LayerMountTests::TempLayerEnvironment env(1);
        const std::string payload(64 * 1024, 'E'); // below metacopy threshold
        env.WriteFile(env.Lower(0), L"eager.bin", payload);

        const std::wstring srcPath = env.Lower(0) + L"\\eager.bin";
        WriteADS(srcPath, L"Zone.Identifier",
                 "[ZoneTransfer]\r\nZoneId=3\r\n");
        const bool compressed = EnableCompression(srcPath);
        // Stamp LAST, after all ADS / compression writes have bumped times.
        const FILETIME srcWrite = MakeFileTime(2017, 6, 15);
        StampFile(srcPath, srcWrite, srcWrite, srcWrite);

        CopyUpRig rig(env.MakeConfig());

        Assert::IsTrue(NT_SUCCESS(rig.copyUp.CopyUpFile(L"eager.bin")));

        const std::wstring upperPath = env.Upper() + L"\\eager.bin";

        FILETIME c{}, a{}, w{};
        GetTimes(upperPath, &c, &a, &w);
        Assert::IsTrue(FileTimesEqual(w, srcWrite),
            L"Eager copy-up must preserve LastWriteTime");
        Assert::IsTrue(ADSExists(upperPath, L"Zone.Identifier"),
            L"Eager copy-up must preserve user ADS");

        if (compressed) {
            Assert::IsTrue(IsCompressed(upperPath),
                L"Eager copy-up must preserve FILE_ATTRIBUTE_COMPRESSED");
        }
    }
};

} // namespace LayerMountTests
