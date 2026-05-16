// Blittable layouts for the native POD structs defined in public/LayerMount.h.
// Every struct uses LayoutKind.Sequential with the default pack so the C#
// compiler reproduces the same field offsets and padding the MSVC compiler
// emits on x64. Pointer-typed fields are typed as IntPtr so consumers can
// populate them with raw pointers from fixed/stackalloc/Marshal allocations
// without forcing every owning struct to be `unsafe`.
//
// Struct-size invariants (asserted at runtime in the managed wrappers where
// relevant; also enforced by the native DLL's own structSize checks for
// forward-extensible structs):
//   LM_CONFIG                 =  64 bytes
//   LM_FILE_INFO              =  72 bytes
//   LM_RESOLVED_PATH          =  40 bytes
//   LM_VOLUME_INFO            =  84 bytes (no trailing pad; fully fixed)
//   LM_STATS                  =  72 bytes
//   LM_EVENT                  =  40 bytes
//   LM_VHD_CONFIG             =  48 bytes
//   LM_VHD_LAYER_INFO         = 256 bytes
//   LM_VSS_SNAPSHOT_INFO      = 160 bytes
//   LM_IMAGE_MANIFEST_ENTRY   = 160 bytes
//   LM_IMAGE_MANIFEST         =  32 bytes
//   LM_IMAGE_PACK_OPTIONS     =  32 bytes
//   LM_IMAGE_METADATA         = 152 bytes
//   LM_MOUNT_POINT_PREP       =  40 bytes (4 BOOL + 4 implicit pad + 8
//                                          UINT64 + 16 fileId + 8 reserved)

using System;
using System.Runtime.InteropServices;

namespace LayerMount.Interop;

[StructLayout(LayoutKind.Sequential)]
internal struct LM_CONFIG
{
    public uint   structSize;
    public uint   abiVersion;
    public uint   hostCapabilities;       // LM_HOST_CAPABILITIES bitfield
    public uint   accessLogCapacity;
    public uint   pathCacheCapacity;
    public int    enableProcessTracking;  // BOOL
    public uint   lowerPathCount;
    public uint   _reserved0;
    public IntPtr upperPath;              // PCWSTR
    public IntPtr workDirPath;            // PCWSTR
    public IntPtr processRulesPath;       // PCWSTR (may be NULL)
    public IntPtr lowerPaths;             // PCWSTR const* (array of char*)
}

[StructLayout(LayoutKind.Sequential)]
internal struct LM_FILE_INFO
{
    public uint  fileAttributes;
    public uint  reparseTag;
    public ulong allocationSize;
    public ulong fileSize;
    public ulong creationTime;
    public ulong lastAccessTime;
    public ulong lastWriteTime;
    public ulong changeTime;
    public ulong indexNumber;
    public uint  hardLinks;
    public uint  eaSize;
}

[StructLayout(LayoutKind.Sequential)]
internal struct LM_RESOLVED_PATH
{
    public IntPtr absolutePath;           // PWSTR (caller-owned)
    public nuint  absolutePathChars;      // SIZE_T in/out
    public nuint  absolutePathRequired;   // SIZE_T out
    public uint   source;                 // LM_LAYER_SOURCE
    public int    lowerIndex;
    public int    isWhiteout;             // BOOL
    public uint   attributes;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct LM_VOLUME_INFO
{
    public ulong totalSize;
    public ulong freeSize;
    public fixed char volumeLabel[32];
    public uint  volumeLabelLength;       // bytes, not chars
}

[StructLayout(LayoutKind.Sequential)]
internal struct LM_STATS
{
    public ulong cacheHits;
    public ulong cacheMisses;
    public ulong copyUpCount;
    public ulong readCount;
    public ulong writeCount;
    public ulong activeHandles;
    public ulong bytesRead;
    public ulong bytesWritten;
    public ulong cleanupMetadataFailureCount;
}

[StructLayout(LayoutKind.Sequential)]
internal struct LM_EVENT
{
    public uint   type;           // LM_EVENT_TYPE
    public int    hr;             // HRESULT (S_OK for informational)
    public IntPtr relativePath;   // PCWSTR, may be NULL
    public IntPtr message;        // PCWSTR, may be NULL
    public ulong  timestamp;      // FILETIME as UINT64, UTC
    public uint   pid;
    // trailing 4 bytes of pad to 40-byte alignment (implicit)
}

[StructLayout(LayoutKind.Sequential)]
internal struct LM_VHD_CONFIG
{
    public uint   structSize;
    public uint   kind;                  // LM_VHD_KIND
    public ulong  sizeBytes;
    public IntPtr path;                  // PCWSTR
    public IntPtr parentPath;            // PCWSTR (DIFFERENCING only)
    public int    readOnly;              // BOOL
    public int    suppressDriveLetter;   // BOOL
    public uint   lifetime;              // LM_VHD_ATTACH_LIFETIME
    public uint   _reserved0;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct LM_VHD_LAYER_INFO
{
    public fixed char id[64];
    public uint       type;                   // LM_VHD_LAYER_TYPE
    // 4 bytes implicit pad before next 8-byte-aligned pointer field
    public IntPtr     path;
    public nuint      pathChars;
    public nuint      pathRequired;
    public IntPtr     parentId;
    public nuint      parentIdChars;
    public nuint      parentIdRequired;
    public IntPtr     mountStatus;
    public nuint      mountStatusChars;
    public nuint      mountStatusRequired;
    public IntPtr     volumeGuid;
    public nuint      volumeGuidChars;
    public nuint      volumeGuidRequired;
    public IntPtr     createdAt;
    public nuint      createdAtChars;
    public nuint      createdAtRequired;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct LM_VSS_SNAPSHOT_INFO
{
    public fixed char id[40];             // GUID without braces, NUL-terminated
    public Guid       vssId;
    public IntPtr     volumePath;
    public nuint      volumePathChars;
    public nuint      volumePathRequired;
    public IntPtr     devicePath;
    public nuint      devicePathChars;
    public nuint      devicePathRequired;
    public int        persistent;         // BOOL
    public ulong      createdAt;          // FILETIME as UINT64
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct LM_IMAGE_MANIFEST_ENTRY
{
    public IntPtr     imagePath;
    public nuint      imagePathChars;
    public nuint      imagePathRequired;
    public fixed char checksumHex[65];    // 64 hex + NUL
}

[StructLayout(LayoutKind.Sequential)]
internal struct LM_IMAGE_MANIFEST
{
    public uint   schemaVersion;
    public uint   entryCount;             // in: capacity; out: written
    public IntPtr entries;                // LM_IMAGE_MANIFEST_ENTRY* (NULL for sizing)
    public nuint  entriesRequired;
}

[StructLayout(LayoutKind.Sequential)]
internal struct LM_IMAGE_PACK_OPTIONS
{
    public uint   structSize;
    public uint   _reserved0;
    public IntPtr author;                 // PCWSTR; NULL/empty -> ""
    public IntPtr description;            // PCWSTR; NULL/empty -> ""
}

[StructLayout(LayoutKind.Sequential)]
internal struct LM_IMAGE_METADATA
{
    public IntPtr id;
    public nuint  idChars;
    public nuint  idRequired;
    public IntPtr parentId;
    public nuint  parentIdChars;
    public nuint  parentIdRequired;
    public IntPtr createdAt;
    public nuint  createdAtChars;
    public nuint  createdAtRequired;
    public IntPtr author;
    public nuint  authorChars;
    public nuint  authorRequired;
    public IntPtr description;
    public nuint  descriptionChars;
    public nuint  descriptionRequired;
    public uint   compression;            // LM_COMPRESSION_TYPE
    // 4 bytes implicit pad before next 8-byte-aligned ulong
    public ulong  fileCount;
    public ulong  uncompressedSize;
    public ulong  compressedSize;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct LM_MOUNT_POINT_PREP
{
    public int        directoryCreatedByUs;   // BOOL (4 bytes)
    // Implicit 4-byte pad inserted by the C compiler before the 8-byte-aligned
    // ulong; sequential layout reproduces it. Total struct size is 40 bytes.
    public ulong      volumeSerial;            // FILE_ID_INFO::VolumeSerialNumber
    public fixed byte fileId[16];              // FILE_ID_128 raw bytes
    public fixed byte reserved[8];             // ABI-growth padding; zero-init
}
