#include "UpperEntryRemover.h"
#include "LayerPath.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"
#include "NtStatusUtil.h"

namespace LayerMount {

UpperEntryRemover::UpperEntryRemover(ConfigRef config,
                                     PathResolver& pathResolver,
                                     WhiteoutManager& whiteoutMgr,
                                     Cache& cache)
    : config_(config.Get())
    , pathResolver_(pathResolver)
    , whiteoutMgr_(whiteoutMgr)
    , cache_(cache) {}

NTSTATUS UpperEntryRemover::Remove(const std::wstring& normalized) {
    const ResolvedPath resolved = pathResolver_.ResolvePath(normalized);
    const bool isDirectory = resolved.Found() &&
        (resolved.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool lowerHasIt = pathResolver_.ResolveLowerPath(normalized).Found();

    const std::wstring upperPath = pathResolver_.GetUpperPath(normalized);
    bool upperExists = false;
    EntryKind kind = EntryKind::File;
    const NTSTATUS probe = ProbeUpperEntry(upperPath, &upperExists, &kind);
    if (!NT_SUCCESS(probe)) {
        return probe;
    }
    if (upperExists) {
        // A marker check through a link reads the markers of its target.
        if (kind == EntryKind::Directory && whiteoutMgr_.IsOpaque(normalized)) {
            whiteoutMgr_.RemoveOpaque(normalized);
        }
        const NTSTATUS removal = RemoveUpperEntryOfKind(upperPath, kind, config_);
        if (!NT_SUCCESS(removal)) {
            cache_.InvalidateWithAncestors(normalized);
            return removal;
        }
    }

    if (lowerHasIt) {
        const NTSTATUS whiteout = whiteoutMgr_.CreateWhiteout(normalized,
            isDirectory ? WhiteoutType::Directory : WhiteoutType::File);
        if (!NT_SUCCESS(whiteout)) {
            cache_.InvalidateWithAncestors(normalized);
            return whiteout;
        }
    }

    cache_.InvalidateWithAncestors(normalized);
    return STATUS_SUCCESS;
}

}
