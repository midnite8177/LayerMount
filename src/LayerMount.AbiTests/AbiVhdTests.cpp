#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

// VHD primitives. Create + Attach + Detach require elevation;
// each method early-returns when the test process isn't elevated. The
// error-path test (Open on a nonexistent .vhdx) runs without admin because
// it fails inside OpenVirtualDisk before any attach is attempted.
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

        LM_VHD_HANDLE vhd = nullptr;
        HRESULT hr = ::LayerMountVhdCreate(mount.Get(), &cfg, &vhd);
        Assert::AreEqual<HRESULT>(S_OK, hr, L"LayerMountVhdCreate");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountVhdClose(vhd));

        // Reopen what we just created.
        vhd = nullptr;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountVhdOpen(mount.Get(), &cfg, &vhd));
        Assert::IsNotNull(vhd);
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountVhdClose(vhd));
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

        LM_VHD_HANDLE vhd = nullptr;
        HRESULT hr = ::LayerMountVhdOpen(mount.Get(), &cfg, &vhd);
        Assert::AreNotEqual<HRESULT>(S_OK, hr,
            L"Opening a nonexistent VHDX must not succeed");
        Assert::IsNull(vhd, L"out handle must remain null on failure");
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
        LM_VHD_HANDLE vhd = nullptr;
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountVhdOpen(mount.Get(), &cfg, &vhd),
            L"Open records the path of an existing file without reading it as a VHD");

        BOOL removed = FALSE;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountVhdUnregisterLayer(mount.Get(), L"layer-a", env.Work().c_str(), &removed));
        Assert::IsTrue(removed != FALSE, L"The seeded layer is registered before the unregister");

        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountVhdClose(vhd));
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
