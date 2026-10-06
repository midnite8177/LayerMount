#include "pch.h"
#include "AbiTestFixture.h"
#include <virtdisk.h>
#pragma comment(lib, "virtdisk.lib")

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

namespace {

constexpr UINT64 kAutoSize = 0;

// Opens the VHD with detach access only and detaches it, outside the engine.
// A zeroed storage type lets OpenVirtualDisk read the format from the file.
DWORD DetachThroughSecondDiskHandle(const std::wstring& vhdPath) {
    VIRTUAL_STORAGE_TYPE storageType{};
    OPEN_VIRTUAL_DISK_PARAMETERS openParams{};
    openParams.Version          = OPEN_VIRTUAL_DISK_VERSION_1;
    openParams.Version1.RWDepth = OPEN_VIRTUAL_DISK_RW_DEPTH_DEFAULT;
    HANDLE disk = nullptr;
    const DWORD openResult = ::OpenVirtualDisk(&storageType, vhdPath.c_str(),
        VIRTUAL_DISK_ACCESS_DETACH, OPEN_VIRTUAL_DISK_FLAG_NONE, &openParams, &disk);
    if (openResult != ERROR_SUCCESS) {
        return openResult;
    }
    const DWORD detachResult = ::DetachVirtualDisk(disk, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
    ::CloseHandle(disk);
    return detachResult;
}

// Detaches the VHD at a path on destruction, so a Permanent attach does not
// outlive a failed test.
class DetachByPathOnExit {
public:
    DetachByPathOnExit(LM_HANDLE mount, const std::wstring& vhdPath)
        : mount_(mount), vhdPath_(vhdPath) {}

    ~DetachByPathOnExit() {
        const LM_VHD_CONFIG cfg = ProcessScopedVhdConfig(vhdPath_);
        VhdHandleHolder vhd;
        if (SUCCEEDED(::LayerMountVhdOpen(mount_, &cfg, vhd.AddressOf()))) {
            (void)::LayerMountVhdDetach(vhd.Get());
        }
    }

    DetachByPathOnExit(const DetachByPathOnExit&)            = delete;
    DetachByPathOnExit& operator=(const DetachByPathOnExit&) = delete;

private:
    LM_HANDLE    mount_;
    std::wstring vhdPath_;
};

}

TEST_CLASS(AbiVhdTests) {
public:
    TEST_METHOD(VhdCreate_Open_Close_RoundTrip) {
        ABI_SKIP_IF_NOT_ADMIN();
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        const std::wstring vhdPath = env.Root() + L"\\probe.vhdx";

        LM_VHD_CONFIG cfg{};
        cfg.structSize          = sizeof(cfg);
        cfg.kind                = LM_VHD_KIND_DYNAMIC;
        cfg.sizeBytes           = 32ull * 1024ull * 1024ull; // 32 MiB
        cfg.path                = vhdPath.c_str();
        cfg.readOnly            = FALSE;
        cfg.suppressDriveLetter = TRUE;
        cfg.lifetime            = LM_VHD_ATTACH_PROCESS_SCOPED;

        VhdHandleHolder vhd;
        HRESULT hr = ::LayerMountVhdCreate(mount.Get(), &cfg, vhd.AddressOf());
        Assert::AreEqual<HRESULT>(S_OK, hr, L"LayerMountVhdCreate");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountVhdClose(vhd.Release()));

        // Reopen what we just created.
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountVhdOpen(mount.Get(), &cfg, vhd.AddressOf()));
        Assert::IsNotNull(vhd.Get());
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountVhdClose(vhd.Release()));
    }

    TEST_METHOD(VhdDetach_WritableAttach_TakesTheVolumeOfflineAndLeavesTheHandleReattachable) {
        ABI_SKIP_IF_NOT_ADMIN();
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring source = env.Root() + L"\\source";
        WriteText(source + L"\\keep.txt", "keep");
        const std::wstring vhdPath = env.Root() + L"\\detach.vhdx";
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountVhdImport(mount.Get(), source.c_str(), vhdPath.c_str(), kAutoSize),
            L"LayerMountVhdImport makes the VHD");

        VhdHandleHolder vhd;
        const std::wstring volumeRoot =
            AttachVhdVolumeWritable(mount.Get(), vhdPath, vhd.AddressOf());
        WriteText(volumeRoot + L"dir\\written.txt", "written");
        Assert::AreEqual<std::string>("written", ReadAllBytes(volumeRoot + L"dir\\written.txt"),
            L"The attached volume takes a write");

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountVhdDetach(vhd.Get()),
            L"LayerMountVhdDetach ends a writable attach");
        Assert::AreEqual<DWORD>(INVALID_FILE_ATTRIBUTES, ::GetFileAttributesW(volumeRoot.c_str()),
            L"The volume root is gone after the detach");

        wchar_t physicalPath[MAX_PATH] = {};
        SIZE_T required = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountVhdAttach(vhd.Get(), physicalPath, MAX_PATH, &required),
            L"The same handle attaches the VHD again");
        const std::wstring reattachedRoot = AttachedVhdVolumeRoot(vhd.Get());
        Assert::AreEqual<std::string>("written", ReadAllBytes(reattachedRoot + L"dir\\written.txt"),
            L"The file written before the detach is on the volume after the second attach");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountVhdClose(vhd.Release()));
    }

    TEST_METHOD(VhdDetach_HandleWithNoAttach_TakesAPermanentAttachOffline) {
        ABI_SKIP_IF_NOT_ADMIN();
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring source = env.Root() + L"\\source";
        WriteText(source + L"\\keep.txt", "keep");
        const std::wstring vhdPath = env.Root() + L"\\permanent.vhdx";
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountVhdImport(mount.Get(), source.c_str(), vhdPath.c_str(), kAutoSize),
            L"LayerMountVhdImport makes the VHD");

        DetachByPathOnExit detachOnExit(mount.Get(), vhdPath);
        LM_VHD_CONFIG cfg = ProcessScopedVhdConfig(vhdPath);
        cfg.lifetime = LM_VHD_ATTACH_PERMANENT;
        VhdHandleHolder attached;
        const std::wstring volumeRoot =
            AttachVhdVolumeWithConfig(mount.Get(), cfg, attached.AddressOf());
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountVhdClose(attached.Release()));
        Assert::AreNotEqual<DWORD>(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(volumeRoot.c_str()),
            L"A Permanent attach outlives the VHD handle that made it");

        VhdHandleHolder unattached;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountVhdOpen(mount.Get(), &cfg, unattached.AddressOf()),
            L"LayerMountVhdOpen opens the attached VHD again");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountVhdDetach(unattached.Get()),
            L"LayerMountVhdDetach on a handle with no attach detaches by path");
        Assert::AreEqual<DWORD>(INVALID_FILE_ATTRIBUTES, ::GetFileAttributesW(volumeRoot.c_str()),
            L"The volume root is gone after the detach");
    }

    TEST_METHOD(VhdDetach_DiskDetachedThroughAnotherHandle_SucceedsAndTheHandleAttachesAgain) {
        ABI_SKIP_IF_NOT_ADMIN();
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring source = env.Root() + L"\\source";
        WriteText(source + L"\\keep.txt", "keep");
        const std::wstring vhdPath = env.Root() + L"\\elsewhere.vhdx";
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountVhdImport(mount.Get(), source.c_str(), vhdPath.c_str(), kAutoSize),
            L"LayerMountVhdImport makes the VHD");

        DetachByPathOnExit detachOnExit(mount.Get(), vhdPath);
        LM_VHD_CONFIG cfg = ProcessScopedVhdConfig(vhdPath);
        cfg.readOnly = TRUE;
        cfg.lifetime = LM_VHD_ATTACH_PERMANENT;
        VhdHandleHolder attached;
        const std::wstring volumeRoot =
            AttachVhdVolumeWithConfig(mount.Get(), cfg, attached.AddressOf());

        Assert::AreEqual<DWORD>(ERROR_SUCCESS, DetachThroughSecondDiskHandle(vhdPath),
            L"A second virtual disk handle detaches the Permanent attach");
        Assert::AreEqual<DWORD>(INVALID_FILE_ATTRIBUTES, ::GetFileAttributesW(volumeRoot.c_str()),
            L"The volume root is gone after the detach through the other handle");

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountVhdDetach(attached.Get()),
            L"LayerMountVhdDetach succeeds on a handle whose disk is already detached");
        wchar_t physicalPath[MAX_PATH] = {};
        SIZE_T required = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountVhdAttach(attached.Get(), physicalPath, MAX_PATH, &required),
            L"The handle attaches the VHD again");
        const std::wstring reattachedRoot = AttachedVhdVolumeRoot(attached.Get());
        Assert::AreNotEqual<DWORD>(INVALID_FILE_ATTRIBUTES,
            ::GetFileAttributesW(reattachedRoot.c_str()),
            L"The volume root exists after the second attach");
    }

    TEST_METHOD(ImportExport_LiveTransientOverlayUpper_LeavesOutTheSidecarStore) {
        ABI_SKIP_IF_NOT_ADMIN();
        TempLayerEnv env(0);
        const std::wstring upper = env.Root() + L"\\transient";
        LayerMountHolder overlay = CreateTransient(upper);
        WriteThroughOverlay(overlay.Get(), L"\\file.txt", "payload");

        const std::wstring vhdPath = env.Root() + L"\\live.vhdx";
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountVhdImport(overlay.Get(), upper.c_str(), vhdPath.c_str(), kAutoSize),
            L"An import of a live overlay's upper succeeds");

        const std::wstring dstDir = env.Root() + L"\\dst";
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountVhdExport(overlay.Get(), vhdPath.c_str(), dstDir.c_str()));
        Assert::IsTrue(SortedNamesIn(dstDir) == std::vector<std::wstring>{L"file.txt"},
            L"The exported tree holds file.txt and no .overlay");
        Assert::AreEqual<std::string>("payload", ReadAllBytes(dstDir + L"\\file.txt"));
    }

    TEST_METHOD(Export_VolumeRootHoldingASidecarStore_WritesOnlyTheOtherEntries) {
        ABI_SKIP_IF_NOT_ADMIN();
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring source = env.Root() + L"\\source";
        std::filesystem::create_directories(source);
        WriteText(source + L"\\keep.txt", "keep");
        const std::wstring vhdPath = env.Root() + L"\\sidecar.vhdx";
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountVhdImport(mount.Get(), source.c_str(), vhdPath.c_str(), kAutoSize),
            L"LayerMountVhdImport makes the VHD");

        VhdHandleHolder vhd;
        const std::wstring volumeRoot =
            AttachVhdVolumeWritable(mount.Get(), vhdPath, vhd.AddressOf());
        std::filesystem::create_directories(volumeRoot + L".OVERLAY");
        WriteText(volumeRoot + L".OVERLAY\\held.txt", "held");
        Assert::IsTrue(::CreateDirectoryW((volumeRoot + L".overlay.").c_str(), nullptr) != FALSE,
            L"The volume path takes a name with a trailing dot as written");
        WriteText(volumeRoot + L".overlay.\\dotted.txt", "dot");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountVhdClose(vhd.Release()),
            L"LayerMountVhdClose ends the attach");

        const std::wstring dstDir = env.Root() + L"\\dst";
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountVhdExport(mount.Get(), vhdPath.c_str(), dstDir.c_str()),
            L"LayerMountVhdExport");
        Assert::IsTrue(SortedNamesIn(dstDir) == std::vector<std::wstring>{L"keep.txt"},
            L"The export writes keep.txt and nothing from .OVERLAY or .overlay.");
        Assert::AreEqual<std::string>("keep", ReadAllBytes(dstDir + L"\\keep.txt"));
    }

    TEST_METHOD(Export_EntryUnderTheShortNameOfAnExistingSidecar_WritesNothingInTheSidecar) {
        ABI_SKIP_IF_NOT_ADMIN();
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring dstDir = env.Root() + L"\\dst";
        const std::wstring sidecar = dstDir + L"\\.overlay";
        std::filesystem::create_directories(sidecar);
        const std::optional<std::wstring> shortName = ShortNameOf(sidecar);
        if (!shortName) {
            Logger::WriteMessage(L"[SKIP] The volume gave .overlay no short name");
            return;
        }

        const std::wstring source = env.Root() + L"\\source";
        WriteText(source + L"\\keep.txt", "keep");
        const std::wstring vhdPath = env.Root() + L"\\short.vhdx";
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountVhdImport(mount.Get(), source.c_str(), vhdPath.c_str(), kAutoSize),
            L"LayerMountVhdImport makes the VHD");

        VhdHandleHolder vhd;
        const std::wstring volumeRoot =
            AttachVhdVolumeWritable(mount.Get(), vhdPath, vhd.AddressOf());
        WriteText(volumeRoot + *shortName + L"\\x.txt", "short");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountVhdClose(vhd.Release()),
            L"LayerMountVhdClose ends the attach");

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountVhdExport(mount.Get(), vhdPath.c_str(), dstDir.c_str()),
            L"LayerMountVhdExport");
        Assert::IsTrue(SortedNamesIn(sidecar).empty(),
            L"The export writes nothing in .overlay through its short name");
        Assert::AreEqual<std::string>("keep", ReadAllBytes(dstDir + L"\\keep.txt"));
    }

    TEST_METHOD(ImportExport_OverlayDirectoryBelowTheRoot_RoundTrips) {
        ABI_SKIP_IF_NOT_ADMIN();
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring source = env.Root() + L"\\source";
        std::filesystem::create_directories(source + L"\\sub\\.overlay");
        WriteText(source + L"\\sub\\.overlay\\data.txt", "nested");

        const std::wstring vhdPath = env.Root() + L"\\nested.vhdx";
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountVhdImport(mount.Get(), source.c_str(), vhdPath.c_str(), kAutoSize));
        const std::wstring dstDir = env.Root() + L"\\dst";
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountVhdExport(mount.Get(), vhdPath.c_str(), dstDir.c_str()));
        Assert::AreEqual<std::string>("nested",
            ReadAllBytes(dstDir + L"\\sub\\.overlay\\data.txt"),
            L"A .overlay below the root imports and exports as user data");
    }

    TEST_METHOD(VhdOpen_NonExistent_ReturnsFileNotFound) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        const std::wstring bogusPath = env.Root() + L"\\missing.vhdx";
        LM_VHD_CONFIG cfg{};
        cfg.structSize          = sizeof(cfg);
        cfg.kind                = LM_VHD_KIND_DYNAMIC;
        cfg.sizeBytes           = 16ull * 1024ull * 1024ull;
        cfg.path                = bogusPath.c_str();
        cfg.suppressDriveLetter = TRUE;
        cfg.lifetime            = LM_VHD_ATTACH_PROCESS_SCOPED;

        VhdHandleHolder vhd;
        HRESULT hr = ::LayerMountVhdOpen(mount.Get(), &cfg, vhd.AddressOf());
        Assert::AreNotEqual<HRESULT>(S_OK, hr,
            L"Opening a nonexistent VHDX must not succeed");
        Assert::IsNull(vhd.Get(), L"out handle must remain null on failure");
    }

    TEST_METHOD(VhdListLayers_NoManifest_ReturnsZeroEntries) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        UINT32 written = 0;
        UINT32 required = 0;
        // manifestDir with no manifest JSON present is defined to return
        // S_OK with *entriesRequired = 0 (idempotent no-op).
        HRESULT hr = ::LayerMountVhdListLayers(
            mount.Get(), env.Root().c_str(),
            nullptr, 0, &written, &required);
        Assert::AreEqual<HRESULT>(S_OK, hr);
        Assert::AreEqual<UINT32>(0u, required);
        Assert::AreEqual<UINT32>(0u, written);
    }

    TEST_METHOD(VhdUnregisterLayer_ThenDestroy_LayerStaysRemoved) {
        TempLayerEnv env(0);
        {
            std::ofstream registry(env.Work() + L"\\layers.manifest.json",
                                   std::ios::binary | std::ios::trunc);
            registry << R"({"schemaVersion":1,"layers":[{"id":"layer-a","type":"vhd","path":"C:\\absent\\a.vhdx"}]})";
        }
        const std::wstring notAVhd = env.Root() + L"\\not-a-vhd.bin";
        {
            std::ofstream file(notAVhd, std::ios::binary | std::ios::trunc);
            file << "plain bytes";
        }

        LayerMountHolder mount = CreateLayerMount(env);

        // Without a VHD handle the overlay never builds its VHD manager, and
        // the test would pass against a manager that writes the registry back.
        LM_VHD_CONFIG cfg{};
        cfg.structSize          = sizeof(cfg);
        cfg.kind                = LM_VHD_KIND_DYNAMIC;
        cfg.path                = notAVhd.c_str();
        cfg.suppressDriveLetter = TRUE;
        cfg.lifetime            = LM_VHD_ATTACH_PROCESS_SCOPED;
        VhdHandleHolder vhd;
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountVhdOpen(mount.Get(), &cfg, vhd.AddressOf()),
            L"Open records the path of an existing file without reading it as a VHD");

        BOOL removed = FALSE;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountVhdUnregisterLayer(mount.Get(), L"layer-a", env.Work().c_str(), &removed));
        Assert::IsTrue(removed != FALSE, L"The seeded layer is registered before the unregister");

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountVhdClose(vhd.Release()));
        mount.Reset();

        LayerMountHolder after = CreateLayerMount(env);
        UINT32 written = 0;
        UINT32 required = 0;
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountVhdListLayers(
            after.Get(), env.Work().c_str(), nullptr, 0, &written, &required));
        Assert::AreEqual<UINT32>(0u, required,
            L"Destroying the overlay must not bring back an unregistered layer");
    }
};

}
