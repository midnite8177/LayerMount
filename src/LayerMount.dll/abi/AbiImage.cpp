// AbiImage.cpp -- Layer-image ABI entry points.
// LayerMountImagePack / Unpack / Validate / GetManifest / GetMetadata /
// Close. All shims route to the engine's lazily-constructed
// LayerImageManager (accessed via LayerMount::Images()).
//
// The layer-image subsystem is path-based -- LayerImageManager has no
// long-lived per-image state -- so the LM_IMAGE_HANDLE returned by
// LayerMountImagePack is essentially a receipt. ImageHolder carries the
// manager back-pointer and the image path so future work (per-image
// caches, streaming readers) can attach state without breaking the ABI.

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

// Shared DWORD -> HRESULT translation. Consistent with AbiVhd.cpp.
inline HRESULT HresultFromWin32Dword(DWORD code) noexcept {
    return code == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(code);
}

// Emit a wide string into a caller-provided two-call buffer. Matches the
// semantics used by the VSS ABI (NUL counted in required size).
inline HRESULT EmitWString(const std::wstring& src,
                           PWSTR  buffer,
                           SIZE_T bufferChars,
                           SIZE_T* bufferRequired) noexcept
{
    const SIZE_T requiredChars = src.size() + 1;
    if (bufferRequired != nullptr) {
        *bufferRequired = requiredChars;
    }
    if (buffer == nullptr || bufferChars == 0) {
        return S_OK;
    }
    if (bufferChars < requiredChars) {
        return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    }
    std::memcpy(buffer, src.c_str(), requiredChars * sizeof(wchar_t));
    return S_OK;
}

// Translate the engine's CompressionType to the public ABI enum.
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

// Populate a fixed-layout string field inside LM_IMAGE_METADATA per the
// two-call buffer pattern. Unlike EmitWString this does not return
// ERROR_MORE_DATA -- the caller learns about overflow by comparing
// *Required vs *Chars on a per-field basis. Simplifies the outer shim's
// error handling at the cost of per-field probing.
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

} // namespace

namespace {

// Fold the caller's LM_IMAGE_PACK_OPTIONS (nullable) into the
// engine-side LayerMetadata seed. structSize gate is forward-compat:
// older callers pass a smaller struct, newer fields stay at their
// default values.
void ApplyPackOptions(const LM_IMAGE_PACK_OPTIONS* options,
                      ::LayerMount::LayerImage::LayerMetadata& metadata) {
    if (options == nullptr) return;
    if (options->structSize < sizeof(LM_IMAGE_PACK_OPTIONS)) return;
    if (options->author != nullptr) {
        metadata.author = options->author;
    }
    if (options->description != nullptr) {
        metadata.description = options->description;
    }
}

// Allocate an LM_IMAGE_HANDLE from an ImageHolder seeded with the
// output path. Used by both Pack and PackDifferential.
HRESULT CompleteImagePack(::LayerMount::LayerImage::LayerImageManager& manager,
                          PCWSTR                                      outputPath,
                          LM_IMAGE_HANDLE*                           outImage,
                          const wchar_t*                              errorPrefix) {
    using namespace ::LayerMount::abi;
    if (outImage == nullptr) return S_OK;   // caller doesn't want a handle

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

} // namespace

extern "C" {

LM_API HRESULT LM_CALL LayerMountImagePack(LM_HANDLE                    mount,
                                          PCWSTR                        sourceDir,
                                          PCWSTR                        outputPath,
                                          INT32                         compressionLevel,
                                          const LM_IMAGE_PACK_OPTIONS* options,
                                          LM_IMAGE_HANDLE*             outImage)
{
    using namespace ::LayerMount::abi;

    if (mount    == nullptr) return E_HANDLE;
    if (sourceDir  == nullptr || *sourceDir  == L'\0') return E_INVALIDARG;
    if (outputPath == nullptr || *outputPath == L'\0') return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) return E_HANDLE;

    auto& manager = mountHolder->core->Images();

    ::LayerMount::LayerImage::LayerMetadata metadata;
    ApplyPackOptions(options, metadata);
    const DWORD dw = manager.CreateImage(sourceDir, outputPath, metadata,
                                         compressionLevel);
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
    using namespace ::LayerMount::abi;

    if (mount    == nullptr) return E_HANDLE;
    if (sourceDir  == nullptr || *sourceDir  == L'\0') return E_INVALIDARG;
    if (baseDir    == nullptr || *baseDir    == L'\0') return E_INVALIDARG;
    if (outputPath == nullptr || *outputPath == L'\0') return E_INVALIDARG;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) return E_HANDLE;

    auto& manager = mountHolder->core->Images();

    ::LayerMount::LayerImage::LayerMetadata metadata;
    ApplyPackOptions(options, metadata);
    const DWORD dw = manager.CreateDifferentialImage(
        sourceDir, baseDir, outputPath, metadata, compressionLevel);
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
    using namespace ::LayerMount::abi;

    if (mount    == nullptr) return E_HANDLE;
    if (outputPath == nullptr || *outputPath == L'\0') return E_INVALIDARG;
    if (imageCount > 0 && imagePaths == nullptr) return E_POINTER;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) return E_HANDLE;

    std::vector<std::wstring> images;
    images.reserve(imageCount);
    for (UINT32 i = 0; i < imageCount; ++i) {
        if (imagePaths[i] == nullptr) return E_INVALIDARG;
        images.emplace_back(imagePaths[i]);
    }

    const DWORD dw = ::LayerMount::LayerImage::LayerImageManager::CreateManifest(
        outputPath, images);
    return HresultFromWin32Dword(dw);

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountImageUnpack(LM_HANDLE mount,
                                            PCWSTR     imagePath,
                                            PCWSTR     targetDir,
                                            BOOL       verifyChecksum)
{
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
            imagePath, targetDir, verifyChecksum != FALSE));

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountImageValidate(LM_HANDLE mount,
                                              PCWSTR     imagePath)
{
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

    // Full validation: reads the header + metadata AND streams the
    // data section to verify the SHA-256 checksum matches header.
    // checksum. An earlier implementation called only GetImageInfo,
    // which passed header-only checks; tampering inside the compressed
    // payload went undetected until a later Unpack(verifyChecksum=true).
    return HresultFromWin32Dword(
        mountHolder->core->Images().ValidateImage(imagePath));

    LM_ABI_END();
}

LM_API HRESULT LM_CALL LayerMountImageGetManifest(LM_HANDLE          mount,
                                                 PCWSTR              imagePath,
                                                 LM_IMAGE_MANIFEST* manifest)
{
    using namespace ::LayerMount::abi;

    if (mount   == nullptr) return E_HANDLE;
    if (imagePath == nullptr || *imagePath == L'\0') return E_INVALIDARG;
    if (manifest  == nullptr) return E_POINTER;

    LM_ABI_BEGIN();

    const std::uint64_t encoded =
        static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(mount));
    auto mountHolder = Handles().mount.Resolve(encoded);
    if (mountHolder == nullptr) {
        return E_HANDLE;
    }
    (void)mountHolder; // LoadManifest is static; kept the handle resolve
                         // so bogus handles fail fast.

    // Despite the parameter name, the ABI's `imagePath` here is the path
    // to a LayerManifest JSON file (LayerImageManager::LoadManifest). The
    // parameter name predates the separation between per-image metadata
    // (LayerMountImageGetMetadata) and multi-image manifests (this
    // function).
    ::LayerMount::LayerImage::LayerManifest loaded;
    const DWORD dw =
        ::LayerMount::LayerImage::LayerImageManager::LoadManifest(imagePath, loaded);
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

        // checksumHex is a fixed-size WCHAR[65]; always copy safely.
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
        // imagePath is a two-call buffer the caller pre-sized; on short
        // buffer Chars is set to 0 and Required is populated so the
        // caller can re-query.
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
    const DWORD dw =
        mountHolder->core->Images().GetImageInfo(imagePath, header, loaded);
    if (dw != ERROR_SUCCESS) {
        return HresultFromWin32Dword(dw);
    }

    // Scalar fields -- always written, regardless of string-buffer state.
    metadata->compression      = ToPublicCompression(loaded.compression);
    metadata->fileCount        = loaded.fileCount;
    metadata->uncompressedSize = loaded.uncompressedSize;
    metadata->compressedSize   = loaded.compressedSize;

    // Per-string two-call-pattern fields. Tags, whiteouts, and labels
    // are intentionally not projected -- they'd require dedicated
    // enumeration APIs that are not yet in the .def (the header
    // documents this deliberate omission).
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

} // extern "C"
