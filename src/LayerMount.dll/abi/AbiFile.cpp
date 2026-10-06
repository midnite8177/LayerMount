#include "../public/LayerMount.h"
#include "AbiGuard.h"
#include "ErrorTls.h"
#include "FileHandleOpen.h"
#include "HandleTable.h"
#include "HandleTypes.h"
#include "../impl/LayerMount.h"
#include "../impl/NtStatusUtil.h"
#include "../impl/WhiteoutManager.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

using ::LayerMount::HresultFromNtStatus;

namespace {

std::uint64_t DecodeFileHandle(LM_FILE_HANDLE file) {
    return static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(file));
}

// Returns null when the slot is free, stale, or has no engine or context.
std::shared_ptr<::LayerMount::abi::FileHolder> ResolveFileHolder(LM_FILE_HANDLE file) {
    auto holder = ::LayerMount::abi::Handles().file.Resolve(DecodeFileHandle(file));
    if (holder == nullptr || holder->mount == nullptr || holder->ctx == nullptr) {
        return nullptr;
    }
    return holder;
}

HRESULT HresultFromMarkerWrite(NTSTATUS status, ::LayerMount::MarkerPath markerPath) {
    return markerPath == ::LayerMount::MarkerPath::Refused ? E_INVALIDARG
                                                           : HresultFromNtStatus(status);
}

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

constexpr DWORD  kAbsentPartOffset = 0;
constexpr size_t kMinSidHeader     = offsetof(SID, SubAuthority);
constexpr size_t kMinAclHeader     = sizeof(ACL);
static_assert(kMinSidHeader == 8);
static_assert(kMinAclHeader == 8);

bool PartHeaderFitsAt(SIZE_T bytes, DWORD off, size_t headerBytes) {
    return off < bytes && bytes - off >= headerBytes;
}

bool SidFitsAt(const BYTE* buf, SIZE_T bytes, DWORD off) {
    if (off == kAbsentPartOffset) return true;
    if (!PartHeaderFitsAt(bytes, off, kMinSidHeader)) return false;
    const BYTE   subAuthorityCount = buf[off + offsetof(SID, SubAuthorityCount)];
    const size_t sidBytes = kMinSidHeader + sizeof(DWORD) * subAuthorityCount;
    return bytes - off >= sidBytes;
}

bool AclFitsAt(const BYTE* buf, SIZE_T bytes, DWORD off) {
    if (off == kAbsentPartOffset) return true;
    if (!PartHeaderFitsAt(bytes, off, kMinAclHeader)) return false;
    WORD aclSize = 0;
    std::memcpy(&aclSize, buf + off + offsetof(ACL, AclSize), sizeof(aclSize));
    return aclSize >= kMinAclHeader && bytes - off >= aclSize;
}

// True when buf holds a self-relative descriptor and every part that it uses
// lies inside [buf, buf+bytes). IsValidSecurityDescriptor and the other Win32 security
// calls take no length and read each part in full, so a part that runs past
// the buffer makes them read past its end. They ignore a DACL or SACL offset
// whose SE_DACL_PRESENT or SE_SACL_PRESENT flag is clear.
inline bool IsValidBoundedSecurityDescriptor(const BYTE* buf, SIZE_T bytes) {
    if (buf == nullptr) return false;
    // Self-relative SECURITY_DESCRIPTORs carry the fixed 20-byte header
    // below followed by inline owner/group/dacl/sacl payload. The
    // documented SECURITY_DESCRIPTOR_MIN_LENGTH macro is sizeof() the
    // ABSOLUTE form (40 bytes on x64, because the absolute struct holds
    // raw pointers in place of those offsets) and must NOT be used to
    // bound a self-relative buffer -- it rejects valid 20-to-39-byte
    // descriptors that legitimately fit a header plus a single SID.
    constexpr SIZE_T kSelfRelativeHeaderBytes = sizeof(SECURITY_DESCRIPTOR_RELATIVE);
    static_assert(kSelfRelativeHeaderBytes == 20);
    if (bytes < kSelfRelativeHeaderBytes) return false;

    constexpr size_t kControlOffset = offsetof(SECURITY_DESCRIPTOR_RELATIVE, Control);
    constexpr size_t kControlBytes = sizeof(SECURITY_DESCRIPTOR_RELATIVE::Control);
    static_assert(kControlOffset == 2);
    static_assert(kControlBytes == sizeof(WORD));
    WORD control = 0;
    std::memcpy(&control, buf + kControlOffset, kControlBytes);
    if ((control & SE_SELF_RELATIVE) == 0) {
        return false;
    }

    auto readDword = [&](size_t offset) -> DWORD {
        DWORD v = 0;
        std::memcpy(&v, buf + offset, sizeof(DWORD));
        return v;
    };
    constexpr size_t kOwnerOffset = offsetof(SECURITY_DESCRIPTOR_RELATIVE, Owner);
    constexpr size_t kGroupOffset = offsetof(SECURITY_DESCRIPTOR_RELATIVE, Group);
    constexpr size_t kSaclOffset  = offsetof(SECURITY_DESCRIPTOR_RELATIVE, Sacl);
    constexpr size_t kDaclOffset  = offsetof(SECURITY_DESCRIPTOR_RELATIVE, Dacl);
    static_assert(kOwnerOffset == 4);
    static_assert(kGroupOffset == 8);
    static_assert(kSaclOffset == 12);
    static_assert(kDaclOffset == 16);
    const bool saclPresent = (control & SE_SACL_PRESENT) != 0;
    const bool daclPresent = (control & SE_DACL_PRESENT) != 0;
    if (!SidFitsAt(buf, bytes, readDword(kOwnerOffset))) return false;
    if (!SidFitsAt(buf, bytes, readDword(kGroupOffset))) return false;
    if (saclPresent && !AclFitsAt(buf, bytes, readDword(kSaclOffset))) return false;
    if (daclPresent && !AclFitsAt(buf, bytes, readDword(kDaclOffset))) return false;

    auto sd = reinterpret_cast<PSECURITY_DESCRIPTOR>(const_cast<BYTE*>(buf));
    if (!::IsValidSecurityDescriptor(sd)) return false;

    const DWORD sdLen = ::GetSecurityDescriptorLength(sd);
    if (sdLen == 0) return false;
    if (static_cast<SIZE_T>(sdLen) > bytes) return false;
    return true;
}

}

extern "C" {

LM_API HRESULT LM_CALL LayerMountOpenFile(LM_HANDLE       handle,
                                         PCWSTR           relativePath,
                                         UINT32           grantedAccess,
                                         UINT32           createOptions,
                                         DWORD            originatorPid,
                                         LM_FILE_HANDLE* outFile,
                                         LM_FILE_INFO*   outInfo)
{
    LM_ABI_ENTRY();
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

    OpenRequest request{};
    request.relativePath = relativePath;
    request.grantedAccess = grantedAccess;
    request.createOptions = createOptions;
    request.callerPid = originatorPid != 0 ? originatorPid : ::GetCurrentProcessId();
    ::LayerMount::InternalFileInfo internalInfo{};
    std::uint64_t encodedFile = 0;
    const HRESULT hr = OpenFileHandle(Handles().file, mountHolder, request,
                                      &encodedFile, &internalInfo);
    if (FAILED(hr)) {
        return hr;
    }

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
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (handle       == nullptr) return E_HANDLE;
    if (outFile      == nullptr) return E_POINTER;
    if (outInfo      == nullptr) return E_POINTER;
    if (relativePath == nullptr) return E_INVALIDARG;
    if (securityDescriptor == nullptr && securityDescriptorBytes != 0) return E_INVALIDARG;
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

    PSECURITY_DESCRIPTOR sd = const_cast<PSECURITY_DESCRIPTOR>(
        reinterpret_cast<const void*>(securityDescriptor));
    (void)securityDescriptorBytes;

    const DWORD pid = originatorPid != 0 ? originatorPid : ::GetCurrentProcessId();
    ::LayerMount::InternalFileInfo internalInfo{};
    ::LayerMount::LayerMount::CreateRequest request{};
    request.relativePath = relativePath;
    request.createOptions = createOptions;
    request.grantedAccess = grantedAccess;
    request.fileAttributes = fileAttributes;
    request.securityDescriptor = sd;
    request.allocationSize = allocationSize;
    request.callerPid = pid;
    std::uint64_t encodedFile = 0;
    const HRESULT hr = CreateFileHandle(Handles().file, mountHolder, request,
                                        &encodedFile, &internalInfo);
    if (FAILED(hr)) {
        return hr;
    }

    ToPublicFileInfo(internalInfo, *outInfo);
    *outFile = reinterpret_cast<LM_FILE_HANDLE>(static_cast<uintptr_t>(encodedFile));
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountReadFile(LM_FILE_HANDLE file,
                                         void*           buffer,
                                         UINT64          offset,
                                         UINT32          length,
                                         UINT32*         bytesTransferred)
{
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (file              == nullptr) return E_HANDLE;
    if (bytesTransferred  == nullptr) return E_POINTER;
    if (buffer == nullptr && length != 0) return E_INVALIDARG;

    *bytesTransferred = 0;

    LM_ABI_BEGIN();

    auto holder = ResolveFileHolder(file);
    if (holder == nullptr) {
        return E_HANDLE;
    }

    ULONG transferred = 0;
    NTSTATUS status = holder->mount->Read(holder->ctx.get(),
                                             buffer, offset, length,
                                             &transferred);
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
                                          UINT32*         bytesTransferred,
                                          LM_FILE_INFO*  outInfo)
{
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (file              == nullptr) return E_HANDLE;
    if (bytesTransferred  == nullptr) return E_POINTER;
    if (buffer == nullptr && length != 0) return E_INVALIDARG;

    *bytesTransferred = 0;

    LM_ABI_BEGIN();

    auto holder = ResolveFileHolder(file);
    if (holder == nullptr) {
        return E_HANDLE;
    }

    ULONG transferred = 0;
    ::LayerMount::InternalFileInfo internalInfo{};
    NTSTATUS status = holder->mount->Write(holder->ctx.get(),
                                              buffer, offset, length,
                                              writeToEnd != FALSE,
                                              constrainedIo != FALSE,
                                              &transferred,
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
                                              LM_FILE_INFO*  outInfo)
{
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (file == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    auto holder = ResolveFileHolder(file);
    if (holder == nullptr) {
        return E_HANDLE;
    }

    ::LayerMount::InternalFileInfo internalInfo{};
    NTSTATUS status = holder->mount->Overwrite(holder->ctx.get(),
        fileAttributes, replaceAttributes != FALSE, allocationSize,
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
                                          LM_FILE_INFO*  outInfo)
{
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (file == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    auto holder = ResolveFileHolder(file);
    if (holder == nullptr) {
        return E_HANDLE;
    }

    ::LayerMount::InternalFileInfo internalInfo{};
    NTSTATUS status = holder->mount->Flush(holder->ctx.get(),
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
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (file    == nullptr) return E_HANDLE;
    if (outInfo == nullptr) return E_POINTER;

    LM_ABI_BEGIN();

    auto holder = ResolveFileHolder(file);
    if (holder == nullptr) {
        return E_HANDLE;
    }
    if (holder->ctx->ownerPid != 0) {
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
    NTSTATUS status = holder->mount->FillFileInfoFromHandle(
        holder->ctx->handle, &internalInfo, &holder->ctx->actualPath,
        holder->ctx->entryIsReparsePoint);
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
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (file == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    auto holder = ResolveFileHolder(file);
    if (holder == nullptr) {
        return E_HANDLE;
    }

    const ::LayerMount::SetInfoRequest request{
        fileAttributes, creationTime, lastAccessTime, lastWriteTime,
        changeTime, allocationSize, fileSize};
    ::LayerMount::InternalFileInfo internalInfo{};
    NTSTATUS status = holder->mount->SetInfo(holder->ctx.get(), request,
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
    LM_ABI_ENTRY();
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
    LM_ABI_ENTRY();
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
    LM_ABI_ENTRY();
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

LM_API HRESULT LM_CALL LayerMountEnumerateStreams(LM_HANDLE        handle,
                                                  PCWSTR           relativePath,
                                                  LM_STREAM_INFO* outBuffer,
                                                  UINT32           bufferCapacity,
                                                  UINT32*          outCount)
{
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (handle       == nullptr) return E_HANDLE;
    if (relativePath == nullptr) return E_INVALIDARG;
    if (outCount     == nullptr) return E_POINTER;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    std::vector<::LayerMount::InternalStreamInfo> streams;
    NTSTATUS status = mountHolder->core->EnumerateStreams(relativePath, streams);
    if (!NT_SUCCESS(status)) {
        *outCount = 0;
        return HresultFromNtStatus(status);
    }

    const UINT32 required = static_cast<UINT32>(streams.size());
    *outCount = required;

    if (outBuffer == nullptr) {
        return S_OK;
    }

    if (bufferCapacity < required) {
        return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    }

    for (UINT32 i = 0; i < required; ++i) {
        LM_STREAM_INFO& dst = outBuffer[i];
        const ::LayerMount::InternalStreamInfo& src = streams[i];

        std::memset(dst.streamName, 0, sizeof(dst.streamName));
        const size_t capacity   = LM_STREAM_NAME_MAX - 1;
        const size_t copyChars  = src.name.size() < capacity
                                      ? src.name.size()
                                      : capacity;
        std::memcpy(dst.streamName, src.name.data(),
                    copyChars * sizeof(WCHAR));
        dst.streamName[copyChars] = L'\0';
        dst.streamSize      = src.streamSize;
        dst.allocationSize  = src.allocationSize;
    }

    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountMergeDirectory(LM_HANDLE             handle,
                                               PCWSTR                 dirRelativePath,
                                               LM_DIR_ENUM_CALLBACK  callback,
                                               void*                  userContext)
{
    LM_ABI_ENTRY();
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

    const ::LayerMount::MergedDirectory merged =
        mountHolder->core->MergeDirectoryEntries(dirRelativePath);
    if (!NT_SUCCESS(merged.status)) {
        return HresultFromNtStatus(merged.status);
    }

    for (const auto& kv : merged.entries) {
        const ::LayerMount::MergedEntry& entry = kv.second;

        LM_FILE_INFO info{};
        info.fileAttributes = entry.findData.dwFileAttributes;
        info.fileSize = ::LayerMount::ComposeUInt64(entry.findData.nFileSizeHigh,
                                                    entry.findData.nFileSizeLow);
        info.allocationSize = ::LayerMount::AllocationSizeFor(info.fileSize);
        info.creationTime   = ::LayerMount::ComposeUInt64(
            entry.findData.ftCreationTime.dwHighDateTime,
            entry.findData.ftCreationTime.dwLowDateTime);
        info.lastAccessTime = ::LayerMount::ComposeUInt64(
            entry.findData.ftLastAccessTime.dwHighDateTime,
            entry.findData.ftLastAccessTime.dwLowDateTime);
        info.lastWriteTime  = ::LayerMount::ComposeUInt64(
            entry.findData.ftLastWriteTime.dwHighDateTime,
            entry.findData.ftLastWriteTime.dwLowDateTime);
        // WIN32_FIND_DATAW has no change time.
        info.changeTime = info.lastWriteTime;
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
                                            UINT32     securityInformation,
                                            UINT32*    outFileAttributes,
                                            BYTE*      securityDescriptor,
                                            SIZE_T     securityDescriptorBytes,
                                            SIZE_T*    requiredBytes)
{
    LM_ABI_ENTRY();
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
        relativePath, securityInformation,
        reinterpret_cast<PUINT32>(outFileAttributes),
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
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (handle                 == nullptr) return E_HANDLE;
    if (relativePath           == nullptr) return E_INVALIDARG;
    if (modificationDescriptor == nullptr) return E_INVALIDARG;
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
    LM_ABI_ENTRY();
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
    LM_ABI_ENTRY();
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
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (file == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    auto holder = ResolveFileHolder(file);
    if (holder == nullptr) {
        return E_HANDLE;
    }

    NTSTATUS status = holder->mount->CanDelete(holder->ctx.get());
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountDeleteOpenFile(LM_FILE_HANDLE file)
{
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (file == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    auto holder = ResolveFileHolder(file);
    if (holder == nullptr) {
        return E_HANDLE;
    }

    NTSTATUS status = holder->mount->Delete(holder->ctx.get());
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
    LM_ABI_ENTRY();
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
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (file            == nullptr) return E_HANDLE;
    if (newRelativePath == nullptr) return E_INVALIDARG;

    LM_ABI_BEGIN();

    auto holder = ResolveFileHolder(file);
    if (holder == nullptr) {
        return E_HANDLE;
    }
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
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (file == nullptr) return E_HANDLE;
    if (newRelativePath == nullptr) return E_INVALIDARG;

    LM_ABI_BEGIN();

    auto holder = ResolveFileHolder(file);
    if (holder == nullptr) {
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
    LM_ABI_ENTRY();
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

    ::LayerMount::MarkerPath markerPath = ::LayerMount::MarkerPath::Accepted;
    const NTSTATUS status = mountHolder->core->CreateWhiteout(relativePath,
        isDirectory ? ::LayerMount::WhiteoutType::Directory
                    : ::LayerMount::WhiteoutType::File,
        &markerPath);
    return HresultFromMarkerWrite(status, markerPath);

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountSetOpaque(LM_HANDLE handle, PCWSTR dirRelativePath)
{
    LM_ABI_ENTRY();
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

    ::LayerMount::MarkerPath markerPath = ::LayerMount::MarkerPath::Accepted;
    const NTSTATUS status = mountHolder->core->SetOpaque(dirRelativePath, &markerPath);
    return HresultFromMarkerWrite(status, markerPath);

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountCloseFile(LM_FILE_HANDLE file)
{
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (file == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    auto holder = Handles().file.Free(DecodeFileHandle(file));
    if (holder == nullptr) {
        return E_HANDLE;
    }
    if (holder->mount != nullptr && holder->ctx != nullptr) {
        holder->mount->Close(holder->ctx.get());
    }
    // After Close, so LayerMountDestroy sees the live child until Close returns its resources.
    if (holder->parentOwner) {
        holder->parentOwner->childCount.fetch_sub(1, std::memory_order_acq_rel);
    }
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountCleanupFile(LM_FILE_HANDLE file)
{
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (file == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    auto holder = ResolveFileHolder(file);
    if (holder == nullptr) {
        return E_HANDLE;
    }

    NTSTATUS status = holder->mount->Cleanup(holder->ctx.get());
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }
    return S_OK;

    LM_ABI_END();
}

}
