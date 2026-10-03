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

// Copies each user alternate data stream of the file or directory at srcPath
// to the same stream of dstPath. A user stream is one `IsUserAlternateStream`
// accepts. The main `::$DATA` stream and the reserved streams are not copied;
// for a file, the data copy carries the main stream.
//
// Stops at the first stream that fails and returns its status. A missing
// source fails with the status of the open. An entry with no named streams
// returns STATUS_SUCCESS.
NTSTATUS CopyUserAlternateDataStreams(const std::wstring& srcPath,
                                      const std::wstring& dstPath);

// The size of the first buffer for the stream list of a file. A longer list
// doubles the buffer until the list fits.
inline constexpr size_t kInitialStreamListSize = 64 * 1024;

// Propagate NTFS EFS state. Unlike sparse/compression, encryption is applied
// path-wise rather than via a writable handle. Run it once the destination
// path exists and no conflicting handle is open; if we cannot preserve the
// encrypted state, fail the copy-up rather than silently materializing
// plaintext in upper.
bool ApplyEncryptedStateIfNeeded(const std::wstring& path, DWORD attrs);

bool HasFileAttribute(DWORD attrs, DWORD flag);

// The attribute bits from GetFileAttributesW, or none when that call failed.
std::optional<DWORD> AttributesOrNone(DWORD attributes);

bool SetSparse(HANDLE handle);

// The status of a refused FSCTL_SET_SPARSE, read right after the call. A
// failed DeviceIoControl can leave the last error at 0, and a plain mapping
// of 0 is STATUS_SUCCESS, so the fallback keeps the refusal an error.
NTSTATUS SparseRefusalStatus();

void SetCompressedIfSource(HANDLE handle, DWORD srcAttrs);

// Gives the directory at dstAbs the compression, encryption and user
// streams of the directory at srcAbs. NTFS compresses or encrypts an entry
// only when it creates it, so a call after the children are in place leaves
// each child with its own state; a dstAbs that was already compressed or
// encrypted gave that state to them. SetFileAttributes ignores
// FILE_ATTRIBUTE_COMPRESSED, so compression goes through the FSCTL, and a
// refusal of it is ignored. Without it, a file created in the directory
// through the mount lands dense. A refused encryption or stream copy
// returns its status. Unreadable source attributes skip the compression and
// encryption. NTFS refuses a new stream on a read-only directory, so the
// caller writes the attributes and times last.
NTSTATUS CopyDirectoryLayoutAndStreams(const std::wstring& srcAbs, const std::wstring& dstAbs);

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

// Copies the link at srcAbsolute, whose attributes are srcAttrs, to
// upperPath, with the copy-up record that policy.record selects. The link
// is built in containerPath, a new path in the work directory, through
// BuildInContainerAndMove, so it inherits the ACEs of its upper parent.
// When an entry holds upperPath, fails with STATUS_OBJECT_NAME_COLLISION
// and leaves that entry as it was.
NTSTATUS CopyLinkThroughWorkDir(const std::wstring& srcAbsolute,
                                DWORD srcAttrs,
                                const std::wstring& containerPath,
                                const std::wstring& upperPath,
                                const EntryCopyPolicy& policy);

// Writes the ACEs of the DACL of the directory at parentAbs, the inherited
// ones included, on the directory at dirAbs. When the process holds
// SE_SECURITY_NAME, also writes the ACEs of the SACL of parentAbs. The DACL,
// and the SACL when written, are protected. Without SE_SECURITY_NAME,
// dirAbs keeps the SACL that it inherits from its own parent. An entry made
// in dirAbs then inherits the DACL ACEs that it inherits in parentAbs.
NTSTATUS WriteSecurityToInheritAs(const std::wstring& dirAbs, const std::wstring& parentAbs);

}
