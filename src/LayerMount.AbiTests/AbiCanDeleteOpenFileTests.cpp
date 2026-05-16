#include "pch.h"
#include "AbiTestFixture.h"

#pragma warning(push)
#pragma warning(disable: 4005)
#include <ntstatus.h>
#pragma warning(pop)

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

// Pin the contract that host adapters rely on: the handle-based
// variants surface the right HRESULT for directory-emptiness and
// reparse-point semantics so the adapter doesn't have to re-derive them.
TEST_CLASS(AbiCanDeleteOpenFileTests) {
public:
    static LM_FILE_HANDLE OpenDir(LM_HANDLE mount, const wchar_t* path) {
        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        const HRESULT hr = ::LayerMountOpenFile(mount, path,
            /*grantedAccess*/ GENERIC_READ | DELETE,
            /*createOptions*/ FILE_DIRECTORY_FILE,
            /*originatorPid*/ 0u, &fh, &info);
        Assert::AreEqual<HRESULT>(S_OK, hr,
            L"LayerMountOpenFile on directory should succeed");
        return fh;
    }

    TEST_METHOD(EmptyDirectory_CanDelete_ReturnsSOk) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        std::filesystem::create_directory(env.Upper() + L"\\empty");
        LM_FILE_HANDLE dh = OpenDir(mount.Get(), L"\\empty");

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountCanDeleteOpenFile(dh),
            L"Empty directory must be deletable");

        ::LayerMountCloseFile(dh);
    }

    TEST_METHOD(NonEmptyDirectory_CanDelete_ReturnsDirectoryNotEmpty) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        std::filesystem::create_directory(env.Upper() + L"\\full");
        std::ofstream(env.Upper() + L"\\full\\child.txt") << "x";

        LM_FILE_HANDLE dh = OpenDir(mount.Get(), L"\\full");

        const HRESULT hr = ::LayerMountCanDeleteOpenFile(dh);
        Assert::AreEqual<HRESULT>(
            HRESULT_FROM_NT(STATUS_DIRECTORY_NOT_EMPTY), hr,
            L"Non-empty directory must report STATUS_DIRECTORY_NOT_EMPTY "
            L"(via HRESULT_FROM_NT) so hosts don't need to count children");

        ::LayerMountCloseFile(dh);
    }

    TEST_METHOD(NonEmptyDirectory_FromMergedLower_ReturnsDirectoryNotEmpty) {
        // A directory is empty in upper but the merged view sees lower
        // children. CanDelete must still report not-empty.
        TempLayerEnv  env(1);
        env.WriteLowerFile(0, L"merged\\hidden.txt", "lower content");
        LayerMountHolder mount = CreateLayerMount(env);

        LM_FILE_HANDLE dh = OpenDir(mount.Get(), L"\\merged");

        const HRESULT hr = ::LayerMountCanDeleteOpenFile(dh);
        Assert::AreEqual<HRESULT>(
            HRESULT_FROM_NT(STATUS_DIRECTORY_NOT_EMPTY), hr,
            L"Lower-layer children must count toward emptiness check");

        ::LayerMountCloseFile(dh);
    }

    TEST_METHOD(RegularFile_CanDelete_ReturnsSOk) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        std::ofstream(env.Upper() + L"\\file.txt") << "data";
        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountOpenFile(mount.Get(), L"\\file.txt",
                GENERIC_READ | DELETE, 0u, 0u, &fh, &info));

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountCanDeleteOpenFile(fh));

        ::LayerMountCloseFile(fh);
    }

    TEST_METHOD(NullHandle_ReturnsEHandle) {
        Assert::AreEqual<HRESULT>(E_HANDLE,
            ::LayerMountCanDeleteOpenFile(nullptr));
    }
};

} // namespace LayerMountAbiTests
