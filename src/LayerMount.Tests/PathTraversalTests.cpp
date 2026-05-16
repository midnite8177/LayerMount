// Unit tests for PathResolver's traversal defenses and normalization.
// Case-only rename and long-path coverage (which needs a mounted overlay)
// lives in LayerMount.IntegrationTests::PathSemanticsTests.

#include "pch.h"
#include "TestFixture.h"

#include "LayerMount.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountTests {

// ---------------------------------------------------------------------------
// Path traversal — at the resolver layer.
// ---------------------------------------------------------------------------

TEST_CLASS(PathTraversalTests) {
public:
    TEST_METHOD(ResolvePath_DotDotOutsideLayerMount_DoesNotEscape) {
        // A `..` segment at the overlay root must not allow the resolver to
        // return a path outside upper_/lower_. If NormalizePath leaves `..`
        // intact, Windows will resolve `upper\..\foo` to the overlay's PARENT
        // directory and may return whatever sits there. Create a decoy file
        // next to the overlay root and verify the resolver does NOT surface it.
        TempLayerEnvironment env(1);
        // Drop a decoy one level above the overlay root.
        std::wstring decoyPath = env.Root() + L"\\decoy.txt";
        HANDLE h = ::CreateFileW(decoyPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ::CloseHandle(h);

        auto config = env.MakeConfig();
        LayerMount::Cache cache;
        LayerMount::WhiteoutManager wm(config, &cache);
        LayerMount::PathResolver resolver(config, wm, cache);

        // upper = env.Upper() = env.Root()\\upper — so `..\\decoy.txt` from
        // inside the overlay would target env.Root()\\decoy.txt. That file
        // exists on disk, so a buggy resolver would happily return it.
        LayerMount::ResolvedPath r = resolver.ResolvePath(L"..\\decoy.txt");
        Assert::IsFalse(r.Found(),
            L"Path traversal via `..` must not escape the overlay roots");

        // Clean up decoy.
        ::DeleteFileW(decoyPath.c_str());
    }

    TEST_METHOD(ResolvePath_DotDotMidPath_DoesNotEscape) {
        TempLayerEnvironment env(1);
        std::wstring decoyPath = env.Root() + L"\\decoy.txt";
        HANDLE h = ::CreateFileW(decoyPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        ::CloseHandle(h);

        auto config = env.MakeConfig();
        LayerMount::Cache cache;
        LayerMount::WhiteoutManager wm(config, &cache);
        LayerMount::PathResolver resolver(config, wm, cache);

        // Synthesize: `a\\..\\..\\decoy.txt`, an even sneakier pattern.
        LayerMount::ResolvedPath r = resolver.ResolvePath(L"a\\..\\..\\decoy.txt");
        Assert::IsFalse(r.Found(),
            L"Path traversal via nested `..` segments must not escape");

        ::DeleteFileW(decoyPath.c_str());
    }

    TEST_METHOD(ResolvePath_SingleDotSegment_IsEquivalentToNoSegment) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"file.txt", "payload");

        auto config = env.MakeConfig();
        LayerMount::Cache cache;
        LayerMount::WhiteoutManager wm(config, &cache);
        LayerMount::PathResolver resolver(config, wm, cache);

        // `.\\file.txt` must resolve identically to `file.txt`.
        LayerMount::ResolvedPath withDot    = resolver.ResolvePath(L".\\file.txt");
        LayerMount::ResolvedPath withoutDot = resolver.ResolvePath(L"file.txt");
        Assert::IsTrue(withDot.Found());
        Assert::IsTrue(withoutDot.Found());
        Assert::IsTrue(withDot.source == withoutDot.source);
    }

    TEST_METHOD(ResolvePath_MixedSlashes_NormalizedConsistently) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sub\\file.txt", "payload");

        auto config = env.MakeConfig();
        LayerMount::Cache cache;
        LayerMount::WhiteoutManager wm(config, &cache);
        LayerMount::PathResolver resolver(config, wm, cache);

        LayerMount::ResolvedPath a = resolver.ResolvePath(L"sub/file.txt");
        LayerMount::ResolvedPath b = resolver.ResolvePath(L"sub\\file.txt");
        Assert::IsTrue(a.Found());
        Assert::IsTrue(b.Found());
        Assert::IsTrue(a.source == b.source);
    }
};

} // namespace LayerMountTests
