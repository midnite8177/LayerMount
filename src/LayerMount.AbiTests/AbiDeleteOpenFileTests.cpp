#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

// Pin the host-adapter contract for LayerMountDeleteOpenFile: a single
// handle-based delete that internally chooses recursive-merge vs
// reparse-point-aware semantics based on the file's stored
// createOptions, so individual host adapters don't have to.
TEST_CLASS(AbiDeleteOpenFileTests) {
public:
    TEST_METHOD(File_Delete_RemovesFromUpper) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        std::ofstream(env.Upper() + L"\\gone.txt") << "to delete";

        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountOpenFile(mount.Get(), L"\\gone.txt",
                GENERIC_READ | DELETE, 0u, 0u, &fh, &info));

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountDeleteOpenFile(fh));

        // LayerMountCloseFile is still safe after delete; the engine
        // already closed the NT handle inside Delete.
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));

        // File should be gone.
        Assert::IsFalse(std::filesystem::exists(env.Upper() + L"\\gone.txt"),
            L"Upper-layer file must be removed after LayerMountDeleteOpenFile");
    }

    TEST_METHOD(LowerOnlyFile_Delete_CreatesWhiteout) {
        TempLayerEnv  env(1);
        env.WriteLowerFile(0, L"hidden.txt", "lower data");
        LayerMountHolder mount = CreateLayerMount(env);

        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountOpenFile(mount.Get(), L"\\hidden.txt",
                GENERIC_READ | DELETE, 0u, 0u, &fh, &info));

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountDeleteOpenFile(fh));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));

        // Subsequent open must report not-found (the lower file is
        // hidden by the whiteout the engine created in upper).
        LM_FILE_HANDLE fh2 = nullptr;
        LM_FILE_INFO   info2{};
        const HRESULT hrReopen = ::LayerMountOpenFile(mount.Get(),
            L"\\hidden.txt", GENERIC_READ, 0u, 0u, &fh2, &info2);
        Assert::IsTrue(IsFileNotFoundHr(hrReopen),
            L"After delete, reopen must surface as FileNotFound");
    }

    TEST_METHOD(EmptyDirectory_Delete_RemovesFromUpper) {
        TempLayerEnv  env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        std::filesystem::create_directory(env.Upper() + L"\\emptydir");
        LM_FILE_HANDLE dh = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountOpenFile(mount.Get(), L"\\emptydir",
                GENERIC_READ | DELETE, FILE_DIRECTORY_FILE, 0u, &dh, &info));

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountDeleteOpenFile(dh));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(dh));

        Assert::IsFalse(std::filesystem::exists(env.Upper() + L"\\emptydir"),
            L"Empty directory must be removed after LayerMountDeleteOpenFile");
    }

    TEST_METHOD(NullHandle_ReturnsEHandle) {
        Assert::AreEqual<HRESULT>(E_HANDLE,
            ::LayerMountDeleteOpenFile(nullptr));
    }
};

} // namespace LayerMountAbiTests
