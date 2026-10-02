# LayerMount.dll — Architecture

This document describes the internal architecture of `LayerMount.dll`, the
native engine that powers every LayerMount host adapter on Windows. The
DLL is deliberately filesystem-host-agnostic: it owns the overlay
semantics (layer precedence, whiteouts, copy-up, metadata) and exposes
them through a stable C ABI. Host adapters live above this DLL and are
the only consumers of its handle types.

The engine takes its model from Linux `layermount(5)`: a single writable
**upper** layer stacks on top of zero or more read-only **lower** layers,
and a **work** directory provides the atomic-operation space the engine
uses for copy-up. Reads pass through to the first layer that has the
file; writes copy the file into the upper layer with a copy-up; deletes
drop a *whiteout* marker that hides the lower entry from the overlay.
The lower layers are never modified.

---

## Repository layout

```
src/LayerMount.dll/
  public/         LayerMount.h          Stable C ABI (the only installed header)
                  LayerMount.def        Exported symbols
  abi/            CapabilityGate.h     Host-adapter capability bitfield wrapper
                  EventEmitter.h       Host-adapter event-callback fan-out
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
                  MetadataStore.{h,cpp} Per-file metadata dispatcher
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

**Filesystem-host-agnostic.** The DLL has no compile-time or run-time
dependency on any particular filesystem host. Host adapters bind the
engine to the filesystem host's callback table and translate the
filesystem host's request shape into ABI calls; the engine answers in
pure HRESULT/NTSTATUS without knowing what delivered the request. A unit
test that drives the engine through the C ABI sees exactly what a
production host adapter sees.

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

**Capability gating, not feature stubs.** When the host adapter advertises
limited capabilities (no ADS, no sparse files, no NTFS ACLs), the engine
takes a documented fallback path rather than refusing the operation.
The `CapabilityGate` wrapper makes the choice explicit at every fork.

**Errors carry context.** Every ABI shim that catches an exception or
records a failure stores a UTF-16 message in thread-local storage; the
caller fetches it via `LayerMountGetLastErrorMessage(hr, ...)`. The HRESULT
reported back is the precise translation of the underlying `GetLastError`
or `STATUS_*`, never a generic `E_FAIL`. A failed metacopy fill keeps the
status of the step that failed, and `LayerMountGetLastFailureWasFill`
tells it apart from a refusal with the same status.

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
    std::wstring workDirPath;                // copy-up work directory
    bool         enableProcessTracking;
    std::wstring processRulesPath;
    size_t       accessLogCapacity;
    size_t       pathCacheCapacity;
    UINT32       hostCapabilities;           // LM_HOST_CAPABILITIES bitfield
};
```

Lookup precedence is **upper, then lowers in declared order, first-match
wins**. A file present in the upper layer is the file the overlay
shows — even if a lower layer also has a copy. A file present in
`lowerPaths[1]` is shadowed by a copy in `lowerPaths[0]`.

`LayerConfig::Validate` checks that `upperPath` is an existing directory
and is writable (by writing and deleting a temp file under
`FILE_FLAG_DELETE_ON_CLOSE`). It verifies every lower path exists. It
emits a debug warning when the upper layer's volume is not NTFS or
ReFS — non-NTFS upper layers cannot use the ADS metadata path and fall
back to sidecar JSON files.

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

`FILE_FLAG_BACKUP_SEMANTICS` on the marker open is intentional: a
parent directory that inherited a `DENY-WRITE` ACE from the lower layer
would otherwise block whiteout creation. Combined with
`SE_RESTORE_NAME` (enabled in `EnableFileSystemPrivileges`), the
backup-semantics handle bypasses the DACL check so the engine can
always persist the marker for an entry it owns.

### Per-layer whiteout scoping

A whiteout in **layer N** hides the corresponding name from **layer N**
itself and from every layer **below N**, but **not** from layers above
N. This matches overlayfs semantics: an upper-layer write can
re-introduce a name that a lower-layer whiteout previously hid. Concretely:

- Whiteout in `lowerPaths[0]` hides the entry from `lowerPaths[0..n]`.
- Whiteout in upper hides the entry from upper and from every lower.
- A new file written into upper at the same path replaces the upper
  whiteout (the create path explicitly removes the marker on commit;
  see "Resurrection windows" below).

### Opaque directories

A directory marked **opaque** in a layer hides every entry of the same
directory in the layers below that layer, regardless of name. The
entries of the layer that holds the marker stay visible. The rule is the same for the
upper and for a lower: a directory opaque in lower N shows the entries
of lower N and hides lowers N+1..end. Opaque is the directory analog of
a whiteout: it expresses "this directory's contents in this layer are
the authoritative set; do not merge children from below". A
`.wh..wh..opq` marker at the root of a layer hides the lowers below
that layer, as a root opaque whiteout in an OCI image layer does.
Kernel overlayfs does not do this. It builds the merged root at mount
and ignores opacity at a layer root.

Two coexisting representations:

1. **NTFS ADS marker** `<dir>:overlay.opaque` — fast, single-stat detection.
2. **Sentinel file** `<dir>\.wh..wh..opq` — present for compatibility
   with image producers that don't speak ADS, and as the fallback on
   non-NTFS upper layers.

`WhiteoutManager` writes both markers when both are available, and
either marker satisfies `IsOpaque`/`IsOpaqueInLayer`. Removal cleans
both stores.

The two main producers of opaque markers:

- `Create` with `FILE_DIRECTORY_FILE` in `createOptions` when a whiteout
  at the path or an opaque ancestor hides a lower directory of the same
  name. A create over a lower directory that the overlay still shows
  fails with `STATUS_OBJECT_NAME_COLLISION` instead. Without the marker, the new
  (empty) upper directory would expose the hidden lower contents through
  the overlay again.
- `CopyUp::RenameLowerDirectory`, which renames a directory that a lower
  layer holds. After the recursive copy, the destination is opaque so
  subsequent merges don't re-pull files from the lower-layer source
  through the post-rename path.

### Inheritance: ancestor whiteouts and ancestor opaques

The resolver applies the rules transitively:

- `WhiteoutManager::HasWhitedOutAncestorInLayer(rel, layer)` walks the
  parent chain and returns true if any ancestor in `layer` carries a
  `.wh.<name>` marker. A whitedout directory hides every descendant
  from its layer downward.
- `WhiteoutManager::HasOpaqueSelfOrAncestorInLayer(dir, layer)` walks
  from `dir` up the parent chain and returns true if `dir`, any
  ancestor, or the layer root is opaque in `layer`. An opaque ancestor
  hides every descendant in the layers below the layer that holds the
  marker. The descendants in the layer that holds the marker stay
  visible. The resolver's hiding checks, the directory merge and the
  type-conflict log ask this question through `LowersBelow`, so they
  agree at every depth.

The whiteout walk stops below the layer root, since a whiteout names an
entry and the root has no name. The opaque walk probes the layer root
too.

### Links in a layer

A junction or a directory symlink in a layer is a non-directory for what
lies below it, as a symlink is in overlayfs. Such a link is a directory
reparse point whose reparse tag is a name surrogate. Any other directory
reparse point stays a directory.

Overlayfs looks up a path one name at a time, and the topmost layer that
holds a name decides. It follows a symlink only when the symlink is that
topmost entry. A lower symlink under a higher directory of the same name
adds nothing, and the lookup stops there, so the deeper lowers add
nothing either. The engine applies the same rule to links:

- When no higher layer holds the link's name, Win32 follows the link. A
  listing of the link, or of a directory under it, shows the link
  target's entries, and a lookup through the link finds an entry in the
  target. The deeper lowers do not merge under the link, so the listing
  shows none of their entries and the lookup finds none.
- When the upper or a higher lower holds the link's name, the lower that
  holds the link adds nothing under that name, to a listing or to a
  lookup. The merge and the lookup stop at that lower, so no deeper lower
  adds anything either. `HasLinkUnderHigherLayerEntry` makes this check
  for each lower before its scan or its probe.

A link cannot carry an opaque marker, because the marker would go into
its target. Instead, `LowersBelow` walks the directory's path in the
layer and stops at a link. A probe or a scan through the link sees the
target as a directory, so the walk runs even when the layer holds the
directory. When the walk cannot read a reparse tag, the lowers stay
hidden, as they do when the walk cannot read a component's attributes.

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

Whiteout markers are *never* visible in the overlay. The resolver
returns not found for any path with a segment that starts with `.wh.`,
so a caller who knows a marker's name still cannot open, read, delete
or rename it. See "Path safety guards". Directory merging in
`LayerMount::MergeDirectoryEntries`:

1. Enumerates upper. For each `.wh.<name>` it sees, it strips the prefix
   and adds `<name>` to a `whitedOutNames` set, then *skips* the marker
   itself. An upper that has no directory at the path adds nothing, and
   the merge goes on to the lowers. An upper that has a file at the path,
   or at an ancestor's path, adds nothing and stops the merge before the
   lowers, because the file hides everything under its path. An upper
   link at the path or at an ancestor's path adds the entries that the
   scan reads through the link, and stops the merge before the lowers.
   See "Links in a layer". A scan that
   fails in any other way, at the first read or mid-stream, adds nothing
   from the upper and stops the merge before the lowers, because a
   whiteout that the scan did not read can hide a lower's entry. The
   merge then returns the scan's status and no entries.
2. For each lower, it enumerates the directory once. It adds the names
   that the lower's whiteouts hide to the same set, and it holds the
   lower's entries back until the scan ends. A lower that has no
   directory at the path adds nothing, and the merge goes on to the next
   lower. A lower that has a file at the path, or at an ancestor's path,
   adds nothing and stops the merge. A lower link at the path or at an
   ancestor's path adds the entries that the scan reads through the link,
   and stops the merge, when no higher layer holds the link's name. When
   the upper or a higher lower holds that name, the lower adds nothing and
   stops the merge. See "Links in a layer". A scan that fails in any other
   way, at the first read or mid-stream, stops the merge, because a
   whiteout that the scan did not read can hide an entry in a deeper
   lower. The merge then returns the scan's status and no entries, not
   the entries of the layers above.
3. After a clean scan, the merge adds a held-back lower entry only if no
   higher layer already produced it AND the name is not in
   `whitedOutNames`, which includes that lower's own whiteouts from
   step 2.
4. An opaque directory limits step 2. If the directory is opaque in the
   upper layer, the merge enumerates no lower. If the directory is
   opaque in lower N, the merge enumerates lower N and skips lowers
   N+1..end. An opaque ancestor in a layer, the layer root included,
   makes the directory opaque in that layer.

`CanDelete` returns a failed merge's status instead of reading the
missing entries as an empty directory, and `LayerMountMergeDirectory`
returns it as an HRESULT before it invokes the callback.

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
2d. Reject paths with a segment that starts with `.wh.`, in every
    layer. This covers whiteout and opaque markers and anything
    beneath a marker-named directory.
3.  Cache lookup; return on hit.
4.  Probe upper. If present, read `:overlay` ADS:
      - If `redirect` is set, recurse with depth+1 (renamed-from-lower
        bookkeeping; see CopyUp section).
      - Otherwise return the upper hit.
5.  If upper has a whiteout marker for this path, return isWhiteout=true.
5a. If any ancestor in upper carries a whiteout marker, treat the
    descendant as whited-out as well.
6.  If any ancestor in upper is opaque, return not-found
    (the ancestor's opacity hides the lower content). The upper root
    counts as an ancestor of every path.
6a. If any ancestor in upper is a file or a link, return not-found.
    The file or the link hides the lower content under its path. An
    ancestor whose attributes or reparse tag the engine cannot read,
    for a reason other than a missing path, also hides it. The check
    runs after every step-4 miss. A miss that reached the parent does
    not prove that the upper holds the parent as a directory, because
    the parent can be a link whose target is a directory.
7.  For each lower in priority order:
      a. Whiteout in this lower => stop iterating.
      b. Whitedout ancestor in this lower => stop iterating.
      c. Link ancestor in this lower whose name the upper or a higher
         lower also holds => stop iterating.
      d. Probe this lower; on hit, capture and break.
      e. Opaque ancestor in this lower, the lower's root included =>
         stop after this lower.
      f. File ancestor, link ancestor, or unreadable ancestor, in this
         lower => stop after this lower.
8.  If a lower hit was captured, scan the deeper lowers that stay
    visible for type conflicts (file vs. directory) and log via
    OutputDebugStringW (the resolved entry still wins; the log is
    diagnostic).
9.  Cache the result and return.
```

`ResolveLowerPath` does not probe the upper. It runs steps 2, 2b, 2c,
2d, 5a, 6a and 7 only, it does not use the cache, and an empty path is not
found. Create, delete, copy-up and rename use it when the engine needs
the *lower* state independent of any upper shadow.

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

`IsReservedRelativePath(normalized)` rejects two kinds of path:

- The `.overlay` directory and anything beneath it. That subtree is the
  sidecar-metadata store on non-ADS upper layers, described below. A
  caller who reaches it through the overlay could open, modify, or
  delete internal records.
- Any path with a segment that starts with `.wh.`. This covers whiteout
  markers, the opaque marker `.wh..wh..opq` and every path beneath a
  marker-named directory. Without it, creating `dir\.wh.name` would
  write a live whiteout, and creating `dir\.wh..wh..opq` would make
  `dir` opaque.

The resolver treats a reserved path as not found in the upper and in
every lower. `Create` returns `STATUS_ACCESS_DENIED`, and so does
`Rename` when either end is reserved. `UpdateContextPath` returns
`STATUS_INVALID_PARAMETER`. The marker compare ignores case, as NTFS
does, so `.WH.` matches too. The whiteout and opaque
bookkeeping inside the engine probes the layers with
`GetFileAttributesW` and does not go through the resolver.

### Stream-qualified paths and case

The write-side ADS surface (`Create` / `Open` / `Overwrite` / `Delete`
/ `UpdateContextPath`) accepts paths of the form
`file[:stream[:$DATA]]`. `TryParseStreamPath` runs *after*
normalization, which means the stream name inherits the
whole-path lowercasing from step 2. A caller that creates
`\example.txt:MyStream` lands `example.txt:mystream` on disk;
`LayerMountEnumerateStreams` returns the on-disk (lowercased) form.
This treats file names the same way and keeps stream resolution
case-insensitive end-to-end — but it does mean stream-name casing is
not round-trippable through the overlay.

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
   `SE_BACKUP_NAME` can read past restrictive DACLs. With the sparse
   capability, a sparse lower file gives a sparse upper copy, or the
   copy-up fails with the volume's error. The copy reads only the
   allocated ranges of a sparse source, so its holes stay holes in the
   upper copy and the upper allocation matches the lower one. Without
   the capability, the upper copy is dense. A compressed lower file
   gives a compressed upper copy when the upper volume can compress.
   The engine sets compression before the data copy and ignores a
   refusal, so on a volume that cannot compress the upper copy is dense
   and the copy-up succeeds. Compression has no capability bit. This
   matches a Windows copy of a compressed file to such a volume.
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

### Metacopy (`CopyUpMetadataOnly`)

For files larger than 1 MiB on host adapters that support sparse files
(`LM_CAP_SPARSE_FILES`), the engine stages a *metacopy shell* in the
upper layer instead of a full data copy:

1. Create the upper file as a sparse file (`FSCTL_SET_SPARSE`) of the
   correct logical size, with no allocated data blocks. A compressed
   lower file gives a compressed shell, under the same rule as the eager
   copy-up. A refused `FSCTL_SET_COMPRESSION` leaves the shell dense and
   never fails the metacopy, so a small file and a large file give the
   same result.
2. Mirror security, attributes, and timestamps. The lower's streams
   arrive with the fill, together with the data.
3. Write the `:overlay` metadata with `metacopy = true` and the origin
   layer recorded.
4. Ownership flips to the upper layer; the file context is marked
   `isMetacopyOnly = true`.

The shell fills at the first open that asks for data: read data, write
data, append data, or execute. `Open` calls `CompleteLazyCopyUp` before
it opens the handle. The call streams the data from the recorded origin
into the upper sparse skeleton and clears the metacopy flag. The fill
takes the sparse attribute off unless the lower file is sparse, so a
filled file has the allocation of a normal copy. An open for
attributes, security, or delete keeps the shell sparse. A failed fill
fails the open with the fill's status and returns no handle. `Read`
never copies a file up and never reopens the handle for a fill. The one
reopen a read can do is the retarget after a rename. `Write` keeps a
fill as a guard for a handle opened without data access. See
[ADR 0006](../adr/0006-metacopy-shell-fills-at-open-for-data-access.md).
Without the sparse capability the engine forces a full copy-up at open
time, because a non-sparse metacopy would be a dense zero-filled stub
with no benefit.

### Directory copy-up (`CopyUpDirectory`)

Creates the directory in the upper layer if missing, copies the
security descriptor and timestamps from the lower-layer source, writes
the `:overlay` metadata, and (importantly) does **not** recurse. The
directory's children remain in the lower layer until they themselves
are copied up on demand.

### Cross-layer directory rename (`RenameLowerDirectory`, `RenameUpperDirectory`)

Directory rename is the worst case: a single Win32 `MoveFileExW` cannot
move a directory tree out of a read-only layer into a writable one.
The engine handles nine cases. The last three also apply to a file source:

- **upper → upper**: a single `MoveFileExW`. Transfer the opaque marker
  if present. When a lower layer has an entry at the destination path,
  mark the destination opaque. A junction or directory symlink gets no
  opaque marker, because the marker would go into its target.
- **lower → upper, dest-not-present**: recursive copy
  via `CopyTreePreservingMetadata` (preserves reparse points, sparse
  bits, and ADS), then mark the destination opaque, then drop a
  whiteout at the source. A junction or directory symlink copies up as
  a link and gets no opaque marker.
- **replace=true, dest is a directory with visible children**: a child
  from the upper or from a lower shows in the merged view. The engine
  rejects the rename with `STATUS_DIRECTORY_NOT_EMPTY` before any side
  effects. This applies to an upper source and to a lower source.
- **replace=true, dest is a directory with no visible children**: an
  upper directory that holds only whiteouts or an opaque marker is
  empty. The engine clears the opaque marker of the upper destination
  and moves the directory into the work directory. Then it does the
  rename as for a destination that is not present. When the rename
  fails, the engine moves the destination back and restores its opaque
  marker. When the rename succeeds, the engine removes the copy in the
  work directory. With the work directory on another volume, the engine
  removes the destination at once, and a failed rename cannot restore
  it. The new directory is opaque when a lower layer has the destination
  path, so no lower child of the old destination shows.
- **rename to a destination that already exists in the merged view
  (with replace=false)**: the engine rejects the rename with
  `STATUS_OBJECT_NAME_COLLISION` before any side effects.
- **rename to a path inside the source directory's own tree**
  (`a` to `a\b` or `a\new\b`): the engine rejects the rename with
  `STATUS_INVALID_PARAMETER` before any side effects, as NTFS does. The
  collision check above runs first, so replace=false onto an existing
  `a\b` still returns `STATUS_OBJECT_NAME_COLLISION`. Replace=true onto
  a non-empty `a\b` returns `STATUS_INVALID_PARAMETER`, not
  `STATUS_DIRECTORY_NOT_EMPTY`. A junction or directory symlink source
  gets the same check. A file source gets no such check. A file
  `a` renamed to `a\b` fails with `STATUS_OBJECT_PATH_NOT_FOUND`.
- **replace=true onto an ancestor of the source** (`a\f` onto `a`):
  the engine rejects the rename with `STATUS_DIRECTORY_NOT_EMPTY`
  before any side effects, as overlayfs returns `ENOTEMPTY`. This
  applies to a file source and to a directory source.
- **replace=true onto an entry of the other type**: a file onto a
  directory fails with `STATUS_FILE_IS_A_DIRECTORY` (overlayfs
  `EISDIR`), and a directory onto a file fails with
  `STATUS_NOT_A_DIRECTORY` (overlayfs `ENOTDIR`). Both fail before any
  side effects, so the engine copies no lower file source up. With
  replace=false, `STATUS_OBJECT_NAME_COLLISION` comes first.
- **a junction or directory symlink as source or destination**:
  overlayfs does not follow the last path component in a rename, so a
  symlink is a non-directory there. The engine treats a junction or a
  directory symlink the same way. See "Links in a layer". When the
  engine cannot read the reparse tag, the rename fails with that error
  before any side effects. With replace=true, a file or a link replaces
  a link, and a link replaces a file. A directory onto a link fails with
  `STATUS_NOT_A_DIRECTORY`, and a link onto a directory fails with
  `STATUS_FILE_IS_A_DIRECTORY`, both before any side effects. The link
  moves as a link. A lower link copies up as a link and leaves a
  whiteout at its old path. The link target and its entries stay
  unchanged, and no opaque marker goes into the target. An upper
  destination moves into the work directory before the rename, as an
  empty directory does, and comes back when the rename fails. With the
  work directory on another volume, a file destination moves there as a
  copy, and a link destination goes at once, so a failed rename cannot
  restore it.

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

`EnableFileSystemPrivileges` in `ElevationUtil.h` enables four
privileges on the process token at first use:

- `SE_CREATE_SYMBOLIC_LINK_NAME` — required by `FSCTL_SET_REPARSE_POINT`
  for `IO_REPARSE_TAG_SYMLINK`. Even elevated tokens carry this
  privilege disabled by default.
- `SE_RESTORE_NAME` and `SE_BACKUP_NAME` — let backup-semantics opens
  bypass DACL checks. Without these, copy-up of a child into a
  restrictive parent fails.
- `SE_SECURITY_NAME` — required to read/write SACL audit ACEs. Tracked
  separately in `IsSecurityPrivilegeHeld` so callers can skip the SACL
  bit when the privilege is not held (standard-user process), instead
  of failing the whole `GetFileSecurityW` call. A create writes the SACL
  of the caller's security descriptor only when the process holds the
  privilege. Without it, the create drops the SACL and still applies
  the owner, group and DACL.

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

`MetadataStore` is the engine's dispatcher. It picks an NTFS
alternate-data-stream store or a sidecar JSON store, based on the host
adapter's advertised capability. It falls back to the sidecar store
when the ADS store has no entry. See
[metadata-dispatcher.md](../metadata-dispatcher.md) for the backend
choice, the SHA-1 rationale, and the read and remove details.

### Reserved namespaces

The engine reserves three namespaces:

- `:overlay`, `:overlay.opaque` ADS streams in the upper layer.
  `Overwrite`, which has CREATE_ALWAYS semantics, deletes user ADS
  streams but skips anything starting with `:overlay`.
- The `<upper>\.overlay\` directory. `IsReservedRelativePath` rejects
  every read and write that targets it, and `MergeDirectoryEntries`
  filters it out of root listings.
- Names that start with `.wh.`, in every directory and every layer.
  `IsReservedRelativePath` rejects a path with such a segment, so the
  ABI calls `LayerMountCreateWhiteout` and `LayerMountSetOpaque` return
  `E_INVALIDARG` for it. `MergeDirectoryEntries` filters the markers out
  of every listing and lists nothing for a marker-named directory.

---

## Caching

`Cache` is an LRU map from normalized relative path to `ResolvedPath`,
guarded by a `std::shared_mutex`. The default capacity is 10000
entries; the host adapter can raise it via `LM_CONFIG::pathCacheCapacity`
for large lower trees that thrash on cold start.

Reads acquire the shared lock and (under the shared lock) update each
entry's `lastAccessTicks` with `_InterlockedExchange64` on a
reinterpret-cast pointer to the tick field, so concurrent readers don't
block each other for LRU bookkeeping. The project targets C++17, where
`std::atomic_ref` is not available. Writes acquire the exclusive lock;
the eviction pass picks the oldest tick.

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
every path primitive consults it with the caller's `callerPid`. Read,
Write, Overwrite, Flush, and the handle-form delete pair consult it with
the `ownerPid` the open recorded on the `FileContext`. The handle-form
rename still consults it with the caller's pid.

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

`VHDLayerManager` wraps the Win32 `OpenVirtualDisk`/`CreateVirtualDisk`/
`AttachVirtualDisk` API to attach a VHD or VHDX file as a Windows
volume and expose its volume GUID path. The VHD ABI keeps a layer
registry in a JSON file, and a named cross-process mutex guards each
read and write of that file. See
[LAYER-SOURCES.md](LAYER-SOURCES.md) for the create, attach, list,
clean-up, and privilege details.

### VSS snapshot manager (`impl/vss/`)

`VSSManager` drives the VSS COM interface to take, list, and delete
shadow copies, with COM initialized per-thread via `ComScope` (RAII).
See [LAYER-SOURCES.md](LAYER-SOURCES.md) for the create, list,
clean-up, and privilege details.

### Layer-image manager (`impl/image/`)

The layer-image manager packs a directory tree into one self-contained
`.lmnt` file: a fixed header, a JSON metadata block, and a compressed
archive of file entries with a SHA-256 checksum. `Pack` builds an
image from a source directory. `Unpack` reverses the process and
verifies the checksum, unless the caller turns verification off.
`PackDifferential` builds an image that records only the entries that
changed against a base directory, with a whiteout entry for each
deletion. A manifest lists an ordered set of images for distribution.
See [LAYER-IMAGE-FORMAT.md](LAYER-IMAGE-FORMAT.md) for the byte-level
format.

---

## ABI surface

The DLL exports a single set of C functions, all `__stdcall`, all
returning `HRESULT`. Patterns:

### Two-call buffer pattern

Output strings, blobs, and arrays use the two-call pattern: pass
`buffer = NULL`, `bufferChars = 0` to receive the required size in
`*requiredChars`; allocate; call again with the populated buffer. A
short buffer returns `HRESULT_FROM_WIN32(ERROR_MORE_DATA)` and always
writes `*requiredChars` so the caller can resize and retry. The fill
call writes the count the engine produced, not the capacity the caller
offered. A caller must cut the result to that count. The engine
honors this on every list, string, and blob surface, including nested
per-entry strings inside `LM_VHD_LAYER_INFO` and `LM_VSS_SNAPSHOT_INFO`.
`LayerMountGetSecurity` conforms as of `LM_ABI_VERSION` 2. That version
also added a caller-supplied `SECURITY_INFORMATION` parameter to the
call.

### Forward-extensible structs

`LM_CONFIG`, `LM_VHD_CONFIG`, and `LM_IMAGE_PACK_OPTIONS` carry a
`structSize` field as their first member. Callers set it to
`sizeof(STRUCT)` at their compile time; the engine compares against
the size it knows about and interprets only the fields it understands.
Adding a field at the end of the struct does not bump
`LM_ABI_VERSION`. Reordering or removing a field does.

Fixed-shape structs (`LM_FILE_INFO`, `LM_RESOLVED_PATH`, `LM_STATS`,
`LM_EVENT`, `LM_VOLUME_INFO`) revise only via `LM_ABI_VERSION` bumps.

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
buggy host adapter that bypasses the `childCount` check still cannot
trigger a use-after-free.

### Error reporting

`ErrorTls::Set` / `Last` provide per-thread last-error storage. Every
ABI shim catches escaping exceptions, records the HRESULT plus a
human-readable message, and returns the HRESULT to the caller. The
caller fetches the message via `LayerMountGetLastErrorMessage(hr, ...)`
on the same thread; passing a different HRESULT than the one stored
returns `*requiredChars = 0` so a caller cannot accidentally read a
stale message belonging to a different operation.

The same storage holds one more fact: whether the most recent call on
the thread failed in a metacopy fill. `LM_ABI_ENTRY` clears it as the
first statement of every export, before the argument checks, so every
call resets it, whether it then succeeds or fails. The exceptions are
the three exports that inspect a failure: `LayerMountGetLastErrorMessage`,
`LayerMountGetLastFailureWasFill` and `LayerMountHResultToNtStatus`.
`CopyUp::CompleteLazyCopyUp` sets the mark when a fill fails. It also
records a message that names the path, the stage that failed and the
NTSTATUS. The HRESULT stays the fill's own status, as ADR 0006 requires.
The managed wrapper reads it on the same thread right after a failed
call and exposes it as `LayerMountException.FromMetacopyFill`. A reopen
of the handle after a successful fill is not part of the fill and does
not set the mark.

Reserved overlay-specific HRESULT range: `FACILITY_ITF` codes
`0xB000..0xBFFF`. Host adapters must not emit codes in this range from
their own layers.

`LayerMountHResultToNtStatus` converts any HRESULT the engine returns
into the equivalent NTSTATUS for adapters that bridge into an
NTSTATUS-shaped filesystem host callback surface. Coverage includes the
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
progress, so the host adapter can safely free the user context (e.g. a
managed GCHandle).

### Mount-point helpers

`LayerMountPointPrepareDirectory` /
`LayerMountPointCaptureIdentity` / `LayerMountPointReleaseIfSafe`
are filesystem-host-agnostic helpers for the directory-mount-point
lifecycle. The host adapter calls them around its own mount/unmount
calls so the ownership-tracked release at the end never deletes a
directory the host adapter did not create. Drive-letter mount points
(`X:` / `X:\`) skip the dance entirely — `LayerMountPointIsDriveLetter`
reports them.

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

- Mounting. `LayerMountSetHostAttached` is the single coupling point.
  While the flag is set, `LayerMountDestroy` returns
  `E_ILLEGAL_METHOD_CALL`. The host adapter must unmount and clear the
  flag first.
- Driver lifecycle. Installing, configuring, or licensing a filesystem
  host lives in the host adapter, not in the engine.
- CLI. A command-line surface (mount/vhd/vss/layer/stats/log) can exist
  as a separate process that links the engine via the C ABI.
- IPC. Control-pipe protocols, the emission of the JSON handshake, and
  background-process daemonization live above the engine. The engine
  answers HRESULTs and emits events; everything else is policy in the
  host adapter. The one exception is the parent-side handshake reader,
  `HandshakeReader` in the managed wrapper, so that a host adapter and
  the tests parse the line through one code path (ADR 0007).

See [0001-engine-is-not-a-filesystem-driver.md](../adr/0001-engine-is-not-a-filesystem-driver.md)
for why the engine draws this boundary.
