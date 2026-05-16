# LayerMount.dll — Architecture

This document describes the internal architecture of `LayerMount.dll`, the
native engine that powers every LayerMount host on Windows. The DLL is
deliberately host-agnostic: it owns the overlay semantics (layer
precedence, whiteouts, copy-up, metadata) and exposes them through a
stable C ABI. Filesystem-host adapters live above this DLL and are the
only consumers of its handle types.

The engine takes its model from Linux `layermount(5)`: a single writable
**upper** layer stacks on top of zero or more read-only **lower** layers,
and a **work** directory provides scratch space for atomic operations.
Reads pass through to the first layer that has the file; writes promote
the file into the upper layer with a copy-up; deletes drop a *whiteout*
marker that hides the lower entry from the merged view. The lower layers
are never modified.

---

## Repository layout

```
src/LayerMount.dll/
  public/         LayerMount.h          Stable C ABI (the only installed header)
                  LayerMount.def        Exported symbols
  abi/            CapabilityGate.h     Host-capability bitfield wrapper
                  EventEmitter.h       Host event-callback fan-out
                  ErrorTls.{h,cpp}     Per-thread last-error storage
                  HandleTypes.h        Per-handle-kind payloads + magics
                  HandleTable.{h,cpp}  Typed slot/generation/magic registry
                  AbiCore.cpp          Lifecycle + diagnostics shims
                  AbiFile.cpp          File-primitive shims
                  AbiPath.cpp          Path/volume shims
                  AbiHostMountPoint.cpp Mount-point validation shims
                  AbiVhd.cpp           VHD shims
                  AbiVss.cpp           VSS shims
                  AbiImage.cpp         Layer-image shims
                  AbiDiagnostics.cpp   Stats / process-tracker shims
                  AbiGuard.h           Catch-all wrappers + ErrorTls plumbing
  impl/           LayerMount.{h,cpp}    Engine class + LayerConfig + utilities
                  PathResolver.{h,cpp} Layer-search + redirect resolution
                  WhiteoutManager.{h,cpp} `.wh.` markers + opaque dirs
                  CopyUp.{h,cpp}       Full + metacopy copy-up + dir rename
                  Cache.{h,cpp}        LRU resolved-path cache
                  MetadataADS.{h,cpp}  Per-file metadata dispatcher
                  SidecarMetadata.{h,cpp} Non-NTFS metadata fallback store
                  ProcessTracker.{h,cpp} Per-PID access log + rule eval
                  ComScope.{h,cpp}     RAII COM init for VSS
                  NtStatusUtil.{h,cpp} Win32 -> NTSTATUS translation
                  host/                Windows mount-point helpers
                  image/               .lmnt layer-image pack/extract
                  vhd/                 VHDX layer manager + manifest
                  vss/                 VSS snapshot manager
```

The split is load-bearing:

- `impl/` knows nothing about the public ABI types and never includes
  `public/LayerMount.h` directly. It works with `std::wstring`, NTSTATUS,
  and engine-native structs (`InternalFileInfo`, `ResolvedPath`, ...).
- `abi/` is the only layer that touches both worlds. Every shim
  translates ABI inputs into engine calls, catches escaping exceptions,
  records the failure into thread-local storage, and returns an HRESULT.
- `public/` contains exactly one header. Internal headers under `abi/`
  and `impl/` are not installed; consumers must never include them.

---

## Design philosophy

**Host-agnostic.** The DLL has no compile-time or run-time dependency on
any particular filesystem-host kernel. Adapters bind the engine to the
host's callback table and translate the host's request shape into ABI
calls; the engine answers in pure HRESULT/NTSTATUS without knowing what
delivered the request. A unit test that drives the engine through the C
ABI sees exactly what a production host sees.

**Lower layers are read-only.** No engine code path writes into a lower
path. The closest the engine comes is *reading* whiteout markers from a
lower layer — which is allowed because some workflows pre-populate
lowers from layer images that already carry whiteouts.

**No silent scope drop.** If a requested capability cannot be honored,
the engine surfaces the failure (or, where the ABI documents a
fallback, executes that fallback explicitly and emits an event). It
does not silently downgrade. Examples:
- A whiteout that cannot be persisted causes the surrounding `Delete` to
  fail, because returning success would let the lower entry resurface.
- An opaque marker that cannot be persisted causes the directory
  `Create` that needed it to roll back and fail.
- A process-tracker rules file that cannot be loaded causes
  `SetProcessTrackerEnabled(true)` to fail rather than coming up with an
  empty (allow-everything) rule set.

**Capability gating, not feature stubs.** When the host advertises
limited capabilities (no ADS, no sparse files, no NTFS ACLs), the engine
takes a documented fallback path rather than refusing the operation.
The `CapabilityGate` wrapper makes the choice explicit at every fork.

**Errors carry context.** Every ABI shim that catches an exception or
records a failure stores a UTF-16 message in thread-local storage; the
caller fetches it via `LayerMountGetLastErrorMessage(hr, ...)`. The HRESULT
reported back is the precise translation of the underlying `GetLastError`
or `STATUS_*`, never a generic `E_FAIL`.

**Atomicity at the boundary.** Anything that mutates the upper layer
(file copy-up, directory rename across layers, whiteout creation) goes
through the work directory and lands in the upper layer with a single
rename, so a crash mid-operation never leaves a half-built shadow.

---

## The layer model

`LayerConfig` (in `impl/LayerMount.h`) carries the layout:

```cpp
struct LayerConfig {
    std::wstring upperPath;                  // writable layer (required)
    std::vector<std::wstring> lowerPaths;    // index 0 = highest-priority lower
    std::wstring workDirPath;                // copy-up scratch
    bool         enableProcessTracking;
    std::wstring processRulesPath;
    size_t       accessLogCapacity;
    size_t       pathCacheCapacity;
    UINT32       hostCapabilities;           // LM_HOST_CAPABILITIES bitfield
};
```

Lookup precedence is **upper, then lowers in declared order, first-match
wins**. A file present in the upper layer is the file the merged view
shows — even if a lower layer also has a copy. A file present in
`lowerPaths[1]` is shadowed by a copy in `lowerPaths[0]`.

`LayerConfig::Validate` checks that `upperPath` is an existing directory
and is writable (by writing and deleting a temp file under
`FILE_FLAG_DELETE_ON_CLOSE`). It verifies every lower path exists. It
emits a debug warning when the upper layer's volume is not NTFS or
ReFS — non-NTFS hosts cannot use the ADS metadata path and fall back to
sidecar JSON files.

`LayerConfig::Prepare` creates the work directory if missing.

A *root* path resolves directly to the upper layer's directory: every
overlay always shows at least the upper layer's contents, even with no
lowers configured.

---

## Whiteout philosophy

A whiteout is the engine's way of saying "this path is logically deleted
in this layer's view". Whiteouts let the engine support deletion without
modifying read-only lower layers.

### Whiteout markers (`.wh.<name>`)

The format is borrowed directly from Linux/AUFS: a deleted file or
directory at `<dir>/<name>` is represented in the layer that owns the
delete by an empty file at `<dir>/.wh.<name>`. The marker is created
with `FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_FLAG_BACKUP_SEMANTICS`.

Constants live in `impl/LayerMount.h`:

```cpp
constexpr const wchar_t* kWhiteoutPrefix    = L".wh.";
constexpr const wchar_t* kOpaqueMarkerFile  = L".wh..wh..opq";
```

`WhiteoutManager` owns the lifecycle:

- `IsWhiteoutName(name)` — does this filename start with `.wh.`?
- `HasWhiteout(rel, layer)` — is `<layer>/<parent>/.wh.<name>` present?
- `HasWhiteoutInAnyLayer(rel)` — checks upper, then lowers in order.
- `CreateWhiteout(rel, type)` — drops the marker in the upper layer
  (always; a delete that needs a whiteout owns the whiteout in upper).
- `RemoveWhiteout(rel)` — used by `Create`/`Rename` when a new file
  resurrects a previously-deleted name.
- `ListWhiteoutsInDirectory(dir, layer, ok)` — directory-merge support;
  the `ok` out-param is **false** when enumeration was interrupted, so
  the caller refuses to expose a partial list (a missing whiteout would
  surface a logically-deleted lower entry).

`FILE_FLAG_BACKUP_SEMANTICS` on the marker open is intentional: a
parent directory that inherited a `DENY-WRITE` ACE from the lower layer
would otherwise block whiteout creation. Combined with
`SE_RESTORE_NAME` (enabled in `CopyUp::EnsureCopyUpPrivileges`), the
backup-semantics handle bypasses the DACL check so the engine can
always persist the marker for an entry it owns.

### Per-layer whiteout scoping

A whiteout in **layer N** hides the corresponding name from **layer N**
itself and from every layer **below N**, but **not** from layers above
N. This matches Linux layermount semantics: an upper-layer write can
re-introduce a name that a lower-layer whiteout previously hid. Concretely:

- Whiteout in `lowerPaths[0]` hides the entry from `lowerPaths[0..n]`.
- Whiteout in upper hides the entry from upper and from every lower.
- A new file written into upper at the same path replaces the upper
  whiteout (the create path explicitly removes the marker on commit;
  see "Resurrection windows" below).

### Opaque directories

A directory marked **opaque** in a layer hides every lower-layer entry
beneath it, regardless of name. Opaque is the directory analog of a
whiteout: it expresses "this directory's contents in this layer are the
authoritative set; do not merge children from below".

Two coexisting representations:

1. **NTFS ADS marker** `<dir>:overlay.opaque` — fast, single-stat detection.
2. **Sentinel file** `<dir>\.wh..wh..opq` — present for compatibility
   with image producers that don't speak ADS, and as the fallback on
   non-NTFS upper layers.

`WhiteoutManager` writes both markers when both are available, and
either marker satisfies `IsOpaque`/`IsOpaqueInLayer`. Removal cleans
both stores.

The two main producers of opaque markers:

- `Create(rel, FILE_DIRECTORY_FILE, ...)` when an entry of the same name
  exists in any lower layer. Without the marker, the new (empty) upper
  directory would still expose the lower contents through the merged
  view.
- `CopyUp::HandleDirectoryRename` when the source directory came from a
  lower layer. After the recursive copy, the destination is opaque so
  subsequent merges don't re-pull files from the lower-layer source
  through the post-rename path.

### Inheritance: ancestor whiteouts and ancestor opaques

The resolver applies the rules transitively:

- `WhiteoutManager::HasWhitedOutAncestorInLayer(rel, layer)` walks the
  parent chain and returns true if any ancestor in `layer` carries a
  `.wh.<name>` marker. A whitedout directory hides every descendant
  from its layer downward.
- `WhiteoutManager::HasOpaqueAncestor(rel)` /
  `HasOpaqueAncestorInLayer(rel, layer)` walks the parent chain and
  returns true if any ancestor is opaque. An opaque ancestor hides
  every descendant in lower layers.

Both walks stop at the layer root.

### Resurrection windows and ordering

A whiteout that lingers after a successful `Create` would hide the
newly-created upper file, so the engine takes care to:

1. Read the whiteout state **before** the create attempts the
   filesystem write.
2. Perform the create (`CreateFileW(... CREATE_NEW)` or
   `CreateDirectoryW`).
3. Only after a successful create, remove the whiteout marker.

This deferred-remove pattern means that a failed `Create` leaves the
marker intact and the path stays logically deleted — the caller does
not see a transient resurrection of a lower entry.

The reverse pattern applies to delete: the upper shadow is removed
first; the whiteout is dropped second. If the whiteout cannot be
written, the delete is reported as failed because the lower entry would
otherwise resurface on the next resolve.

### What about whiteouts in directory listings?

Whiteout markers are *never* visible in the merged view. Directory
merging in `LayerMount::MergeDirectoryEntries`:

1. Enumerates upper. For each `.wh.<name>` it sees, it strips the prefix
   and adds `<name>` to a `whitedOutNames` set, then *skips* the marker
   itself.
2. For each lower, it enumerates; before consuming entries it loads the
   layer's whiteouts (via `ListWhiteoutsInDirectory`) into the same set.
   If enumeration of whiteouts fails mid-stream, the merge aborts
   descent through deeper lowers — a partial whiteout list could leak a
   logically-deleted entry.
3. For each enumerated entry, lower wins only if upper did not already
   produce it AND the name is not in `whitedOutNames`.
4. Opaque directories short-circuit step 2: if the directory is opaque
   in the upper layer, no lower is enumerated. If the directory is
   opaque in lower N, layers N+1..end are skipped.

The reserved sidecar subtree (`.overlay`) is filtered out of the merged
view as well — see "Reserved namespaces" below.

---

## Path resolution

`PathResolver::ResolvePath` is the single read-side entry point. The
algorithm (in `impl/PathResolver.cpp`):

```
1.  Reject if redirect depth > kMaxRedirectDepth (40).
2.  Normalize: strip leading/trailing '\', fold '/' to '\',
    lower-case the whole string. (NTFS is case-insensitive.)
2a. Empty (root) -> return upper-layer root.
2b. Reject unsafe paths (..-segments, embedded ':').
2c. Reject paths inside the reserved `.overlay\` subtree.
3.  Cache lookup; return on hit.
4.  Probe upper. If present, read `:overlay` ADS:
      - If `redirect` is set, recurse with depth+1 (renamed-from-lower
        bookkeeping; see CopyUp section).
      - Otherwise return the upper hit.
5.  If upper has a whiteout marker for this path, return isWhiteout=true.
5a. If any ancestor in upper carries a whiteout marker, treat the
    descendant as whited-out as well.
6.  If any ancestor in upper is opaque, return not-found
    (the ancestor's opacity hides the lower content).
7.  For each lower in priority order:
      a. Whiteout in this lower => stop iterating.
      b. Whitedout ancestor in this lower => stop iterating.
      c. Opaque ancestor in this lower => stop iterating.
      d. Probe this lower; on hit, capture and break.
8.  If a lower hit was captured, scan deeper lowers for type conflicts
    (file vs. directory) and log via OutputDebugStringW (the resolved
    entry still wins; the log is diagnostic).
9.  Cache the result and return.
```

`ResolveLowerPath` runs the same pipeline with step 4 omitted — it is
used by copy-up and by rename when the engine needs to know what the
*lower* state looks like independent of any upper shadow.

The redirect step (4) is the metacopy mechanism. After a `Rename` of an
entry that lived in a lower layer, the engine writes a metacopy stub
into the upper layer's *destination* path with `:overlay.redirect`
pointing back at the source. A subsequent resolve of the destination
follows the redirect to the lower-layer source until a real copy-up
materializes the data. The 40-step depth cap defends against malformed
chains.

### Path safety guards

`IsSafeRelativePath(normalized)` is called at every write-side entry
point (`Create`, `Rename`, `Delete`, `UpdateContextPath`,
`SetReparsePoint`, `MergeDirectoryEntries` for traversal protection).
It rejects:

- empty input,
- any `..` segment (Windows canonicalizes
  `upper\..\escape.txt` to `escape.txt` outside the layer root),
- any `:` (drive letter or ADS suffix injection).

`IsReservedRelativePath(normalized)` rejects the `.overlay` directory
and anything beneath it. That subtree is the sidecar-metadata store on
non-ADS hosts (see below); exposing it through the merged view would
let a caller open, modify, or delete internal records.

---

## Copy-up

Copy-up is the action of moving a file from a lower layer to the upper
layer so the engine can mutate it. `CopyUp` (in `impl/CopyUp.cpp`)
implements four flavors.

### Full copy-up (`CopyUpFile`)

The default for small files and the fallback when sparse files are
unavailable. Steps:

1. Acquire the in-flight lock for `relativePath`. If another thread is
   already copying up the same path, wait on the condition variable
   until it clears, then re-check `ExistsInUpper` and return success
   (the winner committed).
2. Resolve via `ResolveLowerPath` to find the source.
3. Ensure parent directories exist in the upper layer. Each missing
   parent triggers `CopyUpDirectory` so security descriptors and
   timestamps propagate.
4. Generate a unique work-dir path under `workDirPath`.
5. Stream data from the lower handle into the work-dir handle in 64 KB
   chunks. Open the source with `FILE_FLAG_BACKUP_SEMANTICS` so
   `SE_BACKUP_NAME` can read past restrictive DACLs.
6. Mirror metadata: file attributes, all three timestamps, security
   descriptor (DACL/SACL/owner/group), and every alternate data stream
   except the reserved `:overlay*` namespace.
7. Write the `:overlay` metadata record (origin layer, copy-up
   timestamp, captured stable index number).
8. `MoveFileExW(work, upper, MOVEFILE_REPLACE_EXISTING |
   MOVEFILE_WRITE_THROUGH)`. If the destination's parent DACL denies
   the move, fall back to `SetFileInformationByHandle(FileRenameInfo)`
   on a backup-semantics-opened source handle, which honors
   `SE_RESTORE_NAME`.
9. Invalidate the cache for the affected path (and ancestors).
10. Bump `stats.copyUpCount` and emit `LM_EVT_COPY_UP`.

### Metacopy / lazy copy-up (`CopyUpMetadataOnly`)

For files larger than 1 MiB on hosts that support sparse files
(`LM_CAP_SPARSE_FILES`), the engine stages a *metacopy shell* in the
upper layer instead of a full data copy:

1. Create the upper file as a sparse file (`FSCTL_SET_SPARSE`) of the
   correct logical size, with no allocated data blocks.
2. Mirror security, attributes, timestamps, and ADS — everything
   *except* the data.
3. Write the `:overlay` metadata with `metacopy = true` and the origin
   layer recorded.
4. Ownership flips to the upper layer; the file context is marked
   `isMetacopyOnly = true`.

When a read or write subsequently targets this file, `Read`/`Write`
calls `CompleteLazyCopyUp` to stream the actual data from the recorded
origin into the upper sparse skeleton, clear the metacopy flag, and
reopen the handle with the caller's original `grantedAccess` so
subsequent I/O still works. Without the sparse capability the engine
forces a full copy-up at open time — a non-sparse "metacopy" would be a
dense zero-filled stub, the worst of both worlds.

### Directory copy-up (`CopyUpDirectory`)

Creates the directory in the upper layer if missing, copies the
security descriptor and timestamps from the lower-layer source, writes
the `:overlay` metadata, and (importantly) does **not** recurse. The
directory's children remain in the lower layer until they themselves
are copied up on demand.

### Cross-layer directory rename (`HandleDirectoryRename`)

Directory rename is the worst case: a single Win32 `MoveFileExW` cannot
move a directory tree out of a read-only layer into a writable one.
The engine handles four cases:

- **upper → upper**: a single `MoveFileExW`. Transfer the opaque marker
  if present.
- **lower → upper, replace=false, dest-not-present**: recursive copy
  via `CopyTreePreservingMetadata` (preserves reparse points, sparse
  bits, and ADS), then mark the destination opaque, then drop a
  whiteout at the source.
- **lower → upper, replace=true, dest-present-in-upper**: same as
  above but `remove_all` clears the upper destination first.
- **rename to a destination that already exists in lower (with
  replace=false)**: rejected with `STATUS_OBJECT_NAME_COLLISION` before
  any side effects.

Recursive copy-up is expensive and is the main reason single-file
metacopy exists; the engine cannot apply the same trick to directories
because a metacopy shell only makes sense for files.

### Concurrent copy-up coordination

`CopyUp` carries an `inFlightCopyUps_` set guarded by `copyUpMutex_`
and signaled by `copyUpCV_`. A second caller that races on the same
path waits for the first to commit, then short-circuits when the upper
shadow is observed. Without this, both threads would race at the
`MoveFileExW` step and the loser would see `ERROR_ACCESS_DENIED` or a
sharing violation from the just-placed target.

### Privileges

`CopyUp::EnsureCopyUpPrivileges` enables three privileges on the
process token at first use:

- `SE_CREATE_SYMBOLIC_LINK_NAME` — required by `FSCTL_SET_REPARSE_POINT`
  for `IO_REPARSE_TAG_SYMLINK`. Even elevated tokens carry this
  privilege disabled by default.
- `SE_RESTORE_NAME` and `SE_BACKUP_NAME` — let backup-semantics opens
  bypass DACL checks. Without these, copy-up of a child into a
  restrictive parent fails.
- `SE_SECURITY_NAME` — required to read/write SACL audit ACEs. Tracked
  separately in `IsSecurityPrivAvailable` so callers can skip the SACL
  bit when the privilege is not held (standard-user process), instead
  of failing the whole `GetFileSecurityW` call.

---

## Per-file metadata

Each upper-layer file can carry overlay metadata:

```cpp
struct LayerMountMetadata {
    bool         opaque         = false;
    bool         metacopy       = false;
    std::wstring redirect;          // for renamed-from-lower indirection
    FILETIME     copyUpTimestamp;
    std::wstring originLayer;       // path of the source layer
    bool         hasStableIndexNumber;
    uint64_t     stableIndexNumber; // preserves nFileIndex across copy-up
};
```

`stableIndexNumber` is the lower layer's `BY_HANDLE_FILE_INFORMATION`
file ID, captured at copy-up time and replayed in `FillFileInfoFromHandle`
so callers that rely on the index for identity tracking (e.g. open-by-id
shims) see a consistent value before and after copy-up.

### Two backends, one dispatcher

`MetadataADS` is the engine's dispatcher. It picks between two stores
based on the host's advertised capability (`LM_CAP_ADS`):

1. **NTFS Alternate Data Streams** — the optimized path. Metadata lives
   in the `:overlay` ADS on the file itself; the opaque marker lives in
   `:overlay.opaque` on the directory. Single-stat detection, no
   sidecar to keep in sync.
2. **Sidecar JSON** (`SidecarMetadata`) — the fallback for non-NTFS
   upper layers (FAT32, exFAT, network shares without stream support).
   Metadata lives at `<upper>\.overlay\<sha1(rel)>.meta.json` and the
   opaque marker at `<upper>\.overlay\<sha1(rel)>.opaque`.

Why SHA-1 of the path? It side-steps NTFS's filename character
restrictions (`<`, `>`, `:`, ...) and tolerates very long paths in a
flat namespace. Lowercased before hashing so it matches NTFS
case-insensitivity.

Reads from `MetadataADS::ReadLayerMountMetadata` fall through from ADS to
sidecar transparently. Writes pick the store based on the capability
bit. Removes clean **both** stores so a host can flip capabilities (or
the operator can change file system) without leaving stale records
behind.

The `corrupted` out-param on `Read` distinguishes "no metadata
recorded" from "metadata exists but cannot be parsed". Callers whose
correctness depends on metadata fidelity (resolver-side redirect
follow) treat `*corrupted == true` as a hard failure rather than
silently returning the default record.

### Reserved namespaces

The engine reserves two namespaces in the upper layer:

- `:overlay`, `:overlay.opaque` ADS streams. `Overwrite` (CREATE_ALWAYS
  semantics) deletes user ADS streams but skips anything starting with
  `:overlay`.
- The `<upper>\.overlay\` directory. `IsReservedRelativePath` rejects
  every read and write that targets it, and `MergeDirectoryEntries`
  filters it out of root listings.

---

## Caching

`Cache` is an LRU map from normalized relative path to `ResolvedPath`,
guarded by a `std::shared_mutex`. The default capacity is 10000
entries; the host can raise it via `LM_CONFIG::pathCacheCapacity` for
large lower trees that thrash on cold start.

Reads acquire the shared lock and (under the shared lock) update each
entry's `lastAccessTicks` via `std::atomic_ref` so concurrent readers
don't block each other for LRU bookkeeping. Writes acquire the
exclusive lock; the eviction pass picks the oldest tick.

Invalidation is explicit at every mutation:

- `Invalidate(rel)` clears `rel` and every descendant.
- `InvalidateWithAncestors(rel)` clears `rel`, every descendant, and
  every ancestor up to the root in a single pass under one lock
  acquisition. Used after any path-mutating operation
  (Create/Delete/Rename/Write/SetSecurity/SetReparsePoint/Overwrite)
  so a parent directory's enumeration cache cannot retain a stale view
  of a child's existence.

---

## Process tracking and access rules

`ProcessTracker` is optional and off by default. When enabled (via
`LayerMountProcessTrackerEnable` or the `enableProcessTracking` config bit),
every file primitive that takes a `callerPid` consults it.

### Per-PID resolution cache

`ResolveProcess(pid)` returns name, full path, command line, owning
user, and elevation state. Lookups are cached for 30 seconds keyed on
PID — the TTL keeps a long-lived process from holding stale data if
its PID is recycled, while avoiding a `OpenProcess`/`QueryFullProcessImageName`
round trip on every operation.

### Rule evaluation

Rules are loaded from a JSON file (path in `LayerConfig::processRulesPath`).
Each rule has a wildcard pattern for the process name and a wildcard
pattern for the relative path, plus four allow bits (read, write,
execute, delete). The first matching rule wins. The engine refuses to
enable the tracker if a configured rules file fails to load — an empty
rule set means "everything is allowed", which is rarely the operator's
intent.

`CheckAccess(pid, rel, op)` classifies the operation, walks the rules,
and either returns true (allowed) or returns false and emits
`LM_EVT_ACCESS_DENIED`. A denied check causes the engine primitive to
return `STATUS_ACCESS_DENIED`.

### Access log

A circular buffer of `AccessLogEntry` records. `LogAccess` appends;
`GetRecentEntries(count)` returns the most-recent N. The buffer
capacity comes from `LayerConfig::accessLogCapacity` (default 10000).
Full export is available as JSON or CSV via `ExportLog` (file) or
`ExportLogAsJson`/`ExportLogAsCsv` (in-memory string for ABI use).

### Hot-swap safety

`processTracker_` is a `shared_ptr` guarded by a `shared_mutex`.
Readers take the shared lock, copy out the pointer, and release the
lock before calling into the tracker — so a concurrent
`SetProcessTrackerEnabled(false)` cannot destroy the tracker out from
under an in-flight check. The reader's local `shared_ptr` keeps the
tracker alive until the check completes.

---

## Auxiliary subsystems

These live alongside the core engine and share its handle table, but
are independent of overlay semantics. They are lazy-constructed on
first use because most overlays never exercise them.

### VHDX layer manager (`impl/vhd/`)

`VHDLayerManager` wraps the Win32 `VirtAttach`/`AttachVirtualDisk` API
to attach a VHD or VHDX file as a Windows volume, expose its volume
GUID path, and record it in a JSON manifest at
`<workDir>\layers.manifest.json`. The manifest is mutated under a named
mutex (`ManifestLock`) so concurrent processes don't corrupt it.
`Attach` lifetimes are either permanent (survives process exit) or
process-scoped (auto-detached when the last handle closes).

### VSS snapshot manager (`impl/vss/`)

`VSSManager` drives the VSS COM interface to take, list, and delete
shadow copies. COM is initialized per-thread via `ComScope` (RAII).
Snapshots can be persistent (visible to the backup admin until
explicitly deleted) or non-persistent (cleaned up by
`LayerMountVssCleanupSnapshots`).

### Layer-image manager (`impl/image/`)

A `.lmnt` layer image is a self-contained file with:

- 128-byte `LayerImageHeader` (magic `OVLYIMG\0`, version, compression
  type, offsets, SHA-256 of data section).
- JSON metadata block (id, parent id, author, description, tags,
  whiteouts list, file count, sizes).
- Compressed (zstd) tar-like archive of `FileEntryHeader` + UTF-8 path
  + data, terminated by a sentinel header with `nameLength = 0xFFFF`.

`LayerImageManager::Pack` walks a source directory and emits the
archive; `Unpack` reverses the process and verifies the SHA-256 unless
the caller opts out. `PackDifferential` produces an image that records
only the entries that differ from a base directory and writes whiteout
entries for files deleted in the source. A multi-image manifest groups
ordered images for distribution.

---

## ABI surface

The DLL exports a single set of C functions, all `__stdcall`, all
returning `HRESULT`. Patterns:

### Two-call buffer pattern

Output strings, blobs, and arrays use the two-call pattern: pass
`buffer = NULL`, `bufferChars = 0` to receive the required size in
`*requiredChars`; allocate; call again with the populated buffer. A
short buffer returns `HRESULT_FROM_WIN32(ERROR_MORE_DATA)` and always
writes `*requiredChars` so the caller can resize and retry. The engine
honors this on every list/string surface, including nested per-entry
strings inside `LM_VHD_LAYER_INFO` and `LM_VSS_SNAPSHOT_INFO`.

### Forward-extensible structs

`LM_CONFIG`, `LM_VHD_CONFIG`, and `LM_IMAGE_PACK_OPTIONS` carry a
`structSize` field as their first member. Callers set it to
`sizeof(STRUCT)` at their compile time; the engine compares against
the size it knows about and interprets only the fields it understands.
Adding a field at the end of the struct does not bump
`LM_ABI_VERSION`. Reordering or removing a field does.

Fixed-shape structs (`LM_FILE_INFO`, `LM_STATS`, `LM_EVENT`,
`LM_VOLUME_INFO`) revise only via `LM_ABI_VERSION` bumps.

### Opaque handles

Five handle kinds: `LM_HANDLE`, `LM_FILE_HANDLE`, `LM_VHD_HANDLE`,
`LM_VSS_SNAPSHOT_HANDLE`, `LM_IMAGE_HANDLE`. Each is a pointer-typed
typedef pointing at an incomplete sentinel struct so the C type system
prevents wrong-kind handle passes at compile time. The pointer value
itself is *not* a real pointer — it encodes:

```
[63:48] 16-bit per-kind magic      (kMagicLayerMount = 0xFE01, etc.)
[47:24] 24-bit generation counter  (bumped on free)
[23:0]  24-bit slot index          (slot 0 reserved for "invalid")
```

`HandleTable<T, Magic>` (in `abi/HandleTable.h`) is a typed registry
guarded by a shared mutex. `Resolve` returns `nullptr` for stale
generations, wrong magic, or out-of-range slots; the ABI shim
translates `nullptr` to `E_HANDLE`. Payloads are `shared_ptr`-owned, so
a concurrent `Free` cannot destroy a payload while another thread still
holds a `Resolve` result — the slot is marked dead but the payload
outlives the slot until the last reference drops.

`LayerMountHolder` (the payload behind `LM_HANDLE`) carries a
`childCount` that file handles increment on allocation and decrement on
free. `LayerMountDestroy` refuses to release the engine while
`childCount != 0` or `hostAttached == true`. File handles additionally
pin their parent overlay via a `shared_ptr<LayerMountHolder>`, so a
buggy host that bypasses the `childCount` check still cannot trigger a
use-after-free.

### Error reporting

`ErrorTls::Set` / `Last` provide per-thread last-error storage. Every
ABI shim catches escaping exceptions, records the HRESULT plus a
human-readable message, and returns the HRESULT to the caller. The
caller fetches the message via `LayerMountGetLastErrorMessage(hr, ...)`
on the same thread; passing a different HRESULT than the one stored
returns `*requiredChars = 0` so a caller cannot accidentally read a
stale message belonging to a different operation.

Reserved overlay-specific HRESULT range: `FACILITY_ITF` codes
`0xB000..0xBFFF`. Hosts must not emit codes in this range from their
own layers.

`LayerMountHResultToNtStatus` converts any HRESULT the engine returns
into the equivalent NTSTATUS for adapters that bridge into an
NTSTATUS-shaped host callback surface. Coverage includes the
COM-style codes, `FACILITY_WIN32`-wrapped Win32 errors (the path the
engine takes when wrapping `GetLastError`), and `HRESULT_FROM_NT`
inversions. Unknown codes map to `STATUS_UNSUCCESSFUL`.

### Event callback

`LayerMountSetEventCallback` installs a `LM_EVENT_CALLBACK` that the
engine fans out for four event types:

- `LM_EVT_WARNING` — non-fatal degradation.
- `LM_EVT_COPY_UP` — every successful copy-up commit.
- `LM_EVT_WHITEOUT_CREATED` — every persisted whiteout marker.
- `LM_EVT_ACCESS_DENIED` — every denied process-tracker decision.

`EventEmitter` (in `abi/EventEmitter.h`) snapshots the callback under a
shared lock, increments an in-flight counter, releases the lock, and
invokes the callback. `Clear` takes the unique lock to null out the
slot, then spins on the in-flight counter until zero — guaranteeing no
further invocations of the previously-registered callback are in
progress, so the host can safely free the user context (e.g. a managed
GCHandle).

### Mount-point helpers

`LayerMountPointPrepareDirectory` /
`LayerMountPointCaptureIdentity` / `LayerMountPointReleaseIfSafe`
are host-kernel-agnostic helpers for the directory-mount-point
lifecycle. The host calls them around its own mount/unmount calls so
the ownership-tracked release at the end never deletes a directory the
host did not create. Drive-letter mount points (`X:` / `X:\`) skip the
dance entirely — `LayerMountPointIsDriveLetter` reports them.

---

## Threading model

Every public ABI function is safe to call from any thread on any
handle at any time. Internal synchronization is the engine's
responsibility. Two callers may concurrently read and write the same
file handle; the engine serializes inside the call.

Locks owned by the engine:

- `Cache::mutex_` — `shared_mutex`; readers shared, mutations exclusive.
- `CopyUp::copyUpMutex_` — protects `inFlightCopyUps_` and the CV.
- `LayerMount::processTrackerMutex_` — `shared_mutex` for hot-swap.
- `LayerMount::vhdMutex_`, `vssMutex_`, `imagesMutex_` — first-use init.
- `HandleTable<>::mutex_` — `shared_mutex` per typed table.
- `EventEmitter::mutex_` — `shared_mutex` around callback slot.
- `ProcessTracker::processInfoMutex_`, `rulesMutex_`, `logMutex_`.
- `ManifestLock` — cross-process named mutex for the VHD manifest.
- `VhdHolder::stateMutex` — per-handle attach/detach serialization.

Callbacks (`LM_EVENT_CALLBACK`, `LM_DIR_ENUM_CALLBACK`) may fire on
arbitrary engine-internal threads and **must not** re-enter the DLL
synchronously on the same handle.

---

## What this DLL deliberately does *not* own

- Mounting. The engine has no mount/unmount lifecycle. A host adapter
  binds a filesystem-host kernel to the engine's primitives and is
  responsible for surfacing the merged view as a real Windows volume.
  `LayerMountSetHostAttached` is the single coupling point: while the flag
  is set, `LayerMountDestroy` returns `E_ILLEGAL_METHOD_CALL` so the host
  must unmount and clear the flag first.
- Driver lifecycle. Anything that installs, configures, or licenses a
  filesystem-host kernel lives in the host adapter, not in the engine.
- CLI. The shipped command-line surface (mount/vhd/vss/layer/stats/log)
  is a separate process that links the engine via the C ABI.
- IPC. Control-pipe protocols, JSON handshakes, and background-process
  daemonization live above the engine. The engine answers HRESULTs and
  emits events; everything else is policy in the host.
