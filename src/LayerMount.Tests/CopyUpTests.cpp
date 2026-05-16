// Unit tests for CopyUp. Test the class in isolation — do NOT mount
// an overlay, do NOT invoke host-adapter callbacks. Read-triggered lazy
// copy-up (LayerMount::SRead → CompleteLazyCopyUp) is integration-tested
// in 10.0.

#include "pch.h"
#include "TestFixture.h"

#include "CopyUp.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"
#include "MetadataADS.h"

#include <winioctl.h>
#include <set>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;

namespace LayerMountTests {

// ============================================================================
// 9.5 — Full copy-up tests
// ============================================================================

TEST_CLASS(CopyUpTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    // --- Work directory management ---

    TEST_METHOD(GenerateWorkPath_ReturnsUniquePathInWorkDir) {
        TempLayerEnvironment env(1);
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        std::wstring a = cu.GenerateWorkPath();
        std::wstring b = cu.GenerateWorkPath();

        Assert::AreNotEqual(a, b, L"Two calls should return different paths");
        Assert::IsTrue(a.find(env.Work()) == 0, L"Path should be under work dir");
        Assert::IsTrue(a.find(L".tmp") != std::wstring::npos, L"Path should end with .tmp");
    }

    // ------------------------------------------------------------------------
     // Audit gap #11 — GenerateWorkPath used a fixed MAX_PATH wchar_t buffer
     // for swprintf_s. When workDirPath approached or exceeded ~190 chars
     // (deep deployment roots, NT \\?\ prefixes, paths under non-default
     // profile dirs), swprintf_s truncated the result. Two concurrent calls
     // could then produce identical "unique" paths — silent collision that
     // would surface later as CREATE_NEW failures or, worse, one writer
     // overwriting another's staged file mid-flight.
     //
     // The fix uses std::wstring concatenation, which grows as needed and
     // can't truncate. This test verifies that paths near the legacy
     // MAX_PATH boundary stay distinct AND that the concatenated path
     // contains the full workDirPath prefix (no truncation).
     // ------------------------------------------------------------------------
     TEST_METHOD(GenerateWorkPath_LongWorkDirPath_StillUniqueNoTruncation) {
         TempLayerEnvironment env(1);
         LayerConfig config = env.MakeConfig();

         // Build a workDirPath that's ~250 wide chars — well past the 190
         // headroom that the legacy MAX_PATH buffer left for the suffix.
         // Use the NT-style \\?\ prefix so Windows CreateDirectory accepts
         // the long path; without it CreateDirectoryW caps at MAX_PATH.
         std::wstring deepBase = env.Root();
         while (deepBase.size() < 240) {
             deepBase += L"\\padding-segment";
         }
         const std::wstring deepWork = L"\\\\?\\" + deepBase + L"\\overlay-work";
         std::error_code ec;
         std::filesystem::create_directories(deepWork, ec);
         Assert::IsFalse(static_cast<bool>(ec),
             L"Precondition: long-path work dir must be creatable with \\?\\ prefix");

         config.workDirPath = deepWork;

         Cache cache;
         WhiteoutManager wm(config, &cache);
         PathResolver resolver(config, wm, cache);
         LayerMountStats stats;
         CopyUp cu(config, resolver, wm, cache, stats);

         // Generate many paths in tight sequence (counter increments only
         // by 1, so collisions would be invisible if both calls happen in
         // the same FILETIME tick — the fix's correctness comes from
         // counter+pid+tid, not timestamp).
         std::vector<std::wstring> paths;
         constexpr int kCount = 32;
         paths.reserve(kCount);
         for (int i = 0; i < kCount; ++i) {
             paths.push_back(cu.GenerateWorkPath());
         }

         // Every path must start with the full workDirPath (no truncation).
         for (const auto& p : paths) {
             Assert::IsTrue(p.size() > deepWork.size(),
                 L"Generated path must be longer than the work dir prefix");
             Assert::IsTrue(p.compare(0, deepWork.size(), deepWork) == 0,
                 L"Generated path must start with the FULL workDirPath. "
                 L"A shorter prefix means swprintf_s truncated.");
             Assert::IsTrue(p.find(L".tmp") == p.size() - 4,
                 L"Generated path must end with .tmp");
         }

         // All paths must be distinct. A duplicate means truncation made two
         // counter values map to the same string.
         std::set<std::wstring> uniq(paths.begin(), paths.end());
         wchar_t msg[200];
         swprintf_s(msg, L"Generated %d work paths, only %zu unique. Long-path "
                         L"work dir caused collision (gap #11).",
                    kCount, uniq.size());
         Assert::IsTrue(uniq.size() == static_cast<size_t>(kCount), msg);
     }

    TEST_METHOD(CleanWorkDirectory_RemovesHashTempFiles_LeavesOthers) {
        TempLayerEnvironment env(1);
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        // Drop a fake work-dir temp file (CleanWorkDirectory looks for #*.tmp)
        std::wstring tempFile = env.Work() + L"\\#abc.tmp";
        HANDLE h1 = ::CreateFileW(tempFile.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ::CloseHandle(h1);

        // And a non-matching file
        std::wstring otherFile = env.Work() + L"\\other.txt";
        HANDLE h2 = ::CreateFileW(otherFile.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ::CloseHandle(h2);

        cu.CleanWorkDirectory();

        Assert::AreEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(tempFile.c_str()),
            L"Hash-prefixed .tmp file should be removed");
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(otherFile.c_str()),
            L"Non-matching file should remain");
    }

    TEST_METHOD(CommitFromWorkDir_MovesFileFromWorkToFinalPath) {
        TempLayerEnvironment env(1);
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        // Create a test file in the work directory
        std::wstring workPath = env.Work() + L"\\test.tmp";
        std::wstring finalPath = env.Upper() + L"\\final.txt";
        env.WriteFile(env.Work(), L"test.tmp", "content");

        NTSTATUS status = cu.CommitFromWorkDir(workPath, finalPath);

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
        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        std::wstring workPath = env.Work() + L"\\temp.tmp";
        env.WriteFile(env.Work(), L"temp.tmp", "x");
        std::wstring finalPath = env.Upper() + L"\\a\\b\\c.txt";

        NTSTATUS status = cu.CommitFromWorkDir(workPath, finalPath);

        Assert::IsTrue(NT_SUCCESS(status));
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(finalPath.c_str()),
            L"Final path under nested dirs should exist");
    }

    // --- Full copy-up ---

    TEST_METHOD(CopyUpFile_LowerFileOnly_CopiesContentToUpper) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"foo.txt", "lower content");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        NTSTATUS status = cu.CopyUpFile(L"foo.txt");
        Assert::IsTrue(NT_SUCCESS(status));

        Assert::IsTrue(env.FileExists(env.Upper(), L"foo.txt"));
        Assert::AreEqual(std::string("lower content"),
            env.ReadFile(env.Upper(), L"foo.txt"));
    }

    TEST_METHOD(CopyUpFile_PreservesFileSize) {
        TempLayerEnvironment env(1);
        std::string content(8192, 'A'); // 8KB of 'A's
        env.WriteFile(env.Lower(0), L"big.bin", content);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        cu.CopyUpFile(L"big.bin");

        std::string copied = env.ReadFile(env.Upper(), L"big.bin");
        Assert::AreEqual(content.size(), copied.size());
    }

    TEST_METHOD(CopyUpFile_PreservesTimestamps) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"ts.txt", "x");

        // Capture source timestamps before copy-up
        std::wstring srcPath = env.Lower(0) + L"\\ts.txt";
        FILETIME srcCreation, srcAccess, srcWrite;
        {
            HANDLE h = ::CreateFileW(srcPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
            ::GetFileTime(h, &srcCreation, &srcAccess, &srcWrite);
            ::CloseHandle(h);
        }

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        cu.CopyUpFile(L"ts.txt");

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

        // Make the source read-only
        std::wstring srcPath = env.Lower(0) + L"\\readonly.txt";
        ::SetFileAttributesW(srcPath.c_str(), FILE_ATTRIBUTE_READONLY);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        cu.CopyUpFile(L"readonly.txt");

        std::wstring upPath = env.Upper() + L"\\readonly.txt";
        DWORD upAttrs = ::GetFileAttributesW(upPath.c_str());
        Assert::IsTrue((upAttrs & FILE_ATTRIBUTE_READONLY) != 0,
            L"READONLY attribute not preserved");

        // Clear readonly so cleanup can delete it
        ::SetFileAttributesW(srcPath.c_str(), FILE_ATTRIBUTE_NORMAL);
        ::SetFileAttributesW(upPath.c_str(), FILE_ATTRIBUTE_NORMAL);
    }

    TEST_METHOD(CopyUpFile_CreatesParentDirectoryInUpper) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sub\\nested.txt", "x");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        // Upper has no 'sub' dir
        cu.CopyUpFile(L"sub\\nested.txt");

        std::wstring upSub = env.Upper() + L"\\sub";
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(upSub.c_str()),
            L"Parent dir should be auto-created in upper");
    }

    TEST_METHOD(CopyUpFile_InvalidatesCacheForPath) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"c.txt", "x");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        // Prime cache by resolving (caches the Lower result)
        resolver.ResolvePath(L"c.txt");
        Assert::IsTrue(cache.Get(L"c.txt").has_value(), L"Should be cached");

        cu.CopyUpFile(L"c.txt");

        Assert::IsFalse(cache.Get(L"c.txt").has_value(),
            L"CopyUpFile should invalidate cache");
    }

    TEST_METHOD(CopyUpFile_AlreadyInUpper_IsNoOp) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"both.txt", "upper");
        env.WriteFile(env.Lower(0), L"both.txt", "lower");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        NTSTATUS status = cu.CopyUpFile(L"both.txt");
        Assert::IsTrue(NT_SUCCESS(status));
        Assert::AreEqual(std::string("upper"),
            env.ReadFile(env.Upper(), L"both.txt"),
            L"Upper content should be unchanged");
    }

    TEST_METHOD(CopyUpFile_IncrementsCopyUpCount) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"count.txt", "x");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        uint64_t before = stats.copyUpCount.load();
        cu.CopyUpFile(L"count.txt");
        Assert::AreEqual(before + 1, stats.copyUpCount.load());
    }

    // --- Directory copy-up ---

    TEST_METHOD(CopyUpDirectory_LowerDir_CreatesEntryOnlyNotContents) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"mydir");
        env.WriteFile(env.Lower(0), L"mydir\\child.txt", "child");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        cu.CopyUpDirectory(L"mydir");

        std::wstring upDir = env.Upper() + L"\\mydir";
        Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(upDir.c_str()),
            L"Directory entry should be created in upper");

        // Child should NOT be copied (children resolve lazily through overlay)
        std::wstring upChild = env.Upper() + L"\\mydir\\child.txt";
        Assert::AreEqual(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(upChild.c_str()),
            L"Child file should NOT be recursively copied");
    }

    TEST_METHOD(CopyUpDirectory_PreservesDirectoryAttributes) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"hidden_dir");

        // Mark lower dir as hidden
        std::wstring srcDir = env.Lower(0) + L"\\hidden_dir";
        ::SetFileAttributesW(srcDir.c_str(),
            FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        cu.CopyUpDirectory(L"hidden_dir");

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

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        cu.CopyUpDirectory(L"td");

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

    // --- Directory rename ---

    TEST_METHOD(HandleDirectoryRename_WithinUpper_MovesDirectoryOnly) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Upper(), L"srcdir");
        env.WriteFile(env.Upper(), L"srcdir\\file.txt", "data");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        NTSTATUS status = cu.HandleDirectoryRename(L"srcdir", L"dstdir",
                                                    /*sourceIsInLower=*/false);
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

    TEST_METHOD(HandleDirectoryRename_FromLower_CopiesUpAndCreatesWhiteout) {
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"ldir");
        env.WriteFile(env.Lower(0), L"ldir\\file.txt", "lower data");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        NTSTATUS status = cu.HandleDirectoryRename(L"ldir", L"newdir",
                                                    /*sourceIsInLower=*/true);
        Assert::IsTrue(NT_SUCCESS(status));

        // Content recursively copied under new name
        Assert::IsTrue(env.FileExists(env.Upper(), L"newdir\\file.txt"));

        // New location is opaque (hides lower/newdir if any)
        Assert::IsTrue(wm.IsOpaque(L"newdir"));

        // Whiteout for old path (hides lower/ldir from the merged view)
        Assert::IsTrue(wm.HasWhiteout(L"ldir", env.Upper()));
    }
};

// ============================================================================
// 9.6 — Lazy copy-up tests
// ============================================================================

TEST_CLASS(LazyCopyUpTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        AssertTempIsNTFS();
    }

    TEST_METHOD(CopyUpMetadataOnly_CreatesFileWithSourceSizeButNoData) {
        TempLayerEnvironment env(1);
        std::string content(4096, 'Z'); // 4KB
        env.WriteFile(env.Lower(0), L"lazy.bin", content);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        cu.CopyUpMetadataOnly(L"lazy.bin");

        std::wstring upPath = env.Upper() + L"\\lazy.bin";

        // File exists with correct logical size
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

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        cu.CopyUpMetadataOnly(L"m.txt");

        std::wstring upPath = env.Upper() + L"\\m.txt";
        LayerMountMetadata md = MetadataADS::ReadLayerMountMetadata(upPath);
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

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        cu.CopyUpMetadataOnly(L"lts.txt");

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

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        cu.CopyUpMetadataOnly(L"ro.bin");

        std::wstring upPath = env.Upper() + L"\\ro.bin";
        DWORD attrs = ::GetFileAttributesW(upPath.c_str());
        Assert::IsTrue((attrs & FILE_ATTRIBUTE_READONLY) != 0);

        // Clear for cleanup
        ::SetFileAttributesW(srcPath.c_str(), FILE_ATTRIBUTE_NORMAL);
        ::SetFileAttributesW(upPath.c_str(), FILE_ATTRIBUTE_NORMAL);
    }

    TEST_METHOD(CompleteLazyCopyUp_CopiesDataFromOriginLayer) {
        TempLayerEnvironment env(1);
        std::string content = "complete me please";
        env.WriteFile(env.Lower(0), L"cl.txt", content);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        cu.CopyUpMetadataOnly(L"cl.txt");
        NTSTATUS status = cu.CompleteLazyCopyUp(L"cl.txt");
        Assert::IsTrue(NT_SUCCESS(status));

        Assert::AreEqual(content, env.ReadFile(env.Upper(), L"cl.txt"));
    }

    TEST_METHOD(CompleteLazyCopyUp_ClearsMetacopyFlag) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"mc.txt", "data");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        cu.CopyUpMetadataOnly(L"mc.txt");
        std::wstring upPath = env.Upper() + L"\\mc.txt";
        Assert::IsTrue(MetadataADS::ReadLayerMountMetadata(upPath).metacopy);

        cu.CompleteLazyCopyUp(L"mc.txt");
        Assert::IsFalse(MetadataADS::ReadLayerMountMetadata(upPath).metacopy,
            L"metacopy flag should be cleared after completion");
    }

    TEST_METHOD(CompleteLazyCopyUp_AlreadyFullyCopied_IsNoOp) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"full.txt", "data");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        // No metacopy flag written (file doesn't have one)
        NTSTATUS status = cu.CompleteLazyCopyUp(L"full.txt");
        Assert::IsTrue(NT_SUCCESS(status),
            L"Should succeed as no-op when file is not a metacopy");
    }
};

} // namespace LayerMountTests
