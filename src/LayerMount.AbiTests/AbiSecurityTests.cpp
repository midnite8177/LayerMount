#include "pch.h"
#include "AbiTestFixture.h"

#include <aclapi.h>
#include <sddl.h>

#pragma comment(lib, "advapi32.lib")

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

TEST_CLASS(AbiSecurityTests) {
public:
    TEST_METHOD(GetSecurity_NullProbe_ReturnsSuccessWithRequiredSize) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        CreatePlainFile(mount, L"\\probe.txt");

        SIZE_T required = 0;
        HRESULT hr = ::LayerMountGetSecurity(
            mount.Get(), L"\\probe.txt", nullptr, nullptr, 0, &required);
        Assert::AreEqual<HRESULT>(S_OK, hr,
            L"A null-buffer probe must report the size, not fail");
        Assert::IsTrue(required > 0,
            L"An NTFS descriptor is never zero bytes");
    }

    TEST_METHOD(GetSecurity_ShortBuffer_ReturnsMoreDataAndRequiredSize) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        CreatePlainFile(mount, L"\\short.txt");

        SIZE_T required = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountGetSecurity(mount.Get(), L"\\short.txt",
                nullptr, nullptr, 0, &required));

        std::vector<BYTE> tooSmall(required > 1 ? required - 1 : 0);
        SIZE_T secondRequired = 0;
        HRESULT hr = ::LayerMountGetSecurity(
            mount.Get(), L"\\short.txt", nullptr,
            tooSmall.data(), tooSmall.size(), &secondRequired);
        Assert::AreEqual<HRESULT>(HRESULT_FROM_WIN32(ERROR_MORE_DATA), hr,
            L"A too-small buffer must report ERROR_MORE_DATA");
        Assert::AreEqual<SIZE_T>(required, secondRequired,
            L"The required size must still be written on the short-buffer path");
    }

    TEST_METHOD(GetSecurity_ExactBuffer_ReturnsValidDescriptor) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        CreatePlainFile(mount, L"\\exact.txt");

        SIZE_T required = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountGetSecurity(mount.Get(), L"\\exact.txt",
                nullptr, nullptr, 0, &required));
        Assert::IsTrue(required > 0);

        std::vector<BYTE> sd(required);
        SIZE_T actual = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountGetSecurity(mount.Get(), L"\\exact.txt",
                nullptr, sd.data(), sd.size(), &actual));
        Assert::IsTrue(
            ::IsValidSecurityDescriptor(reinterpret_cast<PSECURITY_DESCRIPTOR>(sd.data())) != FALSE,
            L"A filled buffer must hold a well-formed security descriptor");
    }

private:
    static void CreatePlainFile(LayerMountHolder& mount, PCWSTR path) {
        LM_FILE_HANDLE fh = nullptr;
        LM_FILE_INFO   info{};
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountCreateFile(mount.Get(), path, 0u,
                GENERIC_READ | GENERIC_WRITE,
                FILE_ATTRIBUTE_NORMAL, nullptr, 0u, 0u, 0u, &fh, &info),
            L"setup: LayerMountCreateFile");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(fh));
    }
};

} // namespace LayerMountAbiTests
