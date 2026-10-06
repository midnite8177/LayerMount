#include "FileHandleOpen.h"
#include "ErrorTls.h"
#include "../impl/NtStatusUtil.h"

#include <utility>

namespace LayerMount::abi {

namespace {

template <typename EngineCall>
HRESULT ReserveThenRun(FileTable& files,
                       const std::shared_ptr<LayerMountHolder>& mountHolder,
                       PCWSTR exhaustedMessage,
                       EngineCall engineCall,
                       std::uint64_t* outEncoded)
{
    auto fileHolder = std::make_shared<FileHolder>();
    fileHolder->parentOwner = mountHolder;
    fileHolder->mount = mountHolder->core.get();

    const std::uint64_t reserved = files.Reserve();
    if (reserved == 0) {
        ErrorTls::Set(E_OUTOFMEMORY, exhaustedMessage);
        return E_OUTOFMEMORY;
    }

    std::unique_ptr<::LayerMount::FileContext> ctx;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    try {
        status = engineCall(&ctx);
    } catch (...) {
        files.Release(reserved);
        throw;
    }
    if (!NT_SUCCESS(status)) {
        files.Release(reserved);
        return ::LayerMount::HresultFromNtStatus(status);
    }
    if (ctx == nullptr) {
        files.Release(reserved);
        return E_UNEXPECTED;
    }

    fileHolder->ctx = std::move(ctx);
    mountHolder->childCount.fetch_add(1, std::memory_order_acq_rel);
    files.Install(reserved, std::move(fileHolder));
    *outEncoded = reserved;
    return S_OK;
}

}

HRESULT CreateFileHandle(FileTable& files,
                         const std::shared_ptr<LayerMountHolder>& mountHolder,
                         const ::LayerMount::LayerMount::CreateRequest& request,
                         std::uint64_t* outEncoded,
                         ::LayerMount::InternalFileInfo* outInfo)
{
    return ReserveThenRun(
        files, mountHolder, L"LayerMountCreateFile: file handle table exhausted.",
        [&](std::unique_ptr<::LayerMount::FileContext>* ctx) {
            return mountHolder->core->Create(request, ctx, outInfo);
        },
        outEncoded);
}

HRESULT OpenFileHandle(FileTable& files,
                       const std::shared_ptr<LayerMountHolder>& mountHolder,
                       const OpenRequest& request,
                       std::uint64_t* outEncoded,
                       ::LayerMount::InternalFileInfo* outInfo)
{
    return ReserveThenRun(
        files, mountHolder, L"LayerMountOpenFile: file handle table exhausted.",
        [&](std::unique_ptr<::LayerMount::FileContext>* ctx) {
            return mountHolder->core->Open(request.relativePath, request.grantedAccess,
                                           request.createOptions, request.callerPid,
                                           ctx, outInfo);
        },
        outEncoded);
}

}
