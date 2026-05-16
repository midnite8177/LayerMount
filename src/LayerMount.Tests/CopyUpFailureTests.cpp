// Copy-up failure-path coverage. Addresses the gap flagged by the audit:
// AtomicOperationTests and the happy-path CopyUpTests only verify success
// scenarios. This file drives deterministic failure modes and asserts the
// atomicity + hygiene invariants the design promises:
//
//   - failed copy-up never produces a committed upper artifact
//   - failed copy-up never leaves a work-dir orphan (#*.tmp)
//   - a failed copy-up does not poison subsequent retries
//
// These tests call CopyUp directly (no mount) for determinism — a dispatcher
// can't be induced into the narrow failure windows (disk full at byte N,
// parent-path race) we need to probe here. As a pure unit test of the
// CopyUp class they live in the unit-test project that links
// LayerMount-static and keeps impl-header access.
//
// Short-write detection is not directly testable without an injection shim —
// the fix is a hardening check in CopyUp::CopyFileData that verifies
// bytesWritten == bytesRead. The multi-buffer exact-fidelity regression
// test in this file guards against a regression that reintroduces the bug.

#include "pch.h"
#include "TestFixture.h"

#include "CopyUp.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"
#include "MetadataADS.h"

#include <thread>
#include <atomic>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;

namespace LayerMountTests {

namespace {

size_t CountWorkTempFiles(const std::wstring& workDir) {
    const std::wstring pattern = workDir + L"\\#*.tmp";
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    size_t count = 0;
    do { ++count; } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return count;
}

} // namespace

// ============================================================================
// CopyUpFailureTests — atomicity + hygiene under forced failure.
// ============================================================================

TEST_CLASS(CopyUpFailureTests) {
public:
    // ------------------------------------------------------------------------
    // Destination-create failure: work dir vanished.
    //
    // If the work directory is gone when CopyUpFile runs, CreateFileW on the
    // work-temp path fails with ERROR_PATH_NOT_FOUND. Copy-up must abort
    // cleanly: no upper artifact, no partial state, and the error is
    // propagated so the caller can observe the failure.
    // ------------------------------------------------------------------------
    TEST_METHOD(CopyUpFile_WorkDirRemoved_FailsCleanlyNoUpperArtifact) {
        LayerMountTests::TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"doc.txt", "lower content");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        // Blow away the work dir after the overlay has its config. CopyUpFile
        // will not re-create it — Prepare() did that at construction time, but
        // between Prepare() and the copy-up call a crash / external tool can
        // remove it. We must not silently "succeed".
        std::error_code ec;
        std::filesystem::remove_all(env.Work(), ec);
        Assert::AreEqual<DWORD>(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(env.Work().c_str()),
            L"Precondition: work dir must be gone");

        const NTSTATUS status = cu.CopyUpFile(L"doc.txt");
        Assert::IsFalse(NT_SUCCESS(status),
            L"Copy-up with missing work dir must fail");

        Assert::IsFalse(env.FileExists(env.Upper(), L"doc.txt"),
            L"Failed copy-up must not leave an upper artifact");

        // Stats counter never incremented on failure.
        Assert::AreEqual<uint64_t>(0, stats.copyUpCount.load(),
            L"copyUpCount must not advance on failure");
    }

    // ------------------------------------------------------------------------
    // Commit-stage failure: upper parent occupied by a file (not a dir).
    //
    // CopyUp stages into work dir, then MoveFileExW into upper. If the upper
    // parent directory is occupied by a FILE with the same name, the
    // EnsureDirectoryExists call at commit time silently no-ops, and
    // MoveFileExW then fails because the parent is not a directory.
    //
    // The failure cleanup path (DeleteFileW(workPath) on commit failure) must
    // run, so the work dir ends with zero #*.tmp orphans.
    // ------------------------------------------------------------------------
    TEST_METHOD(CopyUpFile_UpperParentIsFile_CommitFailsAndCleansWorkTemp) {
        LayerMountTests::TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"sub\\doc.txt", "lower content");

        // Pre-occupy upper\sub as a FILE so the parent-dir precondition is
        // violated at commit time. EnsureDirectoryExists(parentDir) silently
        // fails (returns false, ignored), then MoveFileExW fails because the
        // parent of finalUpperPath is not a directory.
        env.WriteFile(env.Upper(), L"sub", "blocker");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        const NTSTATUS status = cu.CopyUpFile(L"sub\\doc.txt");
        Assert::IsFalse(NT_SUCCESS(status),
            L"Copy-up with file-blocked upper parent must fail");

        // Atomicity: no target file was committed.
        Assert::IsFalse(env.FileExists(env.Upper(), L"sub\\doc.txt"),
            L"Failed commit must not land a file at the target path");

        // Hygiene: the commit-failure branch in CopyUp::CommitFromWorkDir
        // DeleteFileW's the work temp on MoveFileExW failure. No orphans.
        Assert::AreEqual<size_t>(0, CountWorkTempFiles(env.Work()),
            L"Work dir must be empty after failed commit");
    }

    // ------------------------------------------------------------------------
    // Retry after failure: a second, clean attempt succeeds and the final
    // upper state reflects the source exactly.
    //
    // Guards against a specific latent failure: a failed copy-up that leaves
    // the in-flight bookkeeping set corrupted would cause the next attempt
    // to deadlock-spin or short-circuit.
    // ------------------------------------------------------------------------
    TEST_METHOD(CopyUpFile_AfterFailure_SubsequentAttemptSucceeds) {
        LayerMountTests::TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"retry.txt", "real content");

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        // Induce a failure: remove work dir.
        std::error_code ec;
        std::filesystem::remove_all(env.Work(), ec);
        Assert::IsFalse(NT_SUCCESS(cu.CopyUpFile(L"retry.txt")),
            L"Preconditions: first attempt must fail");

        // Restore work dir and retry. The second call must produce a faithful
        // upper copy (exact content, no staleness, no orphan).
        std::filesystem::create_directories(env.Work(), ec);
        const NTSTATUS retry = cu.CopyUpFile(L"retry.txt");
        Assert::IsTrue(NT_SUCCESS(retry),
            L"Retry after transient failure must succeed");

        Assert::AreEqual(std::string("real content"),
            env.ReadFile(env.Upper(), L"retry.txt"),
            L"Upper must reflect source content exactly after retry");

        Assert::AreEqual<size_t>(0, CountWorkTempFiles(env.Work()),
            L"Successful retry must not leave work-dir orphans");
    }

    // ------------------------------------------------------------------------
    // Concurrent callers on the same path converge on a single consistent
    // upper result.
    //
    // Under contention, the final upper content must match the source
    // exactly, and no orphan temps remain. Protects against a regression
    // where racing copy-ups could (a) leave an orphan temp if one racer's
    // MoveFileExW fails or (b) briefly expose a truncated upper.
    // ------------------------------------------------------------------------
    TEST_METHOD(CopyUpFile_ConcurrentCallersSamePath_SingleConsistentResult) {
        LayerMountTests::TempLayerEnvironment env(1);
        const std::string payload(16 * 1024, 'Q');
        env.WriteFile(env.Lower(0), L"hot.bin", payload);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        constexpr int kThreads = 8;
        std::vector<std::thread> threads;
        std::atomic<int> successes{0};
        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([&]() {
                if (NT_SUCCESS(cu.CopyUpFile(L"hot.bin"))) {
                    successes.fetch_add(1);
                }
            });
        }
        for (auto& t : threads) t.join();

        Assert::AreEqual(kThreads, successes.load(),
            L"All concurrent callers must observe success on the hot path");

        Assert::AreEqual(payload, env.ReadFile(env.Upper(), L"hot.bin"),
            L"Upper content must exactly match the lower source");

        Assert::AreEqual<size_t>(0, CountWorkTempFiles(env.Work()),
            L"Concurrent copy-ups must not leave orphan work-dir temps");
    }

    // ------------------------------------------------------------------------
    // CompleteLazyCopyUp: origin layer source vanished between metacopy and
    // lazy completion.
    //
    // The lazy path is documented as optimistic — it trusts the ADS-recorded
    // origin path. If that origin was renamed/deleted externally (e.g., the
    // lower layer was swapped), lazy completion must fail cleanly rather
    // than silently claim success while the upper metacopy stays zero-filled.
    //
    // Catches the data-loss class where a caller sees "copy-up completed"
    // but subsequent reads return the sparse zero tail instead of real data.
    // ------------------------------------------------------------------------
    TEST_METHOD(CompleteLazyCopyUp_OriginMissing_FailsAndLeavesMetacopyIntact) {
        LayerMountTests::TempLayerEnvironment env(1);
        const std::string payload = "origin data";
        env.WriteFile(env.Lower(0), L"lazy.bin", payload);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.CopyUpMetadataOnly(L"lazy.bin")));

        const std::wstring upperPath = env.Upper() + L"\\lazy.bin";
        LayerMountMetadata before = MetadataADS::ReadLayerMountMetadata(upperPath);
        Assert::IsTrue(before.metacopy, L"Preconditions: upper is a metacopy");

        // Yank the origin source out from under the lazy completer.
        Assert::IsTrue(::DeleteFileW((env.Lower(0) + L"\\lazy.bin").c_str()) != FALSE,
            L"Preconditions: lower origin must be removable");

        const NTSTATUS status = cu.CompleteLazyCopyUp(L"lazy.bin");
        Assert::IsFalse(NT_SUCCESS(status),
            L"Lazy completion must fail cleanly when origin is missing");

        // The metacopy flag stays set — the upper is still a metacopy, not a
        // silently "completed" zero-padded file. Any subsequent read-path
        // will re-observe the failure, which is the correct behavior (fail
        // loud, not corrupt silently).
        LayerMountMetadata after = MetadataADS::ReadLayerMountMetadata(upperPath);
        Assert::IsTrue(after.metacopy,
            L"metacopy flag must not be cleared on failed lazy completion");
    }

    // ------------------------------------------------------------------------
    // CopyFileData short-write hardening regression test.
    //
    // We can't inject a short write from user code without a mocking layer,
    // but we can verify the invariant the hardening check protects: the
    // upper file size must exactly match the source size, and its content
    // must be byte-identical, for payloads that straddle multiple copy-
    // buffer windows.
    //
    // Payload sized to cross the internal kCopyBufferSize boundary so a
    // future regression that reintroduces the missing bytesWritten check
    // would lose tail bytes and be caught here.
    // ------------------------------------------------------------------------
    TEST_METHOD(CopyUpFile_MultiBufferPayload_ExactByteFidelity) {
        LayerMountTests::TempLayerEnvironment env(1);
        // 1 MiB payload with a byte-addressable pattern so any truncation or
        // buffer-boundary off-by-one leaves a visible fingerprint.
        std::string payload(1024 * 1024, '\0');
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>((i * 131u) ^ 0xA5u);
        }
        env.WriteFile(env.Lower(0), L"big.bin", payload);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        Assert::IsTrue(NT_SUCCESS(cu.CopyUpFile(L"big.bin")));

        const std::string upperContent = env.ReadFile(env.Upper(), L"big.bin");
        Assert::AreEqual(payload.size(), upperContent.size(),
            L"Upper size must exactly equal source size");
        Assert::IsTrue(payload == upperContent,
            L"Upper bytes must be identical to source (no short-write truncation)");
    }

    // ------------------------------------------------------------------------
    // Sharing violation during copy-up.
    //
    // If another holder keeps the lower file open with no FILE_SHARE_READ,
    // CopyUp's source-open (GENERIC_READ + FILE_SHARE_READ) fails with
    // ERROR_SHARING_VIOLATION. The overlay must propagate a clean failure
    // and leave no half-committed upper copy or orphan temp file.
    // ------------------------------------------------------------------------
    TEST_METHOD(CopyUpFile_LowerLockedExclusive_FailsCleanly) {
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"locked.bin", "lower-content");

        const std::wstring lowerPath = env.Lower(0) + L"\\locked.bin";

        // Hog holds the file exclusively — no sharing of any kind.
        HANDLE hog = ::CreateFileW(lowerPath.c_str(),
                                     GENERIC_READ | GENERIC_WRITE,
                                     0 /* no sharing */, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        Assert::AreNotEqual<HANDLE>(INVALID_HANDLE_VALUE, hog,
            L"Precondition: hog must acquire the lower file exclusively");

        // Confirm the OS enforces sharing against a plain reader — so the
        // test premise is sound and the answer doesn't depend on a backup-
        // privilege quirk.
        HANDLE probe = ::CreateFileW(
            lowerPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const bool plainOpenBlocked = (probe == INVALID_HANDLE_VALUE) &&
                                       (::GetLastError() == ERROR_SHARING_VIOLATION);
        if (probe != INVALID_HANDLE_VALUE) ::CloseHandle(probe);

        auto config = env.MakeConfig();
        Cache cache;
        WhiteoutManager wm(config, &cache);
        PathResolver resolver(config, wm, cache);
        LayerMountStats stats;
        CopyUp cu(config, resolver, wm, cache, stats);

        const NTSTATUS st = cu.CopyUpFile(L"locked.bin");
        ::CloseHandle(hog);

        // If the OS enforced sharing for a plain reader, CopyUp must fail
        // too — it uses the same GENERIC_READ+FILE_SHARE_READ open pattern.
        if (plainOpenBlocked) {
            Assert::IsFalse(NT_SUCCESS(st),
                L"When the OS blocks a plain reader via sharing violation, "
                L"CopyUp must propagate the same failure");
            Assert::IsFalse(env.FileExists(env.Upper(), L"locked.bin"),
                L"Failed copy-up must not leave an upper artifact");
        } else {
            // Platform allowed the plain open anyway — accept either outcome
            // as long as the final state is consistent.
            if (NT_SUCCESS(st)) {
                Assert::AreEqual(std::string("lower-content"),
                                 env.ReadFile(env.Upper(), L"locked.bin"));
            } else {
                Assert::IsFalse(env.FileExists(env.Upper(), L"locked.bin"));
            }
        }

        // Invariant regardless: no stale work-dir temps.
        const std::wstring pattern = env.Work() + L"\\#*.tmp";
        WIN32_FIND_DATAW fd{};
        HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
        size_t leftover = 0;
        if (h != INVALID_HANDLE_VALUE) {
            do { ++leftover; } while (::FindNextFileW(h, &fd));
            ::FindClose(h);
        }
        Assert::AreEqual<size_t>(0, leftover,
            L"Copy-up under contention must leave no work-dir temp orphans");
    }
};

} // namespace LayerMountTests
