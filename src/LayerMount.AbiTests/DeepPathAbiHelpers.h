#pragma once

#include "AbiTestFixture.h"

#include <aclapi.h>
#include <sddl.h>

namespace LayerMountAbiTests {

inline const std::wstring kDeepParentName = L"dir";

// A name that makes sizingLayer\dir\name longer than MAX_PATH.
inline std::wstring DeepNameSizedBy(const std::wstring& sizingLayer, const std::wstring& prefix) {
    return LayerMountTestShared::DeepLeafName(sizingLayer + L"\\" + kDeepParentName, prefix);
}

inline std::wstring MountPathOf(const std::wstring& name) {
    return L"\\" + kDeepParentName + L"\\" + name;
}

inline std::wstring ExtendedHostPath(const std::wstring& layer, const std::wstring& name) {
    return LayerMountTestShared::ExtendedPathUnder(layer, kDeepParentName + L"\\" + name);
}

inline void MakeParentIn(const std::wstring& layer) {
    std::filesystem::create_directories(
        LayerMountTestShared::ExtendedFormOf(layer + L"\\" + kDeepParentName));
}

inline bool HostEntryExists(const std::wstring& extendedPath) {
    return ::GetFileAttributesW(extendedPath.c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline std::string ReadThroughMount(LM_HANDLE mount, const std::wstring& mountPath, UINT32 access) {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
    OpenedFile opened;
    Assert::AreEqual<HRESULT>(S_OK,
        OpenOverlayFile(mount, mountPath.c_str(), access, kNoCreateOptions, opened),
        L"The open of the deep file succeeds");
    HRESULT readHr = E_FAIL;
    const std::string content = ReadThroughHandle(opened.handle, &readHr);
    Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(opened.handle));
    Assert::AreEqual<HRESULT>(S_OK, readHr, L"The read of the deep file succeeds");
    return content;
}

inline std::wstring DaclSddlOf(const std::wstring& path) {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
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

inline void CreateDirectoryThroughMount(LM_HANDLE mount, const std::wstring& mountPath) {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
    OpenedFile created;
    Assert::AreEqual<HRESULT>(S_OK,
        CreateOverlayFile(mount, mountPath.c_str(), GENERIC_READ, FILE_DIRECTORY_FILE,
                          FILE_ATTRIBUTE_DIRECTORY, created),
        L"The create of the deep directory succeeds");
    Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(created.handle));
}

}
