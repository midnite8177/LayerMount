# Host paths carry their Win32 form

A Win32 call on a plain host path fails past MAX_PATH. The engine cannot turn on long-path awareness, because the host process manifest and a machine-wide registry key control it. Thus, a host path is a `HostPath`. `Text()` is the path as the caller wrote it, for storage, keys and comparisons. `ForWin32()` is the extended form, for Win32, `std::filesystem` and `fstream` calls. We chose a type over a host-file module or a lint rule. Both forms are `std::wstring`, and only a type stops the use of one form where the other must go.

## Consequences

`Text()` keeps an extended root that a caller gives, so the sidecar keys of an existing overlay do not change. Thus, `Text()` is not always a plain path, and code must not remove a prefix from it.

A path written into a script or command line for another program, such as the diskpart script, gets `Text()`. It is stored text, not a Win32 call. The parent of a differencing VHDX gets `ForWin32()`, because VirtDisk opens the parent. VirtDisk stores the same plain parent locator for a short parent, with or without the prefix. It keeps the `\\?\` form only for a parent past MAX_PATH, and it opens that parent on a later attach.
