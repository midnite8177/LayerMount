# LayerMount.dll: layer image format

A layer image is a `.lmnt` file. It holds a packed directory tree: a
128-byte header, a JSON metadata block, and a compressed archive of
file entries. This document gives every offset, size, and field, so a
tool author can read a `.lmnt` file with a hex editor and this document
alone.

Every integer field is little-endian. The header and the entry header
pack with `#pragma pack(push, 1)`, so no field carries padding.

---

## The header

The header is the first 128 bytes of the file.

```
[0..7]    magic           8 bytes,  "OVLYIMG\0"
[8..11]   version         uint32,   format version
[12..15]  flags           uint32,   compression type
[16..23]  metadataOffset  uint64,   byte offset of the metadata block
[24..31]  metadataSize    uint64,   byte length of the metadata block
[32..39]  dataOffset      uint64,   byte offset of the data section
[40..47]  dataSize        uint64,   byte length of the data section
[48..79]  checksum        32 bytes, SHA-256 of the data section
[80..127] reserved        48 bytes, zero
```

### magic

Bytes 0 through 7 hold the literal `OVLYIMG\0`: the letters `O V L Y I
M G` and a trailing zero byte. [ADR 0004](../adr/0004-layer-image-magic-stays-ovlyimg.md)
fixes this value, because published images already carry it. A reader
rejects a header with a different magic. A future format change does
not change the magic.

### version

Bytes 8 through 11 hold the format version as a uint32. The current
version is 1. A reader rejects a header whose version number is higher
than the version it knows.

### flags

Bytes 12 through 15 hold a uint32 that carries the compression type. A
value of 0 means no compression. A value of 1 means Zstd compression.
Every image that the current layer-image manager writes carries the
value 1.

### metadataOffset and metadataSize

Bytes 16 through 23 hold the byte offset of the metadata block, as a
uint64. For every image the current layer-image manager writes, this
offset is 128: the metadata block starts right after the header.

Bytes 24 through 31 hold the byte length of the metadata block, as a
uint64. This length counts one trailing NUL byte that the writer
appends after the JSON text. A reader that takes exactly `metadataSize`
bytes and decodes them as UTF-8 text gets one extra NUL byte at the
end.

### dataOffset and dataSize

Bytes 32 through 39 hold the byte offset of the data section, as a
uint64. This offset always equals `metadataOffset + metadataSize`.

Bytes 40 through 47 hold the byte length of the data section, as a
uint64. This length is the size of the compressed archive, not the
size of the archive after decompression.

### checksum

Bytes 48 through 79 hold a 32-byte SHA-256 hash. The hash covers only
the `dataSize` bytes at `dataOffset`: the compressed data section. The
hash does not cover the header, and it does not cover the metadata
block.

### reserved

Bytes 80 through 127 are zero. No current code writes into this range.

---

## The metadata block

The metadata block sits at `[metadataOffset, metadataOffset +
metadataSize)`. It is UTF-8 JSON text with one trailing NUL byte, as
the `metadataSize` section above states. Its fields are:

```json
{
  "layerId":          "UUID string",
  "parentLayerId":    "UUID string" or null,
  "createdAt":        "ISO 8601 datetime, UTC",
  "author":           "string",
  "description":      "string",
  "tags":              ["string", ...],
  "compressionType":  "none" | "zstd",
  "fileCount":        uint64,
  "uncompressedSize": uint64,
  "compressedSize":   uint64,
  "whiteouts":        ["relative/path", ...],
  "labels":           {"key": "value", ...}
}
```

`layerId` is the unique id of this layer image. `parentLayerId` is the
id of the parent layer image, or `null` when the image has no parent.

`createdAt` is the creation time, in ISO 8601 format, in UTC. `author`
and `description` are free text. `tags` is a list of free-text labels.

`compressionType` names the compression that the data section uses:
`"none"` or `"zstd"`. `fileCount` is the number of file entries in the
archive. `uncompressedSize` is the archive size before compression.
`compressedSize` is the archive size after compression, and matches
`dataSize` in the header.

`whiteouts` lists the logical relative paths that a differential layer
image deletes. Each entry is the original path of the deleted file,
not the on-disk marker path. See "Differential layer images" below.

`labels` is a set of free-text key-value pairs.

---

## The archive stream

The data section holds a compressed archive. A reader decompresses it
first, with Zstd when `flags` is 1. The decompressed bytes are a
sequence of entries. Each entry is a 24-byte entry header, a UTF-8
path, and, for a file with data, the file's bytes.

### The entry header

```
nameLength    uint16,  offset 0,  length of the path that follows
size          uint64,  offset 2,  file size in bytes, uncompressed
attributes    uint32,  offset 10, Windows file attributes
modified      uint64,  offset 14, last-write time, packed FILETIME
isDirectory   uint8,   offset 22, 1 for a directory, 0 for a file
isWhiteout    uint8,   offset 23, 1 for a whiteout marker
```

The entry header is 24 bytes. After it comes `nameLength` bytes of
UTF-8 path text, with forward slashes. After the path comes `size`
bytes of file data, only for a file entry with `size` greater than 0.
A directory entry and a whiteout marker entry both carry `size = 0`
and carry no data bytes.

### The sentinel

The archive stream ends with a sentinel entry. The sentinel is a full
24-byte entry header with `nameLength = 0xFFFF` and every other field
set to 0. No path and no data bytes follow the sentinel.

A real path never reaches the value `0xFFFF`: the writer rejects a
path whose UTF-8 encoding is longer than 65534 bytes.

### Path encoding

A path in the archive is UTF-8 text with forward slashes. A backslash
in a source path converts to a forward slash before the writer stores
it.

A path does not start with a slash. A path does not carry a drive
letter, such as `C:`. A path does not contain a `.` component or a
`..` component. A path can end with a slash; a directory entry uses
this form.

### Whiteout entries

A whiteout entry marks a deleted path. Its path is the marker path:
the last component of the deleted path, with the prefix `.wh.` added.
`"foo/bar.txt"` becomes `"foo/.wh.bar.txt"`. `"bar.txt"` becomes
`".wh.bar.txt"`.

A whiteout entry carries `isDirectory = 0`, `size = 0`, and
`isWhiteout = 1`. It is a zero-size file entry at the marker path.

The whiteout marker is the only deletion marker that the layer image
format defines. A directory-level opaque marker is a separate,
overlay-mount concept, and it does not appear in a layer image.

---

## Differential layer images

A differential layer image compares a source directory against a base
directory, path by path.

A path that exists in the source but not in the base is new. The
writer archives it.

A path whose type differs between source and base, a file in one and a
directory in the other, is a type change. The writer archives it as
the source's current type.

A file whose size or modification time differs between source and
base is modified. The writer archives it.

A file that is identical in source and base is unmodified. The writer
does not archive it. This is what makes the image differential: it
carries only the entries that changed.

A path that exists in the base but not in the source is deleted. The
writer records it twice: the logical path goes into the metadata's
`whiteouts` list, and a whiteout marker entry for it goes into the
archive.

The archive of a differential layer image holds, in order: the new,
modified, and type-changed entries; then the whiteout marker entries;
then the sentinel.

---

## The manifest format

A manifest is a separate JSON file, not a `.lmnt` image. It lists an
ordered set of layer images for distribution. The writer pretty-prints
it with 2-space indent:

```json
{
  "schemaVersion": 1,
  "layers": [
    { "path": "C:\\images\\base.lmnt", "sha256": "<64-char lowercase hex>" }
  ]
}
```

`schemaVersion` is the manifest format version. A reader rejects a
manifest whose `schemaVersion` is less than 1.

Each entry in `layers` names one image's path and its checksum. The
writer reads each image's 128-byte header, checks its magic, and
records the header's `checksum` field as lowercase hex. The manifest
checksum is the same SHA-256 that the image's own header carries: the
hash of the data section, not a hash of the whole file.
