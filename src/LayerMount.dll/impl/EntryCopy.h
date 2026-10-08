#pragma once

#include "LayerMount.h"

#include <optional>
#include <string>
#include <vector>

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

// Sets *has to whether the file or directory at path has a stream that
// `IsUserAlternateStream` accepts. A failed read of the stream list
// returns its status and leaves *has as it was.
NTSTATUS HasUserAlternateDataStream(const std::wstring& path, bool* has);

// The size of the first buffer for the stream list of a file. A longer list
// doubles the buffer until the list fits.
inline constexpr size_t kInitialStreamListSize = 64 * 1024;

// Encrypts the entry at path when attrs carries FILE_ATTRIBUTE_ENCRYPTED.
// EncryptFileW opens path itself and needs exclusive access, so the entry
// must exist and no handle to it may be open. Returns true when attrs is
// not encrypted or the entry ends up encrypted. INVALID_FILE_ATTRIBUTES
// counts as not encrypted, so the call returns true and encrypts nothing.
// Otherwise returns false and leaves the Win32 error in GetLastError. A
// false fails the copy-up, because a plaintext copy in the upper would
// expose the data. Compression, by contrast, is best effort.
bool ApplyEncryptedStateIfNeeded(const HostPath& path, DWORD attrs);

bool HasFileAttribute(DWORD attrs, DWORD flag);

// The attribute bits from GetFileAttributesW, or none when that call failed.
std::optional<DWORD> AttributesOrNone(DWORD attributes);

bool SetSparse(HANDLE handle);

// The status of a refused FSCTL_SET_SPARSE, read right after the call. A
// failed DeviceIoControl can leave the last error at 0, and a plain mapping
// of 0 is STATUS_SUCCESS, so the fallback keeps the refusal an error.
NTSTATUS SparseRefusalStatus();

void SetCompressedIfSource(HANDLE handle, DWORD srcAttrs);

// Gives the directory at dstAbs the compression, encryption, user streams
// and extended attributes of the directory at srcAbs. NTFS compresses or
// encrypts an entry only when it creates it, so a call after the children
// are in place leaves each child with its own state; a dstAbs that was
// already compressed or encrypted gave that state to them.
// SetFileAttributes ignores FILE_ATTRIBUTE_COMPRESSED, so compression goes
// through the FSCTL, and the call ignores a refusal of it. Without the
// compression state, a file created in the directory through the mount
// lands dense. A refused encryption, stream copy or extended attribute copy
// returns its status. Unreadable source attributes skip the compression and
// encryption. NTFS refuses a new stream on a read-only directory, and an
// extended attribute write moves the times, so the caller writes the
// attributes and times last.
NTSTATUS CopyDirectoryOwnMetadata(const std::wstring& srcAbs, const std::wstring& dstAbs);

// Opens the file at path to read its data for a copy-up. With
// SE_BACKUP_NAME, the open reads a file whose DACL denies read to the
// engine's account. The open shares read access only, so it fails while
// another handle has write access, and no writer can open the file while
// the handle stays open. Returns INVALID_HANDLE_VALUE with the Win32 error
// in GetLastError when the open fails.
HANDLE OpenSourceFileForCopy(const std::wstring& path);

// Copy the data of srcHandle to dstHandle. When both files are sparse, the
// copy reads the allocated ranges of the source and writes only those, so a
// hole of the source stays a hole in the destination: NTFS allocates
// clusters for written zeros, but not for a region the file pointer skips
// (Windows file system documentation, "Sparse Files"). The destination end
// of file becomes the source size, so a trailing hole keeps the logical
// size. The copy reads every byte of a source whose data a provider or a
// storage manager can keep elsewhere. A read the provider cannot serve
// fails the copy with the status of that read.
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

// Writes attributes without FILE_ATTRIBUTE_READONLY through entry, a handle
// with FILE_WRITE_ATTRIBUTES access, and keeps the entry's times. attributes
// must be the entry's current attributes. Only the bits that
// WriteEntryTimes writes go through. Does nothing when attributes lack the
// read-only bit. Returns false with the Win32 error in GetLastError.
bool ClearReadOnly(HANDLE entry, DWORD attributes);

// Writes attributes to the entry at path and keeps its times. Only the bits
// a FileBasicInfo write can set go through, the pin and offline bits
// included. The open uses FILE_FLAG_OPEN_REPARSE_POINT, so a junction or a
// symbolic link takes the bits itself, and its target keeps its own.
// Returns false with the Win32 error in GetLastError.
bool WriteOwnAttributes(const std::wstring& path, DWORD attributes);

// Writes metadata as the copy-up record of the new entry at upperPath. When
// the write fails, removes the entry and returns the write's error.
NTSTATUS WriteCopyUpRecordOrRemoveEntry(const std::wstring& upperPath,
                                        const LayerMountMetadata& metadata,
                                        NewUpperEntryKind kind,
                                        const LayerConfig& config);

// The header of FILE_FULL_EA_INFORMATION, which the user-mode SDK headers
// do not define. The name, a NUL and the value follow it.
struct FullEaHeader {
    ULONG nextEntryOffset;
    UCHAR flags;
    UCHAR nameLength;
    USHORT valueLength;
};

using NtQueryEaFileFn = NTSTATUS(NTAPI*)(HANDLE, IO_STATUS_BLOCK*, PVOID, ULONG, BOOLEAN, PVOID,
                                         ULONG, PULONG, BOOLEAN);
using NtSetEaFileFn = NTSTATUS(NTAPI*)(HANDLE, IO_STATUS_BLOCK*, PVOID, ULONG);

// The entries of list, a FILE_FULL_EA_INFORMATION list of length bytes,
// that a user-mode NtSetEaFile can write, as a new list with each entry at
// a ULONG boundary. Leaves out each entry whose name starts with $KERNEL.,
// in upper or lower case: Windows lets only kernel mode set such an
// attribute (FsRtlSetKernelEaFile, "Kernel Extended Attributes" in the
// Windows driver documentation). Stops at the first entry that runs past
// length. Empty when no entry is left.
std::vector<BYTE> ExtendedAttributesUserModeCanSet(const BYTE* list, ULONG length);

// Copies each extended attribute of the entry at srcPath to the entry at
// dstPath, except the ones ExtendedAttributesUserModeCanSet leaves out.
// Opens the entries themselves, not the targets of links. When the source
// has no extended attributes to copy, or either volume refuses them as not
// supported, copies nothing and returns STATUS_SUCCESS. A failed read or write
// returns its status. An extended attribute write moves the times of
// dstPath.
NTSTATUS CopyExtendedAttributes(const std::wstring& srcPath, const std::wstring& dstPath);

// An entry to copy. attributes are the entry's own, as GetFileAttributesW
// reports them without following a link.
struct SourceEntry {
    const std::wstring& path;
    DWORD attributes;
};

// Clones the reparse point of source, such as a link, to dstAbsolute, and
// copies its extended attributes. WSL keeps the mode and the device number
// of a special file in extended attributes, and overlayfs copies xattrs
// on copy-up. Then writes the copy-up record that policy.record selects
// on the clone, not on a link's target, as overlayfs keeps the inode number
// of a copied-up symlink. A failure removes the clone.
NTSTATUS CloneReparsePointWithCopyUpRecord(const SourceEntry& source,
                                           const std::wstring& dstAbsolute,
                                           const EntryCopyPolicy& policy);

// Clones the reparse point of source to upperPath, as
// CloneReparsePointWithCopyUpRecord clones it. BuildInContainerAndMove
// builds the clone in containerPath, a new path in the work directory, so
// the clone inherits the ACEs of its upper parent. When an entry holds
// upperPath, fails with STATUS_OBJECT_NAME_COLLISION and leaves that entry
// as it was.
NTSTATUS CloneReparsePointThroughWorkDir(const SourceEntry& source,
                                         const HostPath& containerPath,
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
