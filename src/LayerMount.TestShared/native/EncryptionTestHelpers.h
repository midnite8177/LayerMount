#pragma once

#include <windows.h>

#include <CppUnitTest.h>

#include <string>

namespace LayerMountTestShared {

// Encrypts the file or directory at path. Logs a skip with the error and
// returns false when EFS refuses.
inline bool EncryptedOrSkipped(const std::wstring& path) {
    if (::EncryptFileW(path.c_str())) {
        return true;
    }
    const DWORD err = ::GetLastError();
    Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage(
        (L"[SKIP] EncryptFileW on " + path + L" failed (error " + std::to_wstring(err) +
         L"). EFS is unavailable or the user has no certificate.").c_str());
    return false;
}

}
