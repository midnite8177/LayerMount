// Public enums mirroring the native C ABI in public/LayerMount.h.
// Cross the ABI as UINT32 per the header's enum conventions.

using System;

namespace LayerMount;

/// <summary>
/// Host-declared capabilities of the target file system and the upper
/// layer, set on <see cref="LayerMountConfig.Capabilities"/>. Combine
/// members with <c>|</c>. Clearing a bit turns on the fallback
/// documented on that member.
/// </summary>
[Flags]
public enum HostCapabilities : uint
{
    /// <summary>No capabilities declared.</summary>
    None             = 0x00000000u,

    /// <summary>
    /// NTFS alternate data streams. Without it, the metadata dispatcher
    /// stores copy-up and opaque markers as sidecar JSON files instead
    /// of alternate data streams.
    /// </summary>
    Ads              = 0x00000001u,

    /// <summary>
    /// Reparse points on the upper layer. Without it, renaming a
    /// directory whose source is a reparse point falls through to a
    /// full recursive copy. The data survives, but the link semantics
    /// do not. With it, a short-circuited copy-up preserves the reparse
    /// point.
    /// </summary>
    ReparsePoints    = 0x00000002u,

    /// <summary>
    /// Sparse files on the upper layer. Without it, metacopy for a file
    /// above the metacopy size threshold copies its data in full at
    /// copy-up time instead of deferring the data to first read.
    /// </summary>
    SparseFiles      = 0x00000004u,

    /// <summary>
    /// Multiple named data streams per file. Clearing it records the
    /// limitation; it does not currently gate any engine fallback.
    /// </summary>
    MultipleStreams  = 0x00000008u,

    /// <summary>
    /// NTFS security descriptors on the upper layer. Without it, a
    /// security read returns a synthetic security descriptor and a
    /// security write silently no-ops instead of failing.
    /// </summary>
    NtfsAcls         = 0x00000010u,

    /// <summary>
    /// Case-sensitive path lookup on the upper layer. Clearing it
    /// records the limitation; it does not currently gate any engine
    /// fallback.
    /// </summary>
    CaseSensitive    = 0x00000020u,
}

/// <summary>
/// Which layer a resolved path was found in.
/// </summary>
public enum LayerSource : uint
{
    /// <summary>The path was not found in either layer.</summary>
    None  = 0,

    /// <summary>The path was found in the upper (writable) layer.</summary>
    Upper = 1,

    /// <summary>The path was found in a lower (read-only) layer.</summary>
    Lower = 2,
}

/// <summary>
/// Event categories emitted through <see cref="LayerMount.Event"/>.
/// </summary>
public enum LayerMountEventType : uint
{
    /// <summary>
    /// A non-fatal degradation, such as the reparse-point capability
    /// fallback.
    /// </summary>
    Warning          = 0,

    /// <summary>A file or directory was copied up from a lower layer into the upper.</summary>
    CopyUp           = 1,

    /// <summary>A whiteout marker was written.</summary>
    WhiteoutCreated  = 2,

    /// <summary>A process-tracker rule denied a request.</summary>
    AccessDenied     = 3,
}

/// <summary>
/// VHD backing kind for <see cref="VhdApi.Create"/> and
/// <see cref="VhdApi.Open"/>.
/// </summary>
public enum VhdKind : uint
{
    /// <summary>A fixed-size VHD; its full capacity is allocated on disk at creation.</summary>
    Fixed        = 0,

    /// <summary>A dynamically expanding VHD; disk usage grows as data is written.</summary>
    Dynamic      = 1,

    /// <summary>
    /// A differencing VHD that stores only the delta from a parent VHD.
    /// Requires a parent, passed as <see cref="VhdApi.Create"/>'s
    /// <c>parentPath</c> parameter.
    /// </summary>
    Differencing = 2,
}

/// <summary>
/// What happens to a VHD attachment at process exit.
/// </summary>
public enum VhdAttachLifetime : uint
{
    /// <summary>The attachment survives process exit.</summary>
    Permanent     = 0,

    /// <summary>The VHD detaches automatically when the last handle to it closes.</summary>
    ProcessScoped = 1,
}

/// <summary>
/// Layer image compression algorithm.
/// </summary>
public enum CompressionType : uint
{
    /// <summary>The image's data section is stored uncompressed.</summary>
    None = 0,

    /// <summary>The image's data section is compressed with Zstandard.</summary>
    Zstd = 1,
}

/// <summary>
/// Storage-backend type of a layer, returned per-entry from
/// <see cref="VhdApi.ListLayers"/>.
/// </summary>
public enum VhdLayerType : uint
{
    /// <summary>The layer is a plain directory.</summary>
    Directory = 0,

    /// <summary>The layer is a directory inside an attached VHD or VHDX.</summary>
    Vhd       = 1,

    /// <summary>The layer is a directory inside a VSS snapshot.</summary>
    Vss       = 2,
}

/// <summary>
/// Process-tracker operation category. One member per kind of file
/// operation the process tracker observes and records in its access
/// log.
/// </summary>
public enum LayerMountOperationType : uint
{
    /// <summary>A create-file request.</summary>
    Create        = 0,

    /// <summary>An open-file request.</summary>
    Open          = 1,

    /// <summary>A read request.</summary>
    Read          = 2,

    /// <summary>A write request.</summary>
    Write         = 3,

    /// <summary>An overwrite request that truncates an existing file.</summary>
    Overwrite     = 4,

    /// <summary>A delete request.</summary>
    Delete        = 5,

    /// <summary>A rename request.</summary>
    Rename        = 6,

    /// <summary>A file-information query.</summary>
    GetInfo       = 7,

    /// <summary>A file-information update.</summary>
    SetInfo       = 8,

    /// <summary>A set-end-of-file (resize) request.</summary>
    SetSize       = 9,

    /// <summary>A security-descriptor query.</summary>
    GetSecurity   = 10,

    /// <summary>A security-descriptor update.</summary>
    SetSecurity   = 11,

    /// <summary>A directory-enumeration request.</summary>
    ReadDirectory = 12,

    /// <summary>A flush-buffers request.</summary>
    Flush         = 13,

    /// <summary>
    /// A handle cleanup notification. Defined for the category, but the
    /// engine does not record it in the access log today; only
    /// <see cref="Close"/> fires.
    /// </summary>
    Cleanup       = 14,

    /// <summary>A handle close notification, recorded when a file handle closes.</summary>
    Close         = 15,
}
