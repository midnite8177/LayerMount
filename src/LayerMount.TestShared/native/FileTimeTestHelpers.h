#pragma once

#include <windows.h>

#include <CppUnitTest.h>

#include <string>

namespace LayerMountTestShared {

// A FILETIME for noon UTC on the given day.
inline FILETIME MakeFileTime(WORD year, WORD month, WORD day) {
    SYSTEMTIME st{};
    st.wYear = year;
    st.wMonth = month;
    st.wDay = day;
    st.wHour = 12;
    FILETIME ft{};
    ::SystemTimeToFileTime(&st, &ft);
    return ft;
}

inline void StampTimes(const std::wstring& path,
                       const FILETIME& creation,
                       const FILETIME& access,
                       const FILETIME& write) {
    HANDLE h = ::CreateFileW(path.c_str(),
                              FILE_WRITE_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    Microsoft::VisualStudio::CppUnitTestFramework::Assert::AreNotEqual<HANDLE>(
        INVALID_HANDLE_VALUE, h, L"StampTimes: CreateFileW must succeed");
    ::SetFileTime(h, &creation, &access, &write);
    ::CloseHandle(h);
}

inline void GetTimes(const std::wstring& path,
                     FILETIME* creation, FILETIME* access, FILETIME* write) {
    HANDLE h = ::CreateFileW(path.c_str(),
                              FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    Microsoft::VisualStudio::CppUnitTestFramework::Assert::AreNotEqual<HANDLE>(
        INVALID_HANDLE_VALUE, h, L"GetTimes: CreateFileW must succeed");
    ::GetFileTime(h, creation, access, write);
    ::CloseHandle(h);
}

}
