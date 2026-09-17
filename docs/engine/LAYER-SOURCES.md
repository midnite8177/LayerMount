# LayerMount.dll: layer sources

A lower's content can come from a directory, a VHD or VHDX file attached
as a volume, or a VSS snapshot. This document covers the second two: how
to create, attach, list, and clean up each one, which privilege it
needs, and what the engine does when a call fails. An operator can
prepare and tear down both kinds of lower from this document alone.

Every function below is a native `LayerMount.dll` export from
`LayerMount.h`, with the matching member of the managed `LayerMount.NET`
wrapper in parentheses.

---

## VHD and VHDX

### Create or open

`LayerMountVhdCreate` (`LayerMount.Vhd.Create`) makes a new VHD or VHDX
file and returns a handle to it. The file is not attached yet.
`config->kind` selects fixed, dynamic, or differencing
(`LM_VHD_KIND_FIXED`, `LM_VHD_KIND_DYNAMIC`, `LM_VHD_KIND_DIFFERENCING`;
`VhdKind.Fixed`, `VhdKind.Dynamic`, `VhdKind.Differencing`).
`config->sizeBytes` is required for a fixed or dynamic disk.
`config->parentPath` is required for a differencing disk.

`LayerMountVhdOpen` (`LayerMount.Vhd.Open`) opens a VHD or VHDX file
that already exists, at `config->path`, also without attaching it. It
fails with a Win32-translated HRESULT if the path does not exist, and
with `HRESULT_FROM_WIN32(ERROR_FILE_INVALID)` if the path names a
directory instead of a file.

`config->lifetime` sets what happens to the attachment at process exit:
`LM_VHD_ATTACH_PERMANENT` (`VhdAttachLifetime.Permanent`) survives it,
`LM_VHD_ATTACH_PROCESS_SCOPED` (`VhdAttachLifetime.ProcessScoped`)
detaches automatically when the last handle to the VHD closes.

### Attach

`LayerMountVhdAttach` (`VhdImage.Attach`) attaches the VHD as a Windows
volume and reports its physical device path. Calling it again on a
handle that is already attached re-emits the cached path without
attaching a second time, so a size probe followed by a fill call is
safe.

Once attached, `LayerMountVhdGetVolumeGuid` (`VhdImage.GetVolumeGuid`)
resolves the volume GUID path (`\\?\Volume{...}\`) that becomes the
lower. Call it only after `Attach` on the same handle; calling it
before returns `E_ILLEGAL_METHOD_CALL`. Volume GUID enumeration can lag
the attach because of Plug and Play, so a caller should retry on an
empty result or `ERROR_GEN_FAILURE` rather than treat either as final.

### List

`LayerMountVhdListLayers` (`LayerMount.Vhd.ListLayers`) lists the VHD
layers recorded in the on-disk layer registry at `<manifestDir>` (the
process working directory when `manifestDir` is null). A named
cross-process mutex guards every read and write of the registry file,
so two processes registering a layer at once cannot corrupt it. If the
registry file does not exist yet, the call returns success with zero
entries rather than an error, so listing before anything has been
registered is safe.

### Clean up

`LayerMountVhdDetach` (`VhdImage.Detach`) detaches the VHD from its
Windows volume and clears the cached physical path, so a later `Attach`
call on the same handle attaches fresh.

`LayerMountVhdUnregisterLayer` (`LayerMount.Vhd.UnregisterLayer`)
removes a layer entry from the registry. It is idempotent. `outRemoved`
comes back true when an entry was found and removed, false when the id
was not in the registry or the registry file was missing, and both
cases return success.

`LayerMountVhdClose` (`VhdImage.Dispose`) releases the handle. A
process-scoped attach also detaches on close; a permanent attach
outlives the handle and needs an explicit `Detach` call to go offline.

### Privilege

Windows' own `AttachVirtualDisk` and `CreateVirtualDisk` enforce
administrator elevation, and the engine surfaces whatever
Win32-translated HRESULT that call returns. The engine does not check
elevation itself before a VHD call.

### Failure behavior

`Open` fails cleanly on a missing path or a path that names a
directory. `Attach` is idempotent on an already-attached handle.
`GetVolumeGuid` needs a retry loop right after `Attach`, because of PnP
lag. `ListLayers` and `UnregisterLayer` both treat a missing registry
as empty rather than as an error. `LayerMountVhdMerge` fails at the
Win32 layer if the child VHD is still attached when merge is called.

---

## VSS

A VSS snapshot has no attach step. Its device path is a mount point the
moment `LayerMountVssCreateSnapshot` returns; there is no separate call
to bring it online.

### Create

`LayerMountVssCreateSnapshot` (`LayerMount.Vss.CreateSnapshot`) creates
a shadow copy of `volumePath` in one call and copies both the snapshot
id and the device path into caller buffers. If either buffer is missing
or too small, the call returns `HRESULT_FROM_WIN32(ERROR_MORE_DATA)`
and creates no snapshot at all.
`LM_VSS_ID_CHARS_REQUIRED` (64) and `LM_VSS_DEVICE_PATH_CHARS_REQUIRED`
(260) are the buffer sizes that always suffice; pass buffers at least
this large and the call completes in one round trip.

`persistent` decides whether the snapshot survives process exit, or is
non-persistent and gets deleted the next time
`LayerMountVssCleanupSnapshots` runs.

### List

`LayerMountVssListSnapshots` (`LayerMount.Vss.ListSnapshots`) lists
every VSS shadow copy on the machine, not only the ones this overlay
created. `LayerMountVssValidateSnapshotPath`
(`LayerMount.Vss.ValidateSnapshotPath`) checks the narrower question of
whether one snapshot this overlay created is still reachable: `S_OK`
with `outReachable` true or false when this overlay tracks the id,
`HRESULT_FROM_WIN32(ERROR_NOT_FOUND)` when it does not. A snapshot
destroyed outside this process stays tracked, so it reports `S_OK` with
`outReachable` false rather than `ERROR_NOT_FOUND`. That distinction
lets a caller tell a snapshot it owns and lost from one it never owned.

### Clean up

`LayerMountVssDeleteSnapshot` (`LayerMount.Vss.DeleteSnapshot`) deletes
one shadow copy by id, whether this overlay tracks it or not, and
whether it is persistent or not. `LayerMountVssCleanupSnapshots`
(`LayerMount.Vss.Cleanup`) deletes every non-persistent snapshot this
overlay created in one call; persistent snapshots are left for the
caller or the backup admin to delete explicitly.

`LayerMountVssCloseSnapshot` (`VssSnapshot.Dispose`) releases the
receipt handle only. It does not delete the snapshot. A non-persistent
snapshot stays tracked by the engine and eligible for
`LayerMountVssCleanupSnapshots` until deleted, and a persistent one
stays until someone deletes it explicitly.

### Privilege

`VSSManager::CreateSnapshot` checks elevation before doing any VSS work
and fails with `ERROR_PRIVILEGE_NOT_HELD` if the process is not
elevated. VSS operations require an administrator account.

### Failure behavior

`CreateSnapshot` refuses to create a snapshot at all when a buffer is
too small, so a failed call never leaves an orphan snapshot behind.
`ValidateSnapshotPath` distinguishes a snapshot this overlay never
created (`ERROR_NOT_FOUND`) from one it created that is no longer
reachable (`S_OK`, `outReachable` false). `Cleanup` only ever touches
non-persistent snapshots.
