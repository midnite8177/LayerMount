# LayerMount engine

The LayerMount engine gives Windows programs Linux-style overlay semantics over real directories. It is a library, not a filesystem driver: a host adapter in another repository shows the result as a volume.

## Language

### Roles

**Engine**:
The library in this repository: the native `LayerMount.dll` and its managed wrapper `LayerMount.NET`. It applies overlay semantics and has no mount lifecycle.
_Avoid_: driver, DLL (when the library as a whole is meant)

**Host adapter**:
Code in another repository that depends on the engine and binds it to a filesystem host.
_Avoid_: host, adapter, kernel

**Filesystem host**:
The userspace-filesystem driver that a host adapter targets to show an overlay as a Windows volume.
_Avoid_: host, filesystem-host kernel, userspace-filesystem driver

### The overlay

**Overlay**:
One live instance of the engine that holds an upper, zero or more lowers, and a work directory. The merged view of those layers.
_Avoid_: mount, layer stack, merged view

**Mount**:
The act a host adapter does to show an overlay at a drive letter or a directory. The engine does not mount.

**Layer**:
One of the directories that an overlay merges: the upper or a lower.

**Upper**:
The one writable layer. All new writes land there. Required.
_Avoid_: upper layer directory, writable layer

**Lower**:
A read-only layer. Lowers have a priority order; the first lower that has a path wins. Zero or more.
_Avoid_: base, base layer

**Work directory**:
The scratch directory the engine uses for atomic copy-up.
_Avoid_: work dir, scratch

**Layer source**:
Where a lower's content comes from: a directory, a VHD or VHDX volume, or a VSS snapshot.

### Operations

**Copy-up**:
The promotion of a file or directory from a lower into the upper on first write. Lowers never change.
_Avoid_: promote, promotion

**Metacopy**:
The copy-up mode in which metadata copies up at once and data copies up at the first open that asks for data access.
_Avoid_: lazy copy-up

**Metacopy shell**:
The sparse upper file that a metacopy leaves in place of the data. It has the lower file's size and no data.
_Avoid_: placeholder, stub

**Paging read**:
A read that the memory manager sends on behalf of a mapped file or the system cache. It is page-aligned and often runs past the end of the file.
_Avoid_: mmap read, cache read

**Whiteout**:
A marker in a layer that hides a same-named entry in every lower below it.

**Opaque directory**:
A directory marker that hides all lower content under that directory.

### Layer images

**Layer image**:
A single `.lmnt` file that holds a packed directory tree, zstd-compressed, with a SHA-256 of the data section in its header.
_Avoid_: bundle, archive, image file

**Pack** / **Unpack**:
The operations that write a directory tree into a layer image and read it back.

**Differential layer image**:
A layer image that records only the entries that differ from a base directory, with whiteout entries for deleted files.

**Manifest**:
A file that lists an ordered set of layer images for distribution.
