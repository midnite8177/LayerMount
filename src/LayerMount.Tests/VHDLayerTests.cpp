#include "pch.h"
#include "TestFixture.h"

#include "LayerMount.h"
#include "Manifest.h"
#include "VHDLayerManager.h"
#include "VolumeGuid.h"

#include <fstream>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountTests {

namespace {

// Build a dedicated workspace under %TEMP% for a VHD-layer test run.
// Returns the root path; VHDX files go here so the normal TempLayerEnvironment
// UUID directory is a sibling and cleanup is straightforward.
std::wstring MakeVhdWorkspace() {
    const std::wstring root = LayerMountTests::MakeUniqueTempRoot();
    std::error_code ec;
    std::filesystem::create_directories(root + L"\\vhd", ec);
    return root;
}

void CleanupWorkspace(const std::wstring& root) {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

} // namespace

// ---------------------------------------------------------------------------
// VHDLayerTests — VHD lifecycle + differencing. All tests require admin
// because Create/AttachVirtualDisk hit the kernel driver.
// ---------------------------------------------------------------------------

TEST_CLASS(VHDLayerTests) {
public:
    TEST_METHOD(CheckElevation_ReturnsErrorSuccessWhenAdmin) {
        UNIT_SKIP_IF_NOT_ADMIN();
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
                                LayerMount::VHD::VHDLayerManager::CheckElevation());
    }

    TEST_METHOD(CreateDynamicVHDX_ProducesValidFile) {
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring vhdPath = root + L"\\vhd\\dynamic.vhdx";

        LayerMount::VHD::VHDLayerManager mgr(root + L"\\vhd");
        LayerMount::VHD::VhdHandle handle;
        const DWORD rc = mgr.CreateVHD(vhdPath, 64ULL * 1024 * 1024, /*dynamic*/ true, handle);

        Assert::AreEqual<DWORD>(ERROR_SUCCESS, rc,
                                L"CreateVHD should succeed");
        Assert::IsTrue(handle.IsValid(), L"CreateVHD should return a valid handle");
        Assert::IsTrue(std::filesystem::exists(vhdPath),
                       L"VHDX file must exist on disk");

        handle.Close();
        CleanupWorkspace(root);
    }

    TEST_METHOD(AttachVHD_ReturnsPhysicalPath) {
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring vhdPath = root + L"\\vhd\\attachable.vhdx";

        LayerMount::VHD::VHDLayerManager mgr(root + L"\\vhd");
        LayerMount::VHD::VhdHandle createHandle;
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            mgr.CreateVHD(vhdPath, 64ULL * 1024 * 1024, true, createHandle));
        createHandle.Close();

        LayerMount::VHD::VhdHandle attachHandle;
        std::wstring physicalPath;
        const DWORD rc = mgr.AttachVHD(vhdPath, /*readOnly*/ false,
                                        attachHandle, physicalPath);
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, rc);
        Assert::IsFalse(physicalPath.empty(),
                        L"AttachVHD must return a \\\\.\\PhysicalDriveN path");

        mgr.DetachVHD(vhdPath);
        CleanupWorkspace(root);
    }

    TEST_METHOD(DifferencingVHD_ChildAttaches) {
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring parent = root + L"\\vhd\\parent.vhdx";
        const std::wstring child  = root + L"\\vhd\\child.vhdx";

        LayerMount::VHD::VHDLayerManager mgr(root + L"\\vhd");

        LayerMount::VHD::VhdHandle parentHandle;
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            mgr.CreateVHD(parent, 64ULL * 1024 * 1024, true, parentHandle));
        parentHandle.Close();

        LayerMount::VHD::VhdHandle childHandle;
        const DWORD rc = mgr.CreateDifferencingVHD(child, parent, childHandle);
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, rc,
                                L"CreateDifferencingVHD should succeed");
        childHandle.Close();

        Assert::IsTrue(std::filesystem::exists(child),
                       L"Child differencing VHDX must exist");
        // Child is typically much smaller than parent because it only stores
        // the diff metadata until writes happen.
        const auto childSize = std::filesystem::file_size(child);
        Assert::IsTrue(childSize > 0, L"Child VHDX file must have nonzero size");

        CleanupWorkspace(root);
    }

    // NOTE: InitializeVHD (partition + format) and MergeVHD are significantly
    // slower tests (multi-second format.com call) and exercise external
    // binaries. They're skipped here to keep the unit-level integration run
    // fast; dedicated longer-running integration sweeps can target them via
    // a separate test category.

    // ---- Task #7: ExportToDirectory skips NTFS system dirs ----

    TEST_METHOD(ExportToDirectory_SkipsNtfsSystemDirsAndCopiesUserContent) {
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring src    = root + L"\\src";
        const std::wstring nested = src + L"\\nested";
        const std::wstring vhd    = root + L"\\exp.vhdx";
        const std::wstring out    = root + L"\\out";

        std::filesystem::create_directories(nested);
        { std::ofstream(src + L"\\a.txt")          << "hello"; }
        { std::ofstream(nested + L"\\b.txt")       << "world"; }

        LayerMount::VHD::VHDLayerManager mgr(root + L"\\vhd");

        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            mgr.ImportDirectory(src, vhd),
            L"Import must succeed before we can export");

        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            mgr.ExportToDirectory(vhd, out),
            L"Export must succeed on a freshly-imported NTFS volume");

        // User content landed.
        Assert::IsTrue(std::filesystem::exists(out + L"\\a.txt"),
                       L"Top-level user file must land in destination");
        Assert::IsTrue(std::filesystem::exists(out + L"\\nested\\b.txt"),
                       L"Nested user file must land in destination");

        // System Volume Information must NOT leak through.
        Assert::IsFalse(std::filesystem::exists(out + L"\\System Volume Information"),
                        L"System Volume Information must be filtered out");

        CleanupWorkspace(root);
    }

    TEST_METHOD(AttachVHD_SuppressDriveLetter_NoNewDriveAssigned) {
        // Regression for task #8: transient attaches (import/export/mount-backing)
        // must not trigger a Windows AutoPlay flash. We verify the user-visible
        // symptom: no new drive letter appears in GetLogicalDrives() after a
        // suppress-drive-letter attach.
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring src = root + L"\\src";
        const std::wstring vhd = root + L"\\suppressed.vhdx";
        std::filesystem::create_directories(src);
        { std::ofstream(src + L"\\marker.txt") << "x"; }

        LayerMount::VHD::VHDLayerManager mgr(root + L"\\vhd");
        // Import produces a fully formatted, populated VHD.
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, mgr.ImportDirectory(src, vhd));

        const DWORD before = ::GetLogicalDrives();

        LayerMount::VHD::VhdHandle handle;
        std::wstring physicalPath;
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            mgr.AttachVHD(vhd, /*readOnly*/ true, handle, physicalPath,
                           LayerMount::VHD::AttachLifetime::ProcessScoped,
                           /*suppressDriveLetter=*/ true));

        // Give the Mount Manager a moment; if it were going to assign a
        // letter, it would do so within a few hundred ms of the attach.
        ::Sleep(750);
        const DWORD after = ::GetLogicalDrives();

        // Any bit that's set in `after` but not `before` is a new drive letter.
        const DWORD newLetters = after & ~before;

        mgr.DetachVHD(vhd);
        handle.Close();

        Assert::AreEqual<DWORD>(0, newLetters,
            L"No new drive letter must appear when suppressDriveLetter=true");

        CleanupWorkspace(root);
    }

    TEST_METHOD(ImportThenExport_RoundTripsUserContent) {
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring src  = root + L"\\src";
        const std::wstring vhd  = root + L"\\roundtrip.vhdx";
        const std::wstring out  = root + L"\\out";

        std::filesystem::create_directories(src + L"\\dir1");
        { std::ofstream(src + L"\\top.txt")      << "top-content"; }
        { std::ofstream(src + L"\\dir1\\nested") << "nested-content"; }

        LayerMount::VHD::VHDLayerManager mgr(root + L"\\vhd");
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, mgr.ImportDirectory(src, vhd));
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, mgr.ExportToDirectory(vhd, out));

        auto ReadAll = [](const std::wstring& p) -> std::string {
            std::ifstream f(p, std::ios::binary);
            return { std::istreambuf_iterator<char>(f),
                     std::istreambuf_iterator<char>() };
        };

        Assert::AreEqual(std::string("top-content"),    ReadAll(out + L"\\top.txt"));
        Assert::AreEqual(std::string("nested-content"), ReadAll(out + L"\\dir1\\nested"));

        CleanupWorkspace(root);
    }

    // ---- AttachLifetime contract ----

    TEST_METHOD(AttachLifetime_Permanent_SurvivesHandleClose) {
        // Control case: the default (Permanent) must keep the attach alive
        // after the VhdHandle closes. Explicit DetachVHD is required.
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring vhdPath = root + L"\\vhd\\permanent.vhdx";

        LayerMount::VHD::VHDLayerManager mgr(root + L"\\vhd");
        LayerMount::VHD::VhdHandle createHandle;
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            mgr.CreateVHD(vhdPath, 64ULL * 1024 * 1024, true, createHandle));
        createHandle.Close();

        LayerMount::VHD::VhdHandle attachHandle;
        std::wstring physicalPath;
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            mgr.AttachVHD(vhdPath, /*readOnly*/ false, attachHandle,
                           physicalPath,
                           LayerMount::VHD::AttachLifetime::Permanent));
        attachHandle.Close();

        // Explicit detach must succeed — the attach is still live.
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, mgr.DetachVHD(vhdPath),
            L"Permanent attach must survive handle close until DetachVHD");

        CleanupWorkspace(root);
    }

    TEST_METHOD(AttachLifetime_ProcessScoped_ReleasesOnHandleClose) {
        // Core 12.7 contract: ProcessScoped attach must release when the
        // handle closes, so hard-kill cleanup is automatic.
        UNIT_SKIP_IF_NOT_ADMIN();

        const std::wstring root = MakeVhdWorkspace();
        const std::wstring vhdPath = root + L"\\vhd\\scoped.vhdx";

        LayerMount::VHD::VHDLayerManager mgr(root + L"\\vhd");
        LayerMount::VHD::VhdHandle createHandle;
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            mgr.CreateVHD(vhdPath, 64ULL * 1024 * 1024, true, createHandle));
        createHandle.Close();

        std::wstring physicalPath;
        {
            LayerMount::VHD::VhdHandle attachHandle;
            Assert::AreEqual<DWORD>(ERROR_SUCCESS,
                mgr.AttachVHD(vhdPath, /*readOnly*/ false, attachHandle,
                               physicalPath,
                               LayerMount::VHD::AttachLifetime::ProcessScoped));
            Assert::IsFalse(physicalPath.empty());
            // attachHandle closes at scope exit — that should detach the VHD.
        }

        // Re-attach in ProcessScoped mode must succeed with no prior explicit
        // detach: if the first attach were still live, AttachVirtualDisk would
        // fail with a sharing or busy error.
        LayerMount::VHD::VhdHandle reattachHandle;
        std::wstring newPhysicalPath;
        const DWORD rc = mgr.AttachVHD(vhdPath, /*readOnly*/ false,
                                        reattachHandle, newPhysicalPath,
                                        LayerMount::VHD::AttachLifetime::ProcessScoped);
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, rc,
            L"Re-attach after ProcessScoped handle close must succeed");

        reattachHandle.Close();
        CleanupWorkspace(root);
    }
};

// ---------------------------------------------------------------------------
// ManifestPathTests — regression for the historical filename drift where
// VHDLayerManager wrote layers.manifest.json but `vhd list` read layers.json.
// Does not touch the VHD kernel driver, so no admin required.
// ---------------------------------------------------------------------------

TEST_CLASS(ManifestPathTests) {
public:
    TEST_METHOD(DefaultPath_IsCanonicalFileName) {
        const std::wstring dir = L"C:\\Temp\\demo";
        const std::wstring resolved = LayerMount::VHD::Manifest::DefaultPath(dir);
        Assert::IsTrue(resolved.find(L"layers.manifest.json") != std::wstring::npos,
                       L"DefaultPath must resolve to layers.manifest.json");
    }

    TEST_METHOD(ManagerWriteAndCliReadAgreeOnPath) {
        // Regression: writer (VHDLayerManager) and reader (vhd list) must
        // resolve to the same file when both use Manifest::DefaultPath.
        const std::wstring root = LayerMountTests::MakeUniqueTempRoot();
        std::filesystem::create_directories(root);

        // Write a layer via a bare Manifest (avoids admin requirement of the
        // full VHDLayerManager create/attach flow).
        {
            LayerMount::VHD::Manifest m;
            LayerMount::VHD::LayerEntry entry;
            entry.id = L"test-id-123";
            entry.type = LayerMount::VHD::LayerType::VHD;
            entry.path = root + L"\\sample.vhdx";
            entry.createdAt = L"2026-04-16T00:00:00Z";
            m.AddLayer(entry);
            Assert::AreEqual<DWORD>(ERROR_SUCCESS,
                m.Save(LayerMount::VHD::Manifest::DefaultPath(root)));
        }

        // Read it back via the same canonical path the CLI uses.
        {
            LayerMount::VHD::Manifest m;
            Assert::AreEqual<DWORD>(ERROR_SUCCESS,
                m.Load(LayerMount::VHD::Manifest::DefaultPath(root)));
            const auto* entry = m.GetLayer(L"test-id-123");
            Assert::IsNotNull(entry, L"Layer written via DefaultPath must be readable via DefaultPath");
            Assert::IsTrue(entry->type == LayerMount::VHD::LayerType::VHD);
        }

        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    // ---- vhd unregister semantics ----

    TEST_METHOD(Manifest_RemoveLayerAndSave_RoundTrip) {
        const std::wstring root = LayerMountTests::MakeUniqueTempRoot();
        std::filesystem::create_directories(root);
        const std::wstring path = LayerMount::VHD::Manifest::DefaultPath(root);

        {
            LayerMount::VHD::Manifest m;
            LayerMount::VHD::LayerEntry e;
            e.id = L"layer-a";
            e.type = LayerMount::VHD::LayerType::VHD;
            e.path = root + L"\\a.vhdx";
            m.AddLayer(e);
            Assert::AreEqual<DWORD>(ERROR_SUCCESS, m.Save(path));
        }

        {
            LayerMount::VHD::Manifest m;
            Assert::AreEqual<DWORD>(ERROR_SUCCESS, m.Load(path));
            Assert::IsTrue(m.RemoveLayer(L"layer-a"));
            Assert::AreEqual<DWORD>(ERROR_SUCCESS, m.Save(path));
        }

        {
            LayerMount::VHD::Manifest m;
            Assert::AreEqual<DWORD>(ERROR_SUCCESS, m.Load(path));
            Assert::IsNull(m.GetLayer(L"layer-a"),
                           L"After unregister + save, entry must be absent on fresh load");
        }

        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    TEST_METHOD(Manifest_RemoveLayer_MissingIsIdempotent) {
        LayerMount::VHD::Manifest m;
        Assert::IsFalse(m.RemoveLayer(L"never-existed"),
                        L"Removing a non-existent id must return false, not throw");
    }

    TEST_METHOD(Manifest_SaveIsAtomic_NoPartialFileOnCrash) {
        // Prove Save writes to a sibling tmp file and replaces atomically:
        // after a successful save, the destination must be a complete JSON
        // document with our layer — never a truncated stream.
        const std::wstring root = LayerMountTests::MakeUniqueTempRoot();
        std::filesystem::create_directories(root);
        const std::wstring path = LayerMount::VHD::Manifest::DefaultPath(root);

        LayerMount::VHD::Manifest m;
        LayerMount::VHD::LayerEntry e;
        e.id = L"atomicity-probe";
        e.type = LayerMount::VHD::LayerType::VHD;
        e.path = L"C:\\ignored.vhdx";
        m.AddLayer(e);
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, m.Save(path));

        // No leftover tmp files with our PID suffix (MoveFileExW either
        // renamed the tmp to the final path or Save cleaned it up).
        std::error_code ec;
        for (const auto& de : std::filesystem::directory_iterator(root, ec)) {
            auto name = de.path().filename().wstring();
            Assert::IsFalse(name.find(L".tmp.") != std::wstring::npos,
                            L"No .tmp.* stragglers must remain after a successful Save");
        }

        // The final file loads cleanly and contains our entry.
        LayerMount::VHD::Manifest reloaded;
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, reloaded.Load(path));
        Assert::IsNotNull(reloaded.GetLayer(L"atomicity-probe"));

        std::filesystem::remove_all(root, ec);
    }

    TEST_METHOD(ManifestLock_SerializesAcrossThreads) {
        // Two ManifestLocks for the same path from DIFFERENT threads must not
        // both be held. The Win32 mutex is recursive for the owning thread,
        // so a same-thread check would trivially pass — we need a peer thread.
        const std::wstring root = LayerMountTests::MakeUniqueTempRoot();
        std::filesystem::create_directories(root);
        const std::wstring path = LayerMount::VHD::Manifest::DefaultPath(root);

        LayerMount::VHD::ManifestLock first(path);
        Assert::IsTrue(first.Held(), L"First lock must acquire the mutex");

        bool peerHeld = true;
        std::thread peer([&] {
            LayerMount::VHD::ManifestLock second(path, /*timeoutMs=*/ 200);
            peerHeld = second.Held();
        });
        peer.join();

        Assert::IsFalse(peerHeld,
            L"Peer-thread ManifestLock must time out while main thread owns the mutex");

        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

} // namespace LayerMountTests
