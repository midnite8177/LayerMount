// AbiPath.cpp -- Path / volume ABI entry points (FR-18, FR-21, FR-22).
// LayerMountResolvePath, LayerMountGetVolumeInfo, LayerMountEnsureInUpperLayer.

#include "../public/LayerMount.h"
#include "AbiGuard.h"
#include "ErrorTls.h"
#include "HandleTable.h"
#include "HandleTypes.h"
#include "../impl/LayerMount.h"
#include "../impl/PathResolver.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace {

inline LM_LAYER_SOURCE ToPublicLayerSource(::LayerMount::LayerSource src) {
    switch (src) {
        case ::LayerMount::LayerSource::Upper: return LM_LAYER_UPPER;
        case ::LayerMount::LayerSource::Lower: return LM_LAYER_LOWER;
        default:                              return LM_LAYER_NONE;
    }
}

inline HRESULT HresultFromNtStatus(NTSTATUS status) {
    if (status == STATUS_SUCCESS) return S_OK;
    return HRESULT_FROM_NT(status);
}

} // namespace

extern "C" {

LM_API HRESULT LM_CALL LayerMountResolvePath(LM_HANDLE         handle,
                                            PCWSTR             relativePath,
                                            LM_RESOLVED_PATH* outResolved)
{
    using namespace ::LayerMount::abi;

    if (handle       == nullptr) return E_HANDLE;
    if (relativePath == nullptr) return E_INVALIDARG;
    if (outResolved  == nullptr) return E_POINTER;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    ::LayerMount::ResolvedPath resolved =
        mountHolder->core->Resolver().ResolvePath(relativePath);

    outResolved->source     = ToPublicLayerSource(resolved.source);
    outResolved->lowerIndex = (resolved.source == ::LayerMount::LayerSource::Lower)
                                  ? resolved.lowerIndex : -1;
    outResolved->isWhiteout = resolved.isWhiteout ? TRUE : FALSE;
    outResolved->attributes = resolved.attributes;

    // Two-call buffer pattern for absolutePath (NUL terminator counted).
    const SIZE_T requiredChars = resolved.absolutePath.size() + 1;
    outResolved->absolutePathRequired = requiredChars;

    if (outResolved->absolutePath == nullptr || outResolved->absolutePathChars == 0) {
        outResolved->absolutePathChars = 0;
        return S_OK;
    }
    if (outResolved->absolutePathChars < requiredChars) {
        outResolved->absolutePathChars = 0;
        return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    }
    std::memcpy(outResolved->absolutePath,
                resolved.absolutePath.c_str(),
                requiredChars * sizeof(wchar_t));
    outResolved->absolutePathChars = requiredChars;
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountGetVolumeInfo(LM_HANDLE       handle,
                                              LM_VOLUME_INFO* outInfo)
{
    using namespace ::LayerMount::abi;

    if (handle  == nullptr) return E_HANDLE;
    if (outInfo == nullptr) return E_POINTER;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(handle));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    NTSTATUS status = mountHolder->core->GetVolumeInfo(
        &outInfo->totalSize, &outInfo->freeSize);
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }

    // Volume label is currently a DLL-side constant. The LM_VOLUME_INFO
    // contract reports volumeLabelLength in BYTES.
    static constexpr wchar_t kLabel[] = L"LayerMount";
    constexpr SIZE_T kLabelChars = (sizeof(kLabel) / sizeof(wchar_t)) - 1; // exclude NUL
    constexpr SIZE_T kLabelCap = sizeof(outInfo->volumeLabel) / sizeof(wchar_t);
    static_assert(kLabelChars + 1 <= kLabelCap, "volume label too long for LM_VOLUME_INFO");

    std::memcpy(outInfo->volumeLabel, kLabel, sizeof(kLabel));
    outInfo->volumeLabelLength = static_cast<UINT32>(kLabelChars * sizeof(wchar_t));
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountEnsureInUpperLayer(LM_HANDLE handle,
                                                   PCWSTR     relativePath)
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

    NTSTATUS status = mountHolder->core->EnsureInUpperLayer(relativePath);
    if (!NT_SUCCESS(status)) {
        return HresultFromNtStatus(status);
    }
    return S_OK;

    LM_ABI_END();
}

} // extern "C"
