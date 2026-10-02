#pragma once

#include <windows.h>

#include <CppUnitTest.h>

#include <string>

namespace LayerMountTestShared {

// True when the named stream at streamPath ("file:name") opens, false when
// the open fails with ERROR_FILE_NOT_FOUND or ERROR_PATH_NOT_FOUND. Any
// other open failure fails the test, so a sharing violation or a denied
// open never reads as an absent stream.
inline bool HasStream(const std::wstring& streamPath) {
    const HANDLE stream = ::CreateFileW(streamPath.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (stream != INVALID_HANDLE_VALUE) {
        ::CloseHandle(stream);
        return true;
    }
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return false;
    }
    Microsoft::VisualStudio::CppUnitTestFramework::Assert::Fail(
        (L"The open of " + streamPath + L" failed with Win32 error " +
         std::to_wstring(error)).c_str());
    return false;
}

// True when path carries the engine's copy-up metadata stream.
inline bool HasOverlayStream(const std::wstring& path) {
    return HasStream(path + L":overlay");
}

}
