# Per-file metadata has two backends behind one dispatcher

The engine stores copy-up metadata and opaque markers per file, but NTFS alternate data streams do not exist on FAT32, exFAT, or a network share. The engine keeps an ADS store and a sidecar-JSON store behind one dispatcher; the host capability bit picks between them, and `docs/metadata-dispatcher.md` has the details. The cost is two stores to keep correct; the gain is that a host can change capabilities or file systems without stale records.
