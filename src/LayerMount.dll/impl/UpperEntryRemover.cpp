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
    const DWORD upperAttrs = ::GetFileAttributesW(upperPath.c_str());
    if (upperAttrs != INVALID_FILE_ATTRIBUTES) {
        RemoveOpaqueUnlessReparsePoint(normalized, upperAttrs);
        const NTSTATUS removal = RemoveUpperEntry(upperPath, config_);
        if (!NT_SUCCESS(removal)) {
            cache_.InvalidateWithAncestors(normalized);
            return removal;
        }
    }

    if (lowerHasIt) {
        if (!whiteoutMgr_.CreateWhiteout(normalized,
                isDirectory ? WhiteoutType::Directory : WhiteoutType::File)) {
            const DWORD whErr = ::GetLastError();
            cache_.InvalidateWithAncestors(normalized);
            return whErr ? NtStatusFromWin32(whErr) : STATUS_ACCESS_DENIED;
        }
    }

    cache_.InvalidateWithAncestors(normalized);
    return STATUS_SUCCESS;
}

void UpperEntryRemover::RemoveOpaqueUnlessReparsePoint(const std::wstring& normalized,
                                                       DWORD upperAttrs) {
    if (IsEnumerableDirectory(upperAttrs) && whiteoutMgr_.IsOpaque(normalized)) {
        whiteoutMgr_.RemoveOpaque(normalized);
    }
}

}
