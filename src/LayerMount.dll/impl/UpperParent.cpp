#include "UpperParent.h"
#include "LayerPath.h"
#include "PathResolver.h"
#include "Cache.h"

namespace LayerMount {

UpperParent::UpperParent(ConfigRef config,
                         const PathResolver& pathResolver,
                         Cache& cache,
                         CopyUpDirectoryFn copyUpDirectory)
    : config_(config.Get())
    , pathResolver_(pathResolver)
    , cache_(cache)
    , copyUpDirectory_(std::move(copyUpDirectory)) {}

NTSTATUS UpperParent::Ensure(const CallerPath& path) const {
    const std::wstring trimmedCallerPath = NormalizePathPreserveCase(path.Text());
    const size_t separator = trimmedCallerPath.find_last_of(L'\\');
    if (separator == std::wstring::npos) {
        return STATUS_SUCCESS;
    }
    const CallerPath parent(trimmedCallerPath.substr(0, separator));
    const std::wstring parentNorm = NormalizePath(parent.Text());
    if (pathResolver_.ExistsInUpper(parentNorm)) {
        return STATUS_SUCCESS;
    }

    const ResolvedPath merged = pathResolver_.ResolvePath(parentNorm);
    if (merged.Found() && merged.source == LayerSource::Lower) {
        if ((merged.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }
        return copyUpDirectory_(parentNorm);
    }
    if (!merged.Found() && pathResolver_.ResolveLowerPath(parentNorm).Found()) {
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }

    NTSTATUS status = Ensure(parent);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = CreateDirectoryOrUseExisting(
        BuildUpperPathPreserveCase(config_.upperPath, parent.Text()));
    if (!NT_SUCCESS(status)) {
        return status;
    }
    cache_.InvalidateWithAncestors(parentNorm);
    return STATUS_SUCCESS;
}

}
