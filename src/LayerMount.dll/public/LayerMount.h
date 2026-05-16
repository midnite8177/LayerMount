/* LayerMount.h -- Public C ABI for LayerMount.dll.
 *
 * SCOPE
 *   This is the only public header the DLL exposes. Every symbol declared
 *   here is stable within a single LM_ABI_VERSION. Internal headers
 *   under abi/ and impl/ are not installed and must never be included by
 *   DLL consumers.
 *
 * THREAD-SAFETY (FR-8)
 *   Every function declared in this header is safe to call from any
 *   thread on any handle at any time. Internal synchronization is the
 *   DLL's responsibility. Callers need not serialize calls. Two callers
 *   may concurrently read and write the same LM_FILE_HANDLE; the DLL
 *   serializes inside the call. Callbacks supplied to
 *   LayerMountSetEventCallback and LayerMountMergeDirectory may fire on
 *   arbitrary DLL-internal threads, and must not re-enter the DLL
 *   synchronously on the same handle.
 *
 * ERROR MODEL (FR-4, FR-9)
 *   Every function returns HRESULT. S_OK on success; any failure HRESULT
 *   on failure. Out-parameters are undefined on failure unless this
 *   header states otherwise. C++ exceptions never cross the ABI
 *   boundary. A human-readable message for the most recent failure on
 *   the calling thread is available via LayerMountGetLastErrorMessage
 *   (FR-10).
 *
 *   Reserved HRESULT range for overlay-specific failure codes:
 *     FACILITY_ITF with codes 0xB000..0xBFFF. Hosts must not emit codes
 *     in this range from their own layers.
 *
 * STRING ENCODING (FR-6)
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
 * HANDLES (FR-5)
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
 *   structs (LM_FILE_INFO, LM_STATS, LM_EVENT, LM_VOLUME_INFO)
 *   revision only via LM_ABI_VERSION bumps.
 *
 * PLATFORM
 *   Windows user-mode only. Requires <windows.h>. The DLL itself has
 *   no dependency on any virtual filesystem host (FR-2).
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
 * Version constants (FR-7)
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
 * Opaque handle typedefs (FR-5)
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

/* Host-declared capabilities (FR-14). Bitfield -- combine with |.
 * Clearing a bit activates the documented capability fallback (FR-15..FR-17
 * and the ACL / streams rows of the PRD degradation table). */
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

/* Event categories emitted through LM_EVENT_CALLBACK (FR-31). */
typedef enum LM_EVENT_TYPE {
    LM_EVT_WARNING           = 0,
    LM_EVT_COPY_UP           = 1,
    LM_EVT_WHITEOUT_CREATED  = 2,
    LM_EVT_ACCESS_DENIED     = 3
} LM_EVENT_TYPE;

/* VHD backing kind for LayerMountVhdCreate / LayerMountVhdOpen (FR-23, FR-24). */
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
 * LM_CONFIG (FR-12)
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
 * LM_FILE_INFO (FR-18, FR-20)
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
 * LM_RESOLVED_PATH (FR-22)
 *
 * Output of LayerMountResolvePath. Uses the two-call buffer pattern for
 * absolutePath: pass absolutePath = NULL, absolutePathChars = 0 on the
 * sizing call; absolutePathRequired is always written.
 * ------------------------------------------------------------------------- */
typedef struct LM_RESOLVED_PATH {
    PWSTR            absolutePath;          /* in/out: caller-provided buffer */
    SIZE_T           absolutePathChars;     /* in: capacity; out: chars written incl. NUL */
    SIZE_T           absolutePathRequired;  /* out: required chars incl. NUL */
    LM_LAYER_SOURCE source;
    INT32            lowerIndex;            /* -1 when source != LM_LAYER_LOWER */
    BOOL             isWhiteout;
    UINT32           attributes;            /* Win32 file attributes; INVALID_FILE_ATTRIBUTES if not found */
} LM_RESOLVED_PATH;

/* -------------------------------------------------------------------------
 * LM_VOLUME_INFO (FR-21)
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
 * LM_STATS (FR-30)
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
 * LM_EVENT (FR-31)
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
 * LM_VHD_CONFIG (FR-23, FR-24)
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
 * LM_VHD_LAYER_TYPE / LM_VHD_LAYER_INFO (FR-35)
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
 * LM_VSS_SNAPSHOT_INFO (FR-26)
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
 * LM_IMAGE_MANIFEST_ENTRY / LM_IMAGE_MANIFEST (FR-28)
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
 * LM_IMAGE_PACK_OPTIONS (FR-35)
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
 * LM_IMAGE_METADATA (FR-28)
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

/* Per-entry merged-directory enumeration (FR-20). Return non-S_OK to abort. */
typedef HRESULT (LM_CALL *LM_DIR_ENUM_CALLBACK)(
    PCWSTR                name,
    const LM_FILE_INFO*  info,
    void*                 userContext);

/* LayerMount event fan-out (FR-31). */
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

/* ---- Lifecycle / diagnostics-lite (FR-7, FR-10, FR-11, FR-13, FR-31) ---- */

LM_API HRESULT LM_CALL LayerMountGetVersion(
    UINT32* major, UINT32* minor, UINT32* patch, UINT32* abiVersion);

LM_API HRESULT LM_CALL LayerMountGetLastErrorMessage(
    HRESULT hr, PWSTR buffer, SIZE_T bufferChars, SIZE_T* requiredChars);

LM_API HRESULT LM_CALL LayerMountCreate(
    const LM_CONFIG* config, LM_HANDLE* outHandle);

/* Convenience for short-lived overlays used by CLI subcommands that need a
 * valid LM_HANDLE to drive VHD/VSS/Image primitives without mounting a
 * filesystem. Equivalent to LayerMountCreate with: upperPath = workDir, no
 * lower layers, no process tracking. Creates `workDir` (and missing
 * parents) on demand; if creation fails, LayerMountCreate's error path
 * surfaces the precise reason via LayerMountGetLastErrorMessage.
 *
 * Hosts pass their own hostCapabilities so the helper stays portable
 * across NTFS / non-NTFS / non-Windows backends. */
LM_API HRESULT LM_CALL LayerMountCreateTransient(
    PCWSTR workDir, UINT32 hostCapabilities, LM_HANDLE* outHandle);

LM_API HRESULT LM_CALL LayerMountDestroy(LM_HANDLE handle);

LM_API HRESULT LM_CALL LayerMountSetEventCallback(
    LM_HANDLE handle, LM_EVENT_CALLBACK callback, void* userContext);

/* ---- Host adapter integration (FR-13) ----------------------------------
 *
 * LayerMountSetHostAttached is called by a host adapter to mark an overlay
 * as "currently mounted by a host". While the flag is TRUE,
 * LayerMountDestroy refuses to release the instance and returns
 * E_ILLEGAL_METHOD_CALL -- the host adapter must
 * unmount and clear the flag (attached = FALSE) first. General DLL
 * consumers should not call this function; the default value is FALSE
 * so LayerMountDestroy works without ceremony for non-hosted use.
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
 * Host-kernel-agnostic: any Windows host adapter calls these to reserve
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
 *        actively harmful, and claiming ownership before the host mount
 *        call would be a contract lie: nothing is reserved yet.
 *   2. (host-specific mount call here -- creates + mounts on the leaf)
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
 *   5. (host-specific unmount call here)
 *   6. LayerMountPointReleaseIfSafe(mp, &prep)
 *        Best-effort: removes the directory iff we claimed ownership,
 *        the identity still matches, and it's empty. Never fails.
 *
 * LM_MOUNT_POINT_PREP is fixed-shape -- revisions via LM_ABI_VERSION
 * bumps. Hosts must zero-initialize before passing to PrepareDirectory.
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

/* ---- Path / volume (FR-18, FR-21, FR-22) ---- */

LM_API HRESULT LM_CALL LayerMountResolvePath(
    LM_HANDLE handle, PCWSTR relativePath, LM_RESOLVED_PATH* outResolved);

LM_API HRESULT LM_CALL LayerMountGetVolumeInfo(
    LM_HANDLE handle, LM_VOLUME_INFO* outInfo);

LM_API HRESULT LM_CALL LayerMountEnsureInUpperLayer(
    LM_HANDLE handle, PCWSTR relativePath);

/* ---- File primitives (FR-18, FR-19, FR-20) ---- */

/*
 * File primitives that can be invoked inside a host-adapter callback
 * accept an `originatorPid`. A host adapter is expected to read the
 * originating process ID from its kernel/dispatch surface and thread it
 * through so process-tracker rules (FR-32) match the actual requester
 * rather than the dispatcher-thread PID. Pass 0 when the calling code is
 * its own originator (unmounted CLI / tests / direct P/Invoke); the DLL
 * will fall back to GetCurrentProcessId().
 */

LM_API HRESULT LM_CALL LayerMountOpenFile(
    LM_HANDLE       handle,
    PCWSTR           relativePath,
    UINT32           grantedAccess,
    UINT32           createOptions,
    DWORD            originatorPid,        /* 0 = use current process */
    LM_FILE_HANDLE* outFile,
    LM_FILE_INFO*   outInfo);

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

LM_API HRESULT LM_CALL LayerMountCloseFile(LM_FILE_HANDLE file);

LM_API HRESULT LM_CALL LayerMountReadFile(
    LM_FILE_HANDLE file,
    void*           buffer,
    UINT64          offset,
    UINT32          length,
    DWORD           originatorPid,         /* 0 = use current process */
    UINT32*         bytesTransferred);

LM_API HRESULT LM_CALL LayerMountWriteFile(
    LM_FILE_HANDLE file,
    const void*     buffer,
    UINT64          offset,
    UINT32          length,
    BOOL            writeToEnd,
    BOOL            constrainedIo,
    DWORD           originatorPid,         /* 0 = use current process */
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
    DWORD           originatorPid,         /* 0 = use current process */
    LM_FILE_INFO*  outInfo);

/*
 * Flush buffered writes for an open file. Fills `outInfo` with
 * post-flush metadata when non-NULL.
 */
LM_API HRESULT LM_CALL LayerMountFlushFile(
    LM_FILE_HANDLE file,
    DWORD           originatorPid,         /* 0 = use current process */
    LM_FILE_INFO*  outInfo);

LM_API HRESULT LM_CALL LayerMountGetFileInfo(
    LM_FILE_HANDLE file, LM_FILE_INFO* outInfo);

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

LM_API HRESULT LM_CALL LayerMountDeleteFile(
    LM_HANDLE handle, PCWSTR relativePath);

LM_API HRESULT LM_CALL LayerMountCanDeleteFile(
    LM_HANDLE handle, PCWSTR relativePath);

LM_API HRESULT LM_CALL LayerMountCanDeleteOpenFile(
    LM_FILE_HANDLE file);

LM_API HRESULT LM_CALL LayerMountDeleteOpenFile(
    LM_FILE_HANDLE file);

LM_API HRESULT LM_CALL LayerMountRenameFile(
    LM_HANDLE handle,
    PCWSTR     oldRelativePath,
    PCWSTR     newRelativePath,
    BOOL       replaceIfExists);

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

LM_API HRESULT LM_CALL LayerMountGetSecurity(
    LM_HANDLE handle,
    PCWSTR     relativePath,
    UINT32*    outFileAttributes,
    BYTE*      securityDescriptor,
    SIZE_T     securityDescriptorBytes,
    SIZE_T*    requiredBytes);

LM_API HRESULT LM_CALL LayerMountSetSecurity(
    LM_HANDLE  handle,
    PCWSTR      relativePath,
    UINT32      securityInformation,
    const BYTE* modificationDescriptor,
    SIZE_T      modificationDescriptorBytes);

LM_API HRESULT LM_CALL LayerMountMergeDirectory(
    LM_HANDLE             handle,
    PCWSTR                 dirRelativePath,
    LM_DIR_ENUM_CALLBACK  callback,
    void*                  userContext);

LM_API HRESULT LM_CALL LayerMountCreateWhiteout(
    LM_HANDLE handle, PCWSTR relativePath, BOOL isDirectory);

LM_API HRESULT LM_CALL LayerMountSetOpaque(
    LM_HANDLE handle, PCWSTR dirRelativePath);

LM_API HRESULT LM_CALL LayerMountGetReparsePoint(
    LM_HANDLE handle,
    PCWSTR     relativePath,
    BYTE*      buffer,
    SIZE_T     bufferBytes,
    SIZE_T*    requiredBytes);

LM_API HRESULT LM_CALL LayerMountSetReparsePoint(
    LM_HANDLE  handle,
    PCWSTR      relativePath,
    const BYTE* buffer,
    SIZE_T      bufferBytes);

LM_API HRESULT LM_CALL LayerMountDeleteReparsePoint(
    LM_HANDLE  handle,
    PCWSTR      relativePath,
    const BYTE* buffer,
    SIZE_T      bufferBytes);

/* ---- VHD primitives (FR-23, FR-24) ---- */

LM_API HRESULT LM_CALL LayerMountVhdCreate(
    LM_HANDLE            mount,
    const LM_VHD_CONFIG* config,
    LM_VHD_HANDLE*       outVhd);

LM_API HRESULT LM_CALL LayerMountVhdOpen(
    LM_HANDLE            mount,
    const LM_VHD_CONFIG* config,
    LM_VHD_HANDLE*       outVhd);

LM_API HRESULT LM_CALL LayerMountVhdAttach(
    LM_VHD_HANDLE vhd,
    PWSTR          physicalPathBuffer,
    SIZE_T         physicalPathChars,
    SIZE_T*        physicalPathRequired);

LM_API HRESULT LM_CALL LayerMountVhdDetach(LM_VHD_HANDLE vhd);

LM_API HRESULT LM_CALL LayerMountVhdMerge(LM_VHD_HANDLE childVhd);

LM_API HRESULT LM_CALL LayerMountVhdImport(
    LM_HANDLE mount,
    PCWSTR     directoryPath,
    PCWSTR     vhdPath,
    UINT64     sizeBytes);

LM_API HRESULT LM_CALL LayerMountVhdExport(
    LM_HANDLE mount,
    PCWSTR     vhdPath,
    PCWSTR     directoryPath);

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

/* ---- VSS primitives (FR-26, FR-27) ---- */

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

LM_API HRESULT LM_CALL LayerMountVssDeleteSnapshot(
    LM_HANDLE mount, PCWSTR snapshotId);

LM_API HRESULT LM_CALL LayerMountVssListSnapshots(
    LM_HANDLE             mount,
    LM_VSS_SNAPSHOT_INFO* entries,
    UINT32                 entriesCapacity,
    UINT32*                entriesWritten,
    UINT32*                entriesRequired);

LM_API HRESULT LM_CALL LayerMountVssCleanupSnapshots(LM_HANDLE mount);

/*
 * Release the handle-table slot backing an LM_VSS_SNAPSHOT_HANDLE. The
 * snapshot itself -- if non-persistent -- remains tracked by the
 * engine's VSSManager and is eligible for LayerMountVssCleanupSnapshots.
 * Persistent snapshots remain until the caller explicitly deletes them
 * or the backup admin does. Closing the receipt handle does not alter
 * the snapshot's existence.
 */
LM_API HRESULT LM_CALL LayerMountVssCloseSnapshot(LM_VSS_SNAPSHOT_HANDLE snapshot);

/* ---- Layer image primitives (FR-28) ---- */

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

LM_API HRESULT LM_CALL LayerMountImageUnpack(
    LM_HANDLE mount,
    PCWSTR     imagePath,
    PCWSTR     targetDir,
    BOOL       verifyChecksum);

LM_API HRESULT LM_CALL LayerMountImageValidate(
    LM_HANDLE mount, PCWSTR imagePath);

LM_API HRESULT LM_CALL LayerMountImageGetManifest(
    LM_HANDLE          mount,
    PCWSTR              imagePath,
    LM_IMAGE_MANIFEST* manifest);

LM_API HRESULT LM_CALL LayerMountImageGetMetadata(
    LM_HANDLE          mount,
    PCWSTR              imagePath,
    LM_IMAGE_METADATA* metadata);

LM_API HRESULT LM_CALL LayerMountImageClose(LM_IMAGE_HANDLE image);

/* ---- Diagnostics, eventing, process tracker (FR-30, FR-31, FR-32) ---- */

LM_API HRESULT LM_CALL LayerMountGetStats(
    LM_HANDLE handle, LM_STATS* outStats);

LM_API HRESULT LM_CALL LayerMountProcessTrackerEnable(
    LM_HANDLE handle, BOOL enable);

LM_API HRESULT LM_CALL LayerMountProcessTrackerSetRules(
    LM_HANDLE handle, PCWSTR rulesPath);

LM_API HRESULT LM_CALL LayerMountProcessTrackerExportJson(
    LM_HANDLE handle,
    PWSTR      buffer,
    SIZE_T     bufferChars,
    SIZE_T*    requiredChars);

LM_API HRESULT LM_CALL LayerMountProcessTrackerExportCsv(
    LM_HANDLE handle,
    PWSTR      buffer,
    SIZE_T     bufferChars,
    SIZE_T*    requiredChars);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LAYERMOUNT_H */
