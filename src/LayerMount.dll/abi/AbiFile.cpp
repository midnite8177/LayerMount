// AbiFile.cpp -- File-primitive ABI entry points:
// LayerMountOpenFile, LayerMountCreateFile, LayerMountCloseFile, plus Read / Write /
// GetInfo / SetInfo / Delete / Rename / Security / MergeDirectory / Reparse.

#include "../public/LayerMount.h"
#include "AbiGuard.h"
#include "ErrorTls.h"
#include "HandleTable.h"
#include "HandleTypes.h"
#include "../impl/LayerMount.h"
#include "../impl/WhiteoutManager.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace {

// InternalFileInfo (impl/) and LM_FILE_INFO (public/) carry the same
// field set in the same order; the casing differs and the public struct
// is fixed-shape (revisions via LM_ABI_VERSION bump). Translate by
// named assignment so a future field reorder of either struct produces a
// compile error rather than silent corruption.
inline void ToPublicFileInfo(const ::LayerMount::InternalFileInfo& src,
                             LM_FILE_INFO& dst) {
    dst.fileAttributes = src.FileAttributes;
    dst.reparseTag     = src.ReparseTag;
    dst.allocationSize = src.AllocationSize;
    dst.fileSize       = src.FileSize;
    dst.creationTime   = src.CreationTime;
    dst.lastAccessTime = src.LastAccessTime;
    dst.lastWriteTime  = src.LastWriteTime;
    dst.changeTime     = src.ChangeTime;
    dst.indexNumber    = src.IndexNumber;
    dst.hardLinks      = src.HardLinks;
    dst.eaSize         = src.EaSize;
}

// HRESULT_FROM_NT for the NTSTATUS values the engine returns. The C ABI
// uniformly returns HRESULT; map success to S_OK, everything else
// through the standard HRESULT-from-NTSTATUS encoding (facility = NT,
// severity from the high bit of the NTSTATUS).
inline HRESULT HresultFromNtStatus(NTSTATUS status) {
    if (status == STATUS_SUCCESS) return S_OK;
    return HRESULT_FROM_NT(status);
}

// Validate that a caller-supplied byte buffer is a structurally valid
// self-relative SECURITY_DESCRIPTOR whose every internal reference
// (owner/group/dacl/sacl) fits within [buf, buf+bytes). SetFileSecurityW /
// SetNamedSecurityInfoW rely on the descriptor being self-describing, but
// a malformed Owner/Group/Dacl/Sacl offset can otherwise cause the kernel
// (or these Win32 wrappers) to read past the caller's supplied extent.
inline bool IsValidBoundedSecurityDescriptor(const BYTE* buf, SIZE_T bytes) {
    if (buf == nullptr) return false;
    // Self-relative SECURITY_DESCRIPTORs carry the fixed 20-byte header
    // below followed by inline owner/group/dacl/sacl payload. The
    // documented SECURITY_DESCRIPTOR_MIN_LENGTH macro is sizeof() the
    // ABSOLUTE form (40 bytes on x64, because the absolute struct holds
    // raw pointers in place of those offsets) and must NOT be used to
    // bound a self-relative buffer -- it rejects valid 20-to-39-byte
    // descriptors that legitimately fit a header plus a single SID.
    constexpr SIZE_T kSelfRelativeHeaderBytes = 20;
    if (bytes < kSelfRelativeHeaderBytes) return false;

    // Fixed header layout (always first 20 bytes, regardless of form):
    //   0  Revision   (1)
    //   1  Sbz1       (1)
    //   2  Control    (2)  -- SE_SELF_RELATIVE bit tells us fields are offsets
    //   4  Owner      (4)
    //   8  Group      (4)
    //  12  Sacl       (4)
    //  16  Dacl       (4)
    WORD control = 0;
    std::memcpy(&control, buf + 2, sizeof(WORD));
    if ((control & SE_SELF_RELATIVE) == 0) {
        // Absolute descriptors carry raw pointers in those fields, which are
        // meaningless across an ABI boundary. Reject.
        return false;
    }

    auto readDword = [&](size_t offset) -> DWORD {
        DWORD v = 0;
        std::memcpy(&v, buf + offset, sizeof(DWORD));
        return v;
    };
    // offset == 0 means "field absent". Otherwise the offset and its minimum
    // footprint (SID or ACL header) must fit within the supplied buffer.
    auto offsetFits = [&](DWORD off, size_t minBytes) {
        if (off == 0) return true;
        if (off >= bytes) return false;
        return bytes - off >= minBytes;
    };
    const size_t kMinSidHeader = 8;  // rev(1) + subAuthCount(1) + authority(6)
    const size_t kMinAclHeader = 8;  // rev(2) + aclSize(2) + aceCount(2) + pad(2)
    if (!offsetFits(readDword(4),  kMinSidHeader)) return false;
    if (!offsetFits(readDword(8),  kMinSidHeader)) return false;
    if (!offsetFits(readDword(12), kMinAclHeader)) return false;
    if (!offsetFits(readDword(16), kMinAclHeader)) return false;

    auto sd = reinterpret_cast<PSECURITY_DESCRIPTOR>(const_cast<BYTE*>(buf));
    if (!::IsValidSecurityDescriptor(sd)) return false;

    // Final consistency check: GetSecurityDescriptorLength walks every
    // component (SIDs, ACLs, and the embedded ACE list) and returns the
    // total byte count. Reject any descriptor whose walked length exceeds
    // the caller's supplied extent -- that rules out an ACL AclSize value
    // or an ACE count that points past the buffer.
    const DWORD sdLen = ::GetSecurityDescriptorLength(sd);
    if (sdLen == 0) return false;
    if (static_cast<SIZE_T>(sdLen) > bytes) return false;
    return true;
}

} // namespace

extern "C" {

LM_API HRESULT LM_CALL LayerMountOpenFile(LM_HANDLE       handle,
                                         PCWSTR           relativePath,
                                         UINT32           grantedAccess,
                                         UINT32           createOptions,
                                         DWORD            originatorPid,
                                         LM_FILE_HANDLE* outFile,
                                         LM_FILE_INFO*   outInfo)
{
    using namespace ::LayerMount::abi;

    if (handle       == nullptr) return E_HANDLE;
    if (outFile      == nullptr) return E_POINTER;
    if (outInfo      == nullptr) return E_POINTER;
    if (relativePath == nullptr) return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encodedLayerMount =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encodedLayerMount);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    const DWORD pid = originatorPid != 0 ? originatorPid : ::GetCurrentProcessId();
    std::unique_ptr<::LayerMount::FileContext> ctx;
    ::LayerMount::InternalFileInfo internalInfo{};
    NTSTATUS status = mountHolder->core->Open(
        relativePath, grantedAccess, createOptions,
        pid, &ctx, &internalInfo);
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }

    // Allocate the slot BEFORE moving ctx so the failure path can still
    // close the NT handle via the engine. We use the shared_ptr
    // Allocate overload so we can keep a local reference, then populate
    // `ctx` through the live slot once allocation succeeds.
    auto fileHolder = std::make_shared<FileHolder>();
    fileHolder->parentOwner = mountHolder;                // pins parent
    fileHolder->mount     = mountHolder->core.get();

    const std::uint64_t encodedFile = Handles().file.Allocate(fileHolder);
    if (encodedFile == 0) {
        // Roll back the engine-side open so we don't leak the NT handle
        // or leave activeHandles inflated.
        mountHolder->core->Close(ctx.get());
        ctx.reset();
        ErrorTls::Set(E_OUTOFMEMORY, L"LayerMountOpenFile: file handle table exhausted.");
        return E_OUTOFMEMORY;
    }

    // The handle is not yet published to the caller, so no other thread
    // can observe this slot -- populating ctx here is race-free.
    fileHolder->ctx = std::move(ctx);
    mountHolder->childCount.fetch_add(1, std::memory_order_acq_rel);

    ToPublicFileInfo(internalInfo, *outInfo);
    *outFile = reinterpret_cast<LM_FILE_HANDLE>(static_cast<uintptr_t>(encodedFile));
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountCreateFile(LM_HANDLE       handle,
                                           PCWSTR           relativePath,
                                           UINT32           createOptions,
                                           UINT32           grantedAccess,
                                           UINT32           fileAttributes,
                                           const BYTE*      securityDescriptor,
                                           SIZE_T           securityDescriptorBytes,
                                           UINT64           allocationSize,
                                           DWORD            originatorPid,
                                           LM_FILE_HANDLE* outFile,
                                           LM_FILE_INFO*   outInfo)
{
    using namespace ::LayerMount::abi;

    if (handle       == nullptr) return E_HANDLE;
    if (outFile      == nullptr) return E_POINTER;
    if (outInfo      == nullptr) return E_POINTER;
    if (relativePath == nullptr) return E_INVALIDARG;
    if (securityDescriptor == nullptr && securityDescriptorBytes != 0) return E_INVALIDARG;
    // Bounded SD validation parity with LayerMountSetSecurity. The previous
    // path discarded securityDescriptorBytes entirely and trusted the
    // descriptor to be self-describing -- a malformed Owner/Group/Dacl/
    // Sacl offset could then cause downstream Win32 wrappers to read
    // past the caller's buffer. SD here is optional (CreateFile may
    // receive a NULL SD); only validate when one was supplied.
    if (securityDescriptor != nullptr && securityDescriptorBytes > 0) {
        if (!IsValidBoundedSecurityDescriptor(securityDescriptor,
                                              securityDescriptorBytes)) {
            return E_INVALIDARG;
        }
    }

    LM_ABI_BEGIN();

    const std::uint64_t encodedLayerMount =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encodedLayerMount);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    // PSECURITY_DESCRIPTOR is opaque void*; the caller's buffer points to
    // a self-relative SD that has been bounded-validated above (when
    // non-null). The engine's Create implementation may or may not consume
    // the size hint; passing it through keeps the call shape uniform.
    PSECURITY_DESCRIPTOR sd = const_cast<PSECURITY_DESCRIPTOR>(
        reinterpret_cast<const void*>(securityDescriptor));
    (void)securityDescriptorBytes;

    const DWORD pid = originatorPid != 0 ? originatorPid : ::GetCurrentProcessId();
    std::unique_ptr<::LayerMount::FileContext> ctx;
    ::LayerMount::InternalFileInfo internalInfo{};
    NTSTATUS status = mountHolder->core->Create(
        relativePath, createOptions, grantedAccess, fileAttributes,
        sd, allocationSize, pid,
        &ctx, &internalInfo);
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }

    // Allocate the slot before moving ctx so the failure path can still
    // close the NT handle via the engine. Same pattern as LayerMountOpenFile.
    auto fileHolder = std::make_shared<FileHolder>();
    fileHolder->parentOwner = mountHolder;
    fileHolder->mount     = mountHolder->core.get();

    const std::uint64_t encodedFile = Handles().file.Allocate(fileHolder);
    if (encodedFile == 0) {
        mountHolder->core->Close(ctx.get());
        ctx.reset();
        ErrorTls::Set(E_OUTOFMEMORY, L"LayerMountCreateFile: file handle table exhausted.");
        return E_OUTOFMEMORY;
    }

    fileHolder->ctx = std::move(ctx);
    mountHolder->childCount.fetch_add(1, std::memory_order_acq_rel);

    ToPublicFileInfo(internalInfo, *outInfo);
    *outFile = reinterpret_cast<LM_FILE_HANDLE>(static_cast<uintptr_t>(encodedFile));
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountReadFile(LM_FILE_HANDLE file,
                                         void*           buffer,
                                         UINT64          offset,
                                         UINT32          length,
                                         DWORD           originatorPid,
                                         UINT32*         bytesTransferred)
{
    using namespace ::LayerMount::abi;

    if (file              == nullptr) return E_HANDLE;
    if (bytesTransferred  == nullptr) return E_POINTER;
    if (buffer == nullptr && length != 0) return E_INVALIDARG;

    *bytesTransferred = 0;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(file));
    auto holder = Handles().file.Resolve(encoded);
    if (holder == nullptr || holder->mount == nullptr || holder->ctx == nullptr) {
        return E_HANDLE;
    }

    ULONG transferred = 0;
    NTSTATUS status = holder->mount->Read(holder->ctx.get(),
                                             buffer, offset, length,
                                             originatorPid, &transferred);
    *bytesTransferred = static_cast<UINT32>(transferred);
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountWriteFile(LM_FILE_HANDLE file,
                                          const void*     buffer,
                                          UINT64          offset,
                                          UINT32          length,
                                          BOOL            writeToEnd,
                                          BOOL            constrainedIo,
                                          DWORD           originatorPid,
                                          UINT32*         bytesTransferred,
                                          LM_FILE_INFO*  outInfo)
{
    using namespace ::LayerMount::abi;

    if (file              == nullptr) return E_HANDLE;
    if (bytesTransferred  == nullptr) return E_POINTER;
    if (buffer == nullptr && length != 0) return E_INVALIDARG;

    *bytesTransferred = 0;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(file));
    auto holder = Handles().file.Resolve(encoded);
    if (holder == nullptr || holder->mount == nullptr || holder->ctx == nullptr) {
        return E_HANDLE;
    }

    ULONG transferred = 0;
    ::LayerMount::InternalFileInfo internalInfo{};
    NTSTATUS status = holder->mount->Write(holder->ctx.get(),
                                              buffer, offset, length,
                                              writeToEnd != FALSE,
                                              constrainedIo != FALSE,
                                              originatorPid, &transferred,
                                              outInfo != nullptr ? &internalInfo : nullptr);
    *bytesTransferred = static_cast<UINT32>(transferred);
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }
    if (outInfo != nullptr) {
        ToPublicFileInfo(internalInfo, *outInfo);
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountOverwriteFile(LM_FILE_HANDLE file,
                                              UINT32          fileAttributes,
                                              BOOL            replaceAttributes,
                                              UINT64          allocationSize,
                                              DWORD           originatorPid,
                                              LM_FILE_INFO*  outInfo)
{
    using namespace ::LayerMount::abi;

    if (file == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(file));
    auto holder = Handles().file.Resolve(encoded);
    if (holder == nullptr || holder->mount == nullptr || holder->ctx == nullptr) {
        return E_HANDLE;
    }

    ::LayerMount::InternalFileInfo internalInfo{};
    NTSTATUS status = holder->mount->Overwrite(holder->ctx.get(),
        fileAttributes, replaceAttributes != FALSE, allocationSize,
        originatorPid,
        outInfo != nullptr ? &internalInfo : nullptr);
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }
    if (outInfo != nullptr) {
        ToPublicFileInfo(internalInfo, *outInfo);
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountFlushFile(LM_FILE_HANDLE file,
                                          DWORD           originatorPid,
                                          LM_FILE_INFO*  outInfo)
{
    using namespace ::LayerMount::abi;

    if (file == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(file));
    auto holder = Handles().file.Resolve(encoded);
    if (holder == nullptr || holder->mount == nullptr || holder->ctx == nullptr) {
        return E_HANDLE;
    }

    ::LayerMount::InternalFileInfo internalInfo{};
    NTSTATUS status = holder->mount->Flush(holder->ctx.get(),
        originatorPid,
        outInfo != nullptr ? &internalInfo : nullptr);
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }
    if (outInfo != nullptr) {
        ToPublicFileInfo(internalInfo, *outInfo);
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountGetFileInfo(LM_FILE_HANDLE file,
                                            LM_FILE_INFO*  outInfo)
{
    using namespace ::LayerMount::abi;

    if (file    == nullptr) return E_HANDLE;
    if (outInfo == nullptr) return E_POINTER;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(file));
    auto holder = Handles().file.Resolve(encoded);
    if (holder == nullptr || holder->ctx == nullptr) {
        return E_HANDLE;
    }
    if (holder->mount != nullptr && holder->ctx->ownerPid != 0) {
        if (auto tracker = holder->mount->Tracker()) {
            tracker->LogAccess(holder->ctx->ownerPid,
                holder->ctx->relativePath, ::LayerMount::OperationType::GetInfo);
        }
    }

    NTSTATUS ready = holder->mount->EnsureHandleReady(holder->ctx.get());
    if (!NT_SUCCESS(ready)) {
        return HresultFromNtStatus(ready);
    }

    ::LayerMount::InternalFileInfo internalInfo{};
    NTSTATUS status = ::LayerMount::LayerMount::FillFileInfoFromHandle(
        holder->ctx->handle, &internalInfo, &holder->ctx->actualPath);
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }
    ToPublicFileInfo(internalInfo, *outInfo);
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountSetFileInfo(LM_FILE_HANDLE file,
                                            UINT32          fileAttributes,
                                            UINT64          creationTime,
                                            UINT64          lastAccessTime,
                                            UINT64          lastWriteTime,
                                            UINT64          changeTime,
                                            UINT64          allocationSize,
                                            UINT64          fileSize,
                                            LM_FILE_INFO*  outInfo)
{
    using namespace ::LayerMount::abi;

    if (file == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(file));
    auto holder = Handles().file.Resolve(encoded);
    if (holder == nullptr || holder->mount == nullptr || holder->ctx == nullptr) {
        return E_HANDLE;
    }

    ::LayerMount::InternalFileInfo internalInfo{};
    NTSTATUS status = holder->mount->SetInfo(holder->ctx.get(),
        fileAttributes, creationTime, lastAccessTime, lastWriteTime,
        changeTime, allocationSize, fileSize,
        outInfo != nullptr ? &internalInfo : nullptr);
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }
    if (outInfo != nullptr) {
        ToPublicFileInfo(internalInfo, *outInfo);
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountGetReparsePoint(LM_HANDLE handle,
                                                PCWSTR     relativePath,
                                                BYTE*      buffer,
                                                SIZE_T     bufferBytes,
                                                SIZE_T*    requiredBytes)
{
    using namespace ::LayerMount::abi;

    if (handle       == nullptr) return E_HANDLE;
    if (relativePath == nullptr) return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    NTSTATUS status = mountHolder->core->GetReparsePoint(
        relativePath, buffer, bufferBytes, requiredBytes);
    if (status == STATUS_BUFFER_OVERFLOW) {
        return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    }
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountSetReparsePoint(LM_HANDLE  handle,
                                                PCWSTR      relativePath,
                                                const BYTE* buffer,
                                                SIZE_T      bufferBytes)
{
    using namespace ::LayerMount::abi;

    if (handle       == nullptr) return E_HANDLE;
    if (relativePath == nullptr) return E_INVALIDARG;
    if (buffer       == nullptr) return E_INVALIDARG;
    if (bufferBytes  == 0)       return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    NTSTATUS status = mountHolder->core->SetReparsePoint(
        relativePath, buffer, bufferBytes, ::GetCurrentProcessId());
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountDeleteReparsePoint(LM_HANDLE  handle,
                                                   PCWSTR      relativePath,
                                                   const BYTE* buffer,
                                                   SIZE_T      bufferBytes)
{
    using namespace ::LayerMount::abi;

    if (handle       == nullptr) return E_HANDLE;
    if (relativePath == nullptr) return E_INVALIDARG;
    if (buffer       == nullptr) return E_INVALIDARG;
    if (bufferBytes  == 0)       return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    NTSTATUS status = mountHolder->core->DeleteReparsePoint(
        relativePath, buffer, bufferBytes, ::GetCurrentProcessId());
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountMergeDirectory(LM_HANDLE             handle,
                                               PCWSTR                 dirRelativePath,
                                               LM_DIR_ENUM_CALLBACK  callback,
                                               void*                  userContext)
{
    using namespace ::LayerMount::abi;

    if (handle          == nullptr) return E_HANDLE;
    if (dirRelativePath == nullptr) return E_INVALIDARG;
    if (callback        == nullptr) return E_POINTER;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    auto entries = mountHolder->core->MergeDirectoryEntries(dirRelativePath);

    for (const auto& kv : entries) {
        const ::LayerMount::MergedEntry& entry = kv.second;

        LM_FILE_INFO info{};
        info.fileAttributes = entry.findData.dwFileAttributes;
        info.fileSize = (static_cast<UINT64>(entry.findData.nFileSizeHigh) << 32) |
                        entry.findData.nFileSizeLow;
        // 4 KiB rounding mirrors the legacy SReadDirectory shape; LM_FILE_INFO
        // doesn't carry a real allocation-size source from FindFirstFileW.
        info.allocationSize = (info.fileSize + 4095) & ~static_cast<UINT64>(4095);
        info.creationTime =
            (static_cast<UINT64>(entry.findData.ftCreationTime.dwHighDateTime) << 32) |
            entry.findData.ftCreationTime.dwLowDateTime;
        info.lastAccessTime =
            (static_cast<UINT64>(entry.findData.ftLastAccessTime.dwHighDateTime) << 32) |
            entry.findData.ftLastAccessTime.dwLowDateTime;
        info.lastWriteTime =
            (static_cast<UINT64>(entry.findData.ftLastWriteTime.dwHighDateTime) << 32) |
            entry.findData.ftLastWriteTime.dwLowDateTime;
        info.changeTime = info.lastWriteTime; // approximation; native ChangeTime
                                              // requires a per-entry handle open.
        if (entry.findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            info.reparseTag = entry.findData.dwReserved0;
        }

        HRESULT cbResult = callback(entry.findData.cFileName, &info, userContext);
        if (cbResult != S_OK) {
            return cbResult;
        }
    }

    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountGetSecurity(LM_HANDLE handle,
                                            PCWSTR     relativePath,
                                            UINT32*    outFileAttributes,
                                            BYTE*      securityDescriptor,
                                            SIZE_T     securityDescriptorBytes,
                                            SIZE_T*    requiredBytes)
{
    using namespace ::LayerMount::abi;

    if (handle       == nullptr) return E_HANDLE;
    if (relativePath == nullptr) return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    PSECURITY_DESCRIPTOR sd = reinterpret_cast<PSECURITY_DESCRIPTOR>(securityDescriptor);
    NTSTATUS status = mountHolder->core->GetSecurity(
        relativePath, reinterpret_cast<PUINT32>(outFileAttributes),
        sd, securityDescriptorBytes, requiredBytes);
    if (status == STATUS_BUFFER_OVERFLOW) {
        return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    }
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountSetSecurity(LM_HANDLE  handle,
                                            PCWSTR      relativePath,
                                            UINT32      securityInformation,
                                            const BYTE* modificationDescriptor,
                                            SIZE_T      modificationDescriptorBytes)
{
    using namespace ::LayerMount::abi;

    if (handle                 == nullptr) return E_HANDLE;
    if (relativePath           == nullptr) return E_INVALIDARG;
    if (modificationDescriptor == nullptr) return E_INVALIDARG;
    // A self-relative SECURITY_DESCRIPTOR carries Owner/Group/Dacl/Sacl as
    // offsets into the caller's buffer. Without validating the buffer extent
    // against those offsets, a malformed descriptor can cause the kernel
    // to read past the supplied byte span. Reject anything that is not a
    // structurally valid self-relative descriptor fitting within the hint.
    if (!IsValidBoundedSecurityDescriptor(modificationDescriptor,
                                          modificationDescriptorBytes)) {
        return E_INVALIDARG;
    }

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    PSECURITY_DESCRIPTOR sd = const_cast<PSECURITY_DESCRIPTOR>(
        reinterpret_cast<const void*>(modificationDescriptor));
    NTSTATUS status = mountHolder->core->SetSecurity(
        relativePath, securityInformation, sd, ::GetCurrentProcessId());
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountDeleteFile(LM_HANDLE handle, PCWSTR relativePath)
{
    using namespace ::LayerMount::abi;

    if (handle       == nullptr) return E_HANDLE;
    if (relativePath == nullptr) return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    NTSTATUS status = mountHolder->core->Delete(relativePath, ::GetCurrentProcessId());
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountCanDeleteFile(LM_HANDLE handle, PCWSTR relativePath)
{
    using namespace ::LayerMount::abi;

    if (handle       == nullptr) return E_HANDLE;
    if (relativePath == nullptr) return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    NTSTATUS status = mountHolder->core->CanDelete(
        relativePath, ::GetCurrentProcessId());
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountCanDeleteOpenFile(LM_FILE_HANDLE file)
{
    using namespace ::LayerMount::abi;

    if (file == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(file));
    auto holder = Handles().file.Resolve(encoded);
    if (holder == nullptr || holder->mount == nullptr || holder->ctx == nullptr) {
        return E_HANDLE;
    }

    NTSTATUS status = holder->mount->CanDelete(
        holder->ctx.get(), ::GetCurrentProcessId());
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountDeleteOpenFile(LM_FILE_HANDLE file)
{
    using namespace ::LayerMount::abi;

    if (file == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(file));
    auto holder = Handles().file.Resolve(encoded);
    if (holder == nullptr || holder->mount == nullptr || holder->ctx == nullptr) {
        return E_HANDLE;
    }

    NTSTATUS status = holder->mount->Delete(
        holder->ctx.get(), ::GetCurrentProcessId());
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountRenameFile(LM_HANDLE handle,
                                           PCWSTR     oldRelativePath,
                                           PCWSTR     newRelativePath,
                                           BOOL       replaceIfExists)
{
    using namespace ::LayerMount::abi;

    if (handle          == nullptr) return E_HANDLE;
    if (oldRelativePath == nullptr) return E_INVALIDARG;
    if (newRelativePath == nullptr) return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    NTSTATUS status = mountHolder->core->Rename(
        oldRelativePath, newRelativePath, replaceIfExists != FALSE,
        ::GetCurrentProcessId());
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountUpdateOpenFilePath(LM_FILE_HANDLE file,
                                                    PCWSTR          newRelativePath)
{
    using namespace ::LayerMount::abi;

    if (file            == nullptr) return E_HANDLE;
    if (newRelativePath == nullptr) return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(file));
    auto holder = Handles().file.Resolve(encoded);
    if (holder == nullptr || holder->mount == nullptr || holder->ctx == nullptr) {
        return E_HANDLE;
    }
    // UpdateContextPath rejects empty, drive-qualified, traversal, and
    // reserved-subtree paths. Surface the NTSTATUS so a buggy host cannot
    // rebind an open handle onto a path outside the overlay root.
    NTSTATUS status = holder->mount->UpdateContextPath(
        holder->ctx.get(), newRelativePath);
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountRenameOpenFile(LM_FILE_HANDLE file,
                                               PCWSTR          newRelativePath,
                                               BOOL            replaceIfExists)
{
    using namespace ::LayerMount::abi;

    if (file == nullptr) return E_HANDLE;
    if (newRelativePath == nullptr) return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(file));
    auto holder = Handles().file.Resolve(encoded);
    if (holder == nullptr || holder->mount == nullptr || holder->ctx == nullptr) {
        return E_HANDLE;
    }

    NTSTATUS status = holder->mount->Rename(
        holder->ctx.get(), newRelativePath, replaceIfExists != FALSE,
        ::GetCurrentProcessId());
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountCreateWhiteout(LM_HANDLE handle,
                                               PCWSTR     relativePath,
                                               BOOL       isDirectory)
{
    using namespace ::LayerMount::abi;

    if (handle       == nullptr) return E_HANDLE;
    if (relativePath == nullptr) return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    // Validate the relative path at the ABI boundary so `..`, drive/stream
    // qualifiers, and reserved subtrees cannot write marker files outside the
    // configured upper layer when WhiteoutManager concatenates with upperPath.
    const std::wstring normalized = ::LayerMount::NormalizePath(relativePath);
    if (!::LayerMount::IsSafeRelativePath(normalized) ||
        ::LayerMount::IsReservedRelativePath(normalized)) {
        return E_INVALIDARG;
    }

    bool ok = mountHolder->core->Whiteouts().CreateWhiteout(relativePath,
        isDirectory ? ::LayerMount::WhiteoutType::Directory
                    : ::LayerMount::WhiteoutType::File);
    return ok ? S_OK : HRESULT_FROM_WIN32(::GetLastError());

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountSetOpaque(LM_HANDLE handle, PCWSTR dirRelativePath)
{
    using namespace ::LayerMount::abi;

    if (handle          == nullptr) return E_HANDLE;
    if (dirRelativePath == nullptr) return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    // Same ABI-boundary validation as LayerMountCreateWhiteout — opaque marker
    // paths must not escape the overlay root or land in reserved subtrees.
    const std::wstring normalized = ::LayerMount::NormalizePath(dirRelativePath);
    if (!::LayerMount::IsSafeRelativePath(normalized) ||
        ::LayerMount::IsReservedRelativePath(normalized)) {
        return E_INVALIDARG;
    }

    bool ok = mountHolder->core->Whiteouts().SetOpaque(dirRelativePath);
    return ok ? S_OK : HRESULT_FROM_WIN32(::GetLastError());

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountCloseFile(LM_FILE_HANDLE file)
{
    using namespace ::LayerMount::abi;

    if (file == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(file));

    // Free returns the shared_ptr<FileHolder> that was in the slot; we
    // still need to call CloseFile on the engine to close the NT handle
    // and decrement the active-handles stat before the holder
    // destructor releases storage. Decrement the parent's child count
    // after the close so LayerMountDestroy can observe the live child
    // until its resources have been returned.
    auto holder = Handles().file.Free(encoded);
    if (holder == nullptr) {
        return E_HANDLE;
    }
    if (holder->mount != nullptr && holder->ctx != nullptr) {
        holder->mount->Close(holder->ctx.get());
    }
    if (holder->parentOwner) {
        holder->parentOwner->childCount.fetch_sub(1, std::memory_order_acq_rel);
    }
    return S_OK;

    LM_ABI_END();
}

} // extern "C"
