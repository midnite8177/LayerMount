// Public enums mirroring the native C ABI in public/LayerMount.h.
// Cross the ABI as UINT32 per the header's enum conventions.

using System;

namespace LayerMount;

[Flags]
public enum HostCapabilities : uint
{
    None             = 0x00000000u,
    Ads              = 0x00000001u,
    ReparsePoints    = 0x00000002u,
    SparseFiles      = 0x00000004u,
    MultipleStreams  = 0x00000008u,
    NtfsAcls         = 0x00000010u,
    CaseSensitive    = 0x00000020u,
}

public enum LayerSource : uint
{
    None  = 0,
    Upper = 1,
    Lower = 2,
}

public enum LayerMountEventType : uint
{
    Warning          = 0,
    CopyUp           = 1,
    WhiteoutCreated  = 2,
    AccessDenied     = 3,
}

public enum VhdKind : uint
{
    Fixed        = 0,
    Dynamic      = 1,
    Differencing = 2,
}

public enum VhdAttachLifetime : uint
{
    Permanent     = 0,
    ProcessScoped = 1,
}

public enum CompressionType : uint
{
    None = 0,
    Zstd = 1,
}

public enum VhdLayerType : uint
{
    Directory = 0,
    Vhd       = 1,
    Vss       = 2,
}

public enum LayerMountOperationType : uint
{
    Create        = 0,
    Open          = 1,
    Read          = 2,
    Write         = 3,
    Overwrite     = 4,
    Delete        = 5,
    Rename        = 6,
    GetInfo       = 7,
    SetInfo       = 8,
    SetSize       = 9,
    GetSecurity   = 10,
    SetSecurity   = 11,
    ReadDirectory = 12,
    Flush         = 13,
    Cleanup       = 14,
    Close         = 15,
}
