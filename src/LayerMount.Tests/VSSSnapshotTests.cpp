#include "pch.h"
#include "TestFixture.h"

#include "LayerMount.h"
#include "VSSManager.h"

#include <algorithm>
#include <combaseapi.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountTests {

namespace {

// RAII COM initializer for tests. VSSManager requires an initialized COM
// apartment; MSTest doesn't run tests inside CoInitializeEx by default.
class ComInit {
public:
    ComInit() {
        hr_ = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
    ~ComInit() {
        // Call CoUninitialize only when this object owns the init call
        // (S_OK) or observed an already-MTA apartment (S_FALSE, which
        // still increments COM's init reference count). Explicitly do
        // NOT uninit on RPC_E_CHANGED_MODE -- that return code means
        // COM was already initialized with an incompatible apartment
        // and THIS call did not take ownership; unbalancing it would
        // break a later test on the same thread.
        if (SUCCEEDED(hr_) || hr_ == S_FALSE) {
            ::CoUninitialize();
        }
    }
    bool Initialized() const {
        // S_OK = newly initialized; S_FALSE = already initialized at this
        // apartment type. RPC_E_CHANGED_MODE means we must not uninit.
        return SUCCEEDED(hr_) || hr_ == S_FALSE;
    }

    ComInit(const ComInit&)            = delete;
    ComInit& operator=(const ComInit&) = delete;

private:
    HRESULT hr_ = E_FAIL;
};

// VSS can legitimately fail for environmental reasons: provider
// unavailable, writer blocked, no shadow-space on disk. Test cases
// want to SKIP on those and FAIL on everything else. Centralize the
// classification so adding a new environmental code is a single-site
// edit instead of a scatter of `if (rc != 0) return;` blocks.
inline bool IsEnvironmentalVssError(DWORD rc) {
    switch (rc) {
        case ERROR_ACCESS_DENIED:
        case ERROR_NOT_FOUND:
        case ERROR_INVALID_FUNCTION:
        case ERROR_SERVICE_NOT_ACTIVE:
        case ERROR_SERVICE_REQUEST_TIMEOUT:
        case ERROR_DISK_FULL:
        case static_cast<DWORD>(0x8004230F): // VSS_E_UNEXPECTED_PROVIDER_ERROR
        case static_cast<DWORD>(0x80042301): // VSS_E_BAD_STATE
        case static_cast<DWORD>(0x80042302): // VSS_E_PROVIDER_ALREADY_REGISTERED
        case static_cast<DWORD>(0x80042305): // VSS_E_PROVIDER_NOT_REGISTERED
        case static_cast<DWORD>(0x80042306): // VSS_E_PROVIDER_VETO
        case static_cast<DWORD>(0x8004230C): // VSS_E_INSUFFICIENT_STORAGE
        case static_cast<DWORD>(0x8004230D): // VSS_E_NO_SNAPSHOTS_IMPORTED
        case static_cast<DWORD>(0x80042313): // VSS_E_UNEXPECTED_WRITER_ERROR
        case static_cast<DWORD>(0x8004231F): // VSS_E_WRITER_ERROR_NONRETRYABLE
        case static_cast<DWORD>(0x80042322): // VSS_E_WRITER_STATUS_NOT_AVAILABLE
            return true;
        default:
            return false;
    }
}

#define VSS_SKIP_OR_FAIL_ON_FAILURE(rc, what)                                   \
    do {                                                                       \
        DWORD _rc = (rc);                                                      \
        if (_rc != ERROR_SUCCESS) {                                            \
            if (IsEnvironmentalVssError(_rc)) {                                \
                std::wstring _msg = L"[SKIP] " what L" failed with code ";     \
                _msg += std::to_wstring(_rc) + L" (environmental)";            \
                Logger::WriteMessage(_msg.c_str());                            \
                return;                                                        \
            }                                                                  \
            std::wstring _msg = what L" failed with code ";                    \
            _msg += std::to_wstring(_rc);                                      \
            Assert::Fail(_msg.c_str());                                        \
        }                                                                      \
    } while (0)

// Derive the volume path of the current %TEMP% so tests can snapshot the
// volume hosting their own scratch space (typically C:\).
std::wstring TempVolumePath() {
    wchar_t tempBuf[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, tempBuf);
    std::wstring temp(tempBuf);
    if (temp.size() >= 3 && temp[1] == L':' && temp[2] == L'\\') {
        return temp.substr(0, 3);
    }
    return L"C:\\";
}

bool ListContainsId(const LayerMount::VSS::VSSManager& mgr,
                    const std::wstring& id) {
    std::vector<LayerMount::VSS::SnapshotInfo> list;
    if (mgr.ListSnapshots(list) != ERROR_SUCCESS) {
        return false;
    }
    return std::any_of(list.begin(), list.end(),
        [&](const LayerMount::VSS::SnapshotInfo& s) { return s.id == id; });
}

// Non-persistent snapshot deletion is driven by IVssBackupComponents::Release
// — the kernel reaps the shadow asynchronously. Poll system-wide until the id
// is no longer visible, or until the bounded deadline elapses.
bool WaitForIdAbsent(const LayerMount::VSS::VSSManager& mgr,
                     const std::wstring& id, DWORD timeoutMs = 30000) {
    const DWORD step = 500;
    for (DWORD elapsed = 0; elapsed <= timeoutMs; elapsed += step) {
        if (!ListContainsId(mgr, id)) return true;
        ::Sleep(step);
    }
    return false;
}

const LayerMount::VSS::SnapshotInfo* FindInList(
    const std::vector<LayerMount::VSS::SnapshotInfo>& list,
    const std::wstring& id) {
    for (const auto& s : list) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// VSSSnapshotTests — VSS snapshot lifecycle. All tests require admin
// elevation; COM must be initialized per-test.
// ---------------------------------------------------------------------------

TEST_CLASS(VSSSnapshotTests) {
public:
    TEST_METHOD(CheckElevation_ReturnsErrorSuccessWhenAdmin) {
        UNIT_SKIP_IF_NOT_ADMIN();
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
                                LayerMount::VSS::VSSManager::CheckElevation());
    }

    TEST_METHOD(CreateNonPersistentSnapshot_ReturnsDevicePath) {
        UNIT_SKIP_IF_NOT_ADMIN();
        ComInit com;
        if (!com.Initialized()) {
            Logger::WriteMessage(L"[SKIP] COM initialization failed");
            return;
        }

        LayerMount::VSS::VSSManager mgr;
        std::wstring snapId;
        std::wstring devicePath;
        const DWORD rc = mgr.CreateSnapshot(TempVolumePath(),
                                             /*persistent*/ false,
                                             snapId, devicePath);

        // Gate failures: known-environmental codes log + skip; anything
        // else is a real implementation bug and should fail the suite.
        VSS_SKIP_OR_FAIL_ON_FAILURE(rc, L"VSS CreateSnapshot");

        Assert::IsFalse(snapId.empty(),    L"Snapshot ID must be returned");
        Assert::IsFalse(devicePath.empty(), L"Device path must be returned");

        // Snapshot is registered and queryable.
        const auto info = mgr.GetSnapshot(snapId);
        Assert::IsTrue(info.has_value());
        Assert::AreEqual(info->id, snapId);
        Assert::AreEqual(info->devicePath, devicePath);
        Assert::IsFalse(info->persistent);

        // Cleanup happens via CleanupNonPersistent so we don't leak
        // non-persistent snapshots past test end.
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, mgr.CleanupNonPersistent());
    }

    TEST_METHOD(ListSnapshots_ReflectsCreateAndDelete) {
        UNIT_SKIP_IF_NOT_ADMIN();
        ComInit com;
        if (!com.Initialized()) {
            Logger::WriteMessage(L"[SKIP] COM initialization failed");
            return;
        }

        LayerMount::VSS::VSSManager mgr;

        std::wstring id, device;
        const DWORD createRc = mgr.CreateSnapshot(TempVolumePath(), false, id, device);
        VSS_SKIP_OR_FAIL_ON_FAILURE(createRc, L"VSS CreateSnapshot");

        Assert::IsTrue(ListContainsId(mgr, id),
                       L"System-wide list must include our newly-created snapshot");

        Assert::AreEqual<DWORD>(ERROR_SUCCESS, mgr.DeleteSnapshot(id));
        Assert::IsTrue(WaitForIdAbsent(mgr, id),
                        L"System-wide list must not include deleted snapshot (after bounded wait)");
    }

    TEST_METHOD(CleanupNonPersistent_RemovesSessionSnapshots) {
        UNIT_SKIP_IF_NOT_ADMIN();
        ComInit com;
        if (!com.Initialized()) {
            Logger::WriteMessage(L"[SKIP] COM initialization failed");
            return;
        }

        LayerMount::VSS::VSSManager mgr;
        std::wstring id1, dev1;
        std::wstring id2, dev2;
        const DWORD rc1 = mgr.CreateSnapshot(TempVolumePath(), false, id1, dev1);
        VSS_SKIP_OR_FAIL_ON_FAILURE(rc1, L"VSS CreateSnapshot");
        const DWORD rc2 = mgr.CreateSnapshot(TempVolumePath(), false, id2, dev2);
        if (rc2 != ERROR_SUCCESS) {
            // Cleanup the first snapshot before bailing so we don't leak.
            mgr.CleanupNonPersistent();
            VSS_SKIP_OR_FAIL_ON_FAILURE(rc2, L"Second VSS CreateSnapshot");
        }

        Assert::IsTrue(ListContainsId(mgr, id1));
        Assert::IsTrue(ListContainsId(mgr, id2));

        Assert::AreEqual<DWORD>(ERROR_SUCCESS, mgr.CleanupNonPersistent());
        Assert::IsTrue(WaitForIdAbsent(mgr, id1),
                        L"CleanupNonPersistent must release session snapshot id1");
        Assert::IsTrue(WaitForIdAbsent(mgr, id2),
                        L"CleanupNonPersistent must release session snapshot id2");
    }

    // --- system-wide enumeration / untracked delete -------------------

    TEST_METHOD(ListSnapshots_SeesSnapshotCreatedByOtherManager) {
        // A second VSSManager inside the same process simulates a separate
        // creator from the perspective of the first — the first has an empty
        // in-process map but the system-wide query must still see the snapshot.
        UNIT_SKIP_IF_NOT_ADMIN();
        ComInit com;
        if (!com.Initialized()) {
            Logger::WriteMessage(L"[SKIP] COM initialization failed");
            return;
        }

        LayerMount::VSS::VSSManager creator;
        std::wstring id, device;
        const DWORD createRc = creator.CreateSnapshot(TempVolumePath(), false,
                                                       id, device);
        VSS_SKIP_OR_FAIL_ON_FAILURE(createRc, L"VSS CreateSnapshot");

        LayerMount::VSS::VSSManager observer;
        Assert::IsTrue(ListContainsId(observer, id),
                       L"ListSnapshots must enumerate system-wide, not just in-process");

        creator.CleanupNonPersistent();
    }

    TEST_METHOD(ListSnapshots_PopulatesPersistenceAndTimestamp) {
        UNIT_SKIP_IF_NOT_ADMIN();
        ComInit com;
        if (!com.Initialized()) {
            Logger::WriteMessage(L"[SKIP] COM initialization failed");
            return;
        }

        LayerMount::VSS::VSSManager mgr;
        std::wstring id, device;
        const DWORD rc = mgr.CreateSnapshot(TempVolumePath(), /*persistent*/ false,
                                             id, device);
        VSS_SKIP_OR_FAIL_ON_FAILURE(rc, L"VSS CreateSnapshot");

        std::vector<LayerMount::VSS::SnapshotInfo> list;
        VSS_SKIP_OR_FAIL_ON_FAILURE(mgr.ListSnapshots(list),
                                    L"VSS ListSnapshots");
        const auto* entry = FindInList(list, id);
        Assert::IsNotNull(entry, L"Snapshot must appear in system-wide list");
        Assert::IsFalse(entry->createdAt.empty(),
                        L"createdAt must be populated from VSS_SNAPSHOT_PROP");
        Assert::IsTrue(entry->createdAt.find(L"T") != std::wstring::npos,
                       L"createdAt must be ISO 8601 formatted");

        mgr.CleanupNonPersistent();
    }

    TEST_METHOD(DeleteSnapshot_AcceptsUntrackedId) {
        // The creating manager goes out of scope; a fresh manager must still
        // be able to delete by id (via VSS_CTX_ALL + DeleteSnapshots).
        UNIT_SKIP_IF_NOT_ADMIN();
        ComInit com;
        if (!com.Initialized()) {
            Logger::WriteMessage(L"[SKIP] COM initialization failed");
            return;
        }

        std::wstring id, device;
        {
            LayerMount::VSS::VSSManager creator;
            const DWORD rc = creator.CreateSnapshot(TempVolumePath(), /*persistent*/ true,
                                                     id, device);
            VSS_SKIP_OR_FAIL_ON_FAILURE(rc, L"persistent VSS CreateSnapshot");
            // Persistent snapshot: releasing `creator` leaves it on the system.
        }

        LayerMount::VSS::VSSManager deleter;
        Assert::IsTrue(ListContainsId(deleter, id),
                       L"Persistent snapshot must survive creator destruction");

        Assert::AreEqual<DWORD>(ERROR_SUCCESS, deleter.DeleteSnapshot(id),
                                L"DeleteSnapshot must accept ids not in the in-process map");
        Assert::IsTrue(WaitForIdAbsent(deleter, id),
                        L"System must no longer list the deleted snapshot");
    }
};

} // namespace LayerMountTests
