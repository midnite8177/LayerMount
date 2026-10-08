#pragma once

#include <windows.h>

#include <CppUnitTest.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace LayerMountTestShared {

constexpr size_t kDeepHostPathLength = 300;
constexpr size_t kMaxNtfsNameLength = 255;

// The \\?\ form of an absolute drive path or UNC path.
inline std::wstring ExtendedFormOf(const std::wstring& absolutePath) {
    if (absolutePath.rfind(L"\\\\?\\", 0) == 0) {
        return absolutePath;
    }
    if (absolutePath.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + absolutePath.substr(2);
    }
    return L"\\\\?\\" + absolutePath;
}

// A name that starts with prefix and makes parentPath\name
// kDeepHostPathLength characters long, under a parent shorter than MAX_PATH.
inline std::wstring DeepLeafName(const std::wstring& parentPath, const std::wstring& prefix) {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
    Assert::IsTrue(parentPath.size() < MAX_PATH,
        L"Precondition: the parent directory path is shorter than MAX_PATH");
    const size_t nameLength = kDeepHostPathLength - parentPath.size() - 1;
    Assert::IsTrue(nameLength <= kMaxNtfsNameLength,
        L"Precondition: the deep name fits in one NTFS name");
    Assert::IsTrue(nameLength > prefix.size(),
        L"Precondition: the deep name is longer than its prefix");
    return prefix + std::wstring(nameLength - prefix.size(), L'n');
}

// Makes a directory whose plain path is kDeepHostPathLength characters long
// under parentPath, which must be shorter than MAX_PATH. Returns the plain
// path.
inline std::wstring MakeDeepLayerRoot(const std::wstring& parentPath, const std::wstring& prefix) {
    const std::wstring root = parentPath + L"\\" + DeepLeafName(parentPath, prefix);
    Microsoft::VisualStudio::CppUnitTestFramework::Assert::IsTrue(
        ::CreateDirectoryW(ExtendedFormOf(root).c_str(), nullptr) != FALSE,
        L"Precondition: the extended form makes the deep layer root");
    return root;
}

inline std::wstring ExtendedPathUnder(const std::wstring& layer, const std::wstring& relativePath) {
    const std::wstring path = layer + L"\\" + relativePath;
    Microsoft::VisualStudio::CppUnitTestFramework::Assert::IsTrue(path.size() > MAX_PATH,
        L"Precondition: the host path is longer than MAX_PATH");
    return ExtendedFormOf(path);
}

inline void AssertHostRefusesPlainPathsPastMaxPath() {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
    wchar_t tempDirectory[MAX_PATH] = {};
    const DWORD length = ::GetTempPathW(MAX_PATH, tempDirectory);
    Assert::IsTrue(length > 0 && length < MAX_PATH,
        L"Precondition: GetTempPathW returns the temporary directory");
    const std::wstring parent = std::wstring(tempDirectory, length - 1) + L"\\LongPathProbe" +
                                std::to_wstring(::GetCurrentProcessId()) +
                                std::wstring(kDeepHostPathLength / 2, L'n');
    const std::wstring extendedParent = ExtendedFormOf(parent);
    Assert::IsTrue(::CreateDirectoryW(extendedParent.c_str(), nullptr) != FALSE ||
                       ::GetLastError() == ERROR_ALREADY_EXISTS,
        L"Precondition: the test makes the parent of the deep probe directory");
    const std::wstring plain = parent + L"\\" + DeepLeafName(parent, L"Probe");
    const std::wstring extended = ExtendedFormOf(plain);
    Assert::IsTrue(::CreateDirectoryW(extended.c_str(), nullptr) != FALSE ||
                       ::GetLastError() == ERROR_ALREADY_EXISTS,
        L"Precondition: the extended form makes the deep probe directory");
    const DWORD plainAttributes = ::GetFileAttributesW(plain.c_str());
    const DWORD extendedAttributes = ::GetFileAttributesW(extended.c_str());
    ::RemoveDirectoryW(extended.c_str());
    ::RemoveDirectoryW(extendedParent.c_str());
    Assert::AreEqual(INVALID_FILE_ATTRIBUTES, plainAttributes,
        L"Precondition: GetFileAttributesW fails on a plain path longer than MAX_PATH");
    Assert::AreNotEqual(INVALID_FILE_ATTRIBUTES, extendedAttributes,
        L"Precondition: GetFileAttributesW finds the extended form of the deep path");
}

// Ignores errors, so a teardown never fails a test.
inline void RemoveTreeOfAnyDepth(const std::wstring& root) {
    std::error_code ec;
    std::filesystem::remove_all(ExtendedFormOf(root), ec);
}

}
