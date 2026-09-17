# Per-file metadata dispatcher

The engine stores copy-up metadata and opaque markers per file, through one dispatcher that picks between two backends.

## Backends

The ADS store, the fast path, keeps metadata in an NTFS alternate data stream on the target file. The sidecar store keeps metadata as a JSON file at `<upper>\.overlay\<sha1(lowercase path)>.meta.json`, with a matching `.opaque` file for the opaque marker. The sidecar store works on any file system; the ADS store needs NTFS streams.

The sidecar path hashes the file's relative path with SHA-1. This avoids NTFS's restricted filename characters, such as `<`, `>`, and `:`. It also keeps the sidecar path short for a long input path. The dispatcher lowercases the path before it hashes, to match NTFS's case-insensitive names.

## Choosing a backend

The host capability bit `LM_CAP_ADS` picks the backend. A host that reports ADS support uses the ADS store. A host that does not, such as one with an upper on FAT32, exFAT, or a network share, uses the sidecar store.

## Reads and removes

A read tries the ADS store first. If the ADS store has no entry, the dispatcher reads the sidecar store and returns that result. A remove clears both stores, so a capability change or a file-system change never leaves a stale record behind.

A read also tells the caller whether to trust its result. The `corrupted` out-param distinguishes no metadata recorded from metadata that exists but failed to parse. Code that depends on metadata fidelity, such as resolver-side redirect follow, treats `*corrupted == true` as a hard failure. It does not fall back to the default record.
