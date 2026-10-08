#pragma once

#include "LayerMount.h"

namespace LayerMount {

class WhiteoutManager;
class Cache;
class CallerPath;

class PathResolver {
public:
    PathResolver(ConfigRef config,
                 WhiteoutManager& whiteoutMgr,
                 Cache& cache);

    ResolvedPath ResolvePath(const std::wstring& relativePath) const;

    // Returns the view path that path names in the merged view, where a
    // relative symbolic link in any layer resolves as overlayfs resolves
    // it. Drops each `.` and empty component of path, then walks it from
    // the root through the entries the view shows. At a relative symbolic
    // link, resolves the link's target from the link's parent directory,
    // puts the result in place of the components up to and including the
    // link, and walks again from the root. In the target, `.` is the same
    // directory, `..` is the parent, `..` at the root is the root, and a
    // leading separator starts at the root. The walk stops at a missing
    // entry, at a file, and at a junction or any other link, which Win32
    // follows on the physical path. finalLink says whether a link at the
    // last component is followed. The view path keeps the case of path and
    // of the link targets, and keeps the stream suffix of path. A path with
    // a `..` component comes back unwalked, as NormalizePathPreserveCase
    // gives it, so the caller's own checks refuse it. More than
    // kMaxLinkHops links fail with STATUS_REPARSE_POINT_NOT_RESOLVED. Costs
    // a ResolvePath per walked component and a reparse read per reparse
    // point.
    ViewLookup ViewPathThroughLinks(const std::wstring& path, FinalLink finalLink) const;

    // Returns the first visible lower that holds the path. An upper entry, a
    // whiteout or an opaque marker at the path itself does not hide it. A
    // whiteout, an opaque marker, a non-directory or a link at an ancestor in
    // the upper does, and the upper root counts as an ancestor of every path.
    ResolvedPath ResolveLowerPath(const std::wstring& relativePath) const;

    CreateResolution ResolveForCreate(const std::wstring& relativePath) const;

    bool ExistsInUpper(const std::wstring& relativePath) const;

    // The attributes of the upper entry at relativePath, a link itself and
    // not its target. INVALID_FILE_ATTRIBUTES when the upper holds none.
    DWORD UpperAttributes(const std::wstring& relativePath) const;

    // The upper path in lowercase, for a lookup of an entry the upper may
    // hold. A new upper entry takes its name from GetUpperPathForNewEntry or
    // GetUpperPathForCopyUp instead.
    std::wstring GetUpperPath(const std::wstring& relativePath) const;

    // The upper path for an entry the caller names, as a create or a rename
    // destination does. The entry takes its name in path's case, as
    // overlayfs keeps a name as given.
    std::wstring GetUpperPathForNewEntry(const CallerPath& path) const;

    // The upper path for a copy-up of lowerSource, the lower entry at
    // relativePath. The upper entry takes the name lowerSource has on disk.
    std::wstring GetUpperPathForCopyUp(const std::wstring& relativePath,
                                       const ResolvedPath& lowerSource) const;

    // The upper path of the upper entry at relativePath, with the last
    // component as the file system stores it.
    std::wstring GetStoredUpperPath(const std::wstring& relativePath) const;

    // The upper path of the rename source at relativePath: GetStoredUpperPath
    // when the upper holds the source, and GetUpperPathForCopyUp of the
    // lower source otherwise.
    std::wstring GetUpperPathForRenameSource(const std::wstring& relativePath) const;

    bool HasTypeConflict(const std::wstring& relativePath) const;

    // IsReservedOverlayPath with this resolver's layers. normalized must be
    // the output of NormalizePath.
    bool IsReservedPath(const std::wstring& normalized) const;

    const LayerConfig& Config() const { return config_; }

private:
    // Whether the path is safe and not reserved. normalized must be the
    // output of NormalizePath.
    bool IsResolvablePath(const std::wstring& normalized) const;

    // The normalized form of relativePath, or no value when it is empty or
    // IsResolvablePath refuses it.
    std::optional<std::wstring> ResolvableNormalized(const std::wstring& relativePath) const;

    // When lowerWalk is not null, it receives ResolveLowerPath's result for
    // relativePath itself if the call learns it: after the walk of the lowers
    // runs, or when an opaque, non-directory or link ancestor in the upper
    // hides the lowers.
    ResolvedPath ResolvePathInternal(const std::wstring& relativePath,
                                     int redirectDepth,
                                     std::optional<ResolvedPath>* lowerWalk) const;

    ResolvedPath ResolveRoot() const;

    // The upper's entry at normalized, after any redirect it holds, or no
    // value when the upper has no entry there. Caches an entry that it finds.
    std::optional<ResolvedPath> ResolveInUpper(const std::wstring& normalized,
                                               int redirectDepth) const;

    enum class UpperHiding {
        None,
        Whiteout,
        OpaqueMarker,
        NonDirectoryOrLink,
    };

    // What in the upper hides the path from the lowers, after the upper's
    // probe of the path missed: a whiteout for the path or for an ancestor,
    // an opaque ancestor, or a non-directory or link ancestor.
    UpperHiding HidingInUpper(const std::wstring& normalized) const;

    // Like HidingInUpper, but only the upper's ancestors of the path count.
    // A whiteout for the path itself does not.
    UpperHiding UpperAncestorHiding(const std::wstring& normalized) const;

    // Returns the first lower at or after firstLower, in priority order,
    // that holds the path. Returns an empty result when no visible lower
    // holds it. The path must be normalized and safe. A whiteout for the
    // path or for an ancestor in a lower hides the path in that lower and in
    // every deeper lower. An opaque ancestor or a non-directory or link
    // ancestor in a lower hides the path in the deeper lowers only. A lower
    // link at an ancestor that a higher layer also holds hides the path in
    // that lower and in every deeper lower.
    ResolvedPath FindInLowers(const std::wstring& normalized, size_t firstLower) const;

    // Logs the first visible lower below the hit that holds the path with
    // the other type, file or directory. The hit stays the result.
    void LogTypeConflictInDeeperLowers(const std::wstring& normalized,
                                       const ResolvedPath& hit) const;

    static constexpr int kMaxRedirectDepth = 40;
    // MAXSYMLINKS in Linux, the most links one lookup follows.
    static constexpr int kMaxLinkHops = 40;

    const LayerConfig& config_;
    WhiteoutManager& whiteoutMgr_;
    Cache& cache_;
};

}
