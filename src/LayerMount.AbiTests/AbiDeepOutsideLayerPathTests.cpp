#include "pch.h"
#include "AbiTestFixture.h"
#include "DeepPathAbiHelpers.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using LayerMountTestShared::DeepLeafName;
using LayerMountTestShared::ExtendedFormOf;
using LayerMountTestShared::MakeDeepLayerRoot;

namespace LayerMountAbiTests {

namespace {

std::wstring WriteDenyReadRulesInDeepDirectory(const TempLayerEnv& env) {
    const std::wstring rulesPath =
        MakeDeepLayerRoot(env.Root(), L"DeepRules") + L"\\rules.json";
    WriteText(ExtendedFormOf(rulesPath),
        R"({"rules":[{"processName":"*","pathPattern":"*","allowRead":false}]})");
    Assert::IsTrue(HostEntryExists(ExtendedFormOf(rulesPath)),
        L"Precondition: the rules file exists at the deep path");
    return rulesPath;
}

std::wstring PathUnderMissingDeepParent(const TempLayerEnv& env,
                                        const std::wstring& prefix,
                                        const std::wstring& leaf) {
    return env.Root() + L"\\" + DeepLeafName(env.Root(), prefix) + L"\\" + leaf;
}

void ClaimOwnershipAsHostAdapter(LM_MOUNT_POINT_PREP& prep) {
    prep.directoryCreatedByUs = TRUE;
}

void AssertReadOpenIsDenied(LM_HANDLE mount) {
    OpenedFile opened;
    Assert::AreEqual<HRESULT>(HRESULT_FROM_NT(STATUS_ACCESS_DENIED),
        OpenOverlayFile(mount, L"\\tracked.txt", GENERIC_READ, kNoCreateOptions, opened),
        L"The rule from the deep rules file denies the open");
    Assert::IsNull(opened.handle, L"The denied open returns no handle");
}

}

TEST_CLASS(AbiDeepOutsideLayerPathTests) {
public:
    TEST_CLASS_INITIALIZE(ClassInit) {
        LayerMountTestShared::AssertHostRefusesPlainPathsPastMaxPath();
    }

    TEST_METHOD(ProcessTrackerSetRules_RulesFileInDeepDirectory_LoadsTheRules) {
        TempLayerEnv env(0);
        env.WriteUpperFile(L"tracked.txt", "tracked");
        LayerMountHolder mount = CreateLayerMount(env);
        constexpr BOOL enable = TRUE;
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountProcessTrackerEnable(mount.Get(), enable));
        const std::wstring rulesPath = WriteDenyReadRulesInDeepDirectory(env);

        const HRESULT hr = ::LayerMountProcessTrackerSetRules(mount.Get(), rulesPath.c_str());

        Assert::AreEqual<HRESULT>(S_OK, hr,
            (L"The rules file at the deep path loads: " + LastErrorMessage(hr)).c_str());
        AssertReadOpenIsDenied(mount.Get());
    }

    TEST_METHOD(Create_ProcessRulesPathInDeepDirectory_LoadsTheRules) {
        TempLayerEnv env(0);
        env.WriteUpperFile(L"tracked.txt", "tracked");
        ConfigBuilder config(env);
        config.EnableProcessTrackingWithRules(WriteDenyReadRulesInDeepDirectory(env));
        LayerMountHolder mount;

        const HRESULT hr = ::LayerMountCreate(config.Ptr(), mount.AddressOf());

        Assert::AreEqual<HRESULT>(S_OK, hr,
            (L"The create loads the rules file at the deep path: " + LastErrorMessage(hr)).c_str());
        AssertReadOpenIsDenied(mount.Get());
    }

    TEST_METHOD(MountPoint_UnderMissingDeepParent_IsPreparedAndReleased) {
        TempLayerEnv env(0);
        const std::wstring mountPoint =
            PathUnderMissingDeepParent(env, L"DeepMountParent", L"mnt");

        LM_MOUNT_POINT_PREP prep{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountPointPrepareDirectory(mountPoint.c_str(), &prep),
            L"The prepare accepts a mount point under a missing deep parent");
        Assert::IsTrue(::CreateDirectoryW(ExtendedFormOf(mountPoint).c_str(), nullptr) != FALSE,
            L"The prepare made the deep parent, so the mount point directory can be made in it");
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountPointCaptureIdentity(mountPoint.c_str(), &prep));
        Assert::AreNotEqual<UINT64>(0, prep.volumeSerial,
            L"The capture reads the identity of the deep mount point directory");
        ClaimOwnershipAsHostAdapter(prep);

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountPointReleaseIfSafe(mountPoint.c_str(), &prep));

        Assert::IsFalse(HostEntryExists(ExtendedFormOf(mountPoint)),
            L"The release removes the deep mount point directory");
    }

    TEST_METHOD(MountPoint_ExistingDeepDirectory_IsRefusedAsACollision) {
        TempLayerEnv env(0);
        const std::wstring mountPoint = MakeDeepLayerRoot(env.Root(), L"DeepExistingMount");

        LM_MOUNT_POINT_PREP prep{};
        const HRESULT hr = ::LayerMountPointPrepareDirectory(mountPoint.c_str(), &prep);

        Assert::AreEqual<HRESULT>(HRESULT_FROM_NT(STATUS_OBJECT_NAME_COLLISION), hr,
            L"The prepare refuses a deep mount point directory that already exists");
    }

    TEST_METHOD(CreateTransient_MissingDeepWorkDirectory_MakesItAndStarts) {
        TempLayerEnv env(0);
        const std::wstring workDir =
            PathUnderMissingDeepParent(env, L"DeepTransient", L"work");
        LayerMountHolder mount;

        const HRESULT hr =
            ::LayerMountCreateTransient(workDir.c_str(), LM_CAP_NONE, mount.AddressOf());

        Assert::AreEqual<HRESULT>(S_OK, hr,
            (L"The transient overlay starts at the deep work directory: " +
             LastErrorMessage(hr)).c_str());
        WriteThroughOverlay(mount.Get(), L"\\written.txt", "transient");
        Assert::AreEqual<std::string>("transient",
            ReadAllBytes(ExtendedFormOf(workDir + L"\\written.txt")),
            L"The write through the transient overlay lands in the deep work directory");
    }
};

}
