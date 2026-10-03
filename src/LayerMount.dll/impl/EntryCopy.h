#pragma once

#include "LayerMount.h"
#include "../abi/CapabilityGate.h"

#include <optional>
#include <string>

namespace LayerMount {

enum class CopiedEntryRecord {
    // A new record that holds the source entry's own file ID. For a copy
    // out of a lower layer.
    NewFromSource,
    // The source entry's record, with the source entry's own file ID added
    // when the record holds none. For a copy of upper entries to a new upper
    // path, so each entry keeps the file ID it reports.
    CarriedFromSource,
};

// The remove call that a new upper entry needs. A file symbolic link is a
// File, and a junction or a directory symbolic link is a Directory.
enum class NewUpperEntryKind { File, Directory };

struct EntryCopyPolicy {
    CopiedEntryRecord record;
    const LayerConfig& config;
    ::LayerMount::abi::CapabilityGate capabilities;
};

LayerMountMetadata MakeCopyUpMetadata(const std::wstring& sourcePath);

LayerMountMetadata CopiedEntryMetadata(const std::wstring& sourcePath,
                                       CopiedEntryRecord record,
                                       const LayerConfig& config);

// Enumerate user-visible alternate data streams on srcPath and copy each to
// dstPath. Skips the main `::$DATA` stream (carried by the normal file-data
// copy) and the overlay's reserved `:overlay*` bookkeeping streams.
//
// Returns true when every stream copied. A file with no streams counts as
// a success.
bool CopyUserAlternateDataStreams(const std::wstring& srcPath,
                                  const std::wstring& dstPath);

// Propagate NTFS EFS state. Unlike sparse/compression, encryption is applied
// path-wise rather than via a writable handle. Run it once the destination
// path exists and no conflicting handle is open; if we cannot preserve the
// encrypted state, fail the copy-up rather than silently materializing
// plaintext in upper.
bool ApplyEncryptedStateIfNeeded(const std::wstring& path, DWORD attrs);

bool HasFileAttribute(DWORD attrs, DWORD flag);

bool SetSparse(HANDLE handle);

// The status of a refused FSCTL_SET_SPARSE, read right after the call. A
// failed DeviceIoControl can leave the last error at 0, and a plain mapping
// of 0 is STATUS_SUCCESS, so the fallback keeps the refusal an error.
NTSTATUS SparseRefusalStatus();

void SetCompressedIfSource(HANDLE handle, DWORD srcAttrs);

// SetFileAttributes ignores FILE_ATTRIBUTE_COMPRESSED; only the FSCTL sets
// it on a directory. Without the FSCTL on the upper directory, a file
// created inside it through the mount lands dense. A failed encrypted state
// returns the failure status.
NTSTATUS ApplyDirectoryLayout(const std::wstring& upperPath, DWORD srcAttrs);

// Copy the data of srcHandle to dstHandle. When both files are sparse, the
// copy reads the allocated ranges of the source and writes only those, so a
// hole of the source stays a hole in the destination: NTFS allocates
// clusters for written zeros, but not for a region the file pointer skips
// (Windows file system documentation, "Sparse Files"). The destination end
// of file becomes the source size, so a trailing hole keeps the logical
// size.
NTSTATUS CopyFileDataKeepingHoles(HANDLE srcHandle, HANDLE dstHandle);

struct EntryTimes {
    FILETIME creation;
    FILETIME access;
    FILETIME write;
};

EntryTimes EntryTimesOf(const WIN32_FILE_ATTRIBUTE_DATA& data);

// The times of the entry at path, or none when GetFileAttributesExW fails.
std::optional<EntryTimes> ReadEntryTimes(const std::wstring& path);

// Writes times to the entry at path, and the attribute bits too when
// attributes holds them, in one call. A zero time keeps the stored value.
// Only the bits a FileBasicInfo write can set go through. A state bit such
// as FILE_ATTRIBUTE_COMPRESSED or FILE_ATTRIBUTE_SPARSE_FILE is left out.
// The open uses FILE_FLAG_BACKUP_SEMANTICS, so it opens a
// directory, and with SE_BACKUP_NAME and SE_RESTORE_NAME enabled a DACL
// that denies the write does not stop it. Returns false with the Win32
// error in GetLastError.
bool WriteEntryTimes(const std::wstring& path,
                     const EntryTimes& times,
                     std::optional<DWORD> attributes);

// Writes metadata as the copy-up record of the new entry at upperPath. When
// the write fails, removes the entry and returns the write's error.
NTSTATUS WriteCopyUpRecordOrRemoveEntry(const std::wstring& upperPath,
                                        const LayerMountMetadata& metadata,
                                        NewUpperEntryKind kind,
                                        const LayerConfig& config);

// Copies the link at srcAbsolute to dstAbsolute as a link and writes the
// copy-up record `policy.record` selects on the new link, not on its
// target, as overlayfs keeps the inode number of a copied-up symlink. A
// failure removes the new link.
NTSTATUS CopyLinkWithCopyUpRecord(const std::wstring& srcAbsolute,
                                  DWORD srcAttrs,
                                  const std::wstring& dstAbsolute,
                                  const EntryCopyPolicy& policy);

}
