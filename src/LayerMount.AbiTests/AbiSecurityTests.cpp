#include "pch.h"
#include "AbiTestFixture.h"

#include <aclapi.h>
#include <sddl.h>

#pragma comment(lib, "advapi32.lib")

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

namespace {
constexpr UINT32 kFullSecInfo =
    OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
    DACL_SECURITY_INFORMATION | SACL_SECURITY_INFORMATION;
}

TEST_CLASS(AbiSecurityTests) {
public:
    TEST_METHOD(GetSecurity_NullProbe_ReturnsSuccessWithRequiredSize) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        CreatePlainFile(mount, L"\\probe.txt");

        SIZE_T required = 0;
        constexpr SIZE_T sizingProbeBytes = 0;
        HRESULT hr = ::LayerMountGetSecurity(
            mount.Get(), L"\\probe.txt", kFullSecInfo, nullptr, nullptr,
            sizingProbeBytes, &required);
        Assert::AreEqual<HRESULT>(S_OK, hr,
            L"A null-buffer probe must report the size, not fail");
        Assert::IsTrue(required > 0,
            L"An NTFS descriptor is never zero bytes");
    }

    TEST_METHOD(GetSecurity_ShortBuffer_ReturnsMoreDataAndRequiredSize) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        CreatePlainFile(mount, L"\\short.txt");

        SIZE_T            required = ProbeSecuritySize(mount, L"\\short.txt", kFullSecInfo);
        std::vector<BYTE> tooSmall(required > 1 ? required - 1 : 0);
        SIZE_T secondRequired = 0;
        HRESULT hr = ::LayerMountGetSecurity(
            mount.Get(), L"\\short.txt", kFullSecInfo, nullptr,
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

        std::vector<BYTE> sd = FetchSecurity(mount, L"\\exact.txt", kFullSecInfo);
        Assert::IsTrue(
            ::IsValidSecurityDescriptor(reinterpret_cast<PSECURITY_DESCRIPTOR>(sd.data())) != FALSE,
            L"A filled buffer must hold a well-formed security descriptor");
    }

    TEST_METHOD(GetSecurity_DaclOnly_OmitsOwnerAndGroup) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        CreatePlainFile(mount, L"\\dacl.txt");

        std::vector<BYTE> sd = FetchSecurity(mount, L"\\dacl.txt", DACL_SECURITY_INFORMATION);

        auto* rawSd = reinterpret_cast<PSECURITY_DESCRIPTOR>(sd.data());
        PSID   owner = nullptr;
        BOOL   ownerDefaulted = FALSE;
        Assert::IsTrue(::GetSecurityDescriptorOwner(rawSd, &owner, &ownerDefaulted) != FALSE);
        Assert::IsNull(owner, L"A DACL-only request must carry no owner");

        PSID  group = nullptr;
        BOOL  groupDefaulted = FALSE;
        Assert::IsTrue(::GetSecurityDescriptorGroup(rawSd, &group, &groupDefaulted) != FALSE);
        Assert::IsNull(group, L"A DACL-only request must carry no group");

        PACL daclPtr = nullptr;
        BOOL daclPresent = FALSE, daclDefaulted = FALSE;
        Assert::IsTrue(
            ::GetSecurityDescriptorDacl(rawSd, &daclPresent, &daclPtr, &daclDefaulted) != FALSE);
        Assert::IsTrue(daclPresent != FALSE, L"A DACL-only request must still carry the DACL");
    }

    TEST_METHOD(GetSecurity_DaclOnlyOnNonAclCapableMount_OmitsOwnerAndGroup) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env,
            LM_CAP_ADS | LM_CAP_REPARSE_POINTS | LM_CAP_SPARSE_FILES | LM_CAP_MULTIPLE_STREAMS);
        CreatePlainFile(mount, L"\\nonacl.txt");

        std::vector<BYTE> sd = FetchSecurity(mount, L"\\nonacl.txt", DACL_SECURITY_INFORMATION);

        auto* rawSd = reinterpret_cast<PSECURITY_DESCRIPTOR>(sd.data());
        PSID   owner = nullptr;
        BOOL   ownerDefaulted = FALSE;
        Assert::IsTrue(::GetSecurityDescriptorOwner(rawSd, &owner, &ownerDefaulted) != FALSE);
        Assert::IsNull(owner,
            L"A DACL-only request on a non-ACL-capable mount must still omit owner");

        PSID  group = nullptr;
        BOOL  groupDefaulted = FALSE;
        Assert::IsTrue(::GetSecurityDescriptorGroup(rawSd, &group, &groupDefaulted) != FALSE);
        Assert::IsNull(group,
            L"A DACL-only request on a non-ACL-capable mount must still omit group");

        PACL daclPtr = nullptr;
        BOOL daclPresent = FALSE, daclDefaulted = FALSE;
        Assert::IsTrue(
            ::GetSecurityDescriptorDacl(rawSd, &daclPresent, &daclPtr, &daclDefaulted) != FALSE);
        Assert::IsTrue(daclPresent != FALSE,
            L"A DACL-only request on a non-ACL-capable mount must still carry the synthetic DACL");
    }

    TEST_METHOD(GetSecurity_ZeroRequest_ReturnsEmptyDescriptor) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        CreatePlainFile(mount, L"\\zero.txt");

        constexpr UINT32 noSecurityInformation = 0u;
        constexpr SIZE_T sizingProbeBytes      = 0;
        SIZE_T required = 0xDEADBEEF;
        HRESULT hr = ::LayerMountGetSecurity(
            mount.Get(), L"\\zero.txt", noSecurityInformation, nullptr, nullptr,
            sizingProbeBytes, &required);
        Assert::AreEqual<HRESULT>(S_OK, hr,
            L"A zero request is not an error");
        Assert::AreEqual<SIZE_T>(0, required,
            L"A zero request needs no descriptor bytes");
    }

    TEST_METHOD(GetSecurity_OversizedBuffer_ReportsTheWrittenSize) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        CreatePlainFile(mount, L"\\oversized.txt");

        constexpr SIZE_T kSlackBytes = 4096;

        SIZE_T            required = ProbeSecuritySize(mount, L"\\oversized.txt", kFullSecInfo);
        std::vector<BYTE> roomy(required + kSlackBytes);
        SIZE_T written = 0;
        HRESULT hr = ::LayerMountGetSecurity(
            mount.Get(), L"\\oversized.txt", kFullSecInfo, nullptr,
            roomy.data(), roomy.size(), &written);
        Assert::AreEqual<HRESULT>(S_OK, hr,
            L"The call must accept a buffer larger than the descriptor");
        Assert::AreEqual<SIZE_T>(required, written,
            L"The call must report the bytes written, not the buffer capacity");
    }

    TEST_METHOD(GetSecurity_DescriptorThatShrankBetweenProbeAndFill_ReportsOnlyTheCurrentBytes) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        CreatePlainFile(mount, L"\\shrink.txt");
        const std::wstring upperPath = env.Upper() + L"\\shrink.txt";

        // The ACE count drives the size of a DACL-only descriptor.
        constexpr PCWSTR kWideDacl =
            L"D:(A;;FR;;;S-1-1-0)(A;;FR;;;S-1-5-11)(A;;FR;;;S-1-5-4)"
            L"(A;;FR;;;S-1-5-6)(A;;FR;;;S-1-5-2)";
        constexpr PCWSTR kNarrowDacl = L"D:(A;;FR;;;S-1-1-0)";

        // Both writes go to the upper layer behind the mount, so the
        // second call sees them only because the engine caches path
        // resolution and attributes and never the descriptor bytes.
        // Descriptor caching would break this test and not the trim.
        SetDaclFromSddl(upperPath, kWideDacl);

        SIZE_T probed = ProbeSecuritySize(mount, L"\\shrink.txt", DACL_SECURITY_INFORMATION);

        SetDaclFromSddl(upperPath, kNarrowDacl);

        std::vector<BYTE> sd(probed);
        SIZE_T written = 0;
        HRESULT hr = ::LayerMountGetSecurity(
            mount.Get(), L"\\shrink.txt", DACL_SECURITY_INFORMATION, nullptr,
            sd.data(), sd.size(), &written);
        Assert::AreEqual<HRESULT>(S_OK, hr,
            L"A descriptor that shrank still fits the buffer the probe sized");
        Assert::IsTrue(written < probed,
            L"The fill call must report the bytes written, not the probed size");
        Assert::IsTrue(
            ::IsValidSecurityDescriptor(reinterpret_cast<PSECURITY_DESCRIPTOR>(sd.data())) != FALSE,
            L"The written prefix must hold a well-formed security descriptor");
    }

private:
    static void CreatePlainFile(LayerMountHolder& mount, PCWSTR path) {
        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            CreateOverlayFile(mount.Get(), path,
                GENERIC_READ | GENERIC_WRITE, kNoCreateOptions,
                FILE_ATTRIBUTE_NORMAL, opened),
            L"setup: LayerMountCreateFile");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));
    }

    static void SetDaclFromSddl(const std::wstring& path, PCWSTR sddl) {
        PSECURITY_DESCRIPTOR sd = nullptr;
        ULONG                size = 0;
        Assert::IsTrue(
            ::ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl, SDDL_REVISION_1, &sd, &size) != FALSE,
            L"setup: ConvertStringSecurityDescriptorToSecurityDescriptorW");
        BOOL ok = ::SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, sd);
        ::LocalFree(sd);
        Assert::IsTrue(ok != FALSE, L"setup: SetFileSecurityW");
    }

    static SIZE_T ProbeSecuritySize(
        LayerMountHolder& mount, PCWSTR path, UINT32 secInfo) {
        constexpr SIZE_T sizingProbeBytes = 0;
        SIZE_T required = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountGetSecurity(mount.Get(), path, secInfo, nullptr, nullptr,
                                    sizingProbeBytes, &required));
        Assert::IsTrue(required > 0);
        return required;
    }

    // Probes for the required size, allocates exactly that, and fills it.
    // Shared by every test that wants a real descriptor rather than one
    // probing a specific error path.
    static std::vector<BYTE> FetchSecurity(
        LayerMountHolder& mount, PCWSTR path, UINT32 secInfo) {
        SIZE_T            required = ProbeSecuritySize(mount, path, secInfo);
        std::vector<BYTE> sd(required);
        SIZE_T actual = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountGetSecurity(mount.Get(), path, secInfo, nullptr, sd.data(), sd.size(), &actual));
        return sd;
    }
};

}
