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

// A buffer whose last byte sits just before a PAGE_GUARD page. The first
// read of the guard page clears the guard, so a cleared guard after a call
// means the call read past the buffer even if it caught the exception.
class BufferAgainstGuardPage {
public:
    explicit BufferAgainstGuardPage(SIZE_T bytes) : bytes_(bytes) {
        SYSTEM_INFO si{};
        ::GetSystemInfo(&si);
        pageSize_ = si.dwPageSize;
        Assert::IsTrue(bytes <= pageSize_, L"setup: the buffer fits in one page");
        pages_.reset(static_cast<BYTE*>(::VirtualAlloc(
            nullptr, 2 * pageSize_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)));
        Assert::IsNotNull(pages_.get(), L"setup: VirtualAlloc");
        DWORD oldProtect = 0;
        Assert::IsTrue(::VirtualProtect(pages_.get() + pageSize_, pageSize_,
                                        PAGE_READWRITE | PAGE_GUARD, &oldProtect) != FALSE,
            L"setup: VirtualProtect");
    }

    BYTE*  Data() const noexcept { return pages_.get() + pageSize_ - bytes_; }
    SIZE_T Size() const noexcept { return bytes_; }

    bool GuardIntact() const {
        MEMORY_BASIC_INFORMATION mbi{};
        Assert::AreEqual<SIZE_T>(sizeof(mbi),
            ::VirtualQuery(pages_.get() + pageSize_, &mbi, sizeof(mbi)), L"VirtualQuery");
        return (mbi.Protect & PAGE_GUARD) != 0;
    }

private:
    struct VirtualFreeDeleter {
        void operator()(BYTE* p) const noexcept { ::VirtualFree(p, 0, MEM_RELEASE); }
    };

    SIZE_T                                    bytes_;
    SIZE_T                                    pageSize_ = 0;
    std::unique_ptr<BYTE, VirtualFreeDeleter> pages_;
};

constexpr SIZE_T kSelfRelativeHeaderBytes = sizeof(SECURITY_DESCRIPTOR_RELATIVE);
constexpr SIZE_T kPartHeaderBytes         = 8;

void WriteDescriptorWithTrailingPartHeader(BufferAgainstGuardPage& buffer, WORD control,
                                           DWORD SECURITY_DESCRIPTOR_RELATIVE::*partOffset,
                                           const BYTE (&partHeader)[kPartHeaderBytes]) {
    Assert::AreEqual<SIZE_T>(kSelfRelativeHeaderBytes + kPartHeaderBytes, buffer.Size(),
        L"setup: the buffer holds exactly the header and one part header");
    SECURITY_DESCRIPTOR_RELATIVE header{};
    header.Revision = SECURITY_DESCRIPTOR_REVISION;
    header.Control  = control;
    header.*partOffset = static_cast<DWORD>(kSelfRelativeHeaderBytes);
    std::memcpy(buffer.Data(), &header, sizeof(header));
    std::memcpy(buffer.Data() + kSelfRelativeHeaderBytes, partHeader, kPartHeaderBytes);
}

HRESULT CreateFileWithDescriptor(LayerMountHolder& mount, PCWSTR path,
                                 const BufferAgainstGuardPage& sd, OpenedFile& out) {
    return CreateOverlayFileWithDescriptor(mount.Get(), path,
                                           GENERIC_READ | GENERIC_WRITE, kNoCreateOptions,
                                           FILE_ATTRIBUTE_NORMAL, sd.Data(), sd.Size(), out);
}
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

    TEST_METHOD(CreateFile_OwnerSidRunningPastTheBuffer_ReturnsInvalidArgWithoutReadingPastIt) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        constexpr BYTE kSidRevision = 1;
        constexpr BYTE kMaxSubAuthorities = 15;
        constexpr BYTE kNtAuthority = 5;
        constexpr BYTE sidHeader[kPartHeaderBytes] = {
            kSidRevision, kMaxSubAuthorities, 0, 0, 0, 0, 0, kNtAuthority};

        BufferAgainstGuardPage buffer(kSelfRelativeHeaderBytes + kPartHeaderBytes);
        WriteDescriptorWithTrailingPartHeader(buffer, SE_SELF_RELATIVE,
            &SECURITY_DESCRIPTOR_RELATIVE::Owner, sidHeader);

        OpenedFile opened;
        HRESULT hr = CreateFileWithDescriptor(mount, L"\\owner.txt",
                                              buffer, opened);
        Assert::AreEqual<HRESULT>(E_INVALIDARG, hr,
            L"LayerMountCreateFile must reject an owner SID longer than the buffer");
        Assert::IsTrue(buffer.GuardIntact(),
            L"Validating the descriptor must not read past the buffer");
    }

    TEST_METHOD(CreateFile_DaclRunningPastTheBuffer_ReturnsInvalidArgWithoutReadingPastIt) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        constexpr WORD kAclSizeBeyondBuffer = 64;
        constexpr BYTE kOneAce = 1;
        constexpr BYTE aclHeader[kPartHeaderBytes] = {
            ACL_REVISION, 0,
            static_cast<BYTE>(kAclSizeBeyondBuffer & 0xFF),
            static_cast<BYTE>(kAclSizeBeyondBuffer >> 8),
            kOneAce, 0, 0, 0};

        BufferAgainstGuardPage buffer(kSelfRelativeHeaderBytes + kPartHeaderBytes);
        WriteDescriptorWithTrailingPartHeader(buffer,
            SE_SELF_RELATIVE | SE_DACL_PRESENT,
            &SECURITY_DESCRIPTOR_RELATIVE::Dacl, aclHeader);

        OpenedFile opened;
        HRESULT hr = CreateFileWithDescriptor(mount, L"\\dacl.txt",
                                              buffer, opened);
        Assert::AreEqual<HRESULT>(E_INVALIDARG, hr,
            L"LayerMountCreateFile must reject a DACL longer than the buffer");
        Assert::IsTrue(buffer.GuardIntact(),
            L"Validating the descriptor must not read past the buffer");
    }

    TEST_METHOD(CreateFile_WellFormedDescriptorEndingAtTheBuffer_CreatesTheFile) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        CreatePlainFile(mount, L"\\source.txt");
        std::vector<BYTE> source = FetchSecurity(mount, L"\\source.txt",
            OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
            DACL_SECURITY_INFORMATION);
        const DWORD sourceBytes = ::GetSecurityDescriptorLength(source.data());
        BufferAgainstGuardPage buffer(sourceBytes);
        std::memcpy(buffer.Data(), source.data(), sourceBytes);

        OpenedFile opened;
        HRESULT hr = CreateFileWithDescriptor(mount, L"\\wellformed.txt",
                                              buffer, opened);
        Assert::AreEqual<HRESULT>(S_OK, hr,
            L"LayerMountCreateFile must accept a descriptor that fits its buffer exactly");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));
        Assert::IsTrue(buffer.GuardIntact(),
            L"Validating the descriptor must not read past the buffer");
    }

    TEST_METHOD(CreateFile_SaclOffsetWithoutSaclPresent_CreatesTheFile) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        CreatePlainFile(mount, L"\\source.txt");
        std::vector<BYTE> source = FetchSecurity(mount, L"\\source.txt",
            OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
            DACL_SECURITY_INFORMATION);
        const DWORD sourceBytes = ::GetSecurityDescriptorLength(source.data());
        BufferAgainstGuardPage buffer(sourceBytes);
        std::memcpy(buffer.Data(), source.data(), sourceBytes);

        SECURITY_DESCRIPTOR_RELATIVE header{};
        std::memcpy(&header, buffer.Data(), sizeof(header));
        Assert::IsTrue((header.Control & SE_SACL_PRESENT) == 0,
            L"setup: the source descriptor has no SACL");
        constexpr DWORD kSaclOffsetPastTheBuffer = 0xFFFFFFF0u;
        header.Sacl = kSaclOffsetPastTheBuffer;
        std::memcpy(buffer.Data(), &header, sizeof(header));

        OpenedFile opened;
        HRESULT hr = CreateFileWithDescriptor(mount, L"\\stale-sacl.txt",
                                              buffer, opened);
        Assert::AreEqual<HRESULT>(S_OK, hr,
            L"LayerMountCreateFile must ignore a SACL offset when SE_SACL_PRESENT is clear");
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));
        Assert::IsTrue(buffer.GuardIntact(),
            L"Validating the descriptor must not read past the buffer");
    }

    TEST_METHOD(CreateFile_DaclOnlyDescriptor_CreatesTheFileWithThatDacl) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(S_OK,
            CreateAndCloseWithSddl(mount, L"\\dacl-only.txt", kNoCreateOptions,
                                   FILE_ATTRIBUTE_NORMAL, kEveryoneFullAccessDacl),
            L"LayerMountCreateFile must accept a descriptor with only a DACL");

        Assert::AreEqual(std::wstring(kEveryoneFullAccessDacl),
            FetchSddl(mount, L"\\dacl-only.txt", DACL_SECURITY_INFORMATION),
            L"The new file's DACL must be the descriptor's DACL");
    }

    TEST_METHOD(CreateDirectory_DaclOnlyDescriptor_CreatesTheDirectoryWithThatDacl) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(S_OK,
            CreateAndCloseWithSddl(mount, L"\\dacl-only", FILE_DIRECTORY_FILE,
                                   FILE_ATTRIBUTE_DIRECTORY, kEveryoneFullAccessDacl),
            L"A directory create must accept a descriptor with only a DACL");

        Assert::AreEqual(std::wstring(kEveryoneFullAccessDacl),
            FetchSddl(mount, L"\\dacl-only", DACL_SECURITY_INFORMATION),
            L"The new directory's DACL must be the descriptor's DACL");
    }

    TEST_METHOD(CreateFile_DescriptorWithSacl_CreatesTheFileWithThatSacl) {
        ABI_SKIP_IF_NO_SECURITY_PRIVILEGE();
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(S_OK,
            CreateAndCloseWithSddl(mount, L"\\audited.txt", kNoCreateOptions,
                                   FILE_ATTRIBUTE_NORMAL, EveryoneFullAccessAuditedDescriptor().c_str()),
            L"LayerMountCreateFile must accept a descriptor with a SACL");

        Assert::AreEqual(std::wstring(kEveryoneAuditSacl),
            FetchSddl(mount, L"\\audited.txt", SACL_SECURITY_INFORMATION),
            L"The new file's SACL must be the descriptor's SACL");
        Assert::AreEqual(std::wstring(kEveryoneFullAccessDacl),
            FetchSddl(mount, L"\\audited.txt", DACL_SECURITY_INFORMATION),
            L"The new file's DACL must be the descriptor's DACL");
    }

    TEST_METHOD(CreateDirectory_DescriptorWithSacl_CreatesTheDirectoryWithThatSacl) {
        ABI_SKIP_IF_NO_SECURITY_PRIVILEGE();
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(S_OK,
            CreateAndCloseWithSddl(mount, L"\\audited", FILE_DIRECTORY_FILE,
                                   FILE_ATTRIBUTE_DIRECTORY, EveryoneFullAccessAuditedDescriptor().c_str()),
            L"A directory create must accept a descriptor with a SACL");

        Assert::AreEqual(std::wstring(kEveryoneAuditSacl),
            FetchSddl(mount, L"\\audited", SACL_SECURITY_INFORMATION),
            L"The new directory's SACL must be the descriptor's SACL");
        Assert::AreEqual(std::wstring(kEveryoneFullAccessDacl),
            FetchSddl(mount, L"\\audited", DACL_SECURITY_INFORMATION),
            L"The new directory's DACL must be the descriptor's DACL");
    }

    TEST_METHOD(CreateFile_DescriptorWithSaclWithoutSecurityPrivilege_CreatesTheFileWithThatDacl) {
        ABI_SKIP_IF_SECURITY_PRIVILEGE_HELD();
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<HRESULT>(S_OK,
            CreateAndCloseWithSddl(mount, L"\\unaudited.txt", kNoCreateOptions,
                                   FILE_ATTRIBUTE_NORMAL,
                                   EveryoneFullAccessAuditedDescriptor().c_str()),
            L"A create without SE_SECURITY_NAME must drop the SACL and succeed");

        Assert::AreEqual(std::wstring(kEveryoneFullAccessDacl),
            FetchSddl(mount, L"\\unaudited.txt", DACL_SECURITY_INFORMATION),
            L"The new file's DACL must be the descriptor's DACL");
    }

    TEST_METHOD(CreateFile_OwnerOnlyDescriptor_SetsTheOwnerAndKeepsTheInheritedDacl) {
        ABI_SKIP_IF_NOT_ADMIN();
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        CreatePlainFile(mount, L"\\inherited.txt");
        const std::wstring inheritedDacl =
            FetchSddl(mount, L"\\inherited.txt", DACL_SECURITY_INFORMATION);

        Assert::AreEqual<HRESULT>(S_OK,
            CreateAndCloseWithSddl(mount, L"\\owner-only.txt", kNoCreateOptions,
                                   FILE_ATTRIBUTE_NORMAL, kAdministratorsOwner),
            L"LayerMountCreateFile must accept a descriptor with only an owner");

        Assert::AreEqual(std::wstring(kAdministratorsOwner),
            FetchSddl(mount, L"\\owner-only.txt", OWNER_SECURITY_INFORMATION),
            L"The new file's owner must be the descriptor's owner");
        Assert::AreEqual(inheritedDacl,
            FetchSddl(mount, L"\\owner-only.txt", DACL_SECURITY_INFORMATION),
            L"A descriptor without a DACL must leave the inherited DACL in place");
    }

private:
    static constexpr PCWSTR kEveryoneFullAccessDacl = L"D:(A;;FA;;;WD)";
    static constexpr PCWSTR kAdministratorsOwner    = L"O:BA";
    static constexpr PCWSTR kEveryoneAuditSacl      = L"S:(AU;SAFA;FA;;;WD)";

    static std::wstring EveryoneFullAccessAuditedDescriptor() {
        return std::wstring(kEveryoneFullAccessDacl) + kEveryoneAuditSacl;
    }

    class SddlDescriptor {
    public:
        explicit SddlDescriptor(PCWSTR sddl) {
            Assert::IsTrue(
                ::ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    sddl, SDDL_REVISION_1, &sd_, &size_) != FALSE,
                L"setup: ConvertStringSecurityDescriptorToSecurityDescriptorW");
        }
        ~SddlDescriptor() { ::LocalFree(sd_); }
        SddlDescriptor(const SddlDescriptor&) = delete;
        SddlDescriptor& operator=(const SddlDescriptor&) = delete;

        PSECURITY_DESCRIPTOR Get() const noexcept { return sd_; }
        const BYTE*          Bytes() const noexcept { return static_cast<const BYTE*>(sd_); }
        SIZE_T               Size() const noexcept { return size_; }

    private:
        PSECURITY_DESCRIPTOR sd_   = nullptr;
        ULONG                size_ = 0;
    };

    static HRESULT CreateAndCloseWithSddl(LayerMountHolder& mount, PCWSTR path,
                                          UINT32 createOptions, UINT32 fileAttributes,
                                          PCWSTR sddl) {
        SddlDescriptor sd(sddl);
        OpenedFile     opened;
        HRESULT hr = CreateOverlayFileWithDescriptor(mount.Get(), path,
            GENERIC_READ, createOptions, fileAttributes, sd.Bytes(), sd.Size(), opened);
        if (SUCCEEDED(hr)) {
            Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));
        }
        return hr;
    }

    static std::wstring FetchSddl(LayerMountHolder& mount, PCWSTR path, UINT32 secInfo) {
        std::vector<BYTE> sd = FetchSecurity(mount, path, secInfo);
        LPWSTR sddl = nullptr;
        Assert::IsTrue(
            ::ConvertSecurityDescriptorToStringSecurityDescriptorW(
                sd.data(), SDDL_REVISION_1, secInfo, &sddl, nullptr) != FALSE,
            L"ConvertSecurityDescriptorToStringSecurityDescriptorW");
        std::wstring result(sddl);
        ::LocalFree(sddl);
        return result;
    }

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
        SddlDescriptor sd(sddl);
        Assert::IsTrue(
            ::SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, sd.Get()) != FALSE,
            L"setup: SetFileSecurityW");
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
