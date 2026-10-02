#pragma once

#include "LayerMount.h"

#include <functional>
#include <string>
#include <utility>

namespace LayerMount {

class PathResolver;
class Cache;

// A path in the case the caller wrote it. A normalized path is lowercase,
// so the constructor is explicit: a call site that holds only a normalized
// path has to name the conversion.
class CallerPath {
public:
    explicit CallerPath(std::wstring text) : text_(std::move(text)) {}

    const std::wstring& Text() const noexcept { return text_; }

private:
    std::wstring text_;
};

// Copies up the lower directory at a normalized path.
using CopyUpDirectoryFn = std::function<NTSTATUS(const std::wstring& normalizedPath)>;

// Makes the parent of a path exist in the upper.
class UpperParent {
public:
    UpperParent(ConfigRef config,
                const PathResolver& pathResolver,
                Cache& cache,
                CopyUpDirectoryFn copyUpDirectory);

    // Copies up, through copyUpDirectory, a parent that the merged view
    // shows as a lower directory, so the upper entry takes the lower's
    // name. Returns STATUS_OBJECT_PATH_NOT_FOUND and writes nothing when
    // the merged view shows the parent or an ancestor as a lower file, as
    // overlayfs fails with ENOTDIR. Returns the same status when the merged
    // view hides the parent or an ancestor that a lower holds, as overlayfs
    // fails with ENOENT. Creates a parent in no layer in path's case after
    // its own parent.
    NTSTATUS Ensure(const CallerPath& path) const;

private:
    const LayerConfig& config_;
    const PathResolver& pathResolver_;
    Cache& cache_;
    CopyUpDirectoryFn copyUpDirectory_;
};

}
