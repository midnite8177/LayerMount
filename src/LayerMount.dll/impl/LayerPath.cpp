#include "LayerPath.h"
#include "NtStatusUtil.h"
#include "WhiteoutManager.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace LayerMount {

namespace fs = std::filesystem;

std::wstring DirWithSeparator(const std::wstring& dirPath) {
    return !dirPath.empty() && dirPath.back() == L'\\' ? dirPath : dirPath + L"\\";
}

std::wstring JoinDirPath(const std::wstring& dirPath,
                         const std::wstring& relativePath) {
    return DirWithSeparator(dirPath) + relativePath;
}

std::wstring NormalizePathPreserveCase(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }

    std::wstring result = path;
    std::replace(result.begin(), result.end(), L'/', L'\\');

    size_t start = 0;
    while (start < result.size() && result[start] == L'\\') {
        ++start;
    }
    if (start > 0) {
        result = result.substr(start);
    }

    while (!result.empty() && result.back() == L'\\') {
        result.pop_back();
    }

    return result;
}

std::wstring BuildUpperPathPreserveCase(const std::wstring& upperRoot,
                                        const std::wstring& relativePath) {
    std::wstring preserved = NormalizePathPreserveCase(relativePath);
    if (preserved.empty()) return upperRoot;
    return JoinDirPath(upperRoot, preserved);
}

std::wstring WithStoredLeafName(const std::wstring& targetPath,
                                const std::wstring& entryPath) {
    WIN32_FIND_DATAW fd{};
    HANDLE find = ::FindFirstFileW(entryPath.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        return targetPath;
    }
    ::FindClose(find);
    return targetPath.substr(0, targetPath.find_last_of(L'\\') + 1) + fd.cFileName;
}

NTSTATUS MoveUpperEntry(const std::wstring& from,
                        const std::wstring& to,
                        ReplaceExisting replace) {
    const DWORD flags = replace == ReplaceExisting::Yes ? MOVEFILE_REPLACE_EXISTING : 0;
    if (::MoveFileExW(from.c_str(), to.c_str(), flags)) {
        return STATUS_SUCCESS;
    }
    const DWORD moveErr = ::GetLastError();
    if (moveErr != ERROR_ACCESS_DENIED) {
        return NtStatusFromWin32(moveErr);
    }

    // When a copy-up carried an inherited deny-write ACE from the lower
    // parent to the upper parent, MoveFileExW fails the destination DACL
    // check although the engine owns the upper entry. With SE_RESTORE_NAME,
    // a handle opened with backup semantics skips that check through
    // FileRenameInfo.
    HANDLE src = ::CreateFileW(from.c_str(),
        GENERIC_READ | DELETE | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (src == INVALID_HANDLE_VALUE) {
        return NtStatusFromWin32(::GetLastError());
    }
    const size_t pathBytes = to.size() * sizeof(wchar_t);
    std::vector<BYTE> buf(sizeof(FILE_RENAME_INFO) + pathBytes);
    auto* ri = reinterpret_cast<FILE_RENAME_INFO*>(buf.data());
    ri->ReplaceIfExists = replace == ReplaceExisting::Yes ? TRUE : FALSE;
    ri->RootDirectory   = nullptr;
    ri->FileNameLength  = static_cast<DWORD>(pathBytes);
    std::memcpy(ri->FileName, to.data(), pathBytes);
    const BOOL renamed = ::SetFileInformationByHandle(
        src, FileRenameInfo, ri, static_cast<DWORD>(buf.size()));
    const DWORD renameErr = renamed ? 0 : ::GetLastError();
    ::CloseHandle(src);
    if (!renamed) {
        return NtStatusFromWin32(renameErr);
    }
    return STATUS_SUCCESS;
}

NTSTATUS CreateDirectoryOrUseExisting(const std::wstring& path) {
    if (::CreateDirectoryW(path.c_str(), nullptr)) {
        return STATUS_SUCCESS;
    }
    const DWORD createErr = ::GetLastError();
    if (createErr != ERROR_ALREADY_EXISTS) {
        return NtStatusFromWin32(createErr);
    }
    const DWORD existingAttrs = ::GetFileAttributesW(path.c_str());
    if (existingAttrs == INVALID_FILE_ATTRIBUTES) {
        return NtStatusFromWin32(::GetLastError());
    }
    if ((existingAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return STATUS_OBJECT_NAME_COLLISION;
    }
    return STATUS_SUCCESS;
}

std::wstring JoinLayerScanPath(const std::wstring& layerPath,
                               const std::wstring& dirRelativePath) {
    return JoinDirPath(layerPath,
                       dirRelativePath.empty() ? L"*" : dirRelativePath + L"\\*");
}

bool HasNonDirectorySelfOrAncestorInLayer(const std::wstring& layerPath,
                                          const std::wstring& dirRelativePath) {
    fs::path walked;
    for (const fs::path& component : fs::path(dirRelativePath)) {
        walked /= component;
        const DWORD attrs = GetFileAttributesW(JoinDirPath(layerPath, walked.wstring()).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = ::GetLastError();
            return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
        }
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return true;
        }
    }
    return false;
}

LowerVisibility LowersBelow(const LayerDirectory& dir, DirectoryProbe probe) {
    if (dir.whiteoutMgr.HasOpaqueSelfOrAncestorInLayer(dir.dirNorm, dir.layerPath)) {
        return LowerVisibility::HiddenByOpaqueMarker;
    }
    if (probe == DirectoryProbe::Missed &&
        HasNonDirectorySelfOrAncestorInLayer(dir.layerPath, dir.dirNorm)) {
        return LowerVisibility::HiddenByNonDirectory;
    }
    return LowerVisibility::Visible;
}

}
