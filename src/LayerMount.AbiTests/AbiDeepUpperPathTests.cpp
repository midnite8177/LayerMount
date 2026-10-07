#include "pch.h"
#include "AbiTestFixture.h"
#include "StreamTestHelpers.h"

#include <aclapi.h>
#include <sddl.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using LayerMountTestShared::DeepLeafName;
using LayerMountTestShared::ExtendedPathUnder;
using LayerMountTestShared::HasStream;

namespace LayerMountAbiTests {

namespace {

const std::wstring kParentName = L"dir";

std::wstring DeepName(const TempLayerEnv& env, const std::wstring& prefix) {
    return DeepLeafName(env.Upper() + L"\\" + kParentName, prefix);
}

std::wstring MountPathOf(const std::wstring& name) {
    return L"\\" + kParentName + L"\\" + name;
}

std::wstring ExtendedHostPath(const std::wstring& layer, const std::wstring& name) {
    return ExtendedPathUnder(layer, kParentName + L"\\" + name);
}

void MakeParentIn(const std::wstring& layer) {
    std::filesystem::create_directories(
        LayerMountTestShared::ExtendedFormOf(layer + L"\\" + kParentName));
}

bool HostEntryExists(const std::wstring& extendedPath) {
    return ::GetFileAttributesW(extendedPath.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring StoredNameOf(const std::wstring& extendedPath) {
    WIN32_FIND_DATAW found{};
    const HANDLE find = ::FindFirstFileW(extendedPath.c_str(), &found);
    Assert::IsTrue(find != INVALID_HANDLE_VALUE, L"FindFirstFileW lists the host entry");
    ::FindClose(find);
    return found.cFileName;
}

void CreateWithContent(LM_HANDLE mount, const std::wstring& name, const std::string& text) {
    WriteThroughOverlay(mount, MountPathOf(name).c_str(), text);
}

std::string ReadThroughMount(LM_HANDLE mount, const std::wstring& name, UINT32 access) {
    OpenedFile opened;
    Assert::AreEqual<HRESULT>(S_OK,
        OpenOverlayFile(mount, MountPathOf(name).c_str(), access, kNoCreateOptions, opened),
        L"The open of the deep file succeeds");
    HRESULT readHr = E_FAIL;
    const std::string content = ReadThroughHandle(opened.handle, &readHr);
    Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));
    Assert::AreEqual<HRESULT>(S_OK, readHr, L"The read of the deep file succeeds");
    return content;
}

std::wstring DaclSddlOf(const std::wstring& path) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    Assert::AreEqual<DWORD>(ERROR_SUCCESS,
        ::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, nullptr, nullptr, &sd),
        L"GetNamedSecurityInfoW reads the DACL of the host file");
    LPWSTR sddl = nullptr;
    const BOOL converted = ::ConvertSecurityDescriptorToStringSecurityDescriptorW(
        sd, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &sddl, nullptr);
    ::LocalFree(sd);
    Assert::IsTrue(converted != FALSE, L"The DACL converts to SDDL");
    const std::wstring text(sddl);
    ::LocalFree(sddl);
    return text;
}

}

TEST_CLASS(AbiDeepUpperPathTests) {
public:
    TEST_METHOD(CreateFile_DeepUpperFile_WritesTheHostFileThatOpenReads) {
        TempLayerEnv env(0);
        MakeParentIn(env.Upper());
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring name = DeepName(env, L"Deep");

        CreateWithContent(mount.Get(), name, "deep");

        Assert::AreEqual<std::string>("deep",
            ReadAllBytes(ExtendedHostPath(env.Upper(), name)),
            L"The create writes the deep host file");
        Assert::AreEqual<std::string>("deep",
            ReadThroughMount(mount.Get(), name, GENERIC_READ),
            L"The open finds the deep file that the create made");
    }

    TEST_METHOD(CreateFile_DeepUpperFileWithDescriptor_WritesTheDaclOnTheHostFile) {
        TempLayerEnv env(0);
        MakeParentIn(env.Upper());
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring name = DeepName(env, L"Deep");
        PSECURITY_DESCRIPTOR sd = nullptr;
        ULONG sdSize = 0;
        Assert::IsTrue(::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;WD)", SDDL_REVISION_1, &sd, &sdSize) != FALSE,
            L"Precondition: the descriptor converts from SDDL");

        OpenedFile created;
        const HRESULT createHr = CreateOverlayFileWithDescriptor(mount.Get(),
            MountPathOf(name).c_str(), GENERIC_READ | GENERIC_WRITE, kNoCreateOptions,
            FILE_ATTRIBUTE_NORMAL, static_cast<const BYTE*>(sd), sdSize, created);
        ::LocalFree(sd);
        Assert::AreEqual<HRESULT>(S_OK, createHr);
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(created.handle));

        Assert::AreEqual(std::wstring(L"D:P(A;;FA;;;WD)"),
            DaclSddlOf(ExtendedHostPath(env.Upper(), name)),
            L"The create writes the caller's DACL on the deep host file");
    }

    TEST_METHOD(CreateFile_StreamOnDeepUpperFile_AddsTheStreamAndKeepsTheData) {
        TempLayerEnv env(0);
        MakeParentIn(env.Upper());
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring name = DeepName(env, L"Deep");
        CreateWithContent(mount.Get(), name, "host");

        OpenedFile created;
        Assert::AreEqual<HRESULT>(S_OK,
            CreateOverlayFile(mount.Get(), (MountPathOf(name) + L":s").c_str(),
                              GENERIC_READ | GENERIC_WRITE, kNoCreateOptions,
                              FILE_ATTRIBUTE_NORMAL, created));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(created.handle));

        const std::wstring hostPath = ExtendedHostPath(env.Upper(), name);
        Assert::IsTrue(HasStream(hostPath + L":s"),
            L"The create adds the stream to the deep host file");
        Assert::AreEqual<std::string>("host", ReadAllBytes(hostPath),
            L"The stream create keeps the data of the deep host file");
    }

    TEST_METHOD(CreateFile_StreamOnDeepUpperDirectory_ReturnsFileIsADirectory) {
        TempLayerEnv env(0);
        MakeParentIn(env.Upper());
        const std::wstring name = DeepName(env, L"Deep");
        std::filesystem::create_directories(ExtendedHostPath(env.Upper(), name));
        LayerMountHolder mount = CreateLayerMount(env);

        OpenedFile created;
        Assert::AreEqual<HRESULT>(HRESULT_FROM_NT(STATUS_FILE_IS_A_DIRECTORY),
            CreateOverlayFile(mount.Get(), (MountPathOf(name) + L":s").c_str(),
                              GENERIC_READ | GENERIC_WRITE, kNoCreateOptions,
                              FILE_ATTRIBUTE_NORMAL, created),
            L"A stream create on a deep upper directory is refused");
        Assert::IsNull(created.handle, L"The refused create returns no handle");
    }

    TEST_METHOD(CreateDirectory_DeepUpperDirectory_MakesTheHostDirectory) {
        TempLayerEnv env(0);
        MakeParentIn(env.Upper());
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring name = DeepName(env, L"Deep");

        OpenedFile created;
        Assert::AreEqual<HRESULT>(S_OK,
            CreateOverlayFile(mount.Get(), MountPathOf(name).c_str(), GENERIC_READ,
                              FILE_DIRECTORY_FILE, FILE_ATTRIBUTE_DIRECTORY, created));
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(created.handle));

        const DWORD attributes =
            ::GetFileAttributesW(ExtendedHostPath(env.Upper(), name).c_str());
        Assert::IsTrue(attributes != INVALID_FILE_ATTRIBUTES &&
                           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
            L"The create makes the deep host directory");
    }

    TEST_METHOD(OpenFile_DeepUpperFileWithMaximumAllowed_ReadsContent) {
        TempLayerEnv env(0);
        MakeParentIn(env.Upper());
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring name = DeepName(env, L"Deep");
        WriteText(ExtendedHostPath(env.Upper(), name), "maximum");

        Assert::AreEqual<std::string>("maximum",
            ReadThroughMount(mount.Get(), name, MAXIMUM_ALLOWED),
            L"The open with MAXIMUM_ALLOWED reads the deep file");
    }

    TEST_METHOD(OpenFile_DeepLowerFile_ReadsContent) {
        TempLayerEnv env(1);
        MakeParentIn(env.Upper());
        MakeParentIn(env.Lower(0));
        const std::wstring name = DeepName(env, L"Deep");
        WriteText(ExtendedHostPath(env.Lower(0), name), "lower");
        LayerMountHolder mount = CreateLayerMount(env);

        Assert::AreEqual<std::string>("lower",
            ReadThroughMount(mount.Get(), name, GENERIC_READ),
            L"The read-only open finds the deep lower file");
    }

    TEST_METHOD(OpenFile_DeepLowerFileUnderUpperWhiteout_ReturnsNotFound) {
        TempLayerEnv env(1);
        MakeParentIn(env.Upper());
        MakeParentIn(env.Lower(0));
        const std::wstring name = DeepName(env, L"Deep");
        WriteText(ExtendedHostPath(env.Lower(0), name), "lower");
        WriteText(ExtendedHostPath(env.Upper(), L".wh." + name), "");
        LayerMountHolder mount = CreateLayerMount(env);

        AssertOpenFailsNotFound(mount.Get(), MountPathOf(name).c_str(),
            L"The whiteout in the upper hides the deep lower file");
    }

    TEST_METHOD(GetSecurity_DeepUpperFile_ReportsItsAttributes) {
        TempLayerEnv env(0);
        MakeParentIn(env.Upper());
        const std::wstring name = DeepName(env, L"Deep");
        const std::wstring hostPath = ExtendedHostPath(env.Upper(), name);
        WriteText(hostPath, "secured");
        Assert::IsTrue(::SetFileAttributesW(hostPath.c_str(), FILE_ATTRIBUTE_HIDDEN) != FALSE,
            L"Precondition: the deep upper file takes the hidden attribute");
        LayerMountHolder mount = CreateLayerMount(env);

        UINT32 attributes = 0;
        SIZE_T required = 0;
        constexpr SIZE_T sizingProbeBytes = 0;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountGetSecurity(mount.Get(), MountPathOf(name).c_str(),
                                    DACL_SECURITY_INFORMATION, &attributes, nullptr,
                                    sizingProbeBytes, &required));
        Assert::AreEqual<UINT32>(FILE_ATTRIBUTE_HIDDEN, attributes,
            L"GetSecurity reports the attributes of the deep upper file");
    }

    TEST_METHOD(Overwrite_DeepUpperFile_TruncatesAndAddsAttributesOnTheHostFile) {
        TempLayerEnv env(0);
        MakeParentIn(env.Upper());
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring name = DeepName(env, L"Deep");
        CreateWithContent(mount.Get(), name, "content");

        OpenedFile opened;
        Assert::AreEqual<HRESULT>(S_OK,
            OpenOverlayFile(mount.Get(), MountPathOf(name).c_str(),
                            GENERIC_READ | GENERIC_WRITE, kNoCreateOptions, opened));
        constexpr UINT64 allocationSize = 0u;
        const HRESULT overwriteHr = OverwriteAddingAttributes(
            opened.handle, FILE_ATTRIBUTE_HIDDEN, allocationSize, nullptr);
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));

        Assert::AreEqual<HRESULT>(S_OK, overwriteHr);
        const std::wstring hostPath = ExtendedHostPath(env.Upper(), name);
        Assert::AreEqual<std::string>("", ReadAllBytes(hostPath),
            L"The overwrite truncates the deep host file");
        Assert::IsTrue((::GetFileAttributesW(hostPath.c_str()) & FILE_ATTRIBUTE_HIDDEN) != 0,
            L"The overwrite adds the attribute to the deep host file");
    }

    TEST_METHOD(RenameFile_DeepUpperFile_MovesTheHostFileUnderTheNewName) {
        TempLayerEnv env(0);
        MakeParentIn(env.Upper());
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring source = DeepName(env, L"Source");
        const std::wstring target = DeepName(env, L"Target");
        CreateWithContent(mount.Get(), source, "moved");

        constexpr BOOL replaceIfExists = FALSE;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountRenameFile(mount.Get(), MountPathOf(source).c_str(),
                                   MountPathOf(target).c_str(), replaceIfExists));

        Assert::IsFalse(HostEntryExists(ExtendedHostPath(env.Upper(), source)),
            L"The rename removes the old deep host name");
        Assert::AreEqual(target, StoredNameOf(ExtendedHostPath(env.Upper(), target)),
            L"The rename stores the new deep name in the caller's case");
        AssertOpenFailsNotFound(mount.Get(), MountPathOf(source).c_str(),
            L"The old deep name is gone from the mount");
        Assert::AreEqual<std::string>("moved",
            ReadThroughMount(mount.Get(), target, GENERIC_READ),
            L"The new deep name opens the moved file");
    }

    TEST_METHOD(RenameFile_DeepUpperFileOntoDeepUpperFile_ReplacesTheDestination) {
        TempLayerEnv env(0);
        MakeParentIn(env.Upper());
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring source = DeepName(env, L"Source");
        const std::wstring target = DeepName(env, L"Target");
        CreateWithContent(mount.Get(), source, "source");
        CreateWithContent(mount.Get(), target, "target");

        constexpr BOOL replaceIfExists = TRUE;
        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountRenameFile(mount.Get(), MountPathOf(source).c_str(),
                                   MountPathOf(target).c_str(), replaceIfExists));

        Assert::IsFalse(HostEntryExists(ExtendedHostPath(env.Upper(), source)),
            L"The replace-rename removes the old deep host name");
        Assert::AreEqual<std::string>("source",
            ReadAllBytes(ExtendedHostPath(env.Upper(), target)),
            L"The replace-rename puts the source data at the deep destination");
    }

    TEST_METHOD(RenameOpenFile_DeepUpperFile_MovesTheHostFileUnderTheNewName) {
        TempLayerEnv env(0);
        MakeParentIn(env.Upper());
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring source = DeepName(env, L"Source");
        const std::wstring target = DeepName(env, L"Target");
        CreateWithContent(mount.Get(), source, "moved");

        LM_FILE_HANDLE opened =
            OpenWithAccess(mount.Get(), MountPathOf(source).c_str(), GENERIC_READ | DELETE);
        constexpr BOOL replaceIfExists = FALSE;
        const HRESULT renameHr = ::LayerMountRenameOpenFile(
            opened, MountPathOf(target).c_str(), replaceIfExists);
        HRESULT readHr = E_FAIL;
        const std::string content = ReadThroughHandle(opened, &readHr);
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened));

        Assert::AreEqual<HRESULT>(S_OK, renameHr);
        Assert::AreEqual<HRESULT>(S_OK, readHr);
        Assert::AreEqual<std::string>("moved", content,
            L"The renamed handle reads the moved file");
        Assert::IsFalse(HostEntryExists(ExtendedHostPath(env.Upper(), source)),
            L"The rename removes the old deep host name");
        Assert::AreEqual(target, StoredNameOf(ExtendedHostPath(env.Upper(), target)),
            L"The rename stores the new deep name in the caller's case");
    }

    TEST_METHOD(DeleteFile_DeepUpperFile_RemovesTheHostFile) {
        TempLayerEnv env(0);
        MakeParentIn(env.Upper());
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring name = DeepName(env, L"Deep");
        CreateWithContent(mount.Get(), name, "gone");

        Assert::AreEqual<HRESULT>(S_OK,
            ::LayerMountDeleteFile(mount.Get(), MountPathOf(name).c_str()));

        Assert::IsFalse(HostEntryExists(ExtendedHostPath(env.Upper(), name)),
            L"The delete removes the deep host file");
        AssertOpenFailsNotFound(mount.Get(), MountPathOf(name).c_str(),
            L"The deleted deep file is gone from the mount");
    }

    TEST_METHOD(DeleteOpenFile_DeepUpperFile_RemovesTheHostFile) {
        TempLayerEnv env(0);
        MakeParentIn(env.Upper());
        LayerMountHolder mount = CreateLayerMount(env);
        const std::wstring name = DeepName(env, L"Deep");
        CreateWithContent(mount.Get(), name, "gone");

        LM_FILE_HANDLE opened =
            OpenWithAccess(mount.Get(), MountPathOf(name).c_str(), GENERIC_READ | DELETE);
        const HRESULT deleteHr = ::LayerMountDeleteOpenFile(opened);
        Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened));

        Assert::AreEqual<HRESULT>(S_OK, deleteHr);
        Assert::IsFalse(HostEntryExists(ExtendedHostPath(env.Upper(), name)),
            L"The delete removes the deep host file");
        AssertOpenFailsNotFound(mount.Get(), MountPathOf(name).c_str(),
            L"The deleted deep file is gone from the mount");
    }
};

}
