# LayerMount.dll: host adapter guide

A host adapter binds the engine to a filesystem host. It turns the
filesystem host's create, read, write, and close requests into calls
against `LM_HANDLE` and `LM_FILE_HANDLE`, and turns the engine's
answers back into whatever shape the filesystem host expects. This
document covers the seven rules a host adapter author needs that live
only in the native header and its implementation: handle lifecycle and
the host-attached flag, capability bits and their fallbacks, the mapping
from driver-style I/O requests to the engine's file API, event callback
threading, the two-call buffer pattern, the reserved HRESULT range, and
HRESULT-to-NTSTATUS conversion. For layer sources (VHD/VHDX, VSS) and
the `.lmnt` layer image format, see
[LAYER-SOURCES.md](LAYER-SOURCES.md) and
[LAYER-IMAGE-FORMAT.md](LAYER-IMAGE-FORMAT.md) instead of repeating
them here.

Every function below is a native `LayerMount.dll` export from
`LayerMount.h`, with the matching member of the managed `LayerMount.NET`
wrapper in parentheses. A host adapter author who reads this document
should not need to open the header for a threading or ownership rule.

---

## Handle lifecycle and the host-attached flag

`LM_HANDLE`, `LM_FILE_HANDLE`, `LM_VHD_HANDLE`, `LM_VSS_SNAPSHOT_HANDLE`,
and `LM_IMAGE_HANDLE` are opaque: the header says not to dereference
them, do arithmetic on them, or compare them by value (LayerMount.h:41-46,
107-121). Internally the engine packs a 16-bit per-kind tag, a 24-bit
generation counter, and a 24-bit slot index into that pointer value, so
a stale or wrong-kind handle is rejected without touching freed memory.
The tag catches a handle passed to the wrong function; the generation
counter catches a handle whose slot has since been freed and reused. A host
adapter never decodes this itself; it only explains why every function
returns `E_HANDLE` cleanly on a handle that no longer points at a live
instance.

`LayerMountCreate` (`LayerMount.Create`) returns an `LM_HANDLE` for a
full overlay: upper, lowers, work directory. `LayerMountCreateTransient`
(`LayerMount.CreateTransient`) returns the same handle type with
`hostAttached` defaulted `FALSE`; it is the CLI/no-mount constructor,
for code that needs a valid handle to drive VHD, VSS, or layer-image
primitives without mounting anything.

`LayerMountSetHostAttached(handle, attached)` (`LayerMount.SetHostAttached`)
marks an overlay as currently mounted by a host adapter. Set it `TRUE`
once mount succeeds, `FALSE` once unmount completes. General DLL
consumers should not call it; its default is `FALSE` so a handle that
no host adapter ever attached releases without ceremony.

`LayerMountDestroy(handle)` (`LayerMount.Dispose`, via
`LayerMountHandle`) releases the overlay handle and its engine instance.
It refuses with `E_ILLEGAL_METHOD_CALL` in either of two cases:

- the instance is still host-attached, so call
  `LayerMountSetHostAttached(handle, FALSE)` after unmount first;
- any `LM_FILE_HANDLE` opened against it is still open.

A host adapter's teardown order follows from that rule: unmount, clear
the host-attached flag, close every outstanding file handle, then call
`LayerMountDestroy`.

## Capability bits and fallbacks

`LM_HOST_CAPABILITIES` (LayerMount.h:132-140) is a bitfield a host
adapter passes in `LM_CONFIG::hostCapabilities` (or to
`LayerMountCreateTransient`) to declare what its target filesystem host
and upper-layer file system actually support. Clearing a bit activates
the fallback below it; a host adapter that wants full fidelity sets
every bit its target actually supports and no more.

- `LM_CAP_ADS`. Without it, the metadata dispatcher stores copy-up and
  opaque markers as sidecar JSON files under the upper layer's
  `.overlay` directory instead of NTFS alternate data streams. See
  [docs/metadata-dispatcher.md](../metadata-dispatcher.md) for how the
  dispatcher picks between the two backends and reads across them.
- `LM_CAP_SPARSE_FILES`. Without it, metacopy for a file above the
  metacopy size threshold copies its data in full at copy-up time
  (`CopyUpFile`), because `FSCTL_SET_SPARSE` is not available on the
  upper. With it, the engine copies the metadata only and fills the data
  at the first open that asks for data access.
- `LM_CAP_REPARSE_POINTS`. Without it, renaming a directory whose
  source is a reparse point falls through to a full recursive copy that
  copies the link target's contents into a regular directory. The data
  survives; the link semantics do not. With the bit set, the same
  rename takes a short-circuited copy-up that preserves the reparse
  point instead. The engine emits one `LM_EVT_WARNING` for each rename
  that takes the degraded path.
- `LM_CAP_NTFS_ACLS`. Without it, a security read returns a synthetic
  security descriptor instead of one read from the upper, and a
  security write silently no-ops and returns success instead of
  failing.
- `LM_CAP_MULTIPLE_STREAMS` and `LM_CAP_CASE_SENSITIVE`. A host adapter
  clears either bit to declare the corresponding limitation. Neither
  bit currently gates any engine fallback; clearing it records the
  limitation for whatever reads `hostCapabilities` back, and nothing
  more.

## Driver I/O request to engine file API mapping

A filesystem host's dispatch surface names its requests differently
from the engine's exports. The mapping:

| Driver request | Engine call |
|---|---|
| Open | `LayerMountOpenFile` |
| Create | `LayerMountCreateFile` |
| Read | `LayerMountReadFile` |
| Write | `LayerMountWriteFile` |
| Close | `LayerMountCloseFile` |

`LayerMountOpenFile` (`LayerMount.OpenFile`) opens the file or directory
at `relativePath` for `grantedAccess` under `createOptions`, returning
a new `LM_FILE_HANDLE` and its `LM_FILE_INFO`. `LayerMountCreateFile`
(`LayerMount.CreateFile`) does the same for a path that does not yet
exist, taking `fileAttributes` and an optional self-relative security
descriptor; it returns `E_INVALIDARG` if that descriptor is non-NULL
but not structurally valid. An open for data access (read data, write
data, append data, or execute) fills a metacopy shell before it
returns. An open for attributes, security, or delete keeps the shell
sparse. A failed fill fails the open with the fill's status and
returns no handle. `LayerMountReadFile` (`LayerMountFile.Read`) never
copies a file up and never reopens the handle for a fill. The one reopen
a read can do is the retarget after a rename. It writes
`*bytesTransferred` even on failure. `LayerMountWriteFile`
(`LayerMountFile.Write`) copies the file up into the upper layer first
if it is not already there; `constrainedIo` rejects a write that would
extend the file past its current allocation. `LayerMountCloseFile`
(`LayerMountFile.Dispose`) releases the file handle's slot and
decrements the parent overlay's open-file count. This is the call that
eventually lets `LayerMountDestroy` succeed, per the handle lifecycle
rule above.

All five, along with every other file primitive invoked from inside a
host-adapter callback, take an `originatorPid` (LayerMount.h:699-707).
Pass 0 to use the current process. A host adapter should instead read
the true originating process ID off its own dispatch surface and pass
that through, so process-tracker rules evaluate against the real
requester rather than the dispatcher thread's PID.

## Event callback threading rules

`LayerMountSetEventCallback(handle, callback, userContext)`
(`LayerMount.Event`) installs a `LM_EVENT_CALLBACK`, replacing any
previous one; passing `NULL` uninstalls it. The callback fires on
arbitrary engine-internal threads and must never re-enter the engine
synchronously on the same handle. Doing so risks deadlock against
whatever internal lock the firing thread already holds. An `LM_EVENT`
pointer handed to the callback is valid only for the duration of that
call; a host adapter that needs the data afterward copies it out before
returning.

Uninstalling drains in flight. `LayerMountSetEventCallback(handle,
NULL, ...)` blocks until every call to the previous callback that was
already in progress has returned, so a caller can free the
`userContext` right after the call returns. That also means a host
adapter must never call `LayerMountSetEventCallback(handle, NULL, ...)`
from inside the callback itself. The drain would wait for a call that
is waiting for it, which never resolves.

The four event types (`LM_EVENT_TYPE`, LayerMount.h:151-156):

- `LM_EVT_WARNING`. A non-fatal degradation, such as the reparse-point
  fallback above.
- `LM_EVT_COPY_UP`. A file or directory was copied up from a lower into
  the upper.
- `LM_EVT_WHITEOUT_CREATED`. A whiteout marker was written.
- `LM_EVT_ACCESS_DENIED`. A process-tracker rule denied a request.

## The two-call buffer pattern

Every list, string, or blob output in the ABI follows one protocol
(LayerMount.h:30-39): pass a null buffer and zero length to receive the
required size, then allocate that much and call again.
`HRESULT_FROM_WIN32(ERROR_MORE_DATA)` means the caller's buffer was too
small; the required size is written on that path as well as on the
sizing call, so a caller can always tell how much to allocate. A host
adapter writes this loop once and reuses it for every buffered output
in the ABI rather than hand-rolling a variant per call.

Consumers of the pattern include `LayerMountGetLastErrorMessage`, and
the string and array fields on `LM_VHD_LAYER_INFO`, `LM_VSS_SNAPSHOT_INFO`
(documented in [LAYER-SOURCES.md](LAYER-SOURCES.md)), and
`LM_IMAGE_MANIFEST` and `LM_IMAGE_METADATA` (documented in
[LAYER-IMAGE-FORMAT.md](LAYER-IMAGE-FORMAT.md)). Each struct's own
fields are covered in those documents; the buffer protocol itself does
not change from one to the next.

## The reserved HRESULT range

Engine-specific failures use `FACILITY_ITF` codes `0xB000..0xBFFF`
(LayerMount.h:19-28). A host adapter must never emit a code in that
range from its own layers. The range exists so a caller can tell an
engine failure from a host-adapter failure without a second lookup,
and a host adapter that borrows a code from it breaks that guarantee
for every caller downstream. The engine does not populate this range today: its
own failures arrive as wrapped Win32 codes, NTSTATUS inversions, or a
small set of standard COM codes, all covered in the HRESULT-to-NTSTATUS
section below. The range is a contract for the future, not a set of
codes to match against right now. See
[docs/adr/0002-hresult-error-model-across-the-abi.md](../adr/0002-hresult-error-model-across-the-abi.md)
for the reasoning behind it.

## HRESULT to NTSTATUS conversion

A filesystem host callback surface is typically NTSTATUS-shaped, not
HRESULT-shaped. `LayerMountHResultToNtStatus(hr, outStatus)`
(`LayerMount.HResultToNtStatus`) converts any HRESULT the engine
returns into the matching NTSTATUS using the engine's own internal
table (LayerMount.h:571-588), so a host adapter bridging to that
surface calls this instead of maintaining a translation table of its
own. It never fails. It always returns `S_OK` and writes `*outStatus`,
except when `outStatus` is null, which returns `E_POINTER`.

The table recognizes three shapes of HRESULT, in this order:

1. The explicit standard-COM-code table: `E_HANDLE`,
   `E_ILLEGAL_METHOD_CALL`, `E_INVALIDARG`, `E_OUTOFMEMORY`,
   `E_ACCESSDENIED`, `E_POINTER`, `E_NOTIMPL`, `E_ABORT`, `E_FAIL` each
   map to a specific NTSTATUS.
2. `FACILITY_WIN32`-wrapped Win32 errors, the shape the engine produces
   when it wraps `::GetLastError()`, translate through the
   Win32-to-NTSTATUS table.
3. `FACILITY_NT_BIT` HRESULTs, the `HRESULT_FROM_NT` inversion, unwrap
   back to their original NTSTATUS.

Anything that matches none of the three maps to
`STATUS_UNSUCCESSFUL`.

---

With this document, a host adapter author can build a complete
mount/unmount and I/O bridge: create and tear down a handle in the
right order, declare capabilities and know what each one changes,
route driver requests to the right file call, install an event
callback that will not deadlock, read every buffered output correctly
on the first try, and turn any HRESULT the engine returns into the
NTSTATUS a filesystem host expects, without opening `LayerMount.h`.
