#include "DirectoryRename.h"
#include "CopyUp.h"
#include "DirectoryMerge.h"
#include "LayerPath.h"
#include "PathResolver.h"
#include "RenameRollback.h"
#include "WhiteoutManager.h"
#include "Cache.h"
#include "MetadataStore.h"
#include "NtStatusUtil.h"
#include "ElevationUtil.h"
#include "ScopedHandle.h"

#include <aclapi.h>
#include <cassert>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace LayerMount {

namespace {

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

// Gives the copied file at dstAbs the encryption, user streams, extended
// attributes, security and copy-up record of the file at srcAbs. Runs after
// every handle to dstAbs is closed, because each step opens it by path.
NTSTATUS CopyFileMetadataAfterData(const std::wstring& srcAbs,
                                   DWORD srcAttrs,
                                   const std::wstring& dstAbs,
                                   const EntryCopyPolicy& policy) {
    if (!ApplyEncryptedStateIfNeeded(dstAbs, srcAttrs)) {
        return ::LayerMount::NtStatusFromWin32(::GetLastError());
    }

    const NTSTATUS streamStatus = CopyUserAlternateDataStreams(srcAbs, dstAbs);
    if (!NT_SUCCESS(streamStatus)) {
        return streamStatus;
    }

    const NTSTATUS eaStatus = CopyExtendedAttributes(srcAbs, dstAbs);
    if (!NT_SUCCESS(eaStatus)) {
        return eaStatus;
    }

    const DWORD securityError = CopySecurityKeepingInheritance(srcAbs, dstAbs);
    if (securityError != ERROR_SUCCESS) {
        return ::LayerMount::NtStatusFromWin32(securityError);
    }

    return WriteCopyUpRecordOrRemoveEntry(
        dstAbs, CopiedEntryMetadata(srcAbs, policy.record, policy.config),
        NewUpperEntryKind::File, policy.config);
}

// Copies a regular file with its ADS, its extended attributes, its sparse
// state when the host adapter has the sparse capability, and the copy-up
// record `policy.record` selects. Does not commit through the work
// directory. On failure the caller removes the whole new subtree.
NTSTATUS CopyFilePreservingMetadata(const std::wstring& srcAbs,
                                     const std::wstring& dstAbs,
                                     const EntryCopyPolicy& policy) {
    ScopedHandle srcHandle(OpenSourceFileForCopy(srcAbs));
    if (!srcHandle.IsValid()) {
        return ::LayerMount::NtStatusFromWin32(::GetLastError());
    }

    const DWORD srcAttrs = ::GetFileAttributesW(srcAbs.c_str());
    if (srcAttrs == INVALID_FILE_ATTRIBUTES) {
        return ::LayerMount::NtStatusFromWin32(::GetLastError());
    }
    EntryTimes srcTimes{};
    ::GetFileTime(srcHandle.Get(), &srcTimes.creation, &srcTimes.access, &srcTimes.write);

    ScopedHandle dstHandle(::CreateFileW(dstAbs.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                         nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                         nullptr));
    if (!dstHandle.IsValid()) {
        return ::LayerMount::NtStatusFromWin32(::GetLastError());
    }

    // SetFileAttributes cannot set FILE_ATTRIBUTE_SPARSE_FILE. Only FSCTL_SET_SPARSE can.
    if (HasFileAttribute(srcAttrs, FILE_ATTRIBUTE_SPARSE_FILE) &&
        policy.config.Capabilities().HasSparseFiles() &&
        !SetSparse(dstHandle.Get())) {
        return SparseRefusalStatus();
    }

    SetCompressedIfSource(dstHandle.Get(), srcAttrs);

    const NTSTATUS dataStatus = CopyFileDataKeepingHoles(srcHandle.Get(), dstHandle.Get());
    if (!NT_SUCCESS(dataStatus)) {
        return dataStatus;
    }

    srcHandle.Reset();
    dstHandle.Reset();

    const NTSTATUS metadataStatus = CopyFileMetadataAfterData(srcAbs, srcAttrs, dstAbs, policy);
    if (!NT_SUCCESS(metadataStatus)) {
        return metadataStatus;
    }

    // The data, stream and extended attribute writes move the last-write
    // time, and CreateFileW made the copy with FILE_ATTRIBUTE_NORMAL, so
    // the attributes and times go on last.
    WriteEntryTimes(dstAbs, srcTimes, srcAttrs);

    return STATUS_SUCCESS;
}

// Writes the copy-up record of the directory at srcAbs to the copy at
// dstAbs. A failure leaves the copy in place for the caller to remove.
NTSTATUS WriteDirectoryCopyUpRecord(const std::wstring& srcAbs,
                                    const std::wstring& dstAbs,
                                    const EntryCopyPolicy& policy) {
    if (MetadataStore::WriteLayerMountMetadata(
            dstAbs, CopiedEntryMetadata(srcAbs, policy.record, policy.config), &policy.config)) {
        return STATUS_SUCCESS;
    }
    const DWORD err = ::GetLastError();
    return ::LayerMount::NtStatusFromWin32(err ? err : ERROR_WRITE_FAULT);
}

// Gives the copied directory at dstAbs the attributes and times of the
// directory at srcAbs, then its security. Runs after the last write inside
// dstAbs. It ignores a failure to read or set the attributes and times.
// The security goes last because a copied DACL can deny listing, and a
// failed rename must still remove the partial tree. The UNPROTECTED DACL
// write passes inheritable ACEs on to the children already in place.
NTSTATUS ApplyDirectoryBasicInfoAndSecurity(const std::wstring& srcAbs,
                                            const std::wstring& dstAbs) {
    WIN32_FILE_ATTRIBUTE_DATA source{};
    if (::GetFileAttributesExW(srcAbs.c_str(), GetFileExInfoStandard, &source)) {
        WriteEntryTimes(dstAbs, EntryTimesOf(source), source.dwFileAttributes);
    }

    const DWORD securityError = CopySecurityKeepingInheritance(srcAbs, dstAbs);
    if (securityError != ERROR_SUCCESS) {
        return ::LayerMount::NtStatusFromWin32(securityError);
    }
    return STATUS_SUCCESS;
}

// Runs once the children of the copied directory at dstAbs are in place.
// The copy takes its layout, streams, extended attributes, attributes,
// times and security from viewSrcAbs and its copy-up record from
// recordSrcAbs. markCopy writes any marker the copy needs after the
// record.
template <typename MarkCopy>
NTSTATUS FinishCopiedDirectory(const std::wstring& viewSrcAbs,
                               const std::wstring& recordSrcAbs,
                               const std::wstring& dstAbs,
                               const EntryCopyPolicy& policy,
                               const MarkCopy& markCopy) {
    const NTSTATUS ownMetadataStatus = CopyDirectoryOwnMetadata(viewSrcAbs, dstAbs);
    if (!NT_SUCCESS(ownMetadataStatus)) {
        return ownMetadataStatus;
    }
    const NTSTATUS recordStatus = WriteDirectoryCopyUpRecord(recordSrcAbs, dstAbs, policy);
    if (!NT_SUCCESS(recordStatus)) {
        return recordStatus;
    }
    const NTSTATUS markStatus = markCopy();
    if (!NT_SUCCESS(markStatus)) {
        return markStatus;
    }
    return ApplyDirectoryBasicInfoAndSecurity(viewSrcAbs, dstAbs);
}

bool IsDirectoryAt(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// The layer root that a merged entry comes from, and the copy-up record its
// copy gets. An upper entry keeps its record, and a lower entry gets a new
// one.
struct MergedEntrySource {
    const std::wstring& root;
    CopiedEntryRecord record;
};

MergedEntrySource SourceOf(const LayerConfig& config, const MergedEntry& entry) {
    if (entry.source == LayerSource::Upper) {
        return MergedEntrySource{config.upperPath, CopiedEntryRecord::CarriedFromSource};
    }
    return MergedEntrySource{config.lowerPaths.at(static_cast<size_t>(entry.lowerIndex)),
                             CopiedEntryRecord::NewFromSource};
}

// copyChildren fills a copied directory before the directory takes the
// layout, times and security of srcAbs.
template <typename CopyChildren>
NTSTATUS CopyEntry(const std::wstring& srcAbs,
                   DWORD srcAttrs,
                   const std::wstring& dstAbs,
                   const EntryCopyPolicy& policy,
                   const CopyChildren& copyChildren) {
    bool clonesReparsePoint = false;
    const NTSTATUS cloneStatus = ClonesReparsePoint(srcAbs, srcAttrs, &clonesReparsePoint);
    if (!NT_SUCCESS(cloneStatus)) {
        return cloneStatus;
    }
    if (clonesReparsePoint) {
        return CloneReparsePointWithCopyUpRecord({srcAbs, srcAttrs}, dstAbs, policy);
    }
    if ((srcAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return CopyFilePreservingMetadata(srcAbs, dstAbs, policy);
    }
    const NTSTATUS dirStatus = CreateDirectoryOrUseExisting(dstAbs);
    if (!NT_SUCCESS(dirStatus)) {
        return dirStatus;
    }
    const NTSTATUS childrenStatus = copyChildren();
    if (!NT_SUCCESS(childrenStatus)) {
        return childrenStatus;
    }
    return FinishCopiedDirectory(srcAbs, srcAbs, dstAbs, policy,
                                 []() -> NTSTATUS { return STATUS_SUCCESS; });
}

}

DirectoryRename::DirectoryRename(ConfigRef config,
                                 PathResolver& pathResolver,
                                 WhiteoutManager& whiteoutMgr,
                                 Cache& cache,
                                 CopyUp& copyUp,
                                 const abi::EventEmitter& events)
    : config_(config.Get())
    , pathResolver_(pathResolver)
    , whiteoutMgr_(whiteoutMgr)
    , cache_(cache)
    , copyUp_(copyUp)
    , events_(events) {
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

NTSTATUS DirectoryRename::CopyMergedDirectory(const ResolvedPath& lowerSource,
                                              const RenamedName& oldName,
                                              const std::wstring& stagedPath) {
    const bool hasUpperShadow = IsDirectoryAt(oldName.upperPath);
    const std::wstring& mergedViewSource =
        hasUpperShadow ? oldName.upperPath : lowerSource.absolutePath;

    if (!::CreateDirectoryW(stagedPath.c_str(), nullptr)) {
        return StatusOfFailedCall(ERROR_WRITE_FAULT);
    }
    const NTSTATUS childrenStatus = CopyMergedChildren(
        MergeDirectoryWithAncestry(config_, whiteoutMgr_, oldName.norm), stagedPath);
    if (!NT_SUCCESS(childrenStatus)) {
        return childrenStatus;
    }
    return FinishCopiedDirectory(
        mergedViewSource, lowerSource.absolutePath, stagedPath,
        {CopiedEntryRecord::NewFromSource, config_},
        [&]() { return whiteoutMgr_.SetOpaqueAtPath(stagedPath); });
}

NTSTATUS DirectoryRename::CopyMergedChildren(const MergedDirectoryWithAncestry& oldDir,
                                             const std::wstring& dstPath) {
    if (!NT_SUCCESS(oldDir.merged.status)) {
        return oldDir.merged.status;
    }
    for (const auto& keyAndEntry : oldDir.merged.entries) {
        const NTSTATUS status = CopyMergedEntry(oldDir, keyAndEntry.second, dstPath);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS DirectoryRename::CopyMergedEntry(const MergedDirectoryWithAncestry& oldParent,
                                          const MergedEntry& entry,
                                          const std::wstring& dstParentPath) {
    const std::wstring name = entry.findData.cFileName;
    const MergedEntrySource source = SourceOf(config_, entry);
    const std::wstring dstAbs = dstParentPath + L"\\" + name;
    return CopyEntry(
        JoinDirPath(source.root, oldParent.dirPath) + L"\\" + name,
        entry.findData.dwFileAttributes, dstAbs, {source.record, config_},
        [&]() {
            return CopyMergedChildren(
                MergeChildDirectory(config_, whiteoutMgr_, oldParent, name), dstAbs);
        });
}

RenameStepResult DirectoryRename::RenameLowerDirectory(const RenameCallerPaths& paths,
                                                       EntryKind sourceKind,
                                                       ReplaceExisting replace) {
    assert(sourceKind == EntryKind::Directory || sourceKind == EntryKind::Link);
    const std::wstring oldNorm = NormalizePath(paths.oldPath.Text());
    const std::wstring newNorm = NormalizePath(paths.newPath.Text());
    const std::wstring newUpperPath = pathResolver_.GetUpperPathForNewEntry(paths.newPath);

    const ResolvedPath source = pathResolver_.ResolveLowerPath(oldNorm);
    if (!source.Found()) {
        return {STATUS_OBJECT_NAME_NOT_FOUND, false};
    }

    const NTSTATUS destinationStatus = PrepareRenameDestination(paths.newPath, replace);
    if (!NT_SUCCESS(destinationStatus)) {
        return {destinationStatus, false};
    }

    const std::wstring oldUpperPath = pathResolver_.GetUpperPath(oldNorm);
    const NTSTATUS status = sourceKind == EntryKind::Link
        ? CloneReparsePointThroughWorkDir({source.absolutePath, source.attributes},
                                          copyUp_.GenerateStagingPath(), newUpperPath,
                                          {CopiedEntryRecord::NewFromSource, config_})
        : BuildInContainerAndMove(
              copyUp_.GenerateStagingPath(), newUpperPath, config_,
              [&](const std::wstring& stagedPath) {
                  return CopyMergedDirectory(source, {oldNorm, oldUpperPath}, stagedPath);
              });
    if (!NT_SUCCESS(status)) {
        return {status, false};
    }

    std::wstring asidePath;
    const UpperEntryMove aside = MoveUpperEntryAside(
        oldUpperPath, [this]() { return copyUp_.GenerateStagingPath(); }, config_, &asidePath);
    WarnRecordLeftBehind(events_, aside, oldNorm);
    const NTSTATUS asideStatus = aside.status;
    bool newNameOccupied = false;
    if (!NT_SUCCESS(asideStatus)) {
        const NTSTATUS removal = RemoveUpperEntry(newUpperPath, config_);
        newNameOccupied = !NT_SUCCESS(removal);
        if (newNameOccupied) {
            events_.Emit(LM_EVT_WARNING, HresultFromNtStatus(removal), newNorm.c_str(),
                         L"The undo of a failed rename could not remove the copy at the new name");
        }
    } else if (!asidePath.empty()) {
        RemoveStagedEntry(asidePath, config_);
    }

    cache_.InvalidateWithAncestors(oldNorm);
    cache_.InvalidateWithAncestors(newNorm);
    return {asideStatus, newNameOccupied};
}

NTSTATUS DirectoryRename::RenameUpperDirectory(const RenameCallerPaths& paths,
                                               EntryKind sourceKind,
                                               ReplaceExisting replace) {
    assert(sourceKind == EntryKind::Directory || sourceKind == EntryKind::Link);
    std::wstring oldNorm = NormalizePath(paths.oldPath.Text());
    std::wstring newNorm = NormalizePath(paths.newPath.Text());
    std::wstring newUpperPath = pathResolver_.GetUpperPathForNewEntry(paths.newPath);

    NTSTATUS destinationStatus = PrepareRenameDestination(paths.newPath, replace);
    if (!NT_SUCCESS(destinationStatus)) {
        return destinationStatus;
    }

    std::wstring oldUpperPath = pathResolver_.GetUpperPath(oldNorm);

    const bool wasOpaque = whiteoutMgr_.IsOpaque(oldNorm);
    const bool marksSource = sourceKind != EntryKind::Link && !wasOpaque &&
                             pathResolver_.ResolveLowerPath(newNorm).Found();
    if (marksSource) {
        const NTSTATUS markStatus = whiteoutMgr_.SetOpaqueAtPath(oldUpperPath);
        if (!NT_SUCCESS(markStatus)) {
            return markStatus;
        }
    }

    const UpperEntryMove move = MoveUpperEntry(oldUpperPath, newUpperPath, replace, config_);
    WarnRecordLeftBehind(events_, move, NT_SUCCESS(move.status) ? newNorm : oldNorm);
    const NTSTATUS moveStatus = move.status;
    if (!NT_SUCCESS(moveStatus)) {
        if (marksSource) {
            cache_.InvalidateWithAncestors(oldNorm);
        }
        return moveStatus;
    }

    cache_.InvalidateWithAncestors(oldNorm);
    cache_.InvalidateWithAncestors(newNorm);

    return STATUS_SUCCESS;
}

}
