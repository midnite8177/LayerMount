#include "pch.h"
#include "AbiTestFixture.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LayerMountAbiTests {

namespace {

// REPARSE_DATA_BUFFER is a kernel-header type and is not declared by
// windows.h, so the mount-point layout is spelled out here. The path buffer
// holds the substitute name, its terminator, and the terminator of an empty
// print name.
#pragma pack(push, 1)
struct MountPointReparseHeader {
    ULONG  ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    USHORT SubstituteNameOffset;
    USHORT SubstituteNameLength;
    USHORT PrintNameOffset;
    USHORT PrintNameLength;
};
#pragma pack(pop)

constexpr SIZE_T kReparseHeaderBytes    = 8;   // tag + length + reserved
constexpr SIZE_T kMountPointFieldBytes  = 8;   // the four USHORT name fields

std::vector<BYTE> BuildMountPointBuffer(const std::wstring& target) {
    const std::wstring substituteName = L"\\??\\" + target;
    const SIZE_T nameBytes = substituteName.size() * sizeof(wchar_t);
    const SIZE_T pathBytes = nameBytes + (2 * sizeof(wchar_t));

    std::vector<BYTE> buffer(
        kReparseHeaderBytes + kMountPointFieldBytes + pathBytes, 0);
    auto* header = reinterpret_cast<MountPointReparseHeader*>(buffer.data());
    header->ReparseTag           = IO_REPARSE_TAG_MOUNT_POINT;
    header->ReparseDataLength    = static_cast<USHORT>(kMountPointFieldBytes + pathBytes);
    header->Reserved             = 0;
    header->SubstituteNameOffset = 0;
    header->SubstituteNameLength = static_cast<USHORT>(nameBytes);
    header->PrintNameOffset      = static_cast<USHORT>(nameBytes + sizeof(wchar_t));
    header->PrintNameLength      = 0;
    std::memcpy(buffer.data() + kReparseHeaderBytes + kMountPointFieldBytes,
                substituteName.c_str(), nameBytes);
    return buffer;
}

}

TEST_CLASS(AbiReparsePointTests) {
public:
    TEST_METHOD(GetReparsePoint_NullProbe_ReturnsSuccessWithRequiredSize) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        CreateJunction(env, mount, L"\\probe", env.Root() + L"\\target");

        SIZE_T required = 0;
        HRESULT hr = ::LayerMountGetReparsePoint(
            mount.Get(), L"\\probe", nullptr, 0, &required);
        Assert::AreEqual<HRESULT>(S_OK, hr,
            L"A null-buffer probe must report the size, not fail");
        Assert::IsTrue(required > 0,
            L"A mount-point descriptor is never zero bytes");
    }

    TEST_METHOD(GetReparsePoint_ShortBuffer_ReturnsMoreDataAndRequiredSize) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        CreateJunction(env, mount, L"\\short", env.Root() + L"\\target");

        SIZE_T required = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountGetReparsePoint(mount.Get(), L"\\short", nullptr, 0, &required));
        Assert::IsTrue(required > 1);

        std::vector<BYTE> tooSmall(required - 1);
        SIZE_T secondRequired = 0;
        HRESULT hr = ::LayerMountGetReparsePoint(
            mount.Get(), L"\\short", tooSmall.data(), tooSmall.size(), &secondRequired);
        Assert::AreEqual<HRESULT>(HRESULT_FROM_WIN32(ERROR_MORE_DATA), hr,
            L"A too-small buffer must report ERROR_MORE_DATA");
        Assert::AreEqual<SIZE_T>(required, secondRequired,
            L"The required size must still be written on the short-buffer path");
    }

    TEST_METHOD(GetReparsePoint_OversizedBuffer_ReportsTheWrittenSize) {
        TempLayerEnv     env(0);
        LayerMountHolder mount = CreateLayerMount(env);
        CreateJunction(env, mount, L"\\oversized", env.Root() + L"\\target");

        SIZE_T required = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountGetReparsePoint(mount.Get(), L"\\oversized", nullptr, 0, &required));
        Assert::IsTrue(required < MAXIMUM_REPARSE_DATA_BUFFER_SIZE);

        std::vector<BYTE> ceiling(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
        SIZE_T written = 0;
        HRESULT hr = ::LayerMountGetReparsePoint(
            mount.Get(), L"\\oversized", ceiling.data(), ceiling.size(), &written);
        Assert::AreEqual<HRESULT>(S_OK, hr,
            L"A buffer larger than the descriptor must be accepted");
        Assert::AreEqual<SIZE_T>(required, written,
            L"An oversized buffer must report the bytes written, not its capacity");

        const auto* header = reinterpret_cast<const MountPointReparseHeader*>(ceiling.data());
        Assert::AreEqual<ULONG>(IO_REPARSE_TAG_MOUNT_POINT, header->ReparseTag);
        Assert::AreEqual<SIZE_T>(written - kReparseHeaderBytes,
            static_cast<SIZE_T>(header->ReparseDataLength),
            L"The reported size must match the descriptor's own data length");
    }

private:
    static void CreateJunction(const TempLayerEnv& env,
                               LayerMountHolder&   mount,
                               const std::wstring& relativePath,
                               const std::wstring& target) {
        std::filesystem::create_directories(env.Upper() + relativePath);
        std::vector<BYTE> descriptor = BuildMountPointBuffer(target);
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountSetReparsePoint(mount.Get(), relativePath.c_str(),
                                        descriptor.data(), descriptor.size()),
            L"setup: LayerMountSetReparsePoint");
    }
};

}
