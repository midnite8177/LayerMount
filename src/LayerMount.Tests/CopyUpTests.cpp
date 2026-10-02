#include "pch.h"
#include "TestFixture.h"

#include "CopyUp.h"
#include "MetadataStore.h"

#include <winioctl.h>
#include <set>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;

namespace LayerMountTests {

static_assert(RefusesTemporaryConfig<CopyUp, PathResolver&, WhiteoutManager&, Cache&, LayerMountStats&>,
    "CopyUp keeps a reference to its LayerConfig");

TEST_CLASS(CopyUpTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(GenerateWorkPath_ReturnsUniquePathInWorkDir) {
        TempLayerEnvironment env(1);
        CopyUpRig rig(env.MakeConfig());

        std::wstring a = rig.copyUp.GenerateWorkPath();
        std::wstring b = rig.copyUp.GenerateWorkPath();

        Assert::AreNotEqual(a, b, L"Two calls should return different paths");
        Assert::IsTrue(a.find(env.Work()) == 0, L"Path should be under work dir");
        Assert::IsTrue(a.find(L".tmp") != std::wstring::npos, L"Path should end with .tmp");
    }

     TEST_METHOD(GenerateWorkPath_LongWorkDirPath_StillUniqueNoTruncation) {
         TempLayerEnvironment env(1);
         LayerConfig config = env.MakeConfig();

         std::wstring deepBase = env.Root();
         while (deepBase.size() < 240) {
             deepBase += L"\\padding-segment";
         }
         // create_directories fails on a path past MAX_PATH without the \\?\ prefix.
         const std::wstring deepWork = L"\\\\?\\" + deepBase + L"\\overlay-work";
         std::error_code ec;
         std::filesystem::create_directories(deepWork, ec);
         Assert::IsFalse(static_cast<bool>(ec),
             L"Precondition: long-path work dir must be creatable with \\?\\ prefix");

         config.workDirPath = deepWork;

         CopyUpRig rig(config);

         std::vector<std::wstring> paths;
         constexpr int kCount = 32;
         paths.reserve(kCount);
         for (int i = 0; i < kCount; ++i) {
             paths.push_back(rig.copyUp.GenerateWorkPath());
         }

         for (const auto& p : paths) {
             Assert::IsTrue(p.size() > deepWork.size(),
                 L"Generated path must be longer than the work dir prefix");
             Assert::IsTrue(p.compare(0, deepWork.size(), deepWork) == 0,
                 L"Generated path must start with the full workDirPath");
             Assert::IsTrue(p.find(L".tmp") == p.size() - 4,
                 L"Generated path must end with .tmp");
         }

         std::set<std::wstring> uniq(paths.begin(), paths.end());
         wchar_t msg[200];
         swprintf_s(msg, L"Generated %d work paths, only %zu unique under a "
                         L"long work dir.",
                    kCount, uniq.size());
         Assert::IsTrue(uniq.size() == static_cast<size_t>(kCount), msg);
     }

    TEST_METHOD(CleanWorkDirectory_RemovesHashTempFiles_LeavesOthers) {
        TempLayerEnvironment env(1);
        CopyUpRig rig(env.MakeConfig());

        std::wstring tempFile = env.Work() + L"\\#abc.tmp";
        HANDLE h1 = ::CreateFileW(tempFile.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ::CloseHandle(h1);

        std::wstring otherFile = env.Work() + L"\\other.txt";
        HANDLE h2 = ::CreateFileW(otherFile.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ::CloseHandle(h2);

        rig.copyUp.CleanWorkDirectory();

        Assert::AreEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(tempFile.c_str()),
            L"Hash-prefixed .tmp file should be removed");
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(otherFile.c_str()),
            L"Non-matching file should remain");
    }

    TEST_METHOD(CleanWorkDirectory_ExtendedWorkDirWithTrailingSeparator_RemovesHashTempFiles) {
        TempLayerEnvironment env(1);
        LayerConfig config = env.MakeConfig();
        config.workDirPath = ExtendedDirWithSeparator(env.Work());
        CopyUpRig rig(config);

        std::wstring tempFile = env.Work() + L"\\#abc.tmp";
        env.WriteFile(env.Work(), L"#abc.tmp", "staged");

        rig.copyUp.CleanWorkDirectory();

        Assert::AreEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(tempFile.c_str()),
            L"CleanWorkDirectory must delete the #abc.tmp file");
    }

    TEST_METHOD(CommitFromWorkDir_MovesFileFromWorkToFinalPath) {
        TempLayerEnvironment env(1);
        CopyUpRig rig(env.MakeConfig());

        std::wstring workPath = env.Work() + L"\\test.tmp";
        std::wstring finalPath = env.Upper() + L"\\final.txt";
        env.WriteFile(env.Work(), L"test.tmp", "content");

        NTSTATUS status = rig.copyUp.CommitFromWorkDir(workPath, finalPath);

        Assert::IsTrue(NT_SUCCESS(status), L"CommitFromWorkDir should succeed");
        Assert::AreEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(workPath.c_str()),
            L"Work path should no longer exist");
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(finalPath.c_str()),
            L"Final path should exist");
    }

    TEST_METHOD(CommitFromWorkDir_CreatesParentDirectoriesIfMissing) {
        TempLayerEnvironment env(1);
        CopyUpRig rig(env.MakeConfig());

        std::wstring workPath = env.Work() + L"\\temp.tmp";
        env.WriteFile(env.Work(), L"temp.tmp", "x");
        std::wstring finalPath = env.Upper() + L"\\a\\b\\c.txt";

        NTSTATUS status = rig.copyUp.CommitFromWorkDir(workPath, finalPath);

        Assert::IsTrue(NT_SUCCESS(status));
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(finalPath.c_str()),
            L"Final path under nested dirs should exist");
    }

    TEST_METHOD(CopyUpFile_LowerFileOnly_CopiesContentToUpper) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"foo.txt", "lower content");

        CopyUpRig rig(env.MakeConfig());

        NTSTATUS status = rig.copyUp.CopyUpFile(L"foo.txt");
        Assert::IsTrue(NT_SUCCESS(status));

        Assert::IsTrue(env.FileExists(env.Upper(), L"foo.txt"));
        Assert::AreEqual(std::string("lower content"),
            env.ReadFile(env.Upper(), L"foo.txt"));
    }

    TEST_METHOD(CopyUpFile_ExtendedWorkDirWithTrailingSeparator_CopiesContentToUpper) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"foo.txt", "lower content");

        LayerConfig config = env.MakeConfig();
        config.workDirPath = ExtendedDirWithSeparator(env.Work());
        CopyUpRig rig(config);

        NTSTATUS status = rig.copyUp.CopyUpFile(L"foo.txt");
        Assert::IsTrue(NT_SUCCESS(status),
            L"Copy-up must stage through a work dir that ends in a separator");

        Assert::AreEqual(std::string("lower content"),
            env.ReadFile(env.Upper(), L"foo.txt"));
    }

    TEST_METHOD(CopyUpFile_PreservesFileSize) {
        TempLayerEnvironment env(1);
        std::string content(8192, 'A');
        env.WriteFile(env.Lower(0), L"big.bin", content);

        CopyUpRig rig(env.MakeConfig());

        rig.copyUp.CopyUpFile(L"big.bin");

        std::string copied = env.ReadFile(env.Upper(), L"big.bin");
        Assert::AreEqual(content.size(), copied.size());
    }

    TEST_METHOD(CopyUpFile_PreservesTimestamps) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"ts.txt", "x");

        std::wstring srcPath = env.Lower(0) + L"\\ts.txt";
        FILETIME srcCreation, srcAccess, srcWrite;
        {
            HANDLE h = ::CreateFileW(srcPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
            ::GetFileTime(h, &srcCreation, &srcAccess, &srcWrite);
            ::CloseHandle(h);
        }

        CopyUpRig rig(env.MakeConfig());

        rig.copyUp.CopyUpFile(L"ts.txt");

        std::wstring upPath = env.Upper() + L"\\ts.txt";
        FILETIME dstCreation, dstAccess, dstWrite;
        {
            HANDLE h = ::CreateFileW(upPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
            ::GetFileTime(h, &dstCreation, &dstAccess, &dstWrite);
            ::CloseHandle(h);
        }

        Assert::IsTrue(::CompareFileTime(&srcCreation, &dstCreation) == 0,
            L"CreationTime not preserved");
        Assert::IsTrue(::CompareFileTime(&srcWrite, &dstWrite) == 0,
            L"LastWriteTime not preserved");
    }

    TEST_METHOD(CopyUpFile_PreservesAttributes) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"readonly.txt", "x");

        std::wstring srcPath = env.Lower(0) + L"\\readonly.txt";
        ::SetFileAttributesW(srcPath.c_str(), FILE_ATTRIBUTE_READONLY);

        CopyUpRig rig(env.MakeConfig());

        rig.copyUp.CopyUpFile(L"readonly.txt");

        std::wstring upPath = env.Upper() + L"\\readonly.txt";
        DWORD upAttrs = ::GetFileAttributesW(upPath.c_str());
        Assert::IsTrue((upAttrs & FILE_ATTRIBUTE_READONLY) != 0,
            L"READONLY attribute not preserved");
    }

    TEST_METHOD(CopyUpFile_CreatesParentDirectoryInUpper) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sub\\nested.txt", "x");

        CopyUpRig rig(env.MakeConfig());

        rig.copyUp.CopyUpFile(L"sub\\nested.txt");

        std::wstring upSub = env.Upper() + L"\\sub";
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(upSub.c_str()),
            L"Parent dir should be auto-created in upper");
    }

    TEST_METHOD(CopyUpFile_InvalidatesCacheForPath) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"c.txt", "x");

        CopyUpRig rig(env.MakeConfig());

        rig.resolver.ResolvePath(L"c.txt");
        Assert::IsTrue(rig.cache.Get(L"c.txt").has_value(), L"Should be cached");

        rig.copyUp.CopyUpFile(L"c.txt");

        Assert::IsFalse(rig.cache.Get(L"c.txt").has_value(),
            L"CopyUpFile should invalidate the cache");
    }

    TEST_METHOD(CopyUpFile_AlreadyInUpper_IsNoOp) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"both.txt", "upper");
        env.WriteFile(env.Lower(0), L"both.txt", "lower");

        CopyUpRig rig(env.MakeConfig());

        NTSTATUS status = rig.copyUp.CopyUpFile(L"both.txt");
        Assert::IsTrue(NT_SUCCESS(status));
        Assert::AreEqual(std::string("upper"),
            env.ReadFile(env.Upper(), L"both.txt"),
            L"Upper content should be unchanged");
    }

    TEST_METHOD(CopyUpFile_IncrementsCopyUpCount) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"count.txt", "x");

        CopyUpRig rig(env.MakeConfig());

        uint64_t before = rig.stats.copyUpCount.load();
        rig.copyUp.CopyUpFile(L"count.txt");
        Assert::AreEqual(before + 1, rig.stats.copyUpCount.load());
    }

    TEST_METHOD(CopyUpDirectory_LowerDir_CreatesEntryOnlyNotContents) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"mydir");
        env.WriteFile(env.Lower(0), L"mydir\\child.txt", "child");

        CopyUpRig rig(env.MakeConfig());

        rig.copyUp.CopyUpDirectory(L"mydir");

        std::wstring upDir = env.Upper() + L"\\mydir";
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(upDir.c_str()),
            L"Directory entry should be created in upper");

        std::wstring upChild = env.Upper() + L"\\mydir\\child.txt";
        Assert::AreEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(upChild.c_str()),
            L"Child file should NOT be recursively copied");
    }

    TEST_METHOD(CopyUpDirectory_PreservesDirectoryAttributes) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"hidden_dir");

        std::wstring srcDir = env.Lower(0) + L"\\hidden_dir";
        ::SetFileAttributesW(srcDir.c_str(),
            FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN);

        CopyUpRig rig(env.MakeConfig());

        rig.copyUp.CopyUpDirectory(L"hidden_dir");

        std::wstring upDir = env.Upper() + L"\\hidden_dir";
        DWORD upAttrs = ::GetFileAttributesW(upDir.c_str());
        Assert::IsTrue((upAttrs & FILE_ATTRIBUTE_HIDDEN) != 0,
            L"HIDDEN attribute should be preserved for directory");
    }

    TEST_METHOD(CopyUpDirectory_PreservesDirectoryTimestamps) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"td");

        std::wstring srcDir = env.Lower(0) + L"\\td";
        FILETIME srcCreation, srcAccess, srcWrite;
        {
            HANDLE h = ::CreateFileW(srcDir.c_str(), GENERIC_READ,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            ::GetFileTime(h, &srcCreation, &srcAccess, &srcWrite);
            ::CloseHandle(h);
        }

        CopyUpRig rig(env.MakeConfig());

        rig.copyUp.CopyUpDirectory(L"td");

        std::wstring upDir = env.Upper() + L"\\td";
        FILETIME dstCreation, dstAccess, dstWrite;
        {
            HANDLE h = ::CreateFileW(upDir.c_str(), GENERIC_READ,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            ::GetFileTime(h, &dstCreation, &dstAccess, &dstWrite);
            ::CloseHandle(h);
        }

        Assert::IsTrue(::CompareFileTime(&srcCreation, &dstCreation) == 0,
            L"Dir CreationTime not preserved");
        Assert::IsTrue(::CompareFileTime(&srcWrite, &dstWrite) == 0,
            L"Dir LastWriteTime not preserved");
    }

    TEST_METHOD(RenameUpperDirectory_MovesDirectoryOnly) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"srcdir");
        env.WriteFile(env.Upper(), L"srcdir\\file.txt", "data");

        CopyUpRig rig(env.MakeConfig());

        NTSTATUS status = rig.copyUp.RenameUpperDirectory(
            CallerPath(L"srcdir"), CallerPath(L"dstdir"), ReplaceExisting::No);
        Assert::IsTrue(NT_SUCCESS(status));

        std::wstring src = env.Upper() + L"\\srcdir";
        std::wstring dst = env.Upper() + L"\\dstdir";
        Assert::AreEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(src.c_str()),
            L"Source should no longer exist");
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(dst.c_str()),
            L"Dest should exist");
        Assert::IsTrue(env.FileExists(env.Upper(), L"dstdir\\file.txt"),
            L"File should move with the directory");
    }

    TEST_METHOD(RenameLowerDirectory_CopiesUpAndCreatesWhiteout) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"ldir");
        env.WriteFile(env.Lower(0), L"ldir\\file.txt", "lower data");

        CopyUpRig rig(env.MakeConfig());

        NTSTATUS status = rig.copyUp.RenameLowerDirectory(
            CallerPath(L"ldir"), CallerPath(L"newdir"), ReplaceExisting::No);
        Assert::IsTrue(NT_SUCCESS(status));

        Assert::IsTrue(env.FileExists(env.Upper(), L"newdir\\file.txt"));

        Assert::IsTrue(rig.whiteouts.IsOpaque(L"newdir"));

        Assert::IsTrue(rig.whiteouts.HasWhiteout(L"ldir", env.Upper()));
    }
};

TEST_CLASS(MetacopyTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(CopyUpMetadataOnly_CreatesFileWithSourceSizeButNoData) {
        TempLayerEnvironment env(1);
        std::string content(4096, 'Z');
        env.WriteFile(env.Lower(0), L"lazy.bin", content);

        CopyUpRig rig(env.MakeConfig());

        rig.copyUp.CopyUpMetadataOnly(L"lazy.bin");

        std::wstring upPath = env.Upper() + L"\\lazy.bin";

        HANDLE h = ::CreateFileW(upPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
        Assert::AreNotEqual(INVALID_HANDLE_VALUE, h);
        LARGE_INTEGER sz = {};
        ::GetFileSizeEx(h, &sz);
        ::CloseHandle(h);
        Assert::AreEqual(static_cast<LONGLONG>(content.size()), sz.QuadPart,
            L"Logical size should match source");
    }

    TEST_METHOD(CopyUpMetadataOnly_WritesMetacopyADS) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"m.txt", "x");

        CopyUpRig rig(env.MakeConfig());

        rig.copyUp.CopyUpMetadataOnly(L"m.txt");

        std::wstring upPath = env.Upper() + L"\\m.txt";
        LayerMountMetadata md = MetadataStore::ReadLayerMountMetadata(upPath, nullptr);
        Assert::IsTrue(md.metacopy, L"metacopy flag should be set");
        Assert::IsFalse(md.originLayer.empty(), L"originLayer should be set");
    }

    TEST_METHOD(CopyUpMetadataOnly_PreservesTimestamps) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"lts.txt", "x");

        std::wstring srcPath = env.Lower(0) + L"\\lts.txt";
        FILETIME srcCreation, srcAccess, srcWrite;
        {
            HANDLE h = ::CreateFileW(srcPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
            ::GetFileTime(h, &srcCreation, &srcAccess, &srcWrite);
            ::CloseHandle(h);
        }

        CopyUpRig rig(env.MakeConfig());

        rig.copyUp.CopyUpMetadataOnly(L"lts.txt");

        std::wstring upPath = env.Upper() + L"\\lts.txt";
        FILETIME dstCreation, dstAccess, dstWrite;
        {
            HANDLE h = ::CreateFileW(upPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
            ::GetFileTime(h, &dstCreation, &dstAccess, &dstWrite);
            ::CloseHandle(h);
        }

        Assert::IsTrue(::CompareFileTime(&srcCreation, &dstCreation) == 0);
        Assert::IsTrue(::CompareFileTime(&srcWrite, &dstWrite) == 0);
    }

    TEST_METHOD(CopyUpMetadataOnly_PreservesAttributes) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"ro.bin", "x");
        std::wstring srcPath = env.Lower(0) + L"\\ro.bin";
        ::SetFileAttributesW(srcPath.c_str(), FILE_ATTRIBUTE_READONLY);

        CopyUpRig rig(env.MakeConfig());

        rig.copyUp.CopyUpMetadataOnly(L"ro.bin");

        std::wstring upPath = env.Upper() + L"\\ro.bin";
        DWORD attrs = ::GetFileAttributesW(upPath.c_str());
        Assert::IsTrue((attrs & FILE_ATTRIBUTE_READONLY) != 0);
    }

    TEST_METHOD(CompleteLazyCopyUp_CopiesDataFromOriginLayer) {
        TempLayerEnvironment env(1);
        std::string content = "complete me please";
        env.WriteFile(env.Lower(0), L"cl.txt", content);

        CopyUpRig rig(env.MakeConfig());

        rig.copyUp.CopyUpMetadataOnly(L"cl.txt");
        NTSTATUS status = rig.copyUp.CompleteLazyCopyUp(L"cl.txt");
        Assert::IsTrue(NT_SUCCESS(status));

        Assert::AreEqual(content, env.ReadFile(env.Upper(), L"cl.txt"));
    }

    TEST_METHOD(CompleteLazyCopyUp_ClearsMetacopyFlag) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"mc.txt", "data");

        CopyUpRig rig(env.MakeConfig());

        rig.copyUp.CopyUpMetadataOnly(L"mc.txt");
        std::wstring upPath = env.Upper() + L"\\mc.txt";
        Assert::IsTrue(MetadataStore::ReadLayerMountMetadata(upPath, nullptr).metacopy);

        rig.copyUp.CompleteLazyCopyUp(L"mc.txt");
        Assert::IsFalse(MetadataStore::ReadLayerMountMetadata(upPath, nullptr).metacopy,
            L"metacopy flag should be cleared after completion");
    }

    TEST_METHOD(CompleteLazyCopyUp_AlreadyFullyCopied_IsNoOp) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"full.txt", "data");

        CopyUpRig rig(env.MakeConfig());

        NTSTATUS status = rig.copyUp.CompleteLazyCopyUp(L"full.txt");
        Assert::IsTrue(NT_SUCCESS(status),
            L"Should succeed as no-op when file is not a metacopy");
    }
};

}
