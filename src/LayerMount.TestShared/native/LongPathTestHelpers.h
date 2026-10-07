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

inline std::wstring ExtendedPathUnder(const std::wstring& layer, const std::wstring& relativePath) {
    const std::wstring path = layer + L"\\" + relativePath;
    Microsoft::VisualStudio::CppUnitTestFramework::Assert::IsTrue(path.size() > MAX_PATH,
        L"Precondition: the host path is longer than MAX_PATH");
    return ExtendedFormOf(path);
}

// Ignores errors, so a teardown never fails a test.
inline void RemoveTreeOfAnyDepth(const std::wstring& root) {
    std::error_code ec;
    std::filesystem::remove_all(ExtendedFormOf(root), ec);
}

}
