#include "DirectoryRename.h"
#include "CopyUp.h"
#include "LayerPath.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"
#include "Cache.h"
#include "NtStatusUtil.h"
#include "ElevationUtil.h"

#include <aclapi.h>
#include <optional>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace LayerMount {

namespace {

bool IsDotOrDotDot(const wchar_t* name) {
    return name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0));
}

// Calls visit on each entry of the directory at dirAbs except "." and "..",
// and stops at the first visit that fails. Returns that visit's status, or
// the listing's Win32 error as an NTSTATUS when the directory cannot be
// listed to its end.
template <typename Visit>
NTSTATUS ForEachChildEntry(const std::wstring& dirAbs, const Visit& visit) {
    WIN32_FIND_DATAW fd{};
    const HANDLE hFind = ::FindFirstFileW((dirAbs + L"\\*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return ::LayerMount::NtStatusFromWin32(::GetLastError());
    }
    NTSTATUS status = STATUS_SUCCESS;
    do {
        if (!IsDotOrDotDot(fd.cFileName)) {
            status = visit(fd);
        }
    } while (NT_SUCCESS(status) && ::FindNextFileW(hFind, &fd));
    // FindNextFileW returns false both at the end of the directory and on a
    // failure. FindClose can overwrite the error, so read it first.
    const DWORD listError = NT_SUCCESS(status) ? ::GetLastError() : ERROR_NO_MORE_FILES;
    ::FindClose(hFind);
    if (listError != ERROR_NO_MORE_FILES) {
        return ::LayerMount::NtStatusFromWin32(listError);
    }
    return status;
}

// Whether the directory at dirAbs holds a whiteout for name. NTFS matches
// the whiteout's name to name without regard to case.
bool HasWhiteoutBeside(const std::wstring& dirAbs, const std::wstring& name) {
    const std::wstring whiteout = dirAbs + L"\\" + kWhiteoutPrefix + name;
    return ::GetFileAttributesW(whiteout.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Copies the owner, group, DACL and SACL of srcAbs to dstAbs. The DACL and
// SACL go through SetNamedSecurityInfoW with the UNPROTECTED flags, so the
// destination drops the source's inherited ACEs and inherits from its own
// parent, and explicit ACEs stay explicit. SetFileSecurityW would write the
// inherited ACEs as explicit ones. The SACL copies only when the process
// holds SE_SECURITY_NAME, because a SACL read fails without it. Tries every
// part and returns the Win32 error of the first write that failed, or
// ERROR_SUCCESS.
DWORD CopySecurityKeepingInheritance(const std::wstring& srcAbs, const std::wstring& dstAbs) {
    DWORD firstError = ERROR_SUCCESS;
    const auto keepFirst = [&firstError](DWORD error) {
        if (firstError == ERROR_SUCCESS) {
            firstError = error;
        }
    };

    const SECURITY_INFORMATION ogInfo = OWNER_SECURITY_INFORMATION |
                                        GROUP_SECURITY_INFORMATION;
    DWORD sdSize = 0;
    ::GetFileSecurityW(srcAbs.c_str(), ogInfo, nullptr, 0, &sdSize);
    if (sdSize > 0) {
        std::vector<BYTE> sdBuf(sdSize);
        auto* sd = reinterpret_cast<PSECURITY_DESCRIPTOR>(sdBuf.data());
        if (::GetFileSecurityW(srcAbs.c_str(), ogInfo, sd, sdSize, &sdSize) &&
            !::SetFileSecurityW(dstAbs.c_str(), ogInfo, sd)) {
            const DWORD err = ::GetLastError();
            keepFirst(err ? err : ERROR_ACCESS_DENIED);
        }
    }

    DWORD dSize = 0;
    ::GetFileSecurityW(srcAbs.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &dSize);
    if (dSize > 0) {
        std::vector<BYTE> dBuf(dSize);
        auto* dsd = reinterpret_cast<PSECURITY_DESCRIPTOR>(dBuf.data());
        if (::GetFileSecurityW(srcAbs.c_str(), DACL_SECURITY_INFORMATION,
                               dsd, dSize, &dSize)) {
            BOOL present = FALSE, defaulted = FALSE;
            PACL dacl = nullptr;
            if (::GetSecurityDescriptorDacl(dsd, &present, &dacl, &defaulted) &&
                present && dacl) {
                const DWORD rc = ::SetNamedSecurityInfoW(
                    const_cast<LPWSTR>(dstAbs.c_str()),
                    SE_FILE_OBJECT,
                    DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                    nullptr, nullptr, dacl, nullptr);
                if (rc != ERROR_SUCCESS) {
                    keepFirst(rc);
                }
            }
        }
    }

    if (IsSecurityPrivilegeHeld()) {
        DWORD ssSize = 0;
        ::GetFileSecurityW(srcAbs.c_str(), SACL_SECURITY_INFORMATION, nullptr, 0, &ssSize);
        if (ssSize > 0) {
            std::vector<BYTE> ssBuf(ssSize);
            auto* ssd = reinterpret_cast<PSECURITY_DESCRIPTOR>(ssBuf.data());
            if (::GetFileSecurityW(srcAbs.c_str(), SACL_SECURITY_INFORMATION,
                                   ssd, ssSize, &ssSize)) {
                BOOL present = FALSE, defaulted = FALSE;
                PACL sacl = nullptr;
                if (::GetSecurityDescriptorSacl(ssd, &present, &sacl, &defaulted) &&
                    present && sacl) {
                    const DWORD rc = ::SetNamedSecurityInfoW(
                        const_cast<LPWSTR>(dstAbs.c_str()),
                        SE_FILE_OBJECT,
                        SACL_SECURITY_INFORMATION | UNPROTECTED_SACL_SECURITY_INFORMATION,
                        nullptr, nullptr, nullptr, sacl);
                    if (rc != ERROR_SUCCESS) {
                        keepFirst(rc);
                    }
                }
            }
        }
    }

    return firstError;
}

// Copies a regular file with its ADS, its sparse state when the host
// adapter has the sparse capability, and the copy-up record
// `policy.record` selects. Does not commit through the work directory. On
// failure the caller removes the whole new subtree.
NTSTATUS CopyFilePreservingMetadata(const std::wstring& srcAbs,
                                     const std::wstring& dstAbs,
                                     const EntryCopyPolicy& policy) {
    HANDLE srcH = ::CreateFileW(srcAbs.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN |
                                      FILE_FLAG_BACKUP_SEMANTICS,
                                  nullptr);
    if (srcH == INVALID_HANDLE_VALUE) {
        return ::LayerMount::NtStatusFromWin32(::GetLastError());
    }

    DWORD srcAttrs = ::GetFileAttributesW(srcAbs.c_str());
    if (srcAttrs == INVALID_FILE_ATTRIBUTES) {
        DWORD err = ::GetLastError();
        ::CloseHandle(srcH);
        return ::LayerMount::NtStatusFromWin32(err);
    }
    FILETIME ftCreate{}, ftAccess{}, ftWrite{};
    ::GetFileTime(srcH, &ftCreate, &ftAccess, &ftWrite);

    HANDLE dstH = ::CreateFileW(dstAbs.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dstH == INVALID_HANDLE_VALUE) {
        DWORD err = ::GetLastError();
        ::CloseHandle(srcH);
        return ::LayerMount::NtStatusFromWin32(err);
    }

    // SetFileAttributes cannot set FILE_ATTRIBUTE_SPARSE_FILE. Only FSCTL_SET_SPARSE can.
    if (HasFileAttribute(srcAttrs, FILE_ATTRIBUTE_SPARSE_FILE) &&
        policy.capabilities.HasSparseFiles() &&
        !SetSparse(dstH)) {
        const NTSTATUS status = SparseRefusalStatus();
        ::CloseHandle(srcH);
        ::CloseHandle(dstH);
        return status;
    }

    SetCompressedIfSource(dstH, srcAttrs);

    const NTSTATUS dataStatus = CopyFileDataKeepingHoles(srcH, dstH);
    if (!NT_SUCCESS(dataStatus)) {
        ::CloseHandle(srcH);
        ::CloseHandle(dstH);
        return dataStatus;
    }

    ::CloseHandle(srcH);
    ::CloseHandle(dstH);

    if (!ApplyEncryptedStateIfNeeded(dstAbs, srcAttrs)) {
        return ::LayerMount::NtStatusFromWin32(::GetLastError());
    }

    if (!CopyUserAlternateDataStreams(srcAbs, dstAbs)) {
        DWORD streamError = ::GetLastError();
        if (streamError == ERROR_SUCCESS) streamError = ERROR_INVALID_DATA;
        ::DeleteFileW(dstAbs.c_str());
        return ::LayerMount::NtStatusFromWin32(streamError);
    }

    const DWORD securityError = CopySecurityKeepingInheritance(srcAbs, dstAbs);
    if (securityError != ERROR_SUCCESS) {
        ::DeleteFileW(dstAbs.c_str());
        return ::LayerMount::NtStatusFromWin32(securityError);
    }

    const NTSTATUS recordStatus = WriteCopyUpRecordOrRemoveEntry(
        dstAbs, CopiedEntryMetadata(srcAbs, policy.record, policy.config),
        NewUpperEntryKind::File, policy.config);
    if (!NT_SUCCESS(recordStatus)) {
        return recordStatus;
    }

    // Re-apply attributes + timestamps last (both are perturbed by ADS writes
    // on NTFS: WriteFile bumps LastWrite, and the dest was created with
    // FILE_ATTRIBUTE_NORMAL).
    ::SetFileAttributesW(dstAbs.c_str(), srcAttrs);
    HANDLE tsH = ::CreateFileW(dstAbs.c_str(), FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (tsH != INVALID_HANDLE_VALUE) {
        ::SetFileTime(tsH, &ftCreate, &ftAccess, &ftWrite);
        ::CloseHandle(tsH);
    }

    return STATUS_SUCCESS;
}

NTSTATUS CopyDirectoryShell(const std::wstring& srcAbs,
                             const std::wstring& dstAbs,
                             const EntryCopyPolicy& policy) {
    const NTSTATUS dirStatus = CreateDirectoryOrUseExisting(dstAbs);
    if (!NT_SUCCESS(dirStatus)) {
        return dirStatus;
    }

    DWORD srcAttrs = ::GetFileAttributesW(srcAbs.c_str());
    if (srcAttrs != INVALID_FILE_ATTRIBUTES) {
        ::SetFileAttributesW(dstAbs.c_str(), srcAttrs);
    }

    const NTSTATUS layoutStatus = ApplyDirectoryLayout(dstAbs, srcAttrs);
    if (!NT_SUCCESS(layoutStatus)) {
        return layoutStatus;
    }

    // Inheritable ACEs on the directory reach the children copied into it.
    const DWORD securityError = CopySecurityKeepingInheritance(srcAbs, dstAbs);
    if (securityError != ERROR_SUCCESS) {
        return ::LayerMount::NtStatusFromWin32(securityError);
    }

    const NTSTATUS recordStatus = WriteCopyUpRecordOrRemoveEntry(
        dstAbs, CopiedEntryMetadata(srcAbs, policy.record, policy.config),
        NewUpperEntryKind::Directory, policy.config);
    if (!NT_SUCCESS(recordStatus)) {
        return recordStatus;
    }

    HANDLE srcH = ::CreateFileW(srcAbs.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (srcH != INVALID_HANDLE_VALUE) {
        FILETIME c{}, a{}, w{};
        ::GetFileTime(srcH, &c, &a, &w);
        ::CloseHandle(srcH);
        HANDLE dstH = ::CreateFileW(dstAbs.c_str(), FILE_WRITE_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (dstH != INVALID_HANDLE_VALUE) {
            ::SetFileTime(dstH, &c, &a, &w);
            ::CloseHandle(dstH);
        }
    }
    return STATUS_SUCCESS;
}

}

DirectoryRename::DirectoryRename(ConfigRef config,
                                 PathResolver& pathResolver,
                                 WhiteoutManager& whiteoutMgr,
                                 Cache& cache,
                                 CopyUp& copyUp,
                                 ::LayerMount::abi::CapabilityGate capabilities)
    : config_(config.Get())
    , pathResolver_(pathResolver)
    , whiteoutMgr_(whiteoutMgr)
    , cache_(cache)
    , copyUp_(copyUp)
    , capabilities_(capabilities) {
}

bool DirectoryRename::DestinationExistsInMerged(const std::wstring& normalizedPath) const {
    const bool destInUpper = pathResolver_.ExistsInUpper(normalizedPath);
    const bool destWhitedOut =
        whiteoutMgr_.HasWhiteout(normalizedPath, config_.upperPath);
    const bool destInLower =
        pathResolver_.ResolveLowerPath(normalizedPath).Found();
    return destInUpper || (destInLower && !destWhitedOut);
}

NTSTATUS DirectoryRename::PrepareRenameDestination(const CallerPath& newCallerPath,
                                                   ReplaceExisting replace) {
    const std::wstring newNormalized = NormalizePath(newCallerPath.Text());
    if (replace == ReplaceExisting::No && DestinationExistsInMerged(newNormalized)) {
        return STATUS_OBJECT_NAME_COLLISION;
    }
    return copyUp_.EnsureUpperParent(newNormalized);
}

NTSTATUS DirectoryRename::RemoveEntryHiddenByUpperWhiteout(const std::wstring& oldUpperPath,
                                                           const std::wstring& newUpperPath,
                                                           const std::wstring& whiteoutName) {
    const std::optional<std::wstring> hidden = WhiteoutManager::WhitedOutNameOfEntry(whiteoutName);
    if (!hidden.has_value() ||
        ::GetFileAttributesW((oldUpperPath + L"\\" + *hidden).c_str()) != INVALID_FILE_ATTRIBUTES) {
        return STATUS_SUCCESS;
    }
    return RemoveUpperEntry(newUpperPath + L"\\" + *hidden, config_);
}

bool DirectoryRename::UpperEntryMergesIntoCopy(const std::wstring& relativePath,
                                               DWORD upperAttrs,
                                               const std::wstring& copyPath) const {
    const DWORD copyAttrs = ::GetFileAttributesW(copyPath.c_str());
    return IsEnumerableDirectory(upperAttrs) &&
           copyAttrs != INVALID_FILE_ATTRIBUTES && IsEnumerableDirectory(copyAttrs) &&
           !whiteoutMgr_.IsOpaqueInLayer(relativePath, config_.upperPath) &&
           !whiteoutMgr_.HasWhiteout(relativePath, config_.upperPath);
}

NTSTATUS DirectoryRename::OverlayUpperShadow(const std::wstring& oldRelativePath,
                                             const std::wstring& oldUpperPath,
                                             const std::wstring& newUpperPath) {
    return ForEachChildEntry(oldUpperPath, [&](const WIN32_FIND_DATAW& fd) -> NTSTATUS {
        const std::wstring name = fd.cFileName;
        if (WhiteoutManager::IsWhiteoutName(name)) {
            return RemoveEntryHiddenByUpperWhiteout(oldUpperPath, newUpperPath, name);
        }
        const std::wstring childRelative = oldRelativePath + L"\\" + name;
        const std::wstring childSrc = oldUpperPath + L"\\" + name;
        const std::wstring childDst = newUpperPath + L"\\" + name;
        if (UpperEntryMergesIntoCopy(childRelative, fd.dwFileAttributes, childDst)) {
            const NTSTATUS shellStatus = CopyDirectoryShell(
                childSrc, childDst, {CopiedEntryRecord::CarriedFromSource, config_, capabilities_});
            if (!NT_SUCCESS(shellStatus)) {
                return shellStatus;
            }
            return OverlayUpperShadow(childRelative, childSrc, childDst);
        }
        const NTSTATUS removeStatus = RemoveUpperEntry(childDst, config_);
        if (!NT_SUCCESS(removeStatus)) {
            return removeStatus;
        }
        return CopyTreeWithoutMarkers(childSrc, fd.dwFileAttributes, childDst,
                                      CopiedEntryRecord::CarriedFromSource);
    });
}

NTSTATUS DirectoryRename::CopyMergedDirectory(const std::wstring& oldNorm,
                                              const std::wstring& lowerAbs,
                                              DWORD lowerAttrs,
                                              const std::wstring& oldUpperPath,
                                              const std::wstring& newNorm,
                                              const std::wstring& newUpperPath) {
    NTSTATUS status = CopyTreeWithoutMarkers(lowerAbs, lowerAttrs, newUpperPath,
                                             CopiedEntryRecord::NewFromSource);
    const DWORD oldUpperAttrs = ::GetFileAttributesW(oldUpperPath.c_str());
    if (NT_SUCCESS(status) && oldUpperAttrs != INVALID_FILE_ATTRIBUTES &&
        (oldUpperAttrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        status = OverlayUpperShadow(oldNorm, oldUpperPath, newUpperPath);
    }
    if (NT_SUCCESS(status)) {
        status = whiteoutMgr_.SetOpaque(newNorm);
    }
    if (!NT_SUCCESS(status)) {
        RemoveUpperEntry(newUpperPath, config_);
    }
    return status;
}

NTSTATUS DirectoryRename::RenameLowerDirectory(const CallerPath& oldCallerPath,
                                               const CallerPath& newCallerPath,
                                               RenameEntryKind sourceKind,
                                               ReplaceExisting replace) {
    const std::wstring oldNorm = NormalizePath(oldCallerPath.Text());
    const std::wstring newNorm = NormalizePath(newCallerPath.Text());
    const std::wstring newUpperPath = pathResolver_.GetUpperPathForNewEntry(newCallerPath);

    const ResolvedPath source = pathResolver_.ResolveLowerPath(oldNorm);
    if (!source.Found()) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    const NTSTATUS destinationStatus = PrepareRenameDestination(newCallerPath, replace);
    if (!NT_SUCCESS(destinationStatus)) {
        return destinationStatus;
    }

    const std::wstring oldUpperPath = pathResolver_.GetUpperPath(oldNorm);
    const NTSTATUS copyStatus = sourceKind == RenameEntryKind::Link
        ? CopyLinkWithCopyUpRecord(source.absolutePath, source.attributes, newUpperPath,
                                   {CopiedEntryRecord::NewFromSource, config_, capabilities_})
        : CopyMergedDirectory(oldNorm, source.absolutePath, source.attributes, oldUpperPath,
                              newNorm, newUpperPath);
    if (!NT_SUCCESS(copyStatus)) {
        return copyStatus;
    }

    RemoveUpperEntry(oldUpperPath, config_);

    cache_.InvalidateWithAncestors(oldNorm);
    cache_.InvalidateWithAncestors(newNorm);
    return STATUS_SUCCESS;
}

NTSTATUS DirectoryRename::RenameUpperDirectory(const CallerPath& oldCallerPath,
                                               const CallerPath& newCallerPath,
                                               RenameEntryKind sourceKind,
                                               ReplaceExisting replace) {
    std::wstring oldNorm = NormalizePath(oldCallerPath.Text());
    std::wstring newNorm = NormalizePath(newCallerPath.Text());
    std::wstring newUpperPath = pathResolver_.GetUpperPathForNewEntry(newCallerPath);

    NTSTATUS destinationStatus = PrepareRenameDestination(newCallerPath, replace);
    if (!NT_SUCCESS(destinationStatus)) {
        return destinationStatus;
    }

    std::wstring oldUpperPath = pathResolver_.GetUpperPath(oldNorm);

    const bool isLink = sourceKind == RenameEntryKind::Link;
    const bool wasOpaque = !isLink && whiteoutMgr_.IsOpaque(oldNorm);

    const NTSTATUS moveStatus = MoveUpperEntry(oldUpperPath, newUpperPath, replace,
                                               CopyAcrossVolumes::No, config_);
    if (!NT_SUCCESS(moveStatus)) {
        return moveStatus;
    }

    if (wasOpaque) {
        whiteoutMgr_.RemoveOpaque(oldNorm);
    }
    if (!isLink &&
        (wasOpaque || pathResolver_.ResolveLowerPath(newNorm).Found())) {
        whiteoutMgr_.SetOpaque(newNorm);
    }

    cache_.InvalidateWithAncestors(oldNorm);
    cache_.InvalidateWithAncestors(newNorm);

    return STATUS_SUCCESS;
}

NTSTATUS DirectoryRename::CopyTreeWithoutMarkers(const std::wstring& srcAbs,
                                                 DWORD srcAttrs,
                                                 const std::wstring& dstAbs,
                                                 CopiedEntryRecord record) {
    const EntryCopyPolicy policy{record, config_, capabilities_};
    if ((srcAttrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return CopyLinkWithCopyUpRecord(srcAbs, srcAttrs, dstAbs, policy);
    }
    if ((srcAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return CopyFilePreservingMetadata(srcAbs, dstAbs, policy);
    }
    return CopyDirectoryTreeWithoutMarkers(srcAbs, dstAbs, record);
}

NTSTATUS DirectoryRename::CopyDirectoryTreeWithoutMarkers(const std::wstring& srcAbs,
                                                          const std::wstring& dstAbs,
                                                          CopiedEntryRecord record) {
    const NTSTATUS shellStatus =
        CopyDirectoryShell(srcAbs, dstAbs, {record, config_, capabilities_});
    if (!NT_SUCCESS(shellStatus)) {
        return shellStatus;
    }

    const bool lowerSource = record == CopiedEntryRecord::NewFromSource;
    return ForEachChildEntry(srcAbs, [&](const WIN32_FIND_DATAW& fd) -> NTSTATUS {
        if (WhiteoutManager::IsWhiteoutName(fd.cFileName) ||
            (lowerSource && HasWhiteoutBeside(srcAbs, fd.cFileName))) {
            return STATUS_SUCCESS;
        }
        return CopyTreeWithoutMarkers(srcAbs + L"\\" + fd.cFileName, fd.dwFileAttributes,
                                      dstAbs + L"\\" + fd.cFileName, record);
    });
}

}
