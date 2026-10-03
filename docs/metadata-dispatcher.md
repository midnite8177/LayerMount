# Per-file metadata dispatcher

The engine stores copy-up metadata and opaque markers per file, through one dispatcher that picks between two backends.

## Backends

The ADS store, the fast path, keeps metadata in an NTFS alternate data stream on the entry itself. The sidecar store keeps metadata as a JSON file at `<upper>\.overlay\<sha1(lowercase path)>.meta.json`, with a matching `.opaque` file for the opaque marker. The sidecar store works on any file system; the ADS store needs NTFS streams.

The sidecar name is the SHA-1 of the entry's full path, lowercased and encoded as UTF-8. The hash avoids NTFS's restricted filename characters, such as `<`, `>`, and `:`, and keeps the sidecar path short for a long input path. The lowercasing matches NTFS's case-insensitive names. Since the key is the full path, the sidecar store finds a record only at a path in the same form as the one it was written at.

## Choosing a backend

The host capability bit `LM_CAP_ADS` picks the backend. A host that reports ADS support uses the ADS store. A host that does not, such as one with an upper on FAT32, exFAT, or a network share, uses the sidecar store.

## Links

A junction, a directory symlink or a file symlink keeps its record on the link itself, as overlayfs keeps the xattrs of a copied-up symlink on the symlink. Every open of an `:overlay` stream uses `FILE_FLAG_OPEN_REPARSE_POINT`. Without it, the open of a file symlink's stream reaches the target's stream, and the open of a directory link's stream fails with `ERROR_DIRECTORY`. `DeleteFileW` and `GetFileAttributesW` do not follow a link, so a remove or an opaque probe also stays on the link.

A copied-up link gets a copy-up record that holds the lower link's own file ID, so an open of the upper link as a link (`FILE_OPEN_REPARSE_POINT`) reports the ID of the lower link. A link gets no opaque marker, since overlayfs gives a symlink no opaque xattr.

An open that follows a link reports the identity of the target, as `stat` through a symlink does on overlayfs. The handle is then on the target, and the info fill reads the record at the handle's final path, so the link's record never reaches it. The final path can differ in form from the upper path, through an 8.3 name, a substituted drive or a junction in an ancestor. So the mount reads the final path of the upper root once, and the info fill puts a final path under that root back onto the upper path before the sidecar lookup. A final path outside the upper root, or any final path when the mount could not read the root's, only loses its `\\?\` prefix. On a host without `LM_CAP_ADS` the lookup then finds a record only when that path is in the upper path's form. Otherwise the open reports the handle's NTFS file ID.

## Reads and removes

A read tries the ADS store first. If the ADS store has no entry, the dispatcher reads the sidecar store and returns that result. `RemoveLayerMountMetadata` clears both stores.

A read also tells the caller whether to trust its result. The `corrupted` out-param distinguishes no metadata recorded from metadata that exists but failed to parse. Code that depends on metadata fidelity, such as resolver-side redirect follow, treats `*corrupted == true` as a hard failure. It does not fall back to the default record.

A corrupt ADS read falls through to the sidecar only on a host without `LM_CAP_ADS`. That host writes the sidecar, so the sidecar holds the current record. On a host with `LM_CAP_ADS`, a corrupt ADS read returns the default record, because the sidecar could hold a record from before the ADS took over.

## Renames and deletes

An ADS record moves with its entry on a rename and goes with it on a delete. The sidecar store keys a record by the entry's path, so on a host without `LM_CAP_ADS` the engine moves it on a rename and removes it on a delete. A rename of an upper entry moves the sidecar record and the sidecar opaque marker of the entry, and of each entry below it, without a walk into a directory link, to the new paths. A directory link is a junction or a directory symlink. The walk enters any other directory reparse point. The move deletes a record already at a new path, so a moved entry never takes the record of the entry it replaced. A delete removes the records of the entries it removed, so a new entry at a reused path starts with none.

The engine walks the tree only when the sidecar store is in use. A host that switched from the sidecar to the ADS can still hold sidecar records at old paths, and the read fallback finds them.
