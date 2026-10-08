#include "pch.h"
#include "AbiTestFixture.h"

#include <array>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using LayerMountTestShared::DeepLeafName;
using LayerMountTestShared::ExtendedFormOf;
using LayerMountTestShared::ExtendedPathUnder;
using LayerMountTestShared::MakeDeepLayerRoot;

namespace LayerMountAbiTests {

namespace {

constexpr UINT64 kAutoSize = 0;

void Import(LM_HANDLE mount, const std::wstring& directoryPath, const std::wstring& vhdPath) {
    const HRESULT hr =
        ::LayerMountVhdImport(mount, directoryPath.c_str(), vhdPath.c_str(), kAutoSize);
    Assert::AreEqual<HRESULT>(S_OK, hr,
        (L"LayerMountVhdImport succeeds: " + LastErrorMessage(hr)).c_str());
}

void Export(LM_HANDLE mount, const std::wstring& vhdPath, const std::wstring& directoryPath) {
    const HRESULT hr = ::LayerMountVhdExport(mount, vhdPath.c_str(), directoryPath.c_str());
    Assert::AreEqual<HRESULT>(S_OK, hr,
        (L"LayerMountVhdExport succeeds: " + LastErrorMessage(hr)).c_str());
}

struct ListedLayer {
    std::wstring id;
    std::wstring path;
};

constexpr SIZE_T kLayerInfoStringChars = 1024;

// The string buffers that one LM_VHD_LAYER_INFO points at.
struct LayerInfoBuffers {
    std::array<wchar_t, kLayerInfoStringChars> path{};
    std::array<wchar_t, kLayerInfoStringChars> parentId{};
    std::array<wchar_t, kLayerInfoStringChars> mountStatus{};
    std::array<wchar_t, kLayerInfoStringChars> volumeGuid{};
    std::array<wchar_t, kLayerInfoStringChars> createdAt{};
};

std::vector<ListedLayer> ListLayers(LM_HANDLE mount, const std::wstring& manifestDir) {
    UINT32 written = 0;
    UINT32 required = 0;
    Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountVhdListLayers(mount, manifestDir.c_str(), nullptr, 0, &written, &required),
        L"The sizing call of LayerMountVhdListLayers succeeds");
    if (required == 0) return {};

    std::vector<LayerInfoBuffers> buffers(required);
    std::vector<LM_VHD_LAYER_INFO> entries(required);
    for (size_t i = 0; i < entries.size(); ++i) {
        LM_VHD_LAYER_INFO& entry = entries[i];
        LayerInfoBuffers& entryBuffers = buffers[i];
        entry.path = entryBuffers.path.data();
        entry.pathChars = entryBuffers.path.size();
        entry.parentId = entryBuffers.parentId.data();
        entry.parentIdChars = entryBuffers.parentId.size();
        entry.mountStatus = entryBuffers.mountStatus.data();
        entry.mountStatusChars = entryBuffers.mountStatus.size();
        entry.volumeGuid = entryBuffers.volumeGuid.data();
        entry.volumeGuidChars = entryBuffers.volumeGuid.size();
        entry.createdAt = entryBuffers.createdAt.data();
        entry.createdAtChars = entryBuffers.createdAt.size();
    }
    Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountVhdListLayers(mount, manifestDir.c_str(), entries.data(), required,
                                  &written, &required),
        L"LayerMountVhdListLayers fills the entries");
    std::vector<ListedLayer> layers;
    for (UINT32 i = 0; i < written; ++i) {
        layers.push_back({entries[i].id, entries[i].path});
    }
    return layers;
}

void CreateDifferencingChild(LM_HANDLE mount, const std::wstring& childPath,
                             const std::wstring& parentPath) {
    LM_VHD_CONFIG cfg = ProcessScopedVhdConfig(childPath);
    cfg.kind       = LM_VHD_KIND_DIFFERENCING;
    cfg.parentPath = parentPath.c_str();
    LM_VHD_HANDLE created = nullptr;
    const HRESULT hr = ::LayerMountVhdCreate(mount, &cfg, &created);
    Assert::AreEqual<HRESULT>(S_OK, hr,
        (L"LayerMountVhdCreate makes the differencing child: " + LastErrorMessage(hr)).c_str());
    Assert::AreEqual<HRESULT>(S_OK, ::LayerMountVhdClose(created));
}

enum class ParentForm { Plain, Extended };

void AssertChildOfDeepParentHoldsTheParentFile(ParentForm parentForm) {
    TempLayerEnv env(0);
    LayerMountHolder mount = CreateLayerMount(env);
    const std::wstring source = env.Root() + L"\\src";
    WriteText(source + L"\\inherited.txt", "inherited");
    const std::wstring parentPath =
        MakeDeepLayerRoot(env.Root(), L"DeepVhds") + L"\\parent.vhdx";
    Import(mount.Get(), source, parentPath);
    const std::wstring childPath = env.Root() + L"\\child.vhdx";

    CreateDifferencingChild(mount.Get(), childPath,
        parentForm == ParentForm::Extended ? ExtendedFormOf(parentPath) : parentPath);

    VhdHandleHolder vhd;
    const std::wstring volumeRoot =
        AttachVhdVolumeReadOnly(mount.Get(), childPath, vhd.AddressOf());
    Assert::AreEqual<std::string>("inherited", ReadAllBytes(volumeRoot + L"inherited.txt"),
        L"The child volume holds the file of the parent at the deep path");
    Assert::AreEqual<HRESULT>(S_OK, ::LayerMountVhdClose(vhd.Release()));
}

LayerMountHolder CreateWithWorkDirectory(const TempLayerEnv& env, const std::wstring& workDirectory) {
    ConfigBuilder builder(env);
    LM_CONFIG config = *builder.Ptr();
    config.workDirPath = workDirectory.c_str();
    LM_HANDLE handle = nullptr;
    const HRESULT hr = ::LayerMountCreate(&config, &handle);
    Assert::AreEqual<HRESULT>(S_OK, hr,
        (L"LayerMountCreate starts the overlay with a deep work directory: " +
         LastErrorMessage(hr)).c_str());
    return LayerMountHolder(handle);
}

}

TEST_CLASS(AbiDeepVhdTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        LayerMountTestShared::AssertHostRefusesPlainPathsPastMaxPath();
    }

    TEST_METHOD(ImportExport_TreeUnderDeepRoot_ExportsTheSameContentIntoADeepTarget) {
        ABI_SKIP_IF_NOT_ADMIN();
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring source = MakeDeepLayerRoot(env.Root(), L"DeepSource");
        WriteText(ExtendedPathUnder(source, L"top.txt"), "top");
        WriteText(ExtendedPathUnder(source, L"sub\\inner\\nested.bin"), "\x01\x02\x03");
        const std::wstring vhdPath =
            MakeDeepLayerRoot(env.Root(), L"DeepVhds") + L"\\tree.vhdx";

        Import(mount.Get(), source, vhdPath);
        const std::wstring target = MakeDeepLayerRoot(env.Root(), L"DeepTarget");
        Export(mount.Get(), vhdPath, target);

        Assert::IsTrue(SortedNamesIn(ExtendedFormOf(target)) ==
                           std::vector<std::wstring>{L"sub", L"top.txt"},
            L"The exported root holds sub and top.txt");
        Assert::AreEqual<std::string>("top", ReadAllBytes(ExtendedPathUnder(target, L"top.txt")),
            L"The export writes the top-level file in the deep target");
        Assert::AreEqual<std::string>("\x01\x02\x03",
            ReadAllBytes(ExtendedPathUnder(target, L"sub\\inner\\nested.bin")),
            L"The export writes the nested file in the deep target");
    }

    TEST_METHOD(ImportExport_ShallowRootsWithAnEntryPastMaxPath_RoundTripsTheEntry) {
        ABI_SKIP_IF_NOT_ADMIN();
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring source = env.Root() + L"\\src";
        const std::wstring target = env.Root() + L"\\dst";
        const std::wstring deepName = DeepLeafName(source, L"DeepDir");
        WriteText(ExtendedFormOf(source + L"\\shallow.txt"), "shallow");
        WriteText(ExtendedPathUnder(source, deepName + L"\\deep.txt"), "deep");
        const std::wstring vhdPath = env.Root() + L"\\shallow.vhdx";

        Import(mount.Get(), source, vhdPath);
        Export(mount.Get(), vhdPath, target);

        Assert::AreEqual<std::string>("shallow", ReadAllBytes(target + L"\\shallow.txt"),
            L"The export writes the shallow file");
        Assert::AreEqual<std::string>("deep",
            ReadAllBytes(ExtendedPathUnder(target, deepName + L"\\deep.txt")),
            L"The export writes the file whose path passes MAX_PATH under the shallow target");
    }

    TEST_METHOD(ImportExport_WorkDirectoryAtDeepRoot_MountsTheVolumeUnderItAndRoundTripsTheTree) {
        ABI_SKIP_IF_NOT_ADMIN();
        TempLayerEnv env(0);
        const std::wstring workDirectory = MakeDeepLayerRoot(env.Root(), L"DeepWork");
        LayerMountHolder mount = CreateWithWorkDirectory(env, workDirectory);
        const std::wstring source = env.Root() + L"\\src";
        const std::wstring target = env.Root() + L"\\dst";
        WriteText(source + L"\\top.txt", "top");
        WriteText(source + L"\\sub\\nested.txt", "nested");
        const std::wstring vhdPath = env.Root() + L"\\tree.vhdx";

        Import(mount.Get(), source, vhdPath);
        Export(mount.Get(), vhdPath, target);

        const std::wstring tempMounts = ExtendedPathUnder(workDirectory, L"temp_mounts");
        Assert::IsTrue(std::filesystem::is_directory(tempMounts),
            L"The import and the export mount the volume under the deep work directory");
        Assert::IsTrue(SortedNamesIn(tempMounts).empty(),
            L"The import and the export remove their mount directories");
        Assert::IsTrue(SortedNamesIn(target) == std::vector<std::wstring>{L"sub", L"top.txt"},
            L"The exported root holds sub and top.txt");
        Assert::AreEqual<std::string>("top", ReadAllBytes(target + L"\\top.txt"),
            L"The export writes the top-level file");
        Assert::AreEqual<std::string>("nested", ReadAllBytes(target + L"\\sub\\nested.txt"),
            L"The export writes the nested file");
    }

    TEST_METHOD(VhdOpen_FileAtDeepPath_AttachesAndGivesItsContent) {
        ABI_SKIP_IF_NOT_ADMIN();
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring source = env.Root() + L"\\src";
        WriteText(source + L"\\only.txt", "only");
        const std::wstring vhdPath =
            MakeDeepLayerRoot(env.Root(), L"DeepVhds") + L"\\one.vhdx";
        Import(mount.Get(), source, vhdPath);

        VhdHandleHolder vhd;
        const std::wstring volumeRoot =
            AttachVhdVolumeReadOnly(mount.Get(), vhdPath, vhd.AddressOf());

        Assert::AreEqual<std::string>("only", ReadAllBytes(volumeRoot + L"only.txt"),
            L"The volume of the VHD at the deep path holds the imported file");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountVhdClose(vhd.Release()));
    }

    TEST_METHOD(VhdCreateDifferencing_PlainParentAtDeepPath_ChildAttachesWithTheParentContent) {
        ABI_SKIP_IF_NOT_ADMIN();
        AssertChildOfDeepParentHoldsTheParentFile(ParentForm::Plain);
    }

    TEST_METHOD(VhdCreateDifferencing_ExtendedParentAtDeepPath_ChildAttachesWithTheParentContent) {
        ABI_SKIP_IF_NOT_ADMIN();
        AssertChildOfDeepParentHoldsTheParentFile(ParentForm::Extended);
    }

    TEST_METHOD(LayerRegistry_InDeepManifestDir_UnregisterSavesAndListReadsTheRest) {
        TempLayerEnv env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring manifestDir = MakeDeepLayerRoot(env.Root(), L"DeepRegistry");
        WriteText(ExtendedPathUnder(manifestDir, L"layers.manifest.json"),
            R"({"schemaVersion":1,"layers":[)"
            R"({"id":"layer-a","type":"vhd","path":"C:\\layers\\a.vhdx"},)"
            R"({"id":"layer-b","type":"vhd","path":"C:\\layers\\kept.vhdx"}]})");

        Assert::AreEqual<size_t>(2, ListLayers(mount.Get(), manifestDir).size(),
            L"The registry in the deep directory lists the two seeded layers");
        BOOL removed = FALSE;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountVhdUnregisterLayer(mount.Get(), L"layer-a", manifestDir.c_str(), &removed),
            L"LayerMountVhdUnregisterLayer saves the registry in the deep directory");
        Assert::IsTrue(removed != FALSE, L"The unregister removes layer-a");

        const std::vector<ListedLayer> left = ListLayers(mount.Get(), manifestDir);
        Assert::AreEqual<size_t>(1, left.size(), L"The saved registry holds one layer");
        Assert::AreEqual(std::wstring(L"layer-b"), left[0].id,
            L"The saved registry holds layer-b");
        Assert::AreEqual(std::wstring(L"C:\\layers\\kept.vhdx"), left[0].path,
            L"The saved registry keeps the path of layer-b");
    }
};

}
