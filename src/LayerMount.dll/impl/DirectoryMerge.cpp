#include "DirectoryMerge.h"
#include "LayerPath.h"
#include "WhiteoutManager.h"
#include "NtStatusUtil.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace LayerMount {

namespace {

// How a scan reads a .wh. name. AreMarkers is for a directory of the
// overlay, where the name is a whiteout or the opaque marker. AreOrdinary is
// for a directory in a link or under one, where the name is an ordinary
// entry, as in overlayfs.
enum class WhiteoutNames {
    AreMarkers,
    AreOrdinary,
};

// A link, or a component that the walk cannot read and that can be a link,
// at the directory or at an ancestor makes a .wh. name there ordinary. A
// file there leaves the layer with no directory to scan.
WhiteoutNames WhiteoutNamesIn(const LayerAncestry& ancestry) {
    return EndsAtLinkOrUnreadable(ancestry.firstNonDirectory) ? WhiteoutNames::AreOrdinary
                                                              : WhiteoutNames::AreMarkers;
}

std::optional<std::wstring> VisibleEntryKey(const std::wstring& dirNorm,
                                            const std::wstring& name) {
    if (name == L"." || name == L"..") return std::nullopt;
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

// Adds one entry that a scan reads to the scan.
using ScanEntryAdder = void (*)(const std::wstring& dirNorm,
                                const WIN32_FIND_DATAW& findData,
                                LayerDirectoryScan& scan);

void AddOrdinaryEntry(const std::wstring& dirNorm,
                      const WIN32_FIND_DATAW& findData,
                      LayerDirectoryScan& scan) {
    const std::optional<std::wstring> key = VisibleEntryKey(dirNorm, findData.cFileName);
    if (key) scan.entries.push_back(LayerDirectoryEntry{*key, findData});
}

// Adds a whiteout's hidden name to whitedOutNames, skips the opaque marker,
// and adds any other entry as AddOrdinaryEntry does.
void AddMarkerOrEntry(const std::wstring& dirNorm,
                      const WIN32_FIND_DATAW& findData,
                      LayerDirectoryScan& scan) {
    const std::wstring name = findData.cFileName;
    if (const std::optional<std::wstring> hidden = WhiteoutManager::WhitedOutNameOfEntry(name)) {
        scan.whitedOutNames.push_back(CaseFoldedName(*hidden));
        return;
    }
    if (WhiteoutManager::IsWhiteoutName(name)) return;
    AddOrdinaryEntry(dirNorm, findData, scan);
}

// Reads one layer's directory in one enumeration. whitedOutNames holds the
// case-folded names that the layer's whiteouts hide, and entries holds the
// layer's visible entries. With WhiteoutNames::AreOrdinary, whitedOutNames
// stays empty and entries holds the .wh. names. A directory absent from the
// layer gives STATUS_SUCCESS and no names. A failed scan gives the status of
// its Win32 error and holds what it read before the failure.
LayerDirectoryScan ScanLayerDirectory(const std::wstring& layerPath,
                                      const std::wstring& dirNorm,
                                      WhiteoutNames whiteoutNames) {
    const ScanEntryAdder addEntry =
        whiteoutNames == WhiteoutNames::AreMarkers ? AddMarkerOrEntry : AddOrdinaryEntry;
    LayerDirectoryScan scan{STATUS_SUCCESS, {}, {}};
    WIN32_FIND_DATAW findData;
    const std::wstring searchPath = JoinLayerScanPath(layerPath, dirNorm);
    HANDLE hFind = FindFirstFileW(WithExtendedPrefix(searchPath).c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        const DWORD openError = ::GetLastError();
        if (!IsDirectoryAbsentError(openError)) {
            scan.status = ScanFailureStatus(openError);
        }
        return scan;
    }

    do {
        addEntry(dirNorm, findData, scan);
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
                           WhiteoutNames whiteoutNames,
                           DirectoryMerge& merge) {
    const LayerDirectoryScan scan = ScanLayerDirectory(layer.path, dirPath, whiteoutNames);
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
    return ancestry.opaque || ancestry.firstNonDirectory != WalkStop::None ||
           (layer.source == LayerSource::Upper && ancestry.whitedOut);
}

// Walks the layers in slots below slotLimit in priority order, and calls
// scanLayer(layer, ancestry) on each layer that the merge of the directory
// scans. ancestryAt(slot, layer) gives the ancestry of the directory in that
// layer, for each layer that the walk reaches. The walk stops at a layer
// that hides itself and the layers below it, after a layer that hides the
// layers below it, and when scanLayer returns false.
template <typename AncestryAt, typename ScanLayer>
void ForEachScannedLayer(const LayerConfig& config,
                         size_t slotLimit,
                         const AncestryAt& ancestryAt,
                         const ScanLayer& scanLayer) {
    const size_t slotCount = (std::min)(slotLimit, config.lowerPaths.size() + 1);
    for (size_t slot = 0; slot < slotCount; ++slot) {
        const MergeLayer layer = MergeLayerAt(config, slot);
        const LayerAncestry& ancestry = ancestryAt(slot, layer);
        if (HidesLayerAndBelow(layer, ancestry)) return;
        if (!scanLayer(layer, ancestry)) return;
        if (HidesLayersBelow(layer, ancestry)) return;
    }
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
    ForEachScannedLayer(
        config, slotLimit,
        [&](size_t slot, const MergeLayer& layer) -> const LayerAncestry& {
            result.layers.push_back(ancestryAt(slot, layer));
            return result.layers.back();
        },
        [&](const MergeLayer& layer, const LayerAncestry& ancestry) {
            result.merged.status =
                MergeLayerEntries(layer, dirPath, WhiteoutNamesIn(ancestry), merge);
            return NT_SUCCESS(result.merged.status);
        });
    if (NT_SUCCESS(result.merged.status)) {
        result.merged.entries = std::move(merge.entries);
    }
    return result;
}

}

bool IsInLinkTarget(const LayerConfig& config,
                    const WhiteoutManager& whiteoutMgr,
                    const std::wstring& dirNorm) {
    return FindLinkInView(config, whiteoutMgr, dirNorm).stop != LinkStop::None;
}

bool IsReservedOverlayPath(const LayerConfig& config,
                           const WhiteoutManager& whiteoutMgr,
                           const std::wstring& normalized) {
    if (IsRootSidecarPath(normalized)) {
        return true;
    }
    const std::optional<std::wstring_view> markerParent = ParentOfFirstMarkerSegment(normalized);
    if (!markerParent.has_value()) {
        return false;
    }
    const bool markerAtRoot = markerParent->empty();
    return markerAtRoot || !IsInLinkTarget(config, whiteoutMgr, std::wstring(*markerParent));
}

MergedDirectoryWithAncestry MergeDirectoryWithAncestry(const LayerConfig& config,
                                                       const WhiteoutManager& whiteoutMgr,
                                                       const std::wstring& dirRelativePath) {
    const std::wstring dirNorm = NormalizePath(dirRelativePath);
    if ((!dirNorm.empty() && !IsSafeRelativePath(dirNorm)) ||
        IsReservedOverlayPath(config, whiteoutMgr, dirNorm)) {
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
