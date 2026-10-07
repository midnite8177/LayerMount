#include "pch.h"
#include "TestFixture.h"

#include "LayerPath.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;

namespace LayerMountTests {

static_assert(RefusesTemporaryConfig<PathResolver, WhiteoutManager&, Cache&>,
    "PathResolver keeps a reference to its LayerConfig");

TEST_CLASS(PathResolverTests) {
public:
    TEST_METHOD(ResolvePath_SingleLayerUpperOnly_FileInUpper_ReturnsUpper) {
        TempLayerEnvironment env(0);
        env.WriteFile(env.Upper(), L"foo.txt", "hello");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        ResolvedPath result = resolver.ResolvePath(L"foo.txt");
        Assert::IsTrue(result.Found(), L"Expected file to be found");
        Assert::IsTrue(result.source == LayerSource::Upper, L"Expected Upper source");
        Assert::AreEqual(-1, result.lowerIndex, L"Upper entries have lowerIndex=-1");
    }

    TEST_METHOD(ResolvePath_SingleLayerUpperOnly_FileMissing_ReturnsNotFound) {
        TempLayerEnvironment env(0);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        ResolvedPath result = resolver.ResolvePath(L"missing.txt");
        Assert::IsFalse(result.Found(), L"Expected file to not be found");
    }

    TEST_METHOD(ResolvePath_TwoLayer_FileOnlyInLower_ReturnsLower) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"bar.txt", "lower content");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        ResolvedPath result = resolver.ResolvePath(L"bar.txt");
        Assert::IsTrue(result.Found(), L"Expected file to be found in lower");
        Assert::IsTrue(result.source == LayerSource::Lower, L"Expected Lower source");
        Assert::AreEqual(0, result.lowerIndex, L"Expected lowerIndex=0");
    }

    TEST_METHOD(ResolvePath_TwoLayer_SamePathBothLayers_UpperWins) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"conflict.txt", "upper");
        env.WriteFile(env.Lower(0), L"conflict.txt", "lower");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        ResolvedPath result = resolver.ResolvePath(L"conflict.txt");
        Assert::IsTrue(result.Found());
        Assert::IsTrue(result.source == LayerSource::Upper, L"Upper should win precedence");
    }

    TEST_METHOD(ResolvePath_ThreeLayer_HitsDeepestLayerOnly_ReturnsCorrectIndex) {
        TempLayerEnvironment env(3);
        env.WriteFile(env.Lower(2), L"deep.txt", "deep");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        ResolvedPath result = resolver.ResolvePath(L"deep.txt");
        Assert::IsTrue(result.Found());
        Assert::IsTrue(result.source == LayerSource::Lower);
        Assert::AreEqual(2, result.lowerIndex, L"Expected lowerIndex=2");
    }

    TEST_METHOD(ResolvePath_ThreeLayer_MultipleLowersHaveFile_HighestPriorityLowerWins) {
        TempLayerEnvironment env(3);
        env.WriteFile(env.Lower(0), L"priority.txt", "lower0");
        env.WriteFile(env.Lower(2), L"priority.txt", "lower2");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        ResolvedPath result = resolver.ResolvePath(L"priority.txt");
        Assert::IsTrue(result.Found());
        Assert::IsTrue(result.source == LayerSource::Lower);
        Assert::AreEqual(0, result.lowerIndex, L"Highest-priority lower (index 0) should win");
    }

    TEST_METHOD(ResolvePath_FileNotFoundAnywhere_ReturnsNotFound) {
        TempLayerEnvironment env(3);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        ResolvedPath result = resolver.ResolvePath(L"nowhere.txt");
        Assert::IsFalse(result.Found());
    }

    TEST_METHOD(ResolvePath_LeadingBackslash_NormalizedIdenticalToWithout) {
        TempLayerEnvironment env(0);
        env.WriteFile(env.Upper(), L"foo.txt", "x");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        ResolvedPath a = resolver.ResolvePath(L"foo.txt");
        ResolvedPath b = resolver.ResolvePath(L"\\foo.txt");
        Assert::IsTrue(a.Found());
        Assert::IsTrue(b.Found());
        Assert::AreEqual(a.absolutePath, b.absolutePath);
    }

    TEST_METHOD(ResolvePath_ForwardSlashes_NormalizedToBackslash) {
        TempLayerEnvironment env(0);
        env.WriteFile(env.Upper(), L"sub\\foo.txt", "x");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        ResolvedPath result = resolver.ResolvePath(L"sub/foo.txt");
        Assert::IsTrue(result.Found(), L"Forward slashes should normalize to backslashes");
        Assert::IsTrue(result.source == LayerSource::Upper);
    }

    TEST_METHOD(ResolvePath_MixedCase_CaseInsensitiveMatch) {
        TempLayerEnvironment env(0);
        env.WriteFile(env.Upper(), L"Foo.TXT", "x");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        ResolvedPath result = resolver.ResolvePath(L"foo.txt");
        Assert::IsTrue(result.Found(), L"NTFS is case-insensitive — lookup should succeed");
        Assert::IsTrue(result.source == LayerSource::Upper);
    }

    TEST_METHOD(ResolvePath_AfterResolution_SecondCallReturnsSameValueAfterDiskMutation) {
        TempLayerEnvironment env(0);
        env.WriteFile(env.Upper(), L"cached.txt", "x");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        ResolvedPath first = resolver.ResolvePath(L"cached.txt");
        Assert::IsTrue(first.Found(), L"First resolution should find the file");

        std::wstring onDisk = env.Upper() + L"\\cached.txt";
        Assert::IsTrue(::DeleteFileW(onDisk.c_str()) != FALSE, L"Failed to delete file");

        ResolvedPath second = resolver.ResolvePath(L"cached.txt");
        Assert::IsTrue(second.Found(),
            L"Second resolution should hit cache and return stale value "
            L"(proving cache was consulted instead of fresh FS check)");
        Assert::AreEqual(first.absolutePath, second.absolutePath);
    }

    TEST_METHOD(ResolvePath_RootPath_ResolvesToUpperRoot) {
        TempLayerEnvironment env(0);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        ResolvedPath result = resolver.ResolvePath(L"");
        Assert::IsTrue(result.Found(), L"Root path should resolve");
        Assert::IsTrue(result.source == LayerSource::Upper);
        Assert::AreEqual(env.Upper(), result.absolutePath);
    }

    TEST_METHOD(ExistsInUpper_FileInLowerOnly_ReturnsFalse) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"onlylower.txt", "x");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        Assert::IsFalse(resolver.ExistsInUpper(L"onlylower.txt"));
    }

    TEST_METHOD(ExistsInUpper_FileInUpper_ReturnsTrue) {
        TempLayerEnvironment env(0);
        env.WriteFile(env.Upper(), L"upperfile.txt", "x");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        Assert::IsTrue(resolver.ExistsInUpper(L"upperfile.txt"));
    }

    TEST_METHOD(GetUpperPath_ReturnsUpperPathSlashRelative) {
        TempLayerEnvironment env(0);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        std::wstring got = resolver.GetUpperPath(L"foo.txt");
        std::wstring expected = env.Upper() + L"\\foo.txt";
        Assert::AreEqual(expected, got);
    }

    TEST_METHOD(ResolveLowerPath_FileInBoth_SkipsUpperReturnsLower) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"both.txt", "upper");
        env.WriteFile(env.Lower(0), L"both.txt", "lower");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        ResolvedPath result = resolver.ResolveLowerPath(L"both.txt");
        Assert::IsTrue(result.Found());
        Assert::IsTrue(result.source == LayerSource::Lower, L"Should skip upper and find lower");
        Assert::AreEqual(0, result.lowerIndex);
    }

    TEST_METHOD(HasTypeConflict_FileInUpperDirInLower_ReturnsTrue) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"conflict", "file content");
        env.CreateDir(env.Lower(0), L"conflict");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        Assert::IsTrue(resolver.HasTypeConflict(L"conflict"));
    }

    TEST_METHOD(HasTypeConflict_SameTypeBothLayers_ReturnsFalse) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"consistent.txt", "upper");
        env.WriteFile(env.Lower(0), L"consistent.txt", "lower");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        Assert::IsFalse(resolver.HasTypeConflict(L"consistent.txt"));
    }

    TEST_METHOD(HasTypeConflict_OnlyInUpper_ReturnsFalse) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"alone.txt", "x");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        Assert::IsFalse(resolver.HasTypeConflict(L"alone.txt"));
    }

    TEST_METHOD(ResolvePath_ExtendedFormLowerWithTrailingSeparator_ResolvesFileAtLowerRoot) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"own.txt", "lower0");
        auto config = env.MakeConfig();
        config.lowerPaths[0] = ExtendedDirWithSeparator(env.Lower(0));
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        ResolvedPath result = resolver.ResolvePath(L"own.txt");

        Assert::IsTrue(result.Found(), L"The file at the root of the lower must resolve");
        Assert::IsTrue(result.source == LayerSource::Lower,
            L"The file must resolve from the lower that holds it");
        Assert::AreEqual(0, result.lowerIndex,
            L"The file must resolve from the first lower");
        Assert::AreEqual(L"\\\\?\\" + env.Lower(0) + L"\\own.txt", result.absolutePath,
            L"The resolved path must have one separator after the lower root");
    }

    TEST_METHOD(ResolvePath_WhiteoutAtRootOfExtendedFormLowerWithTrailingSeparator_HidesDeeperLowersFile) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), WhiteoutMarkerPath(L"a.txt"), "");
        env.WriteFile(env.Lower(1), L"a.txt", "lower1");
        auto config = env.MakeConfig();
        config.lowerPaths[0] = ExtendedDirWithSeparator(env.Lower(0));
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        ResolvedPath result = resolver.ResolvePath(L"a.txt");

        Assert::IsFalse(result.Found(),
            L"The whiteout at the root of the lower must hide a.txt in the deeper lower");
    }

    TEST_METHOD(ResolvePath_OpaqueDirectoryInExtendedFormLowerWithTrailingSeparator_HidesDeeperLowersFile) {
        TempLayerEnvironment env(2);
        env.WriteFile(env.Lower(0), OpaqueMarkerPath(L"sub"), "");
        env.WriteFile(env.Lower(1), L"sub\\x.txt", "lower1");
        auto config = env.MakeConfig();
        config.lowerPaths[0] = ExtendedDirWithSeparator(env.Lower(0));
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        ResolvedPath result = resolver.ResolvePath(L"sub\\x.txt");

        Assert::IsFalse(result.Found(),
            L"The opaque directory in the lower must hide sub\\x.txt in the deeper lower");
    }

    TEST_METHOD(ResolvePath_ExtendedFormUpperWithTrailingSeparator_ResolvesFileInUpper) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"up.txt", "upper");
        auto config = env.MakeConfig();
        config.upperPath = ExtendedDirWithSeparator(env.Upper());
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);

        ResolvedPath result = resolver.ResolvePath(L"up.txt");

        Assert::IsTrue(result.Found(), L"The file in the upper must resolve");
        Assert::IsTrue(result.source == LayerSource::Upper,
            L"The file must resolve from the upper that holds it");
        Assert::AreEqual(L"\\\\?\\" + env.Upper() + L"\\up.txt", result.absolutePath,
            L"The resolved path must have one separator after the upper root");
    }
};

TEST_CLASS(LayerPathTests) {
public:
    TEST_METHOD(WithExtendedPrefix_DrivePath_GetsExtendedPrefix) {
        Assert::AreEqual(std::wstring(L"\\\\?\\C:\\dir\\file.txt"),
                         WithExtendedPrefix(L"C:\\dir\\file.txt"));
    }

    TEST_METHOD(WithExtendedPrefix_UncPath_GetsExtendedUncPrefix) {
        Assert::AreEqual(std::wstring(L"\\\\?\\UNC\\server\\share\\dir\\file.txt"),
                         WithExtendedPrefix(L"\\\\server\\share\\dir\\file.txt"));
    }

    TEST_METHOD(WithExtendedPrefix_ExtendedOrDevicePath_ComesBackUnchanged) {
        for (const std::wstring path : {
                 std::wstring(L"\\\\?\\C:\\dir\\..\\file.txt"),
                 std::wstring(L"\\\\?\\UNC\\server\\share\\file.txt"),
                 std::wstring(L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolumeShadowCopy3\\dir\\file.txt"),
                 std::wstring(L"\\\\?\\Volume{6f1d6a3e-0000-0000-0000-100000000000}\\dir\\file.txt"),
                 std::wstring(L"\\\\.\\C:\\dir\\file.txt")}) {
            Assert::AreEqual(path, WithExtendedPrefix(path),
                L"A path that already has an extended or device prefix must not change");
        }
    }

    TEST_METHOD(WithExtendedPrefix_ForwardSlashesDotsAndDoubledSeparators_AreResolved) {
        Assert::AreEqual(std::wstring(L"\\\\?\\C:\\dir\\file.txt"),
                         WithExtendedPrefix(L"C:/dir/sub/.././/file.txt"));
    }

    TEST_METHOD(WithExtendedPrefix_NameEndingInDotOrSpace_KeepsIt) {
        Assert::AreEqual(std::wstring(L"\\\\?\\C:\\dir\\name."),
                         WithExtendedPrefix(L"C:\\dir\\name."));
        Assert::AreEqual(std::wstring(L"\\\\?\\C:\\dir\\name "),
                         WithExtendedPrefix(L"C:\\dir\\name "));
        Assert::AreEqual(std::wstring(L"\\\\?\\UNC\\server\\share\\name."),
                         WithExtendedPrefix(L"\\\\server\\share\\name."));
    }

    TEST_METHOD(WithExtendedPrefix_PathLongerThanMaxPath_IsResolvedAndPrefixed) {
        const std::wstring segment(60, L's');
        std::wstring body;
        for (int i = 0; i < 5; ++i) {
            body += L"\\" + segment;
        }
        const std::wstring path = L"C:" + body + L"\\skip\\..\\file.txt";
        Assert::IsTrue(path.size() > MAX_PATH,
            L"Precondition: the path is longer than MAX_PATH");

        Assert::AreEqual(L"\\\\?\\C:" + body + L"\\file.txt", WithExtendedPrefix(path));
    }
};

}
