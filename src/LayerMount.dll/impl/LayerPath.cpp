#include "LayerPath.h"
#include "WhiteoutManager.h"

namespace LayerMount {

namespace fs = std::filesystem;

std::wstring LayerDirWithSeparator(const std::wstring& layerPath) {
    return !layerPath.empty() && layerPath.back() == L'\\' ? layerPath : layerPath + L"\\";
}

std::wstring JoinLayerPath(const std::wstring& layerPath,
                           const std::wstring& relativePath) {
    return LayerDirWithSeparator(layerPath) + relativePath;
}

std::wstring JoinLayerScanPath(const std::wstring& layerPath,
                               const std::wstring& dirRelativePath) {
    return JoinLayerPath(layerPath,
                         dirRelativePath.empty() ? L"*" : dirRelativePath + L"\\*");
}

bool HasNonDirectorySelfOrAncestorInLayer(const std::wstring& layerPath,
                                          const std::wstring& dirRelativePath) {
    fs::path walked;
    for (const fs::path& component : fs::path(dirRelativePath)) {
        walked /= component;
        const DWORD attrs = GetFileAttributesW(JoinLayerPath(layerPath, walked.wstring()).c_str());
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
