/* LayerMount.h -- Public C ABI for LayerMount.dll.
 *
 * SCOPE
 *   This is the only public header the DLL exposes. Every symbol declared
 *   here is stable within a single LM_ABI_VERSION. Internal headers
 *   under abi/ and impl/ are not installed and must never be included by
 *   DLL consumers.
 *
 * THREAD-SAFETY
 *   Every function declared in this header is safe to call from any
 *   thread on any handle at any time. Internal synchronization is the
 *   DLL's responsibility. Callers need not serialize calls. Two callers
 *   may concurrently read and write the same LM_FILE_HANDLE; the DLL
 *   serializes inside the call. Callbacks supplied to
 *   LayerMountSetEventCallback and LayerMountMergeDirectory may fire on
 *   arbitrary DLL-internal threads, and must not re-enter the DLL
 *   synchronously on the same handle.
 *
 * ERROR MODEL
 *   Every function returns HRESULT. S_OK on success; any failure HRESULT
 *   on failure. Out-parameters are undefined on failure unless this
 *   header states otherwise. C++ exceptions never cross the ABI
 *   boundary. A human-readable message for the most recent failure on
 *   the calling thread is available via LayerMountGetLastErrorMessage.
 *
 *   Reserved HRESULT range for overlay-specific failure codes:
 *     FACILITY_ITF with codes 0xB000..0xBFFF. Hosts must not emit codes
 *     in this range from their own layers.
 *
 * STRING ENCODING
 *   All wide strings are null-terminated UTF-16LE. Input strings
 *   (PCWSTR) are caller-owned and need only remain valid for the
 *   duration of the call -- the DLL copies them into internal storage
 *   before returning. Output strings follow the two-call buffer
 *   pattern: pass buffer = NULL and bufferChars = 0 to receive the
 *   required size in *requiredChars; then allocate and call again.
 *   ERROR_MORE_DATA (as HRESULT_FROM_WIN32) means the caller-provided
 *   buffer was too small; *requiredChars is always written on that
 *   path.
 *
 * HANDLES
 *   LM_HANDLE, LM_FILE_HANDLE, LM_VHD_HANDLE,
 *   LM_VSS_SNAPSHOT_HANDLE, and LM_IMAGE_HANDLE are opaque. A handle
 *   is valid only between its creating function and its matching
 *   close/destroy. Calling any function on a freed or wrong-kind
 *   handle returns E_HANDLE.
 *
 * ABI VERSIONING
 *   LM_ABI_VERSION is the macro below. A breaking change (field
 *   reordered, function signature changed, enum value removed) bumps
 *   it. Purely additive changes (new struct fields appended behind a
 *   larger structSize, new enum values, new exported functions) do
 *   not. Structs carrying a `structSize` first field (LM_CONFIG,
 *   LM_VHD_CONFIG) are forward-extensible per that rule; fixed-shape
 *   structs (LM_FILE_INFO, LM_RESOLVED_PATH, LM_STATS, LM_EVENT,
 *   LM_VOLUME_INFO) revision only via LM_ABI_VERSION bumps.
 *
 * PLATFORM
 *   Windows user-mode only. Requires <windows.h>. The DLL itself has
 *   no dependency on any virtual filesystem host.
 */

#ifndef LAYERMOUNT_H
#define LAYERMOUNT_H

#if !defined(_WIN32)
#error "LayerMount.dll is Windows-only."
#endif

#include <windows.h>
#include <winternl.h>   /* NTSTATUS typedef for LayerMountHResultToNtStatus */

#ifdef __cplusplus
extern "C" {
#endif

/* Import/export macro. Define LAYERMOUNT_EXPORTS when building the DLL;
 * consumers leave it undefined and pick up __declspec(dllimport). */
#ifdef LAYERMOUNT_EXPORTS
#define LM_API __declspec(dllexport)
#else
#define LM_API __declspec(dllimport)
#endif

/* Calling convention for every exported function and every callback typedef.
 * On x64 this collapses to the Microsoft x64 convention; specifying it
 * explicitly keeps the ABI unambiguous for P/Invoke and guards against
 * silent drift if the DLL is ever compiled for x86. */
#define LM_CALL __stdcall

/* -------------------------------------------------------------------------
 * Version constants
 *
 * LM_ABI_VERSION is the only gate callers should check against.
 * Bumping it requires re-linking. LM_VER_{MAJOR,MINOR,PATCH} follow
 * SemVer for functional versioning and may change without touching the
 * ABI version (pure additive / bug-fix releases).
 *
 * Macros (not constexpr constants) so the header compiles as C.
 *
 * The actual numeric values live in a generated header that MSBuild
 * produces from version.props at build time. See
 * src/LayerMount.dll/LayerMount.version.targets.
 * ------------------------------------------------------------------------- */
#include "LayerMountVersion.h"

/* -------------------------------------------------------------------------
 * Opaque handle typedefs
 *
 * Every handle is a pointer to an incomplete sentinel struct. The struct
 * is never defined in this header; the DLL encodes type/generation/slot
 * information into the pointer value itself. Consumers treat these as
 * opaque tokens -- do not dereference, arithmetic, or compare by value.
 * ------------------------------------------------------------------------- */

typedef struct LM_HANDLE__*              LM_HANDLE;
typedef struct LM_FILE_HANDLE__*         LM_FILE_HANDLE;
typedef struct LM_VHD_HANDLE__*          LM_VHD_HANDLE;
typedef struct LM_VSS_SNAPSHOT_HANDLE__* LM_VSS_SNAPSHOT_HANDLE;
typedef struct LM_IMAGE_HANDLE__*        LM_IMAGE_HANDLE;

/* -------------------------------------------------------------------------
 * Enums
 *
 * All enums cross the ABI as UINT32-sized values. Consumers may pass the
 * numeric constant directly; do not rely on the C++ enum type name.
 * ------------------------------------------------------------------------- */

/* Host-declared capabilities. Bitfield -- combine with |.
 * Clearing a bit activates the documented capability fallback (see the
 * per-capability documentation below). */
typedef enum LM_HOST_CAPABILITIES {
    LM_CAP_NONE             = 0x00000000u,
    LM_CAP_ADS              = 0x00000001u,
    LM_CAP_REPARSE_POINTS   = 0x00000002u,
    LM_CAP_SPARSE_FILES     = 0x00000004u,
    LM_CAP_MULTIPLE_STREAMS = 0x00000008u,
    LM_CAP_NTFS_ACLS        = 0x00000010u,
    LM_CAP_CASE_SENSITIVE   = 0x00000020u
} LM_HOST_CAPABILITIES;

/* Which layer a resolved path was found in. Mirrors the internal
 * LayerSource enum. */
typedef enum LM_LAYER_SOURCE {
    LM_LAYER_NONE  = 0,
    LM_LAYER_UPPER = 1,
    LM_LAYER_LOWER = 2
} LM_LAYER_SOURCE;

/* Event categories emitted through LM_EVENT_CALLBACK. */
typedef enum LM_EVENT_TYPE {
    LM_EVT_WARNING           = 0,
    LM_EVT_COPY_UP           = 1,
    LM_EVT_WHITEOUT_CREATED  = 2,
    LM_EVT_ACCESS_DENIED     = 3
} LM_EVENT_TYPE;

/* VHD backing kind for LayerMountVhdCreate / LayerMountVhdOpen. */
typedef enum LM_VHD_KIND {
    LM_VHD_KIND_FIXED        = 0,
    LM_VHD_KIND_DYNAMIC      = 1,
    LM_VHD_KIND_DIFFERENCING = 2
} LM_VHD_KIND;

/* VHD attach lifetime. Mirrors VHDLayerManager::AttachLifetime. */
typedef enum LM_VHD_ATTACH_LIFETIME {
    LM_VHD_ATTACH_PERMANENT      = 0,
    LM_VHD_ATTACH_PROCESS_SCOPED = 1
} LM_VHD_ATTACH_LIFETIME;

/* Layer image compression algorithm. Mirrors LayerImageFormat::CompressionType. */
typedef enum LM_COMPRESSION_TYPE {
    LM_COMPRESSION_NONE = 0,
    LM_COMPRESSION_ZSTD = 1
} LM_COMPRESSION_TYPE;

/* Process-tracker operation category. Mirrors ProcessTracker::OperationType. */
typedef enum LM_OPERATION_TYPE {
    LM_OP_CREATE         = 0,
    LM_OP_OPEN           = 1,
    LM_OP_READ           = 2,
    LM_OP_WRITE          = 3,
    LM_OP_OVERWRITE      = 4,
    LM_OP_DELETE         = 5,
    LM_OP_RENAME         = 6,
    LM_OP_GET_INFO       = 7,
    LM_OP_SET_INFO       = 8,
    LM_OP_SET_SIZE       = 9,
    LM_OP_GET_SECURITY   = 10,
    LM_OP_SET_SECURITY   = 11,
    LM_OP_READ_DIRECTORY = 12,
    LM_OP_FLUSH          = 13,
    LM_OP_CLEANUP        = 14,
    LM_OP_CLOSE          = 15
} LM_OPERATION_TYPE;

/* -------------------------------------------------------------------------
 * LM_CONFIG
 *
 * LayerMount construction parameters. Forward-extensible via structSize:
 * callers set structSize = sizeof(LM_CONFIG) at their compile time, the
 * DLL compares it to the size it knows about and interprets only the
 * fields it understands. An abiVersion mismatch is a hard error.
 *
 * Ownership: every pointer in this struct is caller-owned and must remain
 * valid only for the duration of the LayerMountCreate call. The DLL copies
 * all referenced strings into internal storage before returning.
 * ------------------------------------------------------------------------- */
typedef struct LM_CONFIG {
    UINT32        structSize;          /* sizeof(LM_CONFIG) at caller compile time */
    UINT32        abiVersion;          /* must equal LM_ABI_VERSION */
    UINT32        hostCapabilities;    /* bitfield of LM_HOST_CAPABILITIES */
    UINT32        accessLogCapacity;   /* ProcessTracker circular buffer size */
    UINT32        pathCacheCapacity;   /* path-resolver cache size */
    BOOL          enableProcessTracking;
    UINT32        lowerPathCount;      /* number of entries in lowerPaths */
    UINT32        _reserved0;          /* keep following pointers 8-byte aligned */
    PCWSTR        upperPath;           /* upper (writable) layer root */
    PCWSTR        workDirPath;         /* atomic-ops staging directory */
    PCWSTR        processRulesPath;    /* JSON rules file; NULL for none */
    PCWSTR const* lowerPaths;          /* ordered; index 0 = highest priority lower */
} LM_CONFIG;

/* -------------------------------------------------------------------------
 * LM_FILE_INFO
 *
 * Flattened Win32 file metadata exposed across the C ABI. Timestamps are
 * 100ns units since 1601 (FILETIME packed as UINT64).
 *
 * Fixed shape -- revisions via LM_ABI_VERSION bumps, not structSize.
 * ------------------------------------------------------------------------- */
typedef struct LM_FILE_INFO {
    UINT32 fileAttributes;
    UINT32 reparseTag;
    UINT64 allocationSize;
    UINT64 fileSize;
    UINT64 creationTime;
    UINT64 lastAccessTime;
    UINT64 lastWriteTime;
    UINT64 changeTime;
    UINT64 indexNumber;
    UINT32 hardLinks;
    UINT32 eaSize;
} LM_FILE_INFO;

/* -------------------------------------------------------------------------
 * LM_STREAM_INFO
 *
 * Single named-data-stream entry returned from LayerMountEnumerateStreams.
 * Stream names carry NTFS's native form (e.g. ":mystream:$DATA"). The
 * main unnamed stream (`::$DATA`) and LayerMount's reserved metadata
 * streams (`:overlay:$DATA`, `:overlay.opaque:$DATA`) are filtered out
 * before this struct is populated, so callers never see them.
 *
 * `streamName` is a fixed-size buffer sized to fit any NTFS stream name
 * (NTFS caps stream names at 255 WCHARs, plus the `:$DATA` suffix and a
 * NUL terminator). The buffer is always NUL-terminated.
 *
 * LM_STREAM_NAME_MAX is mirrored on the managed side
 * (LayerMount.NET/Interop/NativeStructs.cs); the two must stay in sync.
 * ------------------------------------------------------------------------- */
#define LM_STREAM_NAME_MAX 296

typedef struct LM_STREAM_INFO {
    WCHAR  streamName[LM_STREAM_NAME_MAX];
    UINT64 streamSize;                      /* logical end-of-file in bytes */
    UINT64 allocationSize;                  /* streamSize rounded up to a 4 KiB multiple */
} LM_STREAM_INFO;

/* -------------------------------------------------------------------------
 * LM_RESOLVED_PATH
 *
 * Output of LayerMountResolvePath. Uses the two-call buffer pattern for
 * absolutePath: pass absolutePath = NULL, absolutePathChars = 0 on the
 * sizing call; absolutePathRequired is always written.
 *
 * fileSize matches LayerMountGetFileInfo. allocationSize is the file
 * size rounded up to 4 KiB, the number LayerMountMergeDirectory reports.
 * An open handle reports the real on-disk allocation when it is larger,
 * for example after a preallocation. Both are zero for a directory, a
 * whiteout, a path that does not exist, and a path the engine cannot
 * stat.
 * ------------------------------------------------------------------------- */
typedef struct LM_RESOLVED_PATH {
    PWSTR            absolutePath;          /* in/out: caller-provided buffer */
    SIZE_T           absolutePathChars;     /* in: capacity; out: chars written incl. NUL */
    SIZE_T           absolutePathRequired;  /* out: required chars incl. NUL */
    LM_LAYER_SOURCE source;
    INT32            lowerIndex;            /* -1 when source != LM_LAYER_LOWER */
    BOOL             isWhiteout;
    UINT32           attributes;            /* Win32 file attributes; INVALID_FILE_ATTRIBUTES if not found */
    UINT64           fileSize;              /* logical end-of-file in bytes */
    UINT64           allocationSize;        /* allocation in bytes, at least fileSize */
} LM_RESOLVED_PATH;

/* -------------------------------------------------------------------------
 * LM_VOLUME_INFO
 *
 * Volume metadata exposed across the C ABI. volumeLabelLength is in
 * BYTES, not chars.
 * ------------------------------------------------------------------------- */
typedef struct LM_VOLUME_INFO {
    UINT64 totalSize;
    UINT64 freeSize;
    WCHAR  volumeLabel[32];
    UINT32 volumeLabelLength;
} LM_VOLUME_INFO;

/* -------------------------------------------------------------------------
 * LM_STATS
 *
 * Snapshot of internal counters. Values are plain UINT64 -- the DLL loads
 * the underlying std::atomic values with relaxed ordering and copies them
 * here. No std::atomic crosses the boundary.
 * ------------------------------------------------------------------------- */
typedef struct LM_STATS {
    UINT64 cacheHits;
    UINT64 cacheMisses;
    UINT64 copyUpCount;
    UINT64 readCount;
    UINT64 writeCount;
    UINT64 activeHandles;
    UINT64 bytesRead;
    UINT64 bytesWritten;
    UINT64 cleanupMetadataFailureCount;
} LM_STATS;

/* -------------------------------------------------------------------------
 * LM_EVENT
 *
 * Payload for LM_EVENT_CALLBACK. All pointers are valid only for the
 * duration of the callback; consumers that retain data must copy it.
 * ------------------------------------------------------------------------- */
typedef struct LM_EVENT {
    LM_EVENT_TYPE type;
    HRESULT        hr;              /* S_OK for informational events */
    PCWSTR         relativePath;    /* may be NULL */
    PCWSTR         message;         /* may be NULL; UTF-16 NUL-terminated */
    UINT64         timestamp;       /* FILETIME as UINT64, UTC */
    DWORD          pid;             /* 0 if not applicable */
} LM_EVENT;

/* -------------------------------------------------------------------------
 * LM_VHD_CONFIG
 *
 * Consolidated parameter block for VHD create / open / attach operations.
 * Forward-extensible via structSize.
 * ------------------------------------------------------------------------- */
typedef struct LM_VHD_CONFIG {
    UINT32                  structSize;
    LM_VHD_KIND            kind;
    UINT64                  sizeBytes;            /* ignored when kind == DIFFERENCING */
    PCWSTR                  path;                 /* VHDX being created or opened */
    PCWSTR                  parentPath;           /* required when kind == DIFFERENCING */
    BOOL                    readOnly;             /* attach-time */
    BOOL                    suppressDriveLetter;  /* attach-time */
    LM_VHD_ATTACH_LIFETIME lifetime;
    UINT32                  _reserved0;
} LM_VHD_CONFIG;

/* -------------------------------------------------------------------------
 * LM_VHD_LAYER_TYPE / LM_VHD_LAYER_INFO
 *
 * Storage-backend type + scalar metadata returned per-entry from
 * LayerMountVhdListLayers. Per-string fields use the two-call buffer pattern.
 * ------------------------------------------------------------------------- */
typedef enum LM_VHD_LAYER_TYPE {
    LM_VHD_LAYER_DIRECTORY = 0,
    LM_VHD_LAYER_VHD       = 1,
    LM_VHD_LAYER_VSS       = 2
} LM_VHD_LAYER_TYPE;

typedef struct LM_VHD_LAYER_INFO {
    WCHAR              id[64];              /* layer ID; NUL-terminated */
    LM_VHD_LAYER_TYPE type;
    PWSTR              path;
    SIZE_T             pathChars;
    SIZE_T             pathRequired;
    PWSTR              parentId;
    SIZE_T             parentIdChars;
    SIZE_T             parentIdRequired;
    PWSTR              mountStatus;           /* "mounted" / "detached" / "unknown" */
    SIZE_T             mountStatusChars;
    SIZE_T             mountStatusRequired;
    PWSTR              volumeGuid;
    SIZE_T             volumeGuidChars;
    SIZE_T             volumeGuidRequired;
    PWSTR              createdAt;             /* ISO 8601 UTC */
    SIZE_T             createdAtChars;
    SIZE_T             createdAtRequired;
} LM_VHD_LAYER_INFO;

/* -------------------------------------------------------------------------
 * LM_VSS_SNAPSHOT_INFO
 *
 * Returned in arrays from LayerMountVssListSnapshots via the two-call buffer
 * pattern. String fields use the same pattern per-entry.
 * ------------------------------------------------------------------------- */
typedef struct LM_VSS_SNAPSHOT_INFO {
    WCHAR  id[40];              /* GUID without braces, NUL-terminated */
    GUID   vssId;               /* raw VSS GUID */
    PWSTR  volumePath;          /* caller-provided buffer */
    SIZE_T volumePathChars;
    SIZE_T volumePathRequired;
    PWSTR  devicePath;          /* caller-provided buffer */
    SIZE_T devicePathChars;
    SIZE_T devicePathRequired;
    BOOL   persistent;
    UINT64 createdAt;           /* FILETIME as UINT64 */
} LM_VSS_SNAPSHOT_INFO;

/* -------------------------------------------------------------------------
 * LM_IMAGE_MANIFEST_ENTRY / LM_IMAGE_MANIFEST
 *
 * Layer-image manifest returned by LayerMountImageGetManifest. Arrays use
 * the two-call pattern; per-entry strings use caller-provided buffers.
 * ------------------------------------------------------------------------- */
typedef struct LM_IMAGE_MANIFEST_ENTRY {
    PWSTR  imagePath;
    SIZE_T imagePathChars;
    SIZE_T imagePathRequired;
    WCHAR  checksumHex[65];     /* 64 hex chars + NUL */
} LM_IMAGE_MANIFEST_ENTRY;

typedef struct LM_IMAGE_MANIFEST {
    UINT32                    schemaVersion;
    UINT32                    entryCount;        /* in: capacity of entries[]; out: written */
    LM_IMAGE_MANIFEST_ENTRY* entries;           /* caller-owned array; NULL for sizing call */
    SIZE_T                    entriesRequired;   /* out: required entries count */
} LM_IMAGE_MANIFEST;

/* -------------------------------------------------------------------------
 * LM_IMAGE_PACK_OPTIONS
 *
 * Optional metadata stamps for LayerMountImagePack / LayerMountImagePackDifferential.
 * NULL fields are treated as empty strings. Forward-extensible via structSize.
 * ------------------------------------------------------------------------- */
typedef struct LM_IMAGE_PACK_OPTIONS {
    UINT32 structSize;      /* sizeof(LM_IMAGE_PACK_OPTIONS) at caller compile time */
    UINT32 _reserved0;
    PCWSTR author;          /* NULL or empty -> ""  */
    PCWSTR description;     /* NULL or empty -> ""  */
} LM_IMAGE_PACK_OPTIONS;

/* -------------------------------------------------------------------------
 * LM_IMAGE_METADATA
 *
 * Scalar metadata fields for a single layer image. Tag and whiteout
 * arrays are exposed through dedicated enumeration APIs (see
 * LayerMountImageEnumerateTags / LayerMountImageEnumerateWhiteouts in the
 * prototype section) so this struct stays fixed-shape.
 * ------------------------------------------------------------------------- */
typedef struct LM_IMAGE_METADATA {
    PWSTR                id;
    SIZE_T               idChars;
    SIZE_T               idRequired;
    PWSTR                parentId;        /* empty string if none */
    SIZE_T               parentIdChars;
    SIZE_T               parentIdRequired;
    PWSTR                createdAt;       /* ISO 8601 UTC */
    SIZE_T               createdAtChars;
    SIZE_T               createdAtRequired;
    PWSTR                author;
    SIZE_T               authorChars;
    SIZE_T               authorRequired;
    PWSTR                description;
    SIZE_T               descriptionChars;
    SIZE_T               descriptionRequired;
    LM_COMPRESSION_TYPE compression;
    UINT64               fileCount;
    UINT64               uncompressedSize;
    UINT64               compressedSize;
} LM_IMAGE_METADATA;

/* -------------------------------------------------------------------------
 * Callback typedefs
 *
 * Both use LM_CALL so the managed P/Invoke side can declare
 * [UnmanagedFunctionPointer(CallingConvention.StdCall)] matching exactly.
 * Callbacks must not re-enter the DLL synchronously.
 * ------------------------------------------------------------------------- */

/* Per-entry merged-directory enumeration. Return non-S_OK to abort. */
typedef HRESULT (LM_CALL *LM_DIR_ENUM_CALLBACK)(
    PCWSTR                name,
    const LM_FILE_INFO*  info,
    void*                 userContext);

/* LayerMount event fan-out. */
typedef void (LM_CALL *LM_EVENT_CALLBACK)(
    const LM_EVENT* evt,
    void*            userContext);

/* =========================================================================
 * Function prototypes
 *
 * Convention: every function returns HRESULT. S_OK on success. Out-params
 * are undefined on failure unless stated otherwise. List/string/blob
 * returns use the two-call pattern (NULL buffer + count-out on the sizing
 * call; HRESULT_FROM_WIN32(ERROR_MORE_DATA) when the caller's buffer was
 * short). Parameter order is: handle -> inputs -> outputs.
 * ========================================================================= */

/* ---- Lifecycle / diagnostics-lite ---- */

/* Reports the DLL's SemVer and ABI version. Each out-parameter is
 * independently optional; pass NULL for any value the caller does not
 * need. Always returns S_OK. */
LM_API HRESULT LM_CALL LayerMountGetVersion(
    UINT32* major, UINT32* minor, UINT32* patch, UINT32* abiVersion);

/* Fetches the human-readable message for the most recent failure whose
 * HRESULT matches `hr` on the calling thread, using the two-call buffer
 * pattern. Passing an `hr` other than the one currently stored for this
 * thread returns *requiredChars = 0 rather than a stale message. */
LM_API HRESULT LM_CALL LayerMountGetLastErrorMessage(
    HRESULT hr, PWSTR buffer, SIZE_T bufferChars, SIZE_T* requiredChars);

/*
 * Creates an overlay from `config` and returns its handle in *outHandle.
 * Validates that the upper layer exists and is writable and that every
 * lower path exists, then creates the work directory if it is missing.
 *
 * Returns:
 *   S_OK          -- overlay created
 *   E_POINTER     -- config or outHandle is NULL
 *   E_INVALIDARG  -- structSize too small, abiVersion does not match
 *                    LM_ABI_VERSION, a required path is NULL, or layer
 *                    validation failed (see LayerMountGetLastErrorMessage)
 *   E_FAIL        -- the work directory could not be created
 *   E_OUTOFMEMORY -- the overlay handle table is exhausted
 */
LM_API HRESULT LM_CALL LayerMountCreate(
    const LM_CONFIG* config, LM_HANDLE* outHandle);

/* Convenience for short-lived overlays used by CLI subcommands that need a
 * valid LM_HANDLE to drive VHD/VSS/Image primitives without mounting a
 * filesystem. Equivalent to LayerMountCreate with: upperPath = workDir, no
 * lower layers, no process tracking. Creates `workDir` (and missing
 * parents) on demand; if creation fails, LayerMountCreate's error path
 * surfaces the precise reason via LayerMountGetLastErrorMessage.
 *
 * Host adapters pass their own hostCapabilities so the helper stays portable
 * across NTFS / non-NTFS / non-Windows backends. */
LM_API HRESULT LM_CALL LayerMountCreateTransient(
    PCWSTR workDir, UINT32 hostCapabilities, LM_HANDLE* outHandle);

/*
 * Releases the overlay handle and its engine instance.
 *
 * Returns:
 *   S_OK                   -- released
 *   E_HANDLE               -- handle is NULL, stale, or wrong-kind
 *   E_ILLEGAL_METHOD_CALL  -- the instance is host-attached
 *                             (LayerMountSetHostAttached) or still has
 *                             open LM_FILE_HANDLEs
 */
LM_API HRESULT LM_CALL LayerMountDestroy(LM_HANDLE handle);

/* Installs `callback` as the overlay's LM_EVENT_CALLBACK, replacing any
 * previous one. Passing NULL uninstalls it. `userContext` is threaded
 * through to every invocation unchanged. */
LM_API HRESULT LM_CALL LayerMountSetEventCallback(
    LM_HANDLE handle, LM_EVENT_CALLBACK callback, void* userContext);

/* ---- Host adapter integration ----------------------------------
 *
 * LayerMountSetHostAttached is called by a host adapter to mark an overlay
 * as "currently mounted by a host adapter". While the flag is TRUE,
 * LayerMountDestroy refuses to release the instance and returns
 * E_ILLEGAL_METHOD_CALL -- the host adapter must
 * unmount and clear the flag (attached = FALSE) first. General DLL
 * consumers should not call this function; the default value is FALSE
 * so LayerMountDestroy works without ceremony when no host adapter is
 * attached.
 * ------------------------------------------------------------------------- */
LM_API HRESULT LM_CALL LayerMountSetHostAttached(
    LM_HANDLE handle, BOOL attached);

/* ---- Host adapter helpers ---------------------------------------------
 *
 * LayerMountHResultToNtStatus converts an HRESULT returned by any C ABI
 * function into the equivalent NTSTATUS using the canonical translation
 * table maintained inside the DLL. Host adapters that bridge the DLL to
 * an NTSTATUS-shaped callback surface should call this rather than
 * maintain their own table.
 *
 * Coverage: overlay-specific COM codes (E_HANDLE, E_ILLEGAL_METHOD_CALL,
 * etc.), FACILITY_WIN32-wrapped Win32 errors (the path the engine takes
 * when wrapping ::GetLastError()), and FACILITY_NT_BIT HRESULTs
 * (HRESULT_FROM_NT inversions). Unknown codes map to STATUS_UNSUCCESSFUL.
 *
 * The function never fails: returns S_OK and writes to *outStatus
 * unconditionally. outStatus must be non-NULL or E_POINTER is returned.
 * ------------------------------------------------------------------------- */
LM_API HRESULT LM_CALL LayerMountHResultToNtStatus(
    HRESULT hr, NTSTATUS* outStatus);

/* ---- Host adapter helpers: Windows mount points -----------------------
 *
 * Directory-mount-point validation and ownership-tracked cleanup.
 * Filesystem-host-agnostic: any Windows host adapter calls these to reserve
 * and release a mount-point directory without re-implementing the
 * directory-identity check.
 *
 * Drive-letter mount points ("X:" or "X:\") are detected by
 * LayerMountPointIsDriveLetter and require none of the prepare/
 * capture/release dance.
 *
 * Directory mount-point lifecycle:
 *   1. LayerMountPointPrepareDirectory(mp, &prep)
 *        Validates the path is free (no collision, not a reparse point)
 *        and creates parent directories on demand. Does NOT create the
 *        leaf and does NOT set prep.directoryCreatedByUs -- the engine's
 *        mount-point contract is that the host adapter creates the leaf
 *        itself when it mounts (so adapters that fail on a pre-existing
 *        directory aren't pre-empted). An eager create here would be
 *        actively harmful, and claiming ownership before the host adapter's
 *        mount call would be a contract lie: nothing is reserved yet.
 *   2. (host-adapter-specific mount call here -- creates + mounts on the leaf)
 *   3. LayerMountPointCaptureIdentity(mp, &prep)
 *        Captures the volume-serial + file-id of the now-mounted
 *        directory. Side-effect-free on prep.directoryCreatedByUs by
 *        design so callers can use it for diagnostic identity compares
 *        without implying ownership.
 *   4. Host adapter sets prep.directoryCreatedByUs = TRUE after the
 *        mount call succeeded. The contract assumes mount success implies
 *        the leaf was fresh (the mount would have failed otherwise), so
 *        ownership is legitimate at this commit point and no TOCTOU
 *        window remains.
 *   5. (host-adapter-specific unmount call here)
 *   6. LayerMountPointReleaseIfSafe(mp, &prep)
 *        Best-effort: removes the directory iff we claimed ownership,
 *        the identity still matches, and it's empty. Never fails.
 *
 * LM_MOUNT_POINT_PREP is fixed-shape -- revisions via LM_ABI_VERSION
 * bumps. Host adapters must zero-initialize before passing to PrepareDirectory.
 * ------------------------------------------------------------------------- */
typedef struct LM_MOUNT_POINT_PREP {
    BOOL   directoryCreatedByUs;  /* TRUE iff PrepareDirectory created the path */
    UINT64 volumeSerial;          /* FILE_ID_INFO::VolumeSerialNumber */
    UINT8  fileId[16];            /* FILE_ID_128 raw bytes */
    UINT8  reserved[8];           /* future-proof padding; keep zero */
} LM_MOUNT_POINT_PREP;

/* Returns *outIsDriveLetter = TRUE iff `mountPoint` is "X:" or "X:\".
 * NULL `mountPoint` yields *outIsDriveLetter = FALSE. */
LM_API HRESULT LM_CALL LayerMountPointIsDriveLetter(
    PCWSTR mountPoint, BOOL* outIsDriveLetter);

/* Validate a directory mount-point. The path must not already exist and
 * must not be a reparse point; missing parent directories are created on
 * demand. Does NOT create the leaf directory and does NOT set
 * outPrep->directoryCreatedByUs -- both are the host adapter's
 * responsibility after its mount call succeeds (see lifecycle doc above).
 *
 * Returns:
 *   S_OK                                          -- validation passed
 *   HRESULT_FROM_NT(STATUS_OBJECT_NAME_COLLISION) -- path already exists
 *   HRESULT_FROM_NT(STATUS_IO_REPARSE_DATA_INVALID) -- existing reparse point
 *   HRESULT_FROM_NT(STATUS_OBJECT_PATH_NOT_FOUND) -- parent create failed
 *   HRESULT_FROM_NT(STATUS_INVALID_PARAMETER)     -- empty mountPoint
 *   E_POINTER                                     -- outPrep == NULL
 *
 * Pair with LayerMountPointReleaseIfSafe after the host-specific
 * unmount completes. */
LM_API HRESULT LM_CALL LayerMountPointPrepareDirectory(
    PCWSTR mountPoint, LM_MOUNT_POINT_PREP* outPrep);

/* Capture the volume-serial + file-id of an existing mount-point
 * directory. No-op if mountPoint is empty or the directory cannot be
 * opened (the captured identity stays zero, which makes the subsequent
 * ReleaseIfSafe call a no-op as well). */
LM_API HRESULT LM_CALL LayerMountPointCaptureIdentity(
    PCWSTR mountPoint, LM_MOUNT_POINT_PREP* prep);

/* Best-effort: remove the mount-point directory iff
 *   prep->directoryCreatedByUs is TRUE, AND
 *   the directory's current volume-serial + file-id match prep, AND
 *   the directory is empty.
 * Otherwise leaves the directory in place. Never fails. */
LM_API HRESULT LM_CALL LayerMountPointReleaseIfSafe(
    PCWSTR mountPoint, const LM_MOUNT_POINT_PREP* prep);

/* ---- Path / volume ---- */

/* Resolves `relativePath` through the overlay and fills
 * `outResolved` with the winning layer, the lower index (-1 unless the
 * source is a lower), whiteout state, Win32 attributes, file size, and
 * allocation size. Uses the two-call buffer pattern for `absolutePath`.
 * A call on a path that exists costs one attribute query of the resolved
 * file for the two sizes. */
LM_API HRESULT LM_CALL LayerMountResolvePath(
    LM_HANDLE handle, PCWSTR relativePath, LM_RESOLVED_PATH* outResolved);

/* Fills `outInfo` with the overlay's total and free space and a fixed
 * volume label. Returns the translated NTSTATUS as an HRESULT if the
 * underlying volume query fails. */
LM_API HRESULT LM_CALL LayerMountGetVolumeInfo(
    LM_HANDLE handle, LM_VOLUME_INFO* outInfo);

/* Ensures the file or directory at `relativePath` exists in the upper
 * layer, triggering a copy-up if it currently resolves only to a lower
 * layer. No-op if the path is already in the upper layer. */
LM_API HRESULT LM_CALL LayerMountEnsureInUpperLayer(
    LM_HANDLE handle, PCWSTR relativePath);

/* ---- File primitives ---- */

/*
 * LayerMountOpenFile and LayerMountCreateFile accept an `originatorPid`.
 * A host adapter reads the originating process ID from its dispatch
 * context and passes it, so process-tracker rules match the requester
 * and not the dispatcher thread. Pass 0 when the calling code is its
 * own originator (a CLI, a test, a direct P/Invoke); the engine then
 * uses GetCurrentProcessId(). A path call without an `originatorPid`
 * checks the process that calls the engine.
 *
 * A call that takes an open LM_FILE_HANDLE (read, write, overwrite,
 * flush, and the handle-form delete pair) takes no originator. The
 * process tracker checks it against the process that opened the handle,
 * as NT checks access at open. A paging read arrives with the system
 * process as its originator; the engine checks it against the opener,
 * so a rule set that denies the system process does not fail it.
 */

/*
 * Opens the file or directory at `relativePath` for `grantedAccess`
 * under `createOptions`, returning a new LM_FILE_HANDLE in *outFile and
 * its metadata in *outInfo. `originatorPid` identifies the requesting
 * process for process-tracker rules; pass 0 to use the current process.
 * A later handle call is checked against this process (see the file
 * primitives preamble).
 * The returned handle pins its parent overlay until closed.
 *
 * `grantedAccess` can carry generic rights (GENERIC_READ, GENERIC_WRITE,
 * GENERIC_EXECUTE, GENERIC_ALL). The engine maps them to the file-specific
 * rights before it decides whether a lower file copies up and whether a
 * metacopy shell fills, so GENERIC_WRITE and FILE_GENERIC_WRITE behave
 * the same. MAXIMUM_ALLOWED resolves against the caller's rights on the
 * physical file, as a kernel open does, before the same decisions. A
 * failed probe fails the open with the probe's status and returns no
 * handle. The process tracker treats an open with MAXIMUM_ALLOWED as an
 * Open operation, not a Write.
 *
 * When the file is a metacopy shell and `grantedAccess` asks for data
 * (read, write, append, or execute), the engine fills the shell before
 * it returns. *outInfo then reports the filled file. An open for
 * attributes, security, or delete keeps the shell sparse. A failed fill
 * fails the open with the fill's status and returns no handle.
 */
LM_API HRESULT LM_CALL LayerMountOpenFile(
    LM_HANDLE       handle,
    PCWSTR           relativePath,
    UINT32           grantedAccess,
    UINT32           createOptions,
    DWORD            originatorPid,        /* 0 = use current process */
    LM_FILE_HANDLE* outFile,
    LM_FILE_INFO*   outInfo);

/*
 * Creates a new file or directory at `relativePath` with `fileAttributes`
 * and, when non-NULL, a self-relative `securityDescriptor` bounded by
 * `securityDescriptorBytes`. Returns a new LM_FILE_HANDLE in *outFile and
 * its metadata in *outInfo. `originatorPid` identifies the requesting
 * process for process-tracker rules; pass 0 to use the current process.
 * A later handle call is checked against this process (see the file
 * primitives preamble).
 *
 * MAXIMUM_ALLOWED in `grantedAccess` resolves against the caller's rights
 * on the created file or directory. The handle's stored access holds the
 * resolved set. A failed resolution removes the file, directory, or
 * stream the create made and fails the create with the resolution's
 * status.
 *
 * Returns E_INVALIDARG if `securityDescriptor` is non-NULL but is not a
 * structurally valid self-relative descriptor fitting within
 * `securityDescriptorBytes`.
 */
LM_API HRESULT LM_CALL LayerMountCreateFile(
    LM_HANDLE       handle,
    PCWSTR           relativePath,
    UINT32           createOptions,
    UINT32           grantedAccess,
    UINT32           fileAttributes,
    const BYTE*      securityDescriptor,   /* self-relative SD; may be NULL */
    SIZE_T           securityDescriptorBytes,
    UINT64           allocationSize,
    DWORD            originatorPid,        /* 0 = use current process */
    LM_FILE_HANDLE* outFile,
    LM_FILE_INFO*   outInfo);

/* Closes `file`, releasing its handle-table slot and decrementing the
 * parent overlay's open-file count. */
LM_API HRESULT LM_CALL LayerMountCloseFile(LM_FILE_HANDLE file);

/* Closes the NT handle behind `file` and keeps the context and its
 * handle-table slot. It does not flush, does not free the slot, and does
 * not change the parent overlay's open-file count. A second call on a
 * `file` with no open NT handle returns S_OK and does nothing. A later
 * read, write, overwrite, flush, or get-info on `file` reopens the file
 * by its path with the granted access minus DELETE, as the engine does
 * after a rename. A `file` whose granted access has FILE_WRITE_DATA or
 * FILE_APPEND_DATA reopens with FILE_READ_DATA as well, so a later read
 * on it succeeds. A `file` that was granted only DELETE reopens with
 * FILE_READ_ATTRIBUTES, so a later read on it fails. LayerMountCloseFile
 * stays the only call that frees the slot. */
LM_API HRESULT LM_CALL LayerMountCleanupFile(LM_FILE_HANDLE file);

/* Reads up to `length` bytes at `offset` from `file` into `buffer`.
 * A read never copies a file up and never reopens the handle for a
 * fill. A read reopens the handle only after a rename or after
 * LayerMountCleanupFile. A `file` whose granted access has
 * FILE_WRITE_DATA or FILE_APPEND_DATA also reads, so a paging read on a
 * write-only handle succeeds. A read on a handle with neither a
 * read-data, a write-data, nor an append-data right fails.
 * The engine always writes *bytesTransferred, also on failure.
 *
 * A read that starts inside the file and runs past its end succeeds with
 * the short count in *bytesTransferred. The engine then zero-fills `buffer`
 * from that count to `length`. A read that starts at or past the end of
 * the file fails with the end-of-file status and *bytesTransferred = 0. */
LM_API HRESULT LM_CALL LayerMountReadFile(
    LM_FILE_HANDLE file,
    void*           buffer,
    UINT64          offset,
    UINT32          length,
    UINT32*         bytesTransferred);

/*
 * Writes `length` bytes from `buffer` to `file` at `offset` (or at the
 * current end-of-file when `writeToEnd` is TRUE), copying the file up
 * into the upper layer first if it is not already there.
 * `constrainedIo` rejects a write that would extend the file past its
 * current allocation. Fills `outInfo` with post-write metadata when
 * non-NULL.
 */
LM_API HRESULT LM_CALL LayerMountWriteFile(
    LM_FILE_HANDLE file,
    const void*     buffer,
    UINT64          offset,
    UINT32          length,
    BOOL            writeToEnd,
    BOOL            constrainedIo,
    UINT32*         bytesTransferred,
    LM_FILE_INFO*  outInfo);

/*
 * CREATE_ALWAYS-style truncation of an already-open file. Replaces or ORs
 * `fileAttributes` per `replaceAttributes`, sets the allocation, and
 * deletes non-overlay ADS streams. Fills `outInfo` with post-truncation
 * metadata when non-NULL.
 */
LM_API HRESULT LM_CALL LayerMountOverwriteFile(
    LM_FILE_HANDLE file,
    UINT32          fileAttributes,
    BOOL            replaceAttributes,
    UINT64          allocationSize,
    LM_FILE_INFO*  outInfo);

/*
 * Flush buffered writes for an open file. The process tracker gates a
 * flush by the opener's read rule. Fills `outInfo` with post-flush
 * metadata when non-NULL.
 */
LM_API HRESULT LM_CALL LayerMountFlushFile(
    LM_FILE_HANDLE file,
    LM_FILE_INFO*  outInfo);

/* Fills `outInfo` with the current metadata of the open `file`. The
 * info describes the file as the handle opened it. A handle opened for
 * data describes the filled file, and an attribute-only handle on a
 * metacopy shell still describes the sparse shell. */
LM_API HRESULT LM_CALL LayerMountGetFileInfo(
    LM_FILE_HANDLE file, LM_FILE_INFO* outInfo);

/* Updates attributes, timestamps, allocation size, and end-of-file for
 * the open `file`; each parameter's leave-unchanged sentinel is
 * documented at its declaration below. Fills `outInfo` with the
 * resulting metadata when non-NULL. */
LM_API HRESULT LM_CALL LayerMountSetFileInfo(
    LM_FILE_HANDLE file,
    UINT32          fileAttributes,      /* INVALID_FILE_ATTRIBUTES to leave unchanged */
    UINT64          creationTime,        /* 0 to leave unchanged */
    UINT64          lastAccessTime,
    UINT64          lastWriteTime,
    UINT64          changeTime,
    UINT64          allocationSize,      /* UINT64_MAX to leave unchanged */
    UINT64          fileSize,
    LM_FILE_INFO*  outInfo);

/* Deletes the file or directory at `relativePath`, dropping a whiteout
 * marker in the upper layer when the path also exists in a lower layer. */
LM_API HRESULT LM_CALL LayerMountDeleteFile(
    LM_HANDLE handle, PCWSTR relativePath);

/* Reports via the returned HRESULT whether `relativePath` may be
 * deleted, without deleting it. S_OK means the delete would succeed. */
LM_API HRESULT LM_CALL LayerMountCanDeleteFile(
    LM_HANDLE handle, PCWSTR relativePath);

/* Reports via the returned HRESULT whether the open `file` may be
 * deleted, without deleting it. S_OK means the delete would succeed. */
LM_API HRESULT LM_CALL LayerMountCanDeleteOpenFile(
    LM_FILE_HANDLE file);

/* Deletes the open `file`, dropping a whiteout marker in the upper layer
 * when the path also exists in a lower layer. */
LM_API HRESULT LM_CALL LayerMountDeleteOpenFile(
    LM_FILE_HANDLE file);

/* Renames `oldRelativePath` to `newRelativePath`, copying the entry into
 * the upper layer first if it currently resolves only to a lower layer.
 * `replaceIfExists` controls whether an existing entry at the
 * destination is replaced or the call fails. */
LM_API HRESULT LM_CALL LayerMountRenameFile(
    LM_HANDLE handle,
    PCWSTR     oldRelativePath,
    PCWSTR     newRelativePath,
    BOOL       replaceIfExists);

/* Renames the open `file` to `newRelativePath`, with the same copy-up
 * and replace semantics as LayerMountRenameFile. */
LM_API HRESULT LM_CALL LayerMountRenameOpenFile(
    LM_FILE_HANDLE file,
    PCWSTR          newRelativePath,
    BOOL            replaceIfExists);

/* Update the path the engine associates with an open file handle
 * WITHOUT performing any rename. Hosts call this when they observe a
 * path-based rename has affected a concurrently-open handle (e.g.
 * MoveFileExW renaming `a.txt` while a `DELETE_ON_CLOSE` handle on
 * `a.txt` is still open) so subsequent handle-based operations
 * (LayerMountDeleteOpenFile, LayerMountGetFileInfo) target the file's
 * current name rather than its stale pre-rename path. No-op when
 * `newRelativePath` matches the engine's current view. */
LM_API HRESULT LM_CALL LayerMountUpdateOpenFilePath(
    LM_FILE_HANDLE file,
    PCWSTR          newRelativePath);

/*
 * Path-based security accessor. Size-probe pattern:
 *   securityDescriptor == NULL or securityDescriptorBytes == 0:
 *     *requiredBytes is filled with the required size, S_OK.
 *   securityDescriptorBytes < required: *requiredBytes is filled
 *     with the required size, HRESULT_FROM_WIN32(ERROR_MORE_DATA).
 *   otherwise: securityDescriptor is filled, *requiredBytes with the
 *     size written, S_OK. A buffer larger than the descriptor is
 *     accepted, so a caller whose descriptor shrank between the two
 *     calls must cut the result to *requiredBytes.
 *
 * securityInformation names the sections the caller wants
 * (OWNER_SECURITY_INFORMATION, GROUP_SECURITY_INFORMATION,
 * DACL_SECURITY_INFORMATION, SACL_SECURITY_INFORMATION). The engine
 * drops SACL unless the filesystem process holds SE_SECURITY_NAME. A
 * value of 0 is not an error: it writes *requiredBytes = 0 and
 * returns S_OK with an empty descriptor. Other bits pass through to
 * Win32 unexamined.
 */
LM_API HRESULT LM_CALL LayerMountGetSecurity(
    LM_HANDLE handle,
    PCWSTR     relativePath,
    UINT32     securityInformation,
    UINT32*    outFileAttributes,
    BYTE*      securityDescriptor,
    SIZE_T     securityDescriptorBytes,
    SIZE_T*    requiredBytes);

/*
 * Applies the sections named by `securityInformation` from the
 * self-relative `modificationDescriptor` to `relativePath`.
 *
 * Returns:
 *   S_OK          -- applied
 *   E_HANDLE      -- handle is NULL, stale, or wrong-kind
 *   E_INVALIDARG  -- relativePath or modificationDescriptor is NULL, or
 *                    the descriptor is not a structurally valid
 *                    self-relative SECURITY_DESCRIPTOR fitting within
 *                    modificationDescriptorBytes
 *   other         -- an NTSTATUS from applying the security descriptor,
 *                    translated to HRESULT (e.g. access denied, path
 *                    not found)
 */
LM_API HRESULT LM_CALL LayerMountSetSecurity(
    LM_HANDLE  handle,
    PCWSTR      relativePath,
    UINT32      securityInformation,
    const BYTE* modificationDescriptor,
    SIZE_T      modificationDescriptorBytes);

/* Enumerates the merged directory listing at `dirRelativePath`, invoking
 * `callback` once per visible entry with its name and LM_FILE_INFO.
 * Whiteout markers and the entries they hide are never reported. Stops
 * and returns the callback's HRESULT the first time it returns anything
 * other than S_OK. */
LM_API HRESULT LM_CALL LayerMountMergeDirectory(
    LM_HANDLE             handle,
    PCWSTR                 dirRelativePath,
    LM_DIR_ENUM_CALLBACK  callback,
    void*                  userContext);

/* Creates a whiteout marker for `relativePath` in the upper layer,
 * hiding the corresponding lower-layer entry. `isDirectory` selects the
 * directory- or file-shaped marker. Rejects a path outside the overlay
 * root or inside the reserved metadata subtree with E_INVALIDARG. */
LM_API HRESULT LM_CALL LayerMountCreateWhiteout(
    LM_HANDLE handle, PCWSTR relativePath, BOOL isDirectory);

/* Marks the directory at `dirRelativePath` opaque, hiding every
 * lower-layer entry beneath it regardless of name. Rejects a path
 * outside the overlay root or inside the reserved metadata subtree with
 * E_INVALIDARG. */
LM_API HRESULT LM_CALL LayerMountSetOpaque(
    LM_HANDLE handle, PCWSTR dirRelativePath);

/*
 * Path-based reparse-descriptor accessor. Size-probe pattern:
 *   buffer == NULL or bufferBytes == 0:
 *     *requiredBytes is filled with the required size, S_OK.
 *   bufferBytes < required: *requiredBytes is filled with the required
 *     size, HRESULT_FROM_WIN32(ERROR_MORE_DATA).
 *   otherwise: buffer is filled, *requiredBytes with the size written,
 *     S_OK.
 *
 * A buffer larger than the descriptor is accepted, and *requiredBytes
 * then reports the bytes written rather than the capacity offered, so a
 * caller whose descriptor changed size between the probe and the fill
 * can re-read at MAXIMUM_REPARSE_DATA_BUFFER_SIZE (16384 bytes) and get
 * the current descriptor in one further call. That ceiling is the
 * documented maximum for reparse data, and the engine stages every read
 * through a buffer of exactly that size, so no descriptor it returns
 * needs more.
 *
 * A path that exists but carries no reparse data returns
 * HRESULT_FROM_NT(STATUS_NOT_A_REPARSE_POINT); a path that does not
 * resolve returns HRESULT_FROM_NT(STATUS_OBJECT_NAME_NOT_FOUND).
 */
LM_API HRESULT LM_CALL LayerMountGetReparsePoint(
    LM_HANDLE handle,
    PCWSTR     relativePath,
    BYTE*      buffer,
    SIZE_T     bufferBytes,
    SIZE_T*    requiredBytes);

/* Sets the reparse-point data at `relativePath` to the `bufferBytes`
 * bytes in `buffer`, copying the entry into the upper layer first if it
 * currently resolves only to a lower layer. Rejects a buffer larger
 * than MAXIMUM_REPARSE_DATA_BUFFER_SIZE (a translated
 * STATUS_INVALID_PARAMETER). */
LM_API HRESULT LM_CALL LayerMountSetReparsePoint(
    LM_HANDLE  handle,
    PCWSTR      relativePath,
    const BYTE* buffer,
    SIZE_T      bufferBytes);

/* Removes the reparse point at `relativePath`. `buffer` and
 * `bufferBytes` carry the caller's current reparse tag data; the delete
 * fails if it does not match the on-disk reparse point's tag. */
LM_API HRESULT LM_CALL LayerMountDeleteReparsePoint(
    LM_HANDLE  handle,
    PCWSTR      relativePath,
    const BYTE* buffer,
    SIZE_T      bufferBytes);

/*
 * Enumerates the named data streams of the file or directory at
 * `relativePath`. NTFS permits alternate data streams on directories
 * too, and this call handles both transparently. The main unnamed
 * stream (`::$DATA`) and LayerMount's reserved metadata streams are
 * filtered out — callers see only user-visible named streams.
 *
 * Size-probe pattern:
 *   outBuffer == NULL: *outCount is filled with the required count, S_OK.
 *   outBuffer != NULL && bufferCapacity < required: *outCount is filled
 *     with the required count, HRESULT_FROM_WIN32(ERROR_MORE_DATA).
 *   otherwise: outBuffer is filled with up to bufferCapacity entries,
 *     *outCount with the actual number written, S_OK.
 *
 * Returns HRESULT_FROM_NT(STATUS_OBJECT_NAME_NOT_FOUND) when the path
 * does not exist in any layer. Returns S_OK with *outCount == 0 when
 * the path exists but carries no user-visible streams.
 */
LM_API HRESULT LM_CALL LayerMountEnumerateStreams(
    LM_HANDLE        handle,
    PCWSTR           relativePath,
    LM_STREAM_INFO* outBuffer,
    UINT32           bufferCapacity,
    UINT32*          outCount);

/* ---- VHD primitives ---- */

/* Creates a new VHD or VHDX file per `config->kind` (fixed, dynamic, or
 * differencing) and returns a handle to it in *outVhd. The file is
 * created but not attached; call LayerMountVhdAttach to mount it as a
 * volume. `config->sizeBytes` is required for a fixed or dynamic disk;
 * `config->parentPath` is required for a differencing disk. */
LM_API HRESULT LM_CALL LayerMountVhdCreate(
    LM_HANDLE            mount,
    const LM_VHD_CONFIG* config,
    LM_VHD_HANDLE*       outVhd);

/* Opens the existing VHD or VHDX file at `config->path` and returns a
 * handle to it in *outVhd, without attaching it. Fails with a
 * Win32-translated HRESULT if the path does not exist, and with
 * HRESULT_FROM_WIN32(ERROR_FILE_INVALID) if it names a directory. */
LM_API HRESULT LM_CALL LayerMountVhdOpen(
    LM_HANDLE            mount,
    const LM_VHD_CONFIG* config,
    LM_VHD_HANDLE*       outVhd);

/* Attaches `vhd` as a Windows volume and reports its physical device
 * path using the two-call buffer pattern. A second call on an
 * already-attached handle re-emits the cached path without attaching
 * again, so a sizing probe followed by a fill call is safe. */
LM_API HRESULT LM_CALL LayerMountVhdAttach(
    LM_VHD_HANDLE vhd,
    PWSTR          physicalPathBuffer,
    SIZE_T         physicalPathChars,
    SIZE_T*        physicalPathRequired);

/* Detaches `vhd` from its Windows volume and clears the cached physical
 * path, so a later LayerMountVhdAttach call attaches fresh. */
LM_API HRESULT LM_CALL LayerMountVhdDetach(LM_VHD_HANDLE vhd);

/* Merges the differencing disk `childVhd` into its parent. Fails at the
 * Win32 layer if the VHD is currently attached. */
LM_API HRESULT LM_CALL LayerMountVhdMerge(LM_VHD_HANDLE childVhd);

/* Creates a VHD at `vhdPath` and copies the contents of `directoryPath`
 * into it. `sizeBytes` is the requested VHD capacity; 0 auto-sizes to
 * fit the source directory. */
LM_API HRESULT LM_CALL LayerMountVhdImport(
    LM_HANDLE mount,
    PCWSTR     directoryPath,
    PCWSTR     vhdPath,
    UINT64     sizeBytes);

/* Attaches `vhdPath` read-only and copies its user-visible contents into
 * `directoryPath`, creating the directory if missing. Skips the NTFS
 * system entries at the volume root and tolerates a permission error on
 * an individual file so a restrictive ACL does not abort the export. */
LM_API HRESULT LM_CALL LayerMountVhdExport(
    LM_HANDLE mount,
    PCWSTR     vhdPath,
    PCWSTR     directoryPath);

/* Releases the `vhd` handle-table slot. For a process-scoped attach this
 * also detaches it; a permanent attach outlives the handle and needs an
 * explicit LayerMountVhdDetach to go offline. */
LM_API HRESULT LM_CALL LayerMountVhdClose(LM_VHD_HANDLE vhd);

/*
 * Resolve the volume GUID path (\\?\Volume{...}\) for an attached VHD.
 * Must be called AFTER LayerMountVhdAttach on the same LM_VHD_HANDLE;
 * returns E_ILLEGAL_METHOD_CALL if Attach hasn't populated the cached
 * open handle yet. Volume-GUID enumeration can lag the attach because of
 * PnP; callers should retry on empty-string / ERROR_GEN_FAILURE.
 * Two-call buffer pattern.
 */
LM_API HRESULT LM_CALL LayerMountVhdGetVolumeGuid(
    LM_VHD_HANDLE vhd,
    PWSTR          buffer,
    SIZE_T         bufferChars,
    SIZE_T*        requiredChars);

/*
 * List VHD layers recorded in the on-disk manifest JSON. Two-call
 * buffer pattern on the `entries` array; per-entry strings also use the
 * two-call pattern (caller pre-allocates generously or probes per-field).
 * `manifestDir == NULL` -> use the process's current working directory.
 * When the manifest file doesn't exist, returns S_OK with
 * *entriesRequired = 0 (idempotent no-op).
 */
LM_API HRESULT LM_CALL LayerMountVhdListLayers(
    LM_HANDLE          mount,
    PCWSTR              manifestDir,
    LM_VHD_LAYER_INFO* entries,
    UINT32              entriesCapacity,
    UINT32*             entriesWritten,
    UINT32*             entriesRequired);

/*
 * Remove a layer entry from the VHD manifest (idempotent). Uses a
 * cross-process ManifestLock around the load-mutate-save sequence.
 * `outRemoved` is set TRUE when an entry was found and removed, FALSE
 * when the id didn't exist or the manifest file was missing.
 */
LM_API HRESULT LM_CALL LayerMountVhdUnregisterLayer(
    LM_HANDLE mount,
    PCWSTR     layerId,
    PCWSTR     manifestDir,     /* NULL -> cwd */
    BOOL*      outRemoved);

/*
 * Fetch the per-layer metadata map (key/value) as a JSON object string.
 * Two-call buffer pattern. Returns S_OK with *requiredChars = 1 (just
 * the NUL) and an empty "{}" when the layer exists but has no metadata;
 * returns STG_E_PATHNOTFOUND if the layer id isn't in the manifest.
 */
LM_API HRESULT LM_CALL LayerMountVhdGetLayerMetadataJson(
    LM_HANDLE mount,
    PCWSTR     manifestDir,     /* NULL -> cwd */
    PCWSTR     layerId,
    PWSTR      buffer,
    SIZE_T     bufferChars,
    SIZE_T*    requiredChars);

/* ---- VSS primitives ---- */

/*
 * Caller buffer requirements for LayerMountVssCreateSnapshot. Snapshot IDs
 * are GUIDs (~40 chars NUL-terminated) and device paths are bounded by
 * the Windows \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN namespace.
 * These constants are the fixed sizes the ABI expects; pass buffers at
 * least this large and the call completes in one round trip. A sizing
 * probe with both buffer pointers NULL and both char counts zero reports
 * the required sizes without creating a snapshot.
 */
#define LM_VSS_ID_CHARS_REQUIRED          64
#define LM_VSS_DEVICE_PATH_CHARS_REQUIRED 260

/*
 * Single-call create: on success a snapshot exists and its id / device
 * path have been copied into the caller buffers. If either buffer is
 * NULL / too small, the call returns HRESULT_FROM_WIN32(ERROR_MORE_DATA)
 * and does NOT create a snapshot -- earlier builds created the snapshot
 * before the buffer check, so a sizing probe followed by a fill call
 * produced two snapshots for one logical request.
 */
LM_API HRESULT LM_CALL LayerMountVssCreateSnapshot(
    LM_HANDLE               mount,
    PCWSTR                   volumePath,
    BOOL                     persistent,
    LM_VSS_SNAPSHOT_HANDLE* outSnapshot,
    PWSTR                    idBuffer,
    SIZE_T                   idBufferChars,
    SIZE_T*                  idRequired,
    PWSTR                    devicePathBuffer,
    SIZE_T                   devicePathBufferChars,
    SIZE_T*                  devicePathRequired);

/* Deletes the VSS shadow copy at `snapshotId`, tracked by this overlay
 * or not, persistent or not. Accepts either a snapshot id this overlay
 * returned or a raw VSS shadow-copy GUID string. */
LM_API HRESULT LM_CALL LayerMountVssDeleteSnapshot(
    LM_HANDLE mount, PCWSTR snapshotId);

/* Lists every VSS shadow copy visible under VSS_CTX_ALL (the whole
 * machine, not just snapshots this overlay created), using the two-call
 * buffer pattern on `entries`. */
LM_API HRESULT LM_CALL LayerMountVssListSnapshots(
    LM_HANDLE             mount,
    LM_VSS_SNAPSHOT_INFO* entries,
    UINT32                 entriesCapacity,
    UINT32*                entriesWritten,
    UINT32*                entriesRequired);

/* Deletes every non-persistent VSS shadow copy this overlay created.
 * Persistent snapshots are left for the caller or the backup admin to
 * delete explicitly. */
LM_API HRESULT LM_CALL LayerMountVssCleanupSnapshots(LM_HANDLE mount);

/*
 * Report whether a snapshot this overlay created still has a reachable
 * device path. The answer travels in *outReachable; the HRESULT reports
 * only whether the query was made.
 *
 *   S_OK
 *     This overlay tracks the id. *outReachable is TRUE when the device
 *     path resolves and FALSE when it does not.
 *   HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
 *     This overlay never created that id. Scope is per-overlay and
 *     in-process, unlike LayerMountVssListSnapshots, which reports the
 *     whole machine under VSS_CTX_ALL.
 *   E_HANDLE, E_INVALIDARG, E_POINTER, or any other failure
 *     The query was not made. *outReachable is undefined.
 *
 * A snapshot destroyed outside this process stays tracked, so it
 * reports S_OK with *outReachable FALSE, never ERROR_NOT_FOUND. A
 * caller can therefore tell a dead snapshot from one it never owned,
 * which a lower-layer open failure alone cannot.
 */
LM_API HRESULT LM_CALL LayerMountVssValidateSnapshotPath(
    LM_HANDLE mount, PCWSTR snapshotId, BOOL* outReachable);

/*
 * Release the handle-table slot backing an LM_VSS_SNAPSHOT_HANDLE. The
 * snapshot itself -- if non-persistent -- remains tracked by the
 * engine's VSSManager and is eligible for LayerMountVssCleanupSnapshots.
 * Persistent snapshots remain until the caller explicitly deletes them
 * or the backup admin does. Closing the receipt handle does not alter
 * the snapshot's existence.
 */
LM_API HRESULT LM_CALL LayerMountVssCloseSnapshot(LM_VSS_SNAPSHOT_HANDLE snapshot);

/* ---- Layer image primitives ---- */

/* Packs `sourceDir` into a `.lmnt` layer image at `outputPath` with the
 * given zstd `compressionLevel`, stamping `options`' author and
 * description when non-NULL. Returns a receipt handle in *outImage when
 * non-NULL; the handle carries no long-lived state beyond the path. */
LM_API HRESULT LM_CALL LayerMountImagePack(
    LM_HANDLE                    mount,
    PCWSTR                        sourceDir,
    PCWSTR                        outputPath,
    INT32                         compressionLevel,
    const LM_IMAGE_PACK_OPTIONS* options,     /* may be NULL */
    LM_IMAGE_HANDLE*             outImage);

/*
 * Create a differential image that records only files in sourceDir that
 * are new or modified relative to baseDir. Deleted-in-source files get
 * whiteout entries in the metadata. Identical shape + lifetime as
 * LayerMountImagePack.
 */
LM_API HRESULT LM_CALL LayerMountImagePackDifferential(
    LM_HANDLE                    mount,
    PCWSTR                        sourceDir,
    PCWSTR                        baseDir,
    PCWSTR                        outputPath,
    INT32                         compressionLevel,
    const LM_IMAGE_PACK_OPTIONS* options,     /* may be NULL */
    LM_IMAGE_HANDLE*             outImage);

/*
 * Create a multi-image manifest JSON at `outputPath` listing the given
 * image files in order with their SHA-256 checksums. Each image must
 * exist; the helper reads and hashes each one.
 */
LM_API HRESULT LM_CALL LayerMountImageCreateManifest(
    LM_HANDLE    mount,
    PCWSTR        outputPath,
    const PCWSTR* imagePaths,
    UINT32        imageCount);

/* Extracts the layer image at `imagePath` into `targetDir`.
 * `verifyChecksum` rejects the image if the data section's SHA-256 does
 * not match the header. */
LM_API HRESULT LM_CALL LayerMountImageUnpack(
    LM_HANDLE mount,
    PCWSTR     imagePath,
    PCWSTR     targetDir,
    BOOL       verifyChecksum);

/* Validates the layer image at `imagePath`: reads the header and
 * metadata, then streams the data section to verify its SHA-256 against
 * the header's checksum. Does not extract anything. */
LM_API HRESULT LM_CALL LayerMountImageValidate(
    LM_HANDLE mount, PCWSTR imagePath);

/* Loads the multi-image manifest JSON at `manifestPath` and lists its
 * entries in `manifest` using the two-call pattern on `entries`. */
LM_API HRESULT LM_CALL LayerMountImageGetManifest(
    LM_HANDLE          mount,
    PCWSTR              manifestPath,
    LM_IMAGE_MANIFEST* manifest);

/* Reads the header and metadata of the layer image at `imagePath` into
 * `metadata`. Scalar fields are always written; string fields use the
 * two-call pattern per field. Tags and whiteouts are not projected here. */
LM_API HRESULT LM_CALL LayerMountImageGetMetadata(
    LM_HANDLE          mount,
    PCWSTR              imagePath,
    LM_IMAGE_METADATA* metadata);

/* Releases the `image` handle-table slot. The underlying `.lmnt` file on
 * disk is unaffected. */
LM_API HRESULT LM_CALL LayerMountImageClose(LM_IMAGE_HANDLE image);

/* ---- Diagnostics, eventing, process tracker ---- */

/* Snapshots the overlay's internal counters (cache hits/misses, copy-up
 * count, read/write counts and byte totals, active handles, and
 * metadata cleanup failures) into `outStats`. */
LM_API HRESULT LM_CALL LayerMountGetStats(
    LM_HANDLE handle, LM_STATS* outStats);

/* Turns process tracking on or off for the overlay. Enabling fails if a
 * configured rules file cannot be loaded, since an empty rule set would
 * silently allow every operation. */
LM_API HRESULT LM_CALL LayerMountProcessTrackerEnable(
    LM_HANDLE handle, BOOL enable);

/*
 * (Re-)loads the process-tracker rules JSON at `rulesPath`.
 *
 * Returns:
 *   S_OK                   -- loaded
 *   E_HANDLE               -- handle is NULL, stale, or wrong-kind
 *   E_INVALIDARG           -- rulesPath is NULL or empty
 *   E_ILLEGAL_METHOD_CALL  -- the tracker is not enabled
 *                             (LayerMountProcessTrackerEnable)
 *   E_FAIL                 -- the file could not be parsed or read
 */
LM_API HRESULT LM_CALL LayerMountProcessTrackerSetRules(
    LM_HANDLE handle, PCWSTR rulesPath);

/* Exports the process-tracker access log as a JSON string using the
 * two-call buffer pattern. Fails with E_ILLEGAL_METHOD_CALL if the
 * tracker is not enabled. */
LM_API HRESULT LM_CALL LayerMountProcessTrackerExportJson(
    LM_HANDLE handle,
    PWSTR      buffer,
    SIZE_T     bufferChars,
    SIZE_T*    requiredChars);

/* Exports the process-tracker access log as CSV using the two-call
 * buffer pattern. Fails with E_ILLEGAL_METHOD_CALL if the tracker is not
 * enabled. */
LM_API HRESULT LM_CALL LayerMountProcessTrackerExportCsv(
    LM_HANDLE handle,
    PWSTR      buffer,
    SIZE_T     bufferChars,
    SIZE_T*    requiredChars);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LAYERMOUNT_H */
