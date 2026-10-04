#include "pch.h"
#include "TestFixture.h"

#include "CopyUp.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"
#include "MetadataStore.h"
#include "EntryCopy.h"

#include "AclTestHelpers.h"

#include <winioctl.h>

#include <thread>
#include <atomic>
#include <chrono>
#include <future>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LayerMount;
using LayerMountTestShared::AccessDenied;

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

struct DirectoryCopyUpRace {
    DirectoryCopyUpRace(CopyUp& copyUp, std::wstring parent, std::wstring child,
                        std::wstring heldStreamPath)
        : copyUp(copyUp),
          parent(std::move(parent)),
          child(std::move(child)),
          heldStreamPath(std::move(heldStreamPath)),
          secondDone(secondStatus.get_future().share()) {}

    CopyUp& copyUp;
    const std::wstring parent;
    const std::wstring child;
    const std::wstring heldStreamPath;
    std::atomic<bool> started{false};
    std::promise<NTSTATUS> secondStatus;
    std::shared_future<NTSTATUS> secondDone;
    std::thread second;
    bool secondPendingAtHold = false;
    ScopedHandle heldStream;
};

// Fires inside the first copy-up of the child, at the copy-up of its parent.
// The first copy-up holds the child's reservation and has not yet created the
// upper child.
void LM_CALL RaceSecondCopyUpThenHoldLowerStream(const LM_EVENT* evt, void* context) {
    auto* race = static_cast<DirectoryCopyUpRace*>(context);
    if (evt->type != LM_EVT_COPY_UP || evt->relativePath == nullptr ||
        race->parent != evt->relativePath || race->started.exchange(true)) {
        return;
    }
    race->second = std::thread([race]() {
        race->secondStatus.set_value(race->copyUp.CopyUpDirectory(race->child));
    });
    // A serialized second copy-up waits on the reservation, so this wait times
    // out. Without serialization the second copy-up commits the child here.
    race->secondPendingAtHold =
        race->secondDone.wait_for(std::chrono::seconds(1)) == std::future_status::timeout;
    race->heldStream = OpenNewStreamExclusively(race->heldStreamPath);
}

// Reports the changes to the direct children of a directory from
// construction on.
class DirectoryWatch {
public:
    DirectoryWatch(const std::wstring& dirPath, DWORD filter)
        : dir_(::CreateFileW(dirPath.c_str(), FILE_LIST_DIRECTORY,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                             nullptr)),
          event_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          filter_(filter),
          buffer_(16 * 1024) {
        Assert::IsTrue(dir_.IsValid() && event_.IsValid(), L"The test must open the watched directory");
        Issue();
        Assert::IsTrue(pending_, L"ReadDirectoryChangesW must start the watch");
    }
    ~DirectoryWatch() {
        if (pending_) {
            ::CancelIoEx(dir_.Get(), &overlapped_);
            DWORD bytes = 0;
            ::GetOverlappedResult(dir_.Get(), &overlapped_, &bytes, TRUE);
        }
    }
    DirectoryWatch(const DirectoryWatch&) = delete;
    DirectoryWatch& operator=(const DirectoryWatch&) = delete;

    // The actions reported for the child called name, in order. The file
    // system queues a notification before the call that made the change
    // returns, so the read stops when no notification comes within the wait.
    std::vector<DWORD> ActionsFor(const std::wstring& name) {
        std::vector<DWORD> actions;
        while (pending_ && ::WaitForSingleObject(event_.Get(), 500) == WAIT_OBJECT_0) {
            pending_ = false;
            DWORD bytes = 0;
            Assert::IsTrue(::GetOverlappedResult(dir_.Get(), &overlapped_, &bytes, FALSE) != FALSE,
                L"The watch read must complete");
            Assert::AreNotEqual<DWORD>(0, bytes, L"The watch buffer must hold every notification");
            const BYTE* record = reinterpret_cast<const BYTE*>(buffer_.data());
            for (;;) {
                const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(record);
                const std::wstring changed(info->FileName, info->FileNameLength / sizeof(wchar_t));
                if (_wcsicmp(changed.c_str(), name.c_str()) == 0) {
                    actions.push_back(info->Action);
                }
                if (info->NextEntryOffset == 0) break;
                record += info->NextEntryOffset;
            }
            Issue();
        }
        return actions;
    }

private:
    void Issue() {
        ::ResetEvent(event_.Get());
        overlapped_ = {};
        overlapped_.hEvent = event_.Get();
        pending_ = ::ReadDirectoryChangesW(dir_.Get(), buffer_.data(),
                                           static_cast<DWORD>(buffer_.size() * sizeof(DWORD)),
                                           FALSE, filter_, nullptr, &overlapped_, nullptr) != FALSE;
    }

    ScopedHandle dir_;
    ScopedHandle event_;
    DWORD filter_;
    std::vector<DWORD> buffer_;
    OVERLAPPED overlapped_{};
    bool pending_ = false;
};

// A directory that something other than the engine makes at the upper path
// of a child, with one file in it and fixed times, while the engine copies
// up the child's parent.
struct ForeignDirectoryAtParentCopyUp {
    std::wstring parent;
    std::wstring foreignDir;
    std::wstring heldStreamPath;
    FILETIME stampedTime;
    bool made = false;
    ScopedHandle heldStream;
};

// Fires inside the copy-up of the child, at the copy-up of its parent. The
// copy-up of the child has found no upper entry at its path by then.
void LM_CALL MakeForeignDirectoryAtParentCopyUp(const LM_EVENT* evt, void* context) {
    auto* foreign = static_cast<ForeignDirectoryAtParentCopyUp*>(context);
    if (evt->type != LM_EVT_COPY_UP || evt->relativePath == nullptr ||
        foreign->parent != evt->relativePath || foreign->made) {
        return;
    }
    if (!::CreateDirectoryW(foreign->foreignDir.c_str(), nullptr)) {
        return;
    }
    ScopedHandle file(::CreateFileW((foreign->foreignDir + L"\\f.txt").c_str(), GENERIC_WRITE, 0,
                                    nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    ScopedHandle dir(::CreateFileW(foreign->foreignDir.c_str(), FILE_WRITE_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    foreign->made = file.IsValid() && dir.IsValid() &&
                    ::SetFileTime(dir.Get(), &foreign->stampedTime, &foreign->stampedTime,
                                  &foreign->stampedTime) != FALSE;
    if (!foreign->heldStreamPath.empty()) {
        foreign->heldStream = OpenNewStreamExclusively(foreign->heldStreamPath);
    }
}

// While it lives, makes the copy-up of foreign.parent call MakeForeign,
// which makes the foreign entry that foreign describes.
template <typename Foreign, LM_EVENT_CALLBACK MakeForeign>
class ForeignEntryArmed {
public:
    ForeignEntryArmed(CopyUp& copyUp, Foreign armed)
        : foreign(std::move(armed)), copyUp_(copyUp) {
        events_.Set(MakeForeign, &foreign);
        copyUp_.SetEventEmitter(&events_);
    }
    ~ForeignEntryArmed() { copyUp_.SetEventEmitter(nullptr); }
    ForeignEntryArmed(const ForeignEntryArmed&) = delete;
    ForeignEntryArmed& operator=(const ForeignEntryArmed&) = delete;

    Foreign foreign;

private:
    CopyUp& copyUp_;
    ::LayerMount::abi::EventEmitter events_;
};

using ForeignDirectoryArmed =
    ForeignEntryArmed<ForeignDirectoryAtParentCopyUp, &MakeForeignDirectoryAtParentCopyUp>;

// When heldStreamPath is not empty, the armed copy-up also holds that
// stream open with no sharing.
ForeignDirectoryArmed ArmForeignDirectoryAt(CopyUpAndRenameRig& rig,
                                            const std::wstring& parent,
                                            const std::wstring& foreignDir,
                                            const std::wstring& heldStreamPath) {
    ForeignDirectoryAtParentCopyUp foreign;
    foreign.parent = parent;
    foreign.foreignDir = foreignDir;
    foreign.heldStreamPath = heldStreamPath;
    foreign.stampedTime = MakeFileTime(2001, 2, 3);
    return ForeignDirectoryArmed(rig.copyUp, std::move(foreign));
}

void AssertForeignDirectoryUntouched(const ForeignDirectoryAtParentCopyUp& foreign,
                                     const LayerConfig& config) {
    Assert::AreNotEqual<DWORD>(INVALID_FILE_ATTRIBUTES,
        ::GetFileAttributesW((foreign.foreignDir + L"\\f.txt").c_str()),
        L"The foreign directory keeps its file");
    const DWORD attributes = ::GetFileAttributesW(foreign.foreignDir.c_str());
    Assert::AreNotEqual<DWORD>(INVALID_FILE_ATTRIBUTES, attributes,
        L"The foreign directory stays at the upper path");
    Assert::AreEqual<DWORD>(0, attributes & FILE_ATTRIBUTE_HIDDEN,
        L"The foreign directory does not take the lower directory's attributes");
    Assert::AreEqual<DWORD>(INVALID_FILE_ATTRIBUTES,
        ::GetFileAttributesW((foreign.foreignDir + L":s").c_str()),
        L"The foreign directory does not take the lower directory's stream");
    FILETIME creation{}, access{}, write{};
    GetTimes(foreign.foreignDir, &creation, &access, &write);
    Assert::IsTrue(FileTimesEqual(foreign.stampedTime, write),
        L"The foreign directory keeps its last-write time");
    Assert::IsTrue(FileTimesEqual(foreign.stampedTime, creation),
        L"The foreign directory keeps its creation time");
    Assert::AreEqual<DWORD>(0, attributes & FILE_ATTRIBUTE_REPARSE_POINT,
        L"The foreign directory does not become a link");
    Assert::IsTrue(MetadataStore::ReadLayerMountMetadata(foreign.foreignDir, &config)
                       .originLayer.empty(),
        L"The foreign directory gets no copy-up record");
}

// A file that something other than the engine makes at the upper path of a
// child, with fixed contents and times, while the engine copies up the
// child's parent.
struct ForeignFileAtParentCopyUp {
    std::wstring parent;
    std::wstring foreignFile;
    FILETIME stampedTime;
    bool made = false;
};

// Fires inside the copy-up of the child, at the copy-up of its parent. The
// copy-up of the child has found no upper entry at its path by then.
void LM_CALL MakeForeignFileAtParentCopyUp(const LM_EVENT* evt, void* context) {
    auto* foreign = static_cast<ForeignFileAtParentCopyUp*>(context);
    if (evt->type != LM_EVT_COPY_UP || evt->relativePath == nullptr ||
        foreign->parent != evt->relativePath || foreign->made) {
        return;
    }
    ScopedHandle file(::CreateFileW(foreign->foreignFile.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    DWORD written = 0;
    foreign->made = file.IsValid() &&
                    ::WriteFile(file.Get(), "foreign", 7, &written, nullptr) != FALSE &&
                    ::SetFileTime(file.Get(), &foreign->stampedTime, &foreign->stampedTime,
                                  &foreign->stampedTime) != FALSE;
}

using ForeignFileArmed =
    ForeignEntryArmed<ForeignFileAtParentCopyUp, &MakeForeignFileAtParentCopyUp>;

ForeignFileArmed ArmForeignFileAt(CopyUpAndRenameRig& rig,
                                  const std::wstring& parent,
                                  const std::wstring& foreignFile) {
    ForeignFileAtParentCopyUp foreign;
    foreign.parent = parent;
    foreign.foreignFile = foreignFile;
    foreign.stampedTime = MakeFileTime(2001, 2, 3);
    return ForeignFileArmed(rig.copyUp, std::move(foreign));
}

void AssertForeignFileUntouched(const TempLayerEnvironment& env,
                                const std::wstring& relativePath,
                                const ForeignFileAtParentCopyUp& foreign,
                                const LayerConfig& config) {
    const DWORD attributes = ::GetFileAttributesW(foreign.foreignFile.c_str());
    Assert::AreNotEqual<DWORD>(INVALID_FILE_ATTRIBUTES, attributes,
        L"The foreign file stays at the upper path");
    Assert::AreEqual(std::string("foreign"), env.ReadFile(env.Upper(), relativePath),
        L"The foreign file keeps its contents");
    Assert::AreEqual<DWORD>(0, attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN),
        L"The foreign file does not take the lower file's attributes");
    Assert::AreEqual<DWORD>(INVALID_FILE_ATTRIBUTES,
        ::GetFileAttributesW((foreign.foreignFile + L":s").c_str()),
        L"The foreign file does not take the lower file's stream");
    FILETIME creation{}, access{}, write{};
    GetTimes(foreign.foreignFile, &creation, &access, &write);
    Assert::IsTrue(FileTimesEqual(foreign.stampedTime, write),
        L"The foreign file keeps its last-write time");
    Assert::IsTrue(FileTimesEqual(foreign.stampedTime, creation),
        L"The foreign file keeps its creation time");
    Assert::IsTrue(MetadataStore::ReadLayerMountMetadata(foreign.foreignFile, &config)
                       .originLayer.empty(),
        L"The foreign file gets no copy-up record");
}

FILETIME StampedLowerFileTime() {
    return MakeFileTime(1999, 5, 6);
}

// Makes the lower file at relativePath read-only and hidden, with a stream
// named s and the times of StampedLowerFileTime.
void MakeReadOnlyHiddenLowerFileWithStream(const TempLayerEnvironment& env,
                                           const std::wstring& relativePath) {
    env.WriteFile(env.Lower(0), relativePath, "lower");
    const std::wstring lowerFile = env.Lower(0) + L"\\" + relativePath;
    ScopedHandle stream(::CreateFileW((lowerFile + L":s").c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    Assert::IsTrue(stream.IsValid(), L"The test must write the lower file's stream");
    stream.Reset();
    const FILETIME stamped = StampedLowerFileTime();
    StampTimes(lowerFile, stamped, stamped, stamped);
    Assert::IsTrue(::SetFileAttributesW(lowerFile.c_str(),
                                        FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN) != FALSE,
        L"The test must make the lower file read-only and hidden");
}

void AssertTimesOfLowerFile(const std::wstring& upperFile) {
    FILETIME creation{}, access{}, write{};
    GetTimes(upperFile, &creation, &access, &write);
    Assert::IsTrue(FileTimesEqual(StampedLowerFileTime(), write),
        L"The upper file has the lower file's last-write time");
    Assert::IsTrue(FileTimesEqual(StampedLowerFileTime(), creation),
        L"The upper file has the lower file's creation time");
}

constexpr DWORD kFileChanges = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
                               FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
                               FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_SECURITY;

// Makes the lower directory at relativePath hidden, with a stream named s.
void MakeHiddenLowerDirectoryWithStream(const TempLayerEnvironment& env,
                                        const std::wstring& relativePath) {
    env.CreateDir(env.Lower(0), relativePath);
    const std::wstring lowerDir = env.Lower(0) + L"\\" + relativePath;
    ScopedHandle stream(::CreateFileW((lowerDir + L":s").c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    Assert::IsTrue(stream.IsValid(), L"The test must write the lower directory's stream");
    stream.Reset();
    Assert::IsTrue(::SetFileAttributesW(lowerDir.c_str(), FILE_ATTRIBUTE_HIDDEN) != FALSE,
        L"The test must make the lower directory hidden");
}

}


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
    // A failed commit removes the work file, so the work directory has no
    // #*.tmp file left.
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
        LayerMountMetadata before = MetadataStore::ReadLayerMountMetadata(upperPath, nullptr);
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
        LayerMountMetadata after = MetadataStore::ReadLayerMountMetadata(upperPath, nullptr);
        Assert::IsTrue(after.metacopy,
            L"metacopy flag must not be cleared on failed lazy completion");
    }

    // ------------------------------------------------------------------------
    // Short-write regression test for the data copy.
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

    TEST_METHOD(CopyUserAlternateDataStreams_MissingSource_ReturnsNotFound) {
        UNIT_SKIP_IF_NOT_NTFS();
        TempLayerEnvironment env(1);
        env.WriteFile(env.Upper(), L"dst.txt", "dst");

        const NTSTATUS status = CopyUserAlternateDataStreams(
            env.Lower(0) + L"\\absent.txt", env.Upper() + L"\\dst.txt");

        Assert::AreEqual<NTSTATUS>(STATUS_OBJECT_NAME_NOT_FOUND, status,
            L"A source that does not exist fails the stream copy");
    }

    TEST_METHOD(CopyUserAlternateDataStreams_StreamCopyFails_ReturnsThatStreamsStatus) {
        UNIT_SKIP_IF_NOT_NTFS();
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"src.txt", "src");
        env.WriteFile(env.Upper(), L"dst.txt", "dst");
        const std::wstring srcPath = env.Lower(0) + L"\\src.txt";
        const std::wstring dstPath = env.Upper() + L"\\dst.txt";

        HANDLE srcStream = ::CreateFileW((srcPath + L":held").c_str(), GENERIC_WRITE, 0,
                                         nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        Assert::AreNotEqual<HANDLE>(INVALID_HANDLE_VALUE, srcStream);
        ::CloseHandle(srcStream);

        ScopedHandle heldDst = HoldNewStreamExclusively(dstPath + L":held");

        const NTSTATUS status = CopyUserAlternateDataStreams(srcPath, dstPath);
        heldDst.Reset();

        Assert::AreEqual<NTSTATUS>(STATUS_SHARING_VIOLATION, status,
            L"The copy reports the status of the stream that failed");
    }

    TEST_METHOD(CopyUpFile_ParentWatch_SeesTheFileArriveOnceWithItsStreamAndRecord) {
        UNIT_SKIP_IF_NOT_NTFS();
        ForEachMetadataStore([](UINT32 hostCapabilities) {
            TempLayerEnvironment env(1);
            MakeReadOnlyHiddenLowerFileWithStream(env, L"f.txt");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = hostCapabilities;
            CopyUpAndRenameRig rig(config);

            DirectoryWatch watch(env.Upper(), kFileChanges);
            const NTSTATUS status = rig.copyUp.CopyUpFile(L"f.txt");
            const std::vector<DWORD> actions = watch.ActionsFor(L"f.txt");

            Assert::AreEqual<NTSTATUS>(STATUS_SUCCESS, status, L"The file copy-up succeeds");
            Assert::AreEqual<size_t>(1, actions.size(),
                L"The parent sees one change for the file, its arrival");
            Assert::AreEqual<DWORD>(FILE_ACTION_ADDED, actions[0],
                L"The file arrives in the parent by a move from the work directory");
            const std::wstring upperFile = env.Upper() + L"\\f.txt";
            Assert::AreEqual<DWORD>(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN,
                ::GetFileAttributesW(upperFile.c_str()) &
                    (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN),
                L"The file arrives with the lower file's attributes");
            AssertTimesOfLowerFile(upperFile);
            Assert::AreNotEqual<DWORD>(INVALID_FILE_ATTRIBUTES,
                ::GetFileAttributesW((upperFile + L":s").c_str()),
                L"The file arrives with the lower file's stream");
            Assert::AreEqual(0, _wcsicmp((env.Lower(0) + L"\\f.txt").c_str(),
                MetadataStore::ReadLayerMountMetadata(upperFile, &rig.config).originLayer.c_str()),
                L"The upper file's copy-up record names the lower file");
            Assert::IsTrue(EntriesUnder(env.Work()).empty(),
                L"The copy-up leaves nothing in the work directory");
        });
    }

    TEST_METHOD(CopyUpFile_StreamCopyFails_ReturnsThatStreamsStatusAndShowsNothingAtTheUpperPath) {
        UNIT_SKIP_IF_NOT_NTFS();
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"f.txt", "lower");
        ScopedHandle heldSrc = HoldNewStreamExclusively(env.Lower(0) + L"\\f.txt:held");

        CopyUpAndRenameRig rig(env.MakeConfig());
        DirectoryWatch watch(env.Upper(), FILE_NOTIFY_CHANGE_FILE_NAME);
        const NTSTATUS status = rig.copyUp.CopyUpFile(L"f.txt");
        heldSrc.Reset();
        const std::vector<DWORD> actions = watch.ActionsFor(L"f.txt");

        Assert::AreEqual<NTSTATUS>(STATUS_SHARING_VIOLATION, status,
            L"The file copy-up reports the status of the stream that failed");
        Assert::IsTrue(actions.empty(), L"The parent never sees an entry at the upper path");
        Assert::AreEqual<DWORD>(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW((env.Upper() + L"\\f.txt").c_str()),
            L"A failed file copy-up leaves no upper file");
        Assert::IsTrue(EntriesUnder(env.Work()).empty(),
            L"A failed file copy-up leaves nothing in the work directory");
    }

    TEST_METHOD(CopyUpFile_ForeignFileAtTheUpperPath_FailsWithCollisionAndKeepsIt) {
        UNIT_SKIP_IF_NOT_NTFS();
        ForEachMetadataStore([](UINT32 hostCapabilities) {
            TempLayerEnvironment env(1);
            MakeReadOnlyHiddenLowerFileWithStream(env, L"p\\f.txt");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = hostCapabilities;
            CopyUpAndRenameRig rig(config);

            auto armed = ArmForeignFileAt(rig, L"p", env.Upper() + L"\\p\\f.txt");
            const NTSTATUS status = rig.copyUp.CopyUpFile(L"p\\f.txt");

            Assert::IsTrue(armed.foreign.made, L"The test makes the foreign file at the upper path");
            Assert::AreEqual<NTSTATUS>(STATUS_OBJECT_NAME_COLLISION, status,
                L"The copy-up fails when an entry it did not make holds the upper path");
            AssertForeignFileUntouched(env, L"p\\f.txt", armed.foreign, rig.config);
            Assert::IsTrue(EntriesUnder(env.Work()).empty(),
                L"The failed copy-up removes its read-only copy from the work directory");
        });
    }

    TEST_METHOD(CopyUpMetadataOnly_ParentWatch_SeesTheShellArriveOnceWithItsRecord) {
        UNIT_SKIP_IF_NOT_NTFS();
        ForEachMetadataStore([](UINT32 hostCapabilities) {
            TempLayerEnvironment env(1);
            MakeReadOnlyHiddenLowerFileWithStream(env, L"f.txt");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = hostCapabilities;
            CopyUpAndRenameRig rig(config);

            DirectoryWatch watch(env.Upper(), kFileChanges);
            const NTSTATUS status = rig.copyUp.CopyUpMetadataOnly(L"f.txt");
            const std::vector<DWORD> actions = watch.ActionsFor(L"f.txt");

            Assert::AreEqual<NTSTATUS>(STATUS_SUCCESS, status, L"The metacopy succeeds");
            Assert::AreEqual<size_t>(1, actions.size(),
                L"The parent sees one change for the shell, its arrival");
            Assert::AreEqual<DWORD>(FILE_ACTION_ADDED, actions[0],
                L"The shell arrives in the parent by a move from the work directory");
            const std::wstring upperFile = env.Upper() + L"\\f.txt";
            Assert::AreEqual<DWORD>(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN,
                ::GetFileAttributesW(upperFile.c_str()) &
                    (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN),
                L"The shell arrives with the lower file's attributes");
            AssertTimesOfLowerFile(upperFile);
            const LayerMountMetadata md =
                MetadataStore::ReadLayerMountMetadata(upperFile, &rig.config);
            Assert::IsTrue(md.metacopy, L"The shell's copy-up record has the metacopy flag");
            Assert::AreEqual(0, _wcsicmp((env.Lower(0) + L"\\f.txt").c_str(),
                md.originLayer.c_str()),
                L"The shell's copy-up record names the lower file");
            Assert::IsTrue(EntriesUnder(env.Work()).empty(),
                L"The metacopy leaves nothing in the work directory");
        });
    }

    TEST_METHOD(CompleteLazyCopyUp_ReadOnlyShell_FillsTheShellAndKeepsItReadOnly) {
        UNIT_SKIP_IF_NOT_NTFS();
        ForEachMetadataStore([](UINT32 hostCapabilities) {
            TempLayerEnvironment env(1);
            MakeReadOnlyHiddenLowerFileWithStream(env, L"f.txt");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = hostCapabilities;
            CopyUpAndRenameRig rig(config);
            AssertStatus(STATUS_SUCCESS, rig.copyUp.CopyUpMetadataOnly(L"f.txt"),
                L"The metacopy of the read-only lower file succeeds");

            AssertStatus(STATUS_SUCCESS, rig.copyUp.CompleteLazyCopyUp(L"f.txt"),
                L"The fill of the read-only shell succeeds");

            const std::wstring upperFile = env.Upper() + L"\\f.txt";
            Assert::AreEqual(std::string("lower"), env.ReadFile(env.Upper(), L"f.txt"),
                L"The filled file has the lower file's data");
            Assert::AreNotEqual<DWORD>(INVALID_FILE_ATTRIBUTES,
                ::GetFileAttributesW((upperFile + L":s").c_str()),
                L"The filled file has the lower file's stream");
            Assert::IsFalse(MetadataStore::ReadLayerMountMetadata(upperFile, &rig.config).metacopy,
                L"The fill clears the metacopy flag");
            Assert::AreEqual<DWORD>(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN,
                ::GetFileAttributesW(upperFile.c_str()) &
                    (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN),
                L"The filled file keeps the lower file's attributes");
            AssertTimesOfLowerFile(upperFile);
        });
    }

    TEST_METHOD(CopyUpMetadataOnly_ForeignFileAtTheUpperPath_FailsWithCollisionAndKeepsIt) {
        UNIT_SKIP_IF_NOT_NTFS();
        ForEachMetadataStore([](UINT32 hostCapabilities) {
            TempLayerEnvironment env(1);
            MakeReadOnlyHiddenLowerFileWithStream(env, L"p\\f.txt");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = hostCapabilities;
            CopyUpAndRenameRig rig(config);

            auto armed = ArmForeignFileAt(rig, L"p", env.Upper() + L"\\p\\f.txt");
            const NTSTATUS status = rig.copyUp.CopyUpMetadataOnly(L"p\\f.txt");

            Assert::IsTrue(armed.foreign.made, L"The test makes the foreign file at the upper path");
            Assert::AreEqual<NTSTATUS>(STATUS_OBJECT_NAME_COLLISION, status,
                L"The metacopy fails when an entry it did not make holds the upper path");
            AssertForeignFileUntouched(env, L"p\\f.txt", armed.foreign, rig.config);
            Assert::IsTrue(EntriesUnder(env.Work()).empty(),
                L"The failed metacopy removes its read-only shell from the work directory");
        });
    }

    TEST_METHOD(CopyUpDirectory_StreamCopyFails_ReturnsThatStreamsStatusAndLeavesNoUpperDirectory) {
        UNIT_SKIP_IF_NOT_NTFS();
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"dir");
        ScopedHandle heldSrc = HoldNewStreamExclusively(env.Lower(0) + L"\\dir:held");

        CopyUpAndRenameRig rig(env.MakeConfig());
        const NTSTATUS status = rig.copyUp.CopyUpDirectory(L"dir");
        heldSrc.Reset();

        Assert::AreEqual<NTSTATUS>(STATUS_SHARING_VIOLATION, status,
            L"Directory copy-up reports the status of the stream that failed");
        Assert::AreEqual<DWORD>(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW((env.Upper() + L"\\dir").c_str()),
            L"A failed directory copy-up leaves no upper directory");
        Assert::IsTrue(EntriesUnder(env.Work()).empty(),
            L"A failed directory copy-up leaves nothing in the work directory");
    }

    TEST_METHOD(CopyUpDirectory_ParentWatch_SeesTheDirectoryArriveOnceAndNeverChange) {
        UNIT_SKIP_IF_NOT_NTFS();
        TempLayerEnvironment env(1);
        MakeHiddenLowerDirectoryWithStream(env, L"d");

        CopyUpAndRenameRig rig(env.MakeConfig());
        DirectoryWatch watch(env.Upper(),
            FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION |
            FILE_NOTIFY_CHANGE_SECURITY);
        const NTSTATUS status = rig.copyUp.CopyUpDirectory(L"d");
        const std::vector<DWORD> actions = watch.ActionsFor(L"d");

        Assert::AreEqual<NTSTATUS>(STATUS_SUCCESS, status, L"The directory copy-up succeeds");
        // A move from the work directory reaches a watch of the upper parent
        // as FILE_ACTION_ADDED, because NTFS reports a move between two
        // directories as a remove and an add.
        Assert::AreEqual<size_t>(1, actions.size(),
            L"The parent sees one change for the directory: its arrival");
        Assert::AreEqual<DWORD>(FILE_ACTION_ADDED, actions[0],
            L"The directory arrives in the parent by a move from the work directory");
        Assert::AreEqual<DWORD>(FILE_ATTRIBUTE_HIDDEN,
            ::GetFileAttributesW((env.Upper() + L"\\d").c_str()) & FILE_ATTRIBUTE_HIDDEN,
            L"The directory arrives with the lower directory's attributes");
        Assert::AreNotEqual<DWORD>(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW((env.Upper() + L"\\d:s").c_str()),
            L"The directory arrives with the lower directory's stream");
        Assert::IsTrue(EntriesUnder(env.Work()).empty(),
            L"The copy-up leaves nothing in the work directory");
    }

    TEST_METHOD(CopyUpDirectory_RecordFollowsTheDirectoryToItsUpperPath) {
        UNIT_SKIP_IF_NOT_NTFS();
        ForEachMetadataStore([](UINT32 hostCapabilities) {
            TempLayerEnvironment env(1);
            env.CreateDir(env.Lower(0), L"d");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = hostCapabilities;
            CopyUpAndRenameRig rig(config);

            Assert::AreEqual<NTSTATUS>(STATUS_SUCCESS, rig.copyUp.CopyUpDirectory(L"d"),
                L"The directory copy-up succeeds");

            const LayerMountMetadata md =
                MetadataStore::ReadLayerMountMetadata(env.Upper() + L"\\d", &rig.config);
            Assert::AreEqual(0, _wcsicmp((env.Lower(0) + L"\\d").c_str(), md.originLayer.c_str()),
                L"The upper directory's copy-up record names the lower directory");
            Assert::IsTrue(EntriesUnder(env.Work()).empty(),
                L"The copy-up leaves nothing in the work directory");
        });
    }

    TEST_METHOD(CopyUpDirectory_ForeignDirectoryAtTheUpperPath_FailsWithCollisionAndKeepsIt) {
        UNIT_SKIP_IF_NOT_NTFS();
        ForEachMetadataStore([](UINT32 hostCapabilities) {
            TempLayerEnvironment env(1);
            MakeHiddenLowerDirectoryWithStream(env, L"p\\d");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = hostCapabilities;
            CopyUpAndRenameRig rig(config);

            auto armed = ArmForeignDirectoryAt(rig, L"p", env.Upper() + L"\\p\\d", L"");
            const NTSTATUS status = rig.copyUp.CopyUpDirectory(L"p\\d");

            Assert::IsTrue(armed.foreign.made,

                L"The test makes the foreign directory at the upper path");
            Assert::AreEqual<NTSTATUS>(STATUS_OBJECT_NAME_COLLISION, status,
                L"The copy-up fails when an entry it did not make holds the upper path");
            AssertForeignDirectoryUntouched(armed.foreign, rig.config);
            Assert::IsTrue(EntriesUnder(env.Work()).empty(),
                L"The failed copy-up leaves nothing in the work directory");
        });
    }

    TEST_METHOD(CopyUpDirectory_ForeignDirectoryAtTheUpperPathAndStreamCopyFails_KeepsIt) {
        UNIT_SKIP_IF_NOT_NTFS();
        TempLayerEnvironment env(1);
        MakeHiddenLowerDirectoryWithStream(env, L"p\\d");
        CopyUpAndRenameRig rig(env.MakeConfig());

        auto armed = ArmForeignDirectoryAt(rig, L"p", env.Upper() + L"\\p\\d",
                                           env.Lower(0) + L"\\p\\d:held");
        const NTSTATUS status = rig.copyUp.CopyUpDirectory(L"p\\d");
        const bool streamHeld = armed.foreign.heldStream.IsValid();
        armed.foreign.heldStream.Reset();

        Assert::IsTrue(armed.foreign.made,

            L"The test makes the foreign directory at the upper path");
        Assert::IsTrue(streamHeld, L"The test holds the lower stream open with no sharing");
        Assert::AreEqual<NTSTATUS>(STATUS_SHARING_VIOLATION, status,
            L"The copy-up fails at the held stream");
        AssertForeignDirectoryUntouched(armed.foreign, rig.config);
        Assert::IsTrue(EntriesUnder(env.Work()).empty(),
            L"The failed copy-up leaves nothing in the work directory");
    }

    TEST_METHOD(CopyUpDirectory_LowerJunction_MovesTheLinkWithItsRecordToItsUpperPath) {
        UNIT_SKIP_IF_NOT_NTFS();
        ForEachMetadataStore([](UINT32 hostCapabilities) {
            TempLayerEnvironment env(1);
            if (!LowerJunctionCreatedOrSkipped(env, L"p")) {
                return;
            }
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = hostCapabilities;
            CopyUpAndRenameRig rig(config);

            Assert::AreEqual<NTSTATUS>(STATUS_SUCCESS, rig.copyUp.CopyUpDirectory(L"p\\link"),
                L"The copy-up of the lower junction succeeds");

            const std::wstring upperLink = env.Upper() + L"\\p\\link";
            Assert::IsTrue(HasAttribute(upperLink, FILE_ATTRIBUTE_REPARSE_POINT),
                L"The upper entry is a link");
            const LayerMountMetadata md =
                MetadataStore::ReadLayerMountMetadata(upperLink, &rig.config);
            Assert::AreEqual(0,
                _wcsicmp((env.Lower(0) + L"\\p\\link").c_str(), md.originLayer.c_str()),
                L"The upper link's copy-up record names the lower junction");
            Assert::IsTrue(EntriesUnder(env.Work()).empty(),
                L"The copy-up leaves nothing in the work directory");
        });
    }

    TEST_METHOD(CopyUpDirectory_ForeignDirectoryAtTheUpperPathOfALowerJunction_FailsWithCollisionAndKeepsIt) {
        UNIT_SKIP_IF_NOT_NTFS();
        ForEachMetadataStore([](UINT32 hostCapabilities) {
            TempLayerEnvironment env(1);
            if (!LowerJunctionCreatedOrSkipped(env, L"p")) {
                return;
            }
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = hostCapabilities;
            CopyUpAndRenameRig rig(config);

            auto armed = ArmForeignDirectoryAt(rig, L"p", env.Upper() + L"\\p\\link", L"");
            const NTSTATUS status = rig.copyUp.CopyUpDirectory(L"p\\link");

            Assert::IsTrue(armed.foreign.made,

                L"The test makes the foreign directory at the upper path");
            Assert::AreEqual<NTSTATUS>(STATUS_OBJECT_NAME_COLLISION, status,
                L"The copy-up fails when an entry it did not make holds the upper path");
            AssertForeignDirectoryUntouched(armed.foreign, rig.config);
            Assert::IsTrue(EntriesUnder(env.Work()).empty(),
                L"The failed copy-up leaves nothing in the work directory");
        });
    }

    TEST_METHOD(CopyUpDirectory_LowerDaclDeniesListingAndDelete_CommitsTheDirectory) {
        UNIT_SKIP_IF_NOT_NTFS();
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"p\\d");
        const AccessDenied denied(env.Lower(0) + L"\\p\\d", FILE_LIST_DIRECTORY | DELETE);
        CopyUpAndRenameRig rig(env.MakeConfig());

        Assert::AreEqual<NTSTATUS>(STATUS_SUCCESS, rig.copyUp.CopyUpDirectory(L"p\\d"),
            L"The copy-up of a directory whose DACL denies listing and delete succeeds");
        Assert::IsTrue(HasAttribute(env.Upper() + L"\\p\\d", FILE_ATTRIBUTE_DIRECTORY),
            L"The directory moves to its upper path");
        Assert::IsTrue(EntriesUnder(env.Work()).empty(),
            L"The copy-up leaves nothing in the work directory");
    }

    TEST_METHOD(CopyUpDirectory_ForeignDirectoryAtTheUpperPathOfADeniedLowerDirectory_LeavesTheWorkDirectoryEmpty) {
        UNIT_SKIP_IF_NOT_NTFS();
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"p\\d");
        const AccessDenied denied(env.Lower(0) + L"\\p\\d", FILE_LIST_DIRECTORY | DELETE);
        CopyUpAndRenameRig rig(env.MakeConfig());

        auto armed = ArmForeignDirectoryAt(rig, L"p", env.Upper() + L"\\p\\d", L"");
        const NTSTATUS status = rig.copyUp.CopyUpDirectory(L"p\\d");

        Assert::IsTrue(armed.foreign.made,

            L"The test makes the foreign directory at the upper path");
        Assert::AreEqual<NTSTATUS>(STATUS_OBJECT_NAME_COLLISION, status,
            L"The copy-up fails when an entry it did not make holds the upper path");
        Assert::IsTrue(EntriesUnder(env.Work()).empty(),
            L"The failed copy-up removes its copy, which carries the denying DACL");
    }

    TEST_METHOD(CopyUpDirectory_RacingCopyUpFails_KeepsTheDirectoryTheOtherCommitted) {
        UNIT_SKIP_IF_NOT_NTFS();
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"p\\d");
        const std::wstring upperDir = env.Upper() + L"\\p\\d";

        CopyUpAndRenameRig rig(env.MakeConfig());
        DirectoryCopyUpRace race(rig.copyUp, L"p", L"p\\d", env.Lower(0) + L"\\p\\d:held");
        ::LayerMount::abi::EventEmitter events;
        events.Set(&RaceSecondCopyUpThenHoldLowerStream, &race);
        rig.copyUp.SetEventEmitter(&events);

        const NTSTATUS firstStatus = rig.copyUp.CopyUpDirectory(race.child);
        const bool streamHeld = race.heldStream.IsValid();
        race.heldStream.Reset();
        if (race.second.joinable()) {
            race.second.join();
        }
        rig.copyUp.SetEventEmitter(nullptr);

        Assert::IsTrue(race.started.load(), L"The copy-up of the parent p starts the second copy-up");
        Assert::IsTrue(streamHeld, L"The test holds the lower stream open with no sharing");
        Assert::IsTrue(race.secondPendingAtHold,
            L"The second copy-up waits until the first copy-up ends");
        Assert::AreEqual<NTSTATUS>(STATUS_SHARING_VIOLATION, firstStatus,
            L"The first copy-up fails at the held stream");

        // The second copy-up can wake before the test releases the stream.
        // Then it fails at the stream, as the first copy-up did.
        const NTSTATUS secondStatus = race.secondDone.get();
        if (secondStatus == STATUS_SHARING_VIOLATION) {
            Assert::AreEqual<DWORD>(INVALID_FILE_ATTRIBUTES,
                ::GetFileAttributesW(upperDir.c_str()),
                L"A second copy-up that fails at the stream leaves no upper directory");
        } else {
            Assert::AreEqual<NTSTATUS>(STATUS_SUCCESS, secondStatus,
                L"The second copy-up succeeds once the stream is free");
            Assert::AreNotEqual<DWORD>(INVALID_FILE_ATTRIBUTES,
                ::GetFileAttributesW(upperDir.c_str()),
                L"The directory that the second copy-up committed stays in the upper");
            const LayerMountMetadata md = MetadataStore::ReadLayerMountMetadata(upperDir, nullptr);
            Assert::IsFalse(md.originLayer.empty(),
                L"The committed directory keeps its copy-up record");
        }
    }

    TEST_METHOD(DirectoryRename_StreamCopyFails_ReturnsThatStreamsStatusAndLeavesNoCopy) {
        UNIT_SKIP_IF_NOT_NTFS();
        TempLayerEnvironment env(1);
        env.CreateDir(env.Lower(0), L"tree");
        ScopedHandle heldSrc = HoldNewStreamExclusively(env.Lower(0) + L"\\tree:held");

        CopyUpAndRenameRig rig(env.MakeConfig());
        const NTSTATUS status = rig.directoryRename.RenameLowerDirectory(
            CallerPath(L"tree"), CallerPath(L"moved"),
            EntryKind::Directory, ReplaceExisting::No);
        heldSrc.Reset();

        Assert::AreEqual<NTSTATUS>(STATUS_SHARING_VIOLATION, status,
            L"The rename reports the status of the stream that failed");
        Assert::AreEqual<DWORD>(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW((env.Upper() + L"\\moved").c_str()),
            L"A failed rename leaves no copy at the new name");
        Assert::IsTrue(EntriesUnder(env.Work()).empty(),
            L"A failed rename leaves nothing in the work directory");
    }

    TEST_METHOD(DirectoryRename_ParentWatch_SeesTheDirectoryArriveOnceAndNeverChange) {
        UNIT_SKIP_IF_NOT_NTFS();
        TempLayerEnvironment env(1);
        MakeHiddenLowerDirectoryWithStream(env, L"tree");
        env.WriteFile(env.Lower(0), L"tree\\a.txt", "a");

        CopyUpAndRenameRig rig(env.MakeConfig());
        DirectoryWatch watch(env.Upper(),
            FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION |
            FILE_NOTIFY_CHANGE_SECURITY);
        const NTSTATUS status = rig.directoryRename.RenameLowerDirectory(
            CallerPath(L"tree"), CallerPath(L"moved"),
            EntryKind::Directory, ReplaceExisting::No);
        const std::vector<DWORD> actions = watch.ActionsFor(L"moved");

        Assert::AreEqual<NTSTATUS>(STATUS_SUCCESS, status, L"The rename succeeds");
        Assert::AreEqual<size_t>(1, actions.size(),
            L"The parent sees one change for the new name: its arrival");
        Assert::AreEqual<DWORD>(FILE_ACTION_ADDED, actions[0],
            L"The directory arrives in the parent by a move from the work directory");
        const std::wstring moved = env.Upper() + L"\\moved";
        Assert::AreEqual<DWORD>(FILE_ATTRIBUTE_HIDDEN,
            ::GetFileAttributesW(moved.c_str()) & FILE_ATTRIBUTE_HIDDEN,
            L"The directory arrives with the lower directory's attributes");
        Assert::AreNotEqual<DWORD>(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW((moved + L":s").c_str()),
            L"The directory arrives with the lower directory's stream");
        Assert::AreEqual(std::string("a"), env.ReadFile(env.Upper(), L"moved\\a.txt"),
            L"The directory arrives with its child");
        Assert::IsTrue(rig.whiteouts.IsOpaque(L"moved"), L"The directory arrives opaque");
        Assert::IsTrue(EntriesUnder(env.Work()).empty(),
            L"The rename leaves nothing in the work directory");
    }

    TEST_METHOD(DirectoryRename_LowerTree_ArrivesWithItsRecordsAndOpaqueMetadata) {
        UNIT_SKIP_IF_NOT_NTFS();
        ForEachMetadataStore([](UINT32 hostCapabilities) {
            TempLayerEnvironment env(1);
            env.WriteFile(env.Lower(0), L"tree\\sub\\x.txt", "x");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = hostCapabilities;
            CopyUpAndRenameRig rig(config);

            AssertStatus(STATUS_SUCCESS, rig.directoryRename.RenameLowerDirectory(
                CallerPath(L"tree"), CallerPath(L"moved"),
                EntryKind::Directory, ReplaceExisting::No),
                L"The rename of the lower tree succeeds");

            const std::wstring moved = env.Upper() + L"\\moved";
            Assert::IsTrue(MetadataStore::HasOpaqueMetadata(moved, &rig.config),
                L"The new name has the opaque metadata, not only the opaque marker file");
            Assert::AreEqual(0, _wcsicmp((env.Lower(0) + L"\\tree").c_str(),
                MetadataStore::ReadLayerMountMetadata(moved, &rig.config).originLayer.c_str()),
                L"The copy-up record of the new name names the lower directory");
            Assert::AreEqual(0, _wcsicmp((env.Lower(0) + L"\\tree\\sub").c_str(),
                MetadataStore::ReadLayerMountMetadata(moved + L"\\sub", &rig.config)
                    .originLayer.c_str()),
                L"The copy-up record of the child directory names the lower child");
            Assert::IsTrue(EntriesUnder(env.Work()).empty(),
                L"The rename leaves nothing in the work directory");
        });
    }

    TEST_METHOD(DirectoryRename_ChildStreamCopyFails_ShowsNothingAtTheNewName) {
        UNIT_SKIP_IF_NOT_NTFS();
        TempLayerEnvironment env(1);
        env.WriteFile(env.Lower(0), L"tree\\a.txt", "a");
        env.WriteFile(env.Lower(0), L"tree\\b.txt", "b");
        ScopedHandle heldSrc = HoldNewStreamExclusively(env.Lower(0) + L"\\tree\\b.txt:held");

        CopyUpAndRenameRig rig(env.MakeConfig());
        DirectoryWatch watch(env.Upper(), FILE_NOTIFY_CHANGE_DIR_NAME);
        const NTSTATUS status = rig.directoryRename.RenameLowerDirectory(
            CallerPath(L"tree"), CallerPath(L"moved"),
            EntryKind::Directory, ReplaceExisting::No);
        heldSrc.Reset();
        const std::vector<DWORD> actions = watch.ActionsFor(L"moved");

        Assert::AreEqual<NTSTATUS>(STATUS_SHARING_VIOLATION, status,
            L"The rename reports the status of the child stream that failed");
        Assert::IsTrue(actions.empty(), L"The parent never sees an entry at the new name");
        Assert::AreEqual<DWORD>(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW((env.Upper() + L"\\moved").c_str()),
            L"A failed rename leaves no copy at the new name");
        Assert::IsTrue(EntriesUnder(env.Work()).empty(),
            L"A failed rename leaves nothing in the work directory");
    }

    TEST_METHOD(DirectoryRename_ForeignDirectoryAtTheNewName_FailsWithCollisionAndKeepsIt) {
        UNIT_SKIP_IF_NOT_NTFS();
        ForEachMetadataStore([](UINT32 hostCapabilities) {
            TempLayerEnvironment env(1);
            MakeHiddenLowerDirectoryWithStream(env, L"p\\tree");
            env.WriteFile(env.Lower(0), L"p\\tree\\a.txt", "a");
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = hostCapabilities;
            CopyUpAndRenameRig rig(config);

            auto armed = ArmForeignDirectoryAt(rig, L"p", env.Upper() + L"\\p\\moved", L"");
            const NTSTATUS status = rig.directoryRename.RenameLowerDirectory(
                CallerPath(L"p\\tree"), CallerPath(L"p\\moved"),
                EntryKind::Directory, ReplaceExisting::No);

            Assert::IsTrue(armed.foreign.made,

                L"The test makes the foreign directory at the new name");
            Assert::AreEqual<NTSTATUS>(STATUS_OBJECT_NAME_COLLISION, status,
                L"The rename fails when an entry it did not make holds the new name");
            AssertForeignDirectoryUntouched(armed.foreign, rig.config);
            Assert::IsFalse(env.FileExists(env.Upper(), L"p\\moved\\a.txt"),
                L"The foreign directory does not take the renamed directory's child");
            Assert::IsFalse(rig.whiteouts.IsOpaque(L"p\\moved"),
                L"The foreign directory does not become opaque");
            Assert::IsTrue(EntriesUnder(env.Work()).empty(),
                L"The failed rename leaves nothing in the work directory");
        });
    }

    TEST_METHOD(DirectoryRename_ForeignDirectoryAtTheNewNameOfALowerJunction_FailsWithCollisionAndKeepsIt) {
        UNIT_SKIP_IF_NOT_NTFS();
        ForEachMetadataStore([](UINT32 hostCapabilities) {
            TempLayerEnvironment env(1);
            if (!LowerJunctionCreatedOrSkipped(env, L"p")) {
                return;
            }
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = hostCapabilities;
            CopyUpAndRenameRig rig(config);

            auto armed = ArmForeignDirectoryAt(rig, L"p", env.Upper() + L"\\p\\moved", L"");
            const NTSTATUS status = rig.directoryRename.RenameLowerDirectory(
                CallerPath(L"p\\link"), CallerPath(L"p\\moved"),
                EntryKind::Link, ReplaceExisting::No);

            Assert::IsTrue(armed.foreign.made,

                L"The test makes the foreign directory at the new name");
            Assert::AreEqual<NTSTATUS>(STATUS_OBJECT_NAME_COLLISION, status,
                L"The rename fails when an entry it did not make holds the new name");
            AssertForeignDirectoryUntouched(armed.foreign, rig.config);
            Assert::IsTrue(EntriesUnder(env.Work()).empty(),
                L"The failed rename leaves nothing in the work directory");
        });
    }

    TEST_METHOD(CaseOnlyRename_ForeignDirectoryAtTheNewNameOfALowerJunction_FailsWithCollisionAndKeepsIt) {
        UNIT_SKIP_IF_NOT_NTFS();
        ForEachMetadataStore([](UINT32 hostCapabilities) {
            TempLayerEnvironment env(1);
            if (!LowerJunctionCreatedOrSkipped(env, L"p")) {
                return;
            }
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = hostCapabilities;
            CopyUpAndRenameRig rig(config);

            auto armed = ArmForeignDirectoryAt(rig, L"p", env.Upper() + L"\\p\\LINK", L"");
            const NTSTATUS status = rig.copyUp.RenameDirectoryCase(
                CallerPath(L"p\\link"), CallerPath(L"p\\LINK"), EntryKind::Link);

            Assert::IsTrue(armed.foreign.made,

                L"The test makes the foreign directory at the new name");
            Assert::AreEqual<NTSTATUS>(STATUS_OBJECT_NAME_COLLISION, status,
                L"The rename fails when an entry it did not make holds the new name");
            AssertForeignDirectoryUntouched(armed.foreign, rig.config);
            Assert::IsTrue(EntriesUnder(env.Work()).empty(),
                L"The failed rename leaves nothing in the work directory");
        });
    }

    TEST_METHOD(CaseOnlyRename_LowerJunction_MovesTheLinkWithItsRecordToTheNewName) {
        UNIT_SKIP_IF_NOT_NTFS();
        ForEachMetadataStore([](UINT32 hostCapabilities) {
            TempLayerEnvironment env(1);
            if (!LowerJunctionCreatedOrSkipped(env, L"p")) {
                return;
            }
            LayerConfig config = env.MakeConfig();
            config.hostCapabilities = hostCapabilities;
            CopyUpAndRenameRig rig(config);
            const uint64_t copyUpsBefore = rig.stats.copyUpCount.load();

            Assert::AreEqual<NTSTATUS>(STATUS_SUCCESS, rig.copyUp.RenameDirectoryCase(
                CallerPath(L"p\\link"), CallerPath(L"p\\LINK"), EntryKind::Link),
                L"The case-only rename of the lower junction succeeds");

            const std::wstring upperLink = env.Upper() + L"\\p\\LINK";
            Assert::IsTrue(HasAttribute(upperLink, FILE_ATTRIBUTE_REPARSE_POINT),
                L"The upper entry is a link");
            const LayerMountMetadata md =
                MetadataStore::ReadLayerMountMetadata(upperLink, &rig.config);
            Assert::AreEqual(0,
                _wcsicmp((env.Lower(0) + L"\\p\\link").c_str(), md.originLayer.c_str()),
                L"The upper link's copy-up record names the lower junction");
            Assert::AreEqual<uint64_t>(copyUpsBefore + 1, rig.stats.copyUpCount.load(),
                L"Only the copy-up of the parent p counts, not the rename of the link");
            Assert::IsTrue(EntriesUnder(env.Work()).empty(),
                L"The rename leaves nothing in the work directory");
        });
    }
};

}
