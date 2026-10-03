#include "DirectoryMerge.h"
#include "LayerPath.h"
#include "WhiteoutManager.h"
#include "NtStatusUtil.h"

#include <algorithm>
#include <optional>
#include <unordered_set>
#include <vector>

namespace LayerMount {

namespace {

std::optional<std::wstring> VisibleEntryKey(const std::wstring& dirNorm,
                                            const std::wstring& name) {
    if (name == L"." || name == L"..") return std::nullopt;
    if (WhiteoutManager::IsWhiteoutName(name)) return std::nullopt;
    std::wstring key = CaseFoldedName(name);
    if (dirNorm.empty() && IsReservedRelativePath(key)) return std::nullopt;
    return key;
}

struct DirectoryMerge {
    std::map<std::wstring, MergedEntry> entries;
    std::unordered_set<std::wstring> whitedOutNames;
};

// FindFirstFileW gives ERROR_DIRECTORY when the layer holds a file at the
// directory's path. The layer then has no directory there to list.
bool IsDirectoryAbsentError(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
           error == ERROR_DIRECTORY;
}

// Maps a failed scan's Win32 error to its NTSTATUS. An error that maps to a
// success status, such as ERROR_IO_PENDING, gives STATUS_UNSUCCESSFUL, so an
// incomplete scan never counts as complete.
NTSTATUS ScanFailureStatus(DWORD error) {
    const NTSTATUS status = NtStatusFromWin32(error);
    return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
}

struct LayerDirectoryEntry {
    std::wstring key;
    WIN32_FIND_DATAW findData;
};

struct LayerDirectoryScan {
    NTSTATUS status;
    std::vector<std::wstring> whitedOutNames;
    std::vector<LayerDirectoryEntry> entries;
};

// Reads one layer's directory in one enumeration. whitedOutNames holds the
// case-folded names that the layer's whiteouts hide, and entries holds the
// layer's visible entries. A directory absent from the layer gives
// STATUS_SUCCESS and no names. A failed scan gives the status of its Win32
// error and holds what it read before the failure.
LayerDirectoryScan ScanLayerDirectory(const std::wstring& layerPath,
                                      const std::wstring& dirNorm) {
    LayerDirectoryScan scan{STATUS_SUCCESS, {}, {}};
    WIN32_FIND_DATAW findData;
    const std::wstring searchPath = JoinLayerScanPath(layerPath, dirNorm);
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        const DWORD openError = ::GetLastError();
        if (!IsDirectoryAbsentError(openError)) {
            scan.status = ScanFailureStatus(openError);
        }
        return scan;
    }

    do {
        const std::wstring name = findData.cFileName;
        if (const std::optional<std::wstring> hidden =
                WhiteoutManager::WhitedOutNameOfEntry(name)) {
            scan.whitedOutNames.push_back(CaseFoldedName(*hidden));
            continue;
        }
        const std::optional<std::wstring> key = VisibleEntryKey(dirNorm, name);
        if (!key) continue;
        scan.entries.push_back(LayerDirectoryEntry{*key, findData});
    } while (FindNextFileW(hFind, &findData));

    // FindNextFileW returns false both at the end of the directory and on a
    // failure. FindClose can overwrite the error, so read it first.
    const DWORD scanEndError = ::GetLastError();
    FindClose(hFind);
    if (scanEndError != ERROR_NO_MORE_FILES) scan.status = ScanFailureStatus(scanEndError);
    return scan;
}

void AddLayerWhiteouts(const LayerDirectoryScan& scan, DirectoryMerge& merge) {
    merge.whitedOutNames.insert(scan.whitedOutNames.begin(), scan.whitedOutNames.end());
}

// One layer of the merge: its path, and where its entries come from.
// lowerIndex is the index of the lower, or -1 for the upper.
struct MergeLayer {
    const std::wstring& path;
    LayerSource source;
    int lowerIndex;
};

// Slot 0 is the upper, and slot i + 1 is lower i.
MergeLayer MergeLayerAt(const LayerConfig& config, size_t slot) {
    if (slot == 0) {
        return MergeLayer{config.upperPath, LayerSource::Upper, -1};
    }
    return MergeLayer{config.lowerPaths[slot - 1], LayerSource::Lower, static_cast<int>(slot - 1)};
}

// Adds the layer's entries in the directory that no higher layer lists and
// that no whiteout hides, and adds the names that the layer's whiteouts hide
// to merge.whitedOutNames. A lower's whiteouts hide its own entries too; the
// upper's whiteouts hide only the lowers' entries. A failed scan adds
// nothing and returns the scan's status.
NTSTATUS MergeLayerEntries(const MergeLayer& layer,
                           const std::wstring& dirPath,
                           DirectoryMerge& merge) {
    const LayerDirectoryScan scan = ScanLayerDirectory(layer.path, dirPath);
    if (!NT_SUCCESS(scan.status)) return scan.status;

    if (layer.source == LayerSource::Lower) AddLayerWhiteouts(scan, merge);
    for (const LayerDirectoryEntry& entry : scan.entries) {
        if (merge.entries.count(entry.key) || merge.whitedOutNames.count(entry.key)) continue;
        merge.entries[entry.key] = MergedEntry{entry.findData, layer.source, layer.lowerIndex};
    }
    if (layer.source == LayerSource::Upper) AddLayerWhiteouts(scan, merge);
    return STATUS_SUCCESS;
}

// Whether the layer adds nothing to the directory, and no layer below it
// does either. A lower whiteout hides the lower's own entries. A lower link
// at a path that a higher layer holds adds nothing.
bool HidesLayerAndBelow(const MergeLayer& layer, const LayerAncestry& ancestry) {
    return layer.source == LayerSource::Lower &&
           (ancestry.whitedOut || ancestry.linkUnderHigherEntry);
}

// Whether the layers below the layer add nothing to the directory. An upper
// whiteout hides the lowers, as it does for an open.
bool HidesLayersBelow(const MergeLayer& layer, const LayerAncestry& ancestry) {
    return ancestry.opaque || ancestry.nonDirectoryOrLink ||
           (layer.source == LayerSource::Upper && ancestry.whitedOut);
}

// Merges the directory at dirPath across the layers in slots below
// slotLimit, in priority order. ancestryAt(slot, layer) gives the ancestry
// of the directory in that layer. The result holds the ancestry of each
// layer that the merge reached.
template <typename AncestryAt>
MergedDirectoryWithAncestry MergeLayers(const LayerConfig& config,
                                        const std::wstring& dirPath,
                                        size_t slotLimit,
                                        const AncestryAt& ancestryAt) {
    MergedDirectoryWithAncestry result{MergedDirectory{STATUS_SUCCESS, {}}, dirPath, {}};
    DirectoryMerge merge;
    const size_t slotCount = (std::min)(slotLimit, config.lowerPaths.size() + 1);
    for (size_t slot = 0; slot < slotCount; ++slot) {
        const MergeLayer layer = MergeLayerAt(config, slot);
        result.layers.push_back(ancestryAt(slot, layer));
        const LayerAncestry& ancestry = result.layers.back();
        if (HidesLayerAndBelow(layer, ancestry)) break;
        const NTSTATUS status = MergeLayerEntries(layer, dirPath, merge);
        if (!NT_SUCCESS(status)) {
            result.merged.status = status;
            return result;
        }
        if (HidesLayersBelow(layer, ancestry)) break;
    }
    result.merged.entries = std::move(merge.entries);
    return result;
}

}

MergedDirectoryWithAncestry MergeDirectoryWithAncestry(const LayerConfig& config,
                                                       const WhiteoutManager& whiteoutMgr,
                                                       const std::wstring& dirRelativePath) {
    const std::wstring dirNorm = NormalizePath(dirRelativePath);
    if ((!dirNorm.empty() && !IsSafeRelativePath(dirNorm)) || IsReservedRelativePath(dirNorm)) {
        return MergedDirectoryWithAncestry{MergedDirectory{STATUS_SUCCESS, {}}, dirNorm, {}};
    }
    return MergeLayers(config, dirNorm, config.lowerPaths.size() + 1,
                       [&](size_t, const MergeLayer& layer) {
                           return LayerAncestryOf(
                               config, LayerDirectory{whiteoutMgr, layer.path, dirNorm},
                               layer.lowerIndex);
                       });
}

MergedDirectoryWithAncestry MergeChildDirectory(const LayerConfig& config,
                                                const WhiteoutManager& whiteoutMgr,
                                                const MergedDirectoryWithAncestry& parent,
                                                const std::wstring& childName) {
    const std::wstring childPath = parent.dirPath + L"\\" + childName;
    return MergeLayers(config, childPath, parent.layers.size(),
                       [&](size_t slot, const MergeLayer& layer) {
                           return StepLayerAncestry(
                               config, LayerDirectory{whiteoutMgr, layer.path, childPath},
                               layer.lowerIndex, parent.layers[slot]);
                       });
}

MergedDirectory MergeDirectoryAcrossLayers(const LayerConfig& config,
                                           const WhiteoutManager& whiteoutMgr,
                                           const std::wstring& dirRelativePath) {
    return MergeDirectoryWithAncestry(config, whiteoutMgr, dirRelativePath).merged;
}

}
