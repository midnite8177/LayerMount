# LayerMount.dll: host adapter guide

A host adapter binds the engine to a filesystem host. It turns the
filesystem host's create, read, write, and close requests into calls
against `LM_HANDLE` and `LM_FILE_HANDLE`, and turns the engine's
answers back into whatever shape the filesystem host expects. This
document covers the nine rules a host adapter author needs that live
only in the native header and its implementation: handle lifecycle and
the host-attached flag, capability bits and their fallbacks, the mapping
from driver-style I/O requests to the engine's file API, paging reads,
event callback threading, the two-call buffer pattern, the reserved
HRESULT range, HRESULT-to-NTSTATUS conversion, and the mount handshake
a background mount writes. For layer sources
(VHD/VHDX, VSS) and the `.lmnt` layer image format, see
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

A filesystem host can merge handles. It reports only the first open
and the last close of a file. It routes every later open through the
first user handle, whatever access or process that open has. A host
adapter must ask its filesystem host for an open callback and a close
callback for every user handle. Each user handle then has its own
`LM_FILE_HANDLE`. Under merged handles a second program's open of a
file never reaches the engine. The engine's open-file count stays at
one while two user handles are open.

The engine records the granted access and the originator process ID
per `LM_FILE_HANDLE`. `LayerMountOpenFile` (`LayerMount.OpenFile`)
takes `grantedAccess` and `originatorPid`, and a reopen after cleanup
uses the granted access of that handle. Under merged handles the first
open's access and originator apply to every later open. The open-time
rule and the close log use the first opener's process. An overwrite of
a file whose first user handle sits between cleanup and close fails.
The filesystem host rejects it before the engine sees it.

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
  at the first open that asks for data access. The bit also applies to
  a copy-up of a sparse lower file. With it, the upper copy is sparse,
  or the copy-up fails with the volume's error. Without it, the upper
  copy is dense.
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

NTFS compression has no capability bit. A copy-up of a compressed lower
file sets compression on the upper copy and ignores a refusal, on every
copy-up path. On an upper volume that cannot compress, the upper copy is
dense and the copy-up succeeds, which is what a Windows copy of a
compressed file to such a volume does. The sparse rule above is
different because the host adapter declares the sparse capability, so a
refusal there is a real error.

## Driver I/O request to engine file API mapping

A filesystem host's dispatch surface names its requests differently
from the engine's exports. The mapping:

| Driver request | Engine call |
|---|---|
| Open | `LayerMountOpenFile` |
| Create | `LayerMountCreateFile` |
| Read | `LayerMountReadFile` |
| Write | `LayerMountWriteFile` |
| Cleanup | `LayerMountCleanupFile` |
| Close | `LayerMountCloseFile` |

`LayerMountOpenFile` (`LayerMount.OpenFile`) opens the file or directory
at `relativePath` for `grantedAccess` under `createOptions`, returning
a new `LM_FILE_HANDLE` and its `LM_FILE_INFO`. `LayerMountCreateFile`
(`LayerMount.CreateFile`) does the same for a path that does not yet
exist, taking `fileAttributes` and an optional self-relative security
descriptor; it returns `E_INVALIDARG` if that descriptor is non-NULL
but not structurally valid. An open for data access (read data, write
data, append data, or execute) fills a metacopy shell before it
returns. The fill takes the sparse attribute off unless the lower file
is sparse, so a filled file has the allocation of a normal copy. An
open for attributes, security, or delete keeps the shell sparse. A
failed fill fails the open with the fill's status and returns no
handle. `LayerMountReadFile` (`LayerMountFile.Read`) never
copies a file up and never reopens the handle for a fill. A read reopens
the handle only after a rename or after a cleanup. It writes
`*bytesTransferred` even on failure.
`LayerMountWriteFile` (`LayerMountFile.Write`) copies the file up into
the upper layer first if it is not already there; `constrainedIo`
rejects a write that would extend the file past its current allocation.
`LayerMountCleanupFile` (`LayerMountFile.Cleanup`) closes the NT handle
and keeps the file handle's slot. It does not flush and does not change
the open-file count. A second cleanup on a file handle with no open NT
handle returns success and does nothing. A later read, write, overwrite, flush, or
get-info on that handle reopens the file by its path with the granted
access minus `DELETE`; a granted mask with `FILE_WRITE_DATA` or
`FILE_APPEND_DATA` reopens with `FILE_READ_DATA` as well. On a deleted
file that call fails with file not found. A later delete on that handle
also works: a plain file deletes by its path, and a stream reopens by
its path with `DELETE` access. `LayerMountCloseFile` (`LayerMountFile.Dispose`) releases the
file handle's slot and decrements the parent overlay's open-file count.
It is the only call that frees the slot. When the last slot is free,
`LayerMountDestroy` can succeed, per the handle lifecycle rule above.

Only Open and Create take an `originatorPid` (see the file primitives
preamble in `LayerMount.h`). Pass 0 to use the current process. A host
adapter must read the originating process ID from its own dispatch
context and pass that, so process-tracker rules match the requester and
not the dispatcher thread. A call that takes an open file handle (Read,
Write, Overwrite, Flush, and the handle-form delete pair) takes no
originator. The process tracker checks it against the process that
opened the handle, as NT checks access at open.

## Paging reads

A paging read is a read that the memory manager sends on behalf of a
mapped file or the system cache. It is page-aligned and often runs
past the end of the file. It can arrive after the host adapter's
cleanup, because a mapped section or the cache outlives the user
handle.

A paging read arrives with the system process as its originator, not
the program that mapped the file. The engine checks the read against
the process that opened the handle, so a rule set that denies the
system process does not fail a paging read, and a rule set that allows
the opener applies to it.

Four rules cover a paging read.

Keep the `LM_FILE_HANDLE` until the host adapter's close and call
`LayerMountCloseFile` (`LayerMountFile.Dispose`) there. Or call
`LayerMountCleanupFile` (`LayerMountFile.Cleanup`) at the host
adapter's cleanup and `LayerMountCloseFile` at the host adapter's
close. Never call `LayerMountCloseFile` at cleanup. These rules assume
one `LM_FILE_HANDLE` per user handle, per the handle lifecycle section
above. A read after
`LayerMountCleanupFile` reopens the file by its path, per the cleanup
rule in the mapping section above, and then succeeds. On a deleted
file that read fails with file not found. A handle whose granted mask
has `FILE_WRITE_DATA` or `FILE_APPEND_DATA` also reads, before and
after `LayerMountCleanupFile`. So the paging read that follows a
truncation on a write-only handle succeeds. A handle granted only
`DELETE` reopens with `FILE_READ_ATTRIBUTES`, so a read on it after
`LayerMountCleanupFile` fails.

Clamp the read length to the file size before the call, or treat the
end-of-file status as a read of zero bytes. A read that starts inside
the file and runs past its end returns the short count in
`*bytesTransferred`. The engine then zero-fills the rest of the buffer
up to the requested length. A read that starts at or past the end of
the file returns the end-of-file status with `*bytesTransferred` set
to zero. That status arrives as an HRESULT in one of two shapes: the
NT inversion of `STATUS_END_OF_FILE`, or the Win32 wrap of
`ERROR_HANDLE_EOF`. Call `LayerMountHResultToNtStatus`
(`LayerMount.HResultToNtStatus`) and compare the result with
`STATUS_END_OF_FILE`.

Report the `allocationSize` the engine gives. Do not compute one. An
open handle reports the real on-disk allocation when that is larger
than the file size rounded up to 4 KiB, for example after a
preallocation. A path query and a directory listing report the rounded
size. A stream listing reports each stream's size rounded up to 4 KiB.
Every producer reports a value that is never below the file size. A path query reports zero for a directory, a whiteout, and a
path that does not exist.

A file in the upper, a file in a lower, a file the engine copied up,
and a metacopy shell all answer a paging read alike. The open and read
rules in the mapping section above explain why.

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
route driver requests to the right file call, hold a file handle
through cleanup and answer a paging read past the end of the file,
install an event callback that will not deadlock, read every buffered
output correctly on the first try, and turn any HRESULT the engine
returns into the NTSTATUS a filesystem host expects, without opening
`LayerMount.h`.

---

## The mount handshake

A host adapter that mounts in the background starts a copy of itself as
a child process with `--handshake-child` and waits for one JSON line on
the child's stdout. The child writes the line after the mount succeeded
or after it failed, and writes nothing else to stdout before it. The
parent reads that line with `HandshakeReader.ReadLine` from the managed
`LayerMount.NET` wrapper, which returns a `MountedHandshake`, a
`FailedHandshake`, or an `UnknownHandshake`; the engine's tests and every
host adapter's tests read the line through the same reader. Emitting the
line, the control pipe it names, and the daemonization stay in the host
adapter (ADR 0007).

A mounted line:

```json
{"status":"mounted","instanceId":"...","mountPoint":"X:\\","controlPipe":"\\\\.\\pipe\\...","pid":4242,"processCreationTimeFiletime":133000000000000000,"processCreationTimeUtc":"2026-01-02T03:04:05.0000000Z","startedAtUtc":"2026-01-02T03:04:06.0000000Z","host":"..."}
```

A failed line:

```json
{"status":"failed","error":"mount_point_busy","message":"...","host":"..."}
```

Field rules:

- `status` is `mounted` or `failed`. Any other value, or no value, parses
  as `UnknownHandshake`, and the parent treats it as a failure.
- `host` is on every line. It is an opaque identifier the host adapter
  chooses for itself; the reader never checks it against a list. A line
  with no `host` parses with an empty `Host`, and a parent treats that as
  a malformed handshake.
- `instanceId` is the id of the mounted overlay, as the host adapter
  assigned it. `mountPoint` is the path the child mounted at.
  `controlPipe` is the full name of the child's control pipe; the parent
  strips the pipe prefix before it connects.
- `pid` is the child's process id and `processCreationTimeFiletime` its
  creation time as a Windows FILETIME.
- `processCreationTimeUtc` and `startedAtUtc` are ISO-8601 UTC
  timestamps. `startedAtUtc` is the time the child emitted the handshake,
  after the mount succeeded. The reader parses both into
  `DateTimeOffset?` and gives `null` for a value it cannot parse.
- `error` and `message` are on a failed line only. `error` is a short
  code the host adapter defines; `message` is for a person.
- The reader tolerates a missing or mistyped field: a string field reads
  as empty, a number as zero. Only a line that is not JSON throws.

The read is bounded by a timeout. On a timeout the parent kills the
child, because the reader's thread stays blocked in the read and the
child's stdout is not readable again.

