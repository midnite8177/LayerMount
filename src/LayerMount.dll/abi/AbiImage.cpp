#include "../public/LayerMount.h"
#include "AbiGuard.h"
#include "ErrorTls.h"
#include "HandleTable.h"
#include "HandleTypes.h"
#include "../impl/LayerMount.h"
#include "../impl/image/LayerImageManager.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace {

inline HRESULT HresultFromWin32Dword(DWORD code) noexcept {
    return code == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(code);
}

inline LM_COMPRESSION_TYPE ToPublicCompression(
    ::LayerMount::LayerImage::CompressionType ct) noexcept
{
    switch (ct) {
        case ::LayerMount::LayerImage::CompressionType::Zstd:
            return LM_COMPRESSION_ZSTD;
        case ::LayerMount::LayerImage::CompressionType::None:
        default:
            return LM_COMPRESSION_NONE;
    }
}

// Fills a caller-owned string field of LM_IMAGE_METADATA or
// LM_IMAGE_MANIFEST_ENTRY per the two-call buffer pattern. A null or short
// buffer does not fail the call. The helper sets *Chars to 0 and sets
// *Required, and the caller compares the two per field.
inline bool FillCallerWString(const std::wstring& src,
                              PWSTR*  bufferRef,
                              SIZE_T* charsRef,
                              SIZE_T* requiredRef) noexcept
{
    const SIZE_T requiredChars = src.size() + 1;
    if (requiredRef != nullptr) {
        *requiredRef = requiredChars;
    }
    if (bufferRef == nullptr || *bufferRef == nullptr) {
        if (charsRef != nullptr) *charsRef = 0;
        return false;
    }
    if (charsRef == nullptr || *charsRef < requiredChars) {
        if (charsRef != nullptr) *charsRef = 0;
        return false;
    }
    std::memcpy(*bufferRef, src.c_str(), requiredChars * sizeof(wchar_t));
    *charsRef = requiredChars;
    return true;
}

inline HRESULT ValidateMountHandle(std::uint64_t encoded) noexcept {
    return ::LayerMount::abi::Handles().mount.Resolve(encoded) != nullptr
               ? S_OK
               : E_HANDLE;
}

}

namespace {

HRESULT ReadPackOptions(const LM_IMAGE_PACK_OPTIONS* options,
                        ::LayerMount::LayerImage::LayerMetadata& metadata) {
    if (options == nullptr) return S_OK;
    if (!LM_STRUCT_SIZE_COVERS(options, LM_IMAGE_PACK_OPTIONS, description)) return E_INVALIDARG;
    if (options->author != nullptr) {
        metadata.author = options->author;
    }
    if (options->description != nullptr) {
        metadata.description = options->description;
    }
    return S_OK;
}

HRESULT CompleteImagePack(::LayerMount::LayerImage::LayerImageManager& manager,
                          PCWSTR                                      outputPath,
                          LM_IMAGE_HANDLE*                           outImage,
                          const wchar_t*                              errorPrefix) {
    using namespace ::LayerMount::abi;
    if (outImage == nullptr) return S_OK;

    auto holder = std::make_unique<ImageHolder>();
    holder->manager   = &manager;
    holder->imagePath = outputPath;

    const std::uint64_t encodedImg = Handles().image.Allocate(std::move(holder));
    if (encodedImg == 0) {
        std::wstring msg = errorPrefix;
        msg += L": image handle table exhausted.";
        ErrorTls::Set(E_OUTOFMEMORY, msg.c_str());
        return E_OUTOFMEMORY;
    }
    *outImage = reinterpret_cast<LM_IMAGE_HANDLE>(
        static_cast<uintptr_t>(encodedImg));
    return S_OK;
}

}

extern "C" {

LM_API HRESULT LM_CALL LayerMountImagePack(LM_HANDLE                    mount,
                                          PCWSTR                        sourceDir,
                                          PCWSTR                        outputPath,
                                          INT32                         compressionLevel,
                                          const LM_IMAGE_PACK_OPTIONS* options,
                                          LM_IMAGE_HANDLE*             outImage)
{
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (mount    == nullptr) return E_HANDLE;
    if (sourceDir  == nullptr || *sourceDir  == L'\0') return E_INVALIDARG;
    if (outputPath == nullptr || *outputPath == L'\0') return E_INVALIDARG;

    LM_ABI_BEGIN();

    ::LayerMount::LayerImage::LayerMetadata metadata;
    const HRESULT optionsHr = ReadPackOptions(options, metadata);
    if (FAILED(optionsHr)) return optionsHr;

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) return E_HANDLE;

    auto& manager = mountHolder->core->Images();

    const ::LayerMount::LayerImage::ImageOutput output{
        ::LayerMount::HostPath(outputPath), compressionLevel};
    const DWORD dw = manager.CreateImage(::LayerMount::HostPath(sourceDir), output, metadata);
    if (dw != ERROR_SUCCESS) return HresultFromWin32Dword(dw);

    return CompleteImagePack(manager, outputPath, outImage, L"LayerMountImagePack");

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountImagePackDifferential(
    LM_HANDLE                    mount,
    PCWSTR                        sourceDir,
    PCWSTR                        baseDir,
    PCWSTR                        outputPath,
    INT32                         compressionLevel,
    const LM_IMAGE_PACK_OPTIONS* options,
    LM_IMAGE_HANDLE*             outImage)
{
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (mount    == nullptr) return E_HANDLE;
    if (sourceDir  == nullptr || *sourceDir  == L'\0') return E_INVALIDARG;
    if (baseDir    == nullptr || *baseDir    == L'\0') return E_INVALIDARG;
    if (outputPath == nullptr || *outputPath == L'\0') return E_INVALIDARG;

    LM_ABI_BEGIN();

    ::LayerMount::LayerImage::LayerMetadata metadata;
    const HRESULT optionsHr = ReadPackOptions(options, metadata);
    if (FAILED(optionsHr)) return optionsHr;

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) return E_HANDLE;

    auto& manager = mountHolder->core->Images();

    const ::LayerMount::LayerImage::ImageOutput output{
        ::LayerMount::HostPath(outputPath), compressionLevel};
    const DWORD dw = manager.CreateDifferentialImage(
        ::LayerMount::HostPath(sourceDir), ::LayerMount::HostPath(baseDir), output, metadata);
    if (dw != ERROR_SUCCESS) return HresultFromWin32Dword(dw);

    return CompleteImagePack(manager, outputPath, outImage,
                             L"LayerMountImagePackDifferential");

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountImageCreateManifest(LM_HANDLE    mount,
                                                     PCWSTR        outputPath,
                                                     const PCWSTR* imagePaths,
                                                     UINT32        imageCount)
{
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (mount    == nullptr) return E_HANDLE;
    if (outputPath == nullptr || *outputPath == L'\0') return E_INVALIDARG;
    if (imageCount > 0 && imagePaths == nullptr) return E_POINTER;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) return E_HANDLE;

    std::vector<::LayerMount::HostPath> images;
    images.reserve(imageCount);
    for (UINT32 i = 0; i < imageCount; ++i) {
        if (imagePaths[i] == nullptr) return E_INVALIDARG;
        images.emplace_back(imagePaths[i]);
    }

    const DWORD dw = ::LayerMount::LayerImage::LayerImageManager::CreateManifest(
        ::LayerMount::HostPath(outputPath), images);
    return HresultFromWin32Dword(dw);

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountImageUnpack(LM_HANDLE mount,
                                            PCWSTR     imagePath,
                                            PCWSTR     targetDir,
                                            BOOL       verifyChecksum)
{
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (mount   == nullptr) return E_HANDLE;
    if (imagePath == nullptr || *imagePath == L'\0') return E_INVALIDARG;
    if (targetDir == nullptr || *targetDir == L'\0') return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    return HresultFromWin32Dword(
        mountHolder->core->Images().ExtractImage(
            ::LayerMount::HostPath(imagePath), ::LayerMount::HostPath(targetDir),
            verifyChecksum != FALSE));

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountImageValidate(LM_HANDLE mount,
                                              PCWSTR     imagePath)
{
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (mount   == nullptr) return E_HANDLE;
    if (imagePath == nullptr || *imagePath == L'\0') return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    return HresultFromWin32Dword(
        mountHolder->core->Images().ValidateImage(::LayerMount::HostPath(imagePath)));

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountImageGetManifest(LM_HANDLE          mount,
                                                 PCWSTR              manifestPath,
                                                 LM_IMAGE_MANIFEST* manifest)
{
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (mount   == nullptr) return E_HANDLE;
    if (manifestPath == nullptr || *manifestPath == L'\0') return E_INVALIDARG;
    if (manifest  == nullptr) return E_POINTER;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    const HRESULT validateHr = ValidateMountHandle(encoded);
    if (FAILED(validateHr)) {
        return validateHr;
    }

    ::LayerMount::LayerImage::LayerManifest loaded;
    const DWORD dw =
        ::LayerMount::LayerImage::LayerImageManager::LoadManifest(
            ::LayerMount::HostPath(manifestPath), loaded);
    if (dw != ERROR_SUCCESS) {
        return HresultFromWin32Dword(dw);
    }

    manifest->schemaVersion  = loaded.schemaVersion;
    manifest->entriesRequired = loaded.layers.size();

    const UINT32 capacity = manifest->entryCount;
    if (manifest->entries == nullptr || capacity == 0) {
        manifest->entryCount = 0;
        return S_OK;
    }
    if (capacity < loaded.layers.size()) {
        manifest->entryCount = 0;
        return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    }

    for (SIZE_T i = 0; i < loaded.layers.size(); ++i) {
        LM_IMAGE_MANIFEST_ENTRY& dst = manifest->entries[i];
        const auto&               src = loaded.layers[i];

        const SIZE_T hexCapChars = sizeof(dst.checksumHex) / sizeof(dst.checksumHex[0]);
        const SIZE_T hexNeeded   = src.checksumHex.size() + 1;
        if (hexNeeded <= hexCapChars) {
            std::memcpy(dst.checksumHex, src.checksumHex.c_str(),
                        hexNeeded * sizeof(wchar_t));
        } else {
            std::memcpy(dst.checksumHex, src.checksumHex.c_str(),
                        (hexCapChars - 1) * sizeof(wchar_t));
            dst.checksumHex[hexCapChars - 1] = L'\0';
        }
        FillCallerWString(src.imagePath,
                          &dst.imagePath, &dst.imagePathChars, &dst.imagePathRequired);
    }
    manifest->entryCount = static_cast<UINT32>(loaded.layers.size());
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountImageGetMetadata(LM_HANDLE          mount,
                                                 PCWSTR              imagePath,
                                                 LM_IMAGE_METADATA* metadata)
{
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (mount   == nullptr) return E_HANDLE;
    if (imagePath == nullptr || *imagePath == L'\0') return E_INVALIDARG;
    if (metadata  == nullptr) return E_POINTER;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }

    ::LayerMount::LayerImage::LayerImageHeader header;
    ::LayerMount::LayerImage::LayerMetadata    loaded;
    const DWORD dw = mountHolder->core->Images().GetImageInfo(
        ::LayerMount::HostPath(imagePath), header, loaded);
    if (dw != ERROR_SUCCESS) {
        return HresultFromWin32Dword(dw);
    }

    metadata->compression      = ToPublicCompression(loaded.compression);
    metadata->fileCount        = loaded.fileCount;
    metadata->uncompressedSize = loaded.uncompressedSize;
    metadata->compressedSize   = loaded.compressedSize;

    FillCallerWString(loaded.id,
                      &metadata->id, &metadata->idChars, &metadata->idRequired);
    FillCallerWString(loaded.parentId,
                      &metadata->parentId, &metadata->parentIdChars,
                      &metadata->parentIdRequired);
    FillCallerWString(loaded.createdAt,
                      &metadata->createdAt, &metadata->createdAtChars,
                      &metadata->createdAtRequired);
    FillCallerWString(loaded.author,
                      &metadata->author, &metadata->authorChars,
                      &metadata->authorRequired);
    FillCallerWString(loaded.description,
                      &metadata->description, &metadata->descriptionChars,
                      &metadata->descriptionRequired);
    return S_OK;

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountImageClose(LM_IMAGE_HANDLE image)
{
    LM_ABI_ENTRY();
    using namespace ::LayerMount::abi;

    if (image == nullptr) return E_HANDLE;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(image));
    auto freed = Handles().image.Free(encoded);
    if (!freed) {
        return E_HANDLE;
    }
    return S_OK;

    LM_ABI_END();
}

}
