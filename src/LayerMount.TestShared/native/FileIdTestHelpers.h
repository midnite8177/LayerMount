#pragma once

#include <windows.h>

#include <CppUnitTest.h>

#include <string>

// The LM_CAP_* bits come from the public LayerMount.h, which each test
// project includes before this header.

namespace LayerMountTestShared {

// The capabilities of a host whose upper is on NTFS. The engine keeps its
// metadata records in :overlay streams.
inline constexpr UINT32 kDefaultHostCapabilities =
    LM_CAP_ADS | LM_CAP_REPARSE_POINTS | LM_CAP_SPARSE_FILES |
    LM_CAP_MULTIPLE_STREAMS | LM_CAP_NTFS_ACLS;

// kDefaultHostCapabilities without LM_CAP_ADS. The engine keeps its
// metadata records in the sidecar store.
inline constexpr UINT32 kHostCapabilitiesWithoutAds = kDefaultHostCapabilities & ~LM_CAP_ADS;

// Whether an open at a link opens the link itself or its target.
enum class LinkOpen { Itself, Follow };

// The file ID that NTFS reports for path. Fails the test when the open or
// the read fails.
inline UINT64 NtfsFileIdOf(const std::wstring& path, LinkOpen linkOpen) {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
    const DWORD linkFlag = linkOpen == LinkOpen::Itself ? FILE_FLAG_OPEN_REPARSE_POINT : 0u;
    const HANDLE h = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | linkFlag, nullptr);
    Assert::IsTrue(h != INVALID_HANDLE_VALUE, (L"The open of " + path + L" must succeed").c_str());
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL read = ::GetFileInformationByHandle(h, &info);
    ::CloseHandle(h);
    Assert::IsTrue(read != FALSE, (L"The file ID of " + path + L" must read").c_str());
    return (static_cast<UINT64>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
}

}
