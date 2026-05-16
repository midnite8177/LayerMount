#pragma once

#include "LayerImageFormat.h"

#include <windows.h>
#include <string>
#include <vector>

namespace LayerMount::LayerImage {

// ===========================================================================
// LayerImageManager — binary layer image create/extract/inspect
// ===========================================================================
// Handles creation of .lmnt layer images (zstd-compressed, SHA-256 verified),
// full and differential image creation, extraction with integrity verification,
// metadata-only inspection, and manifest file management.

class LayerImageManager {
public:
    LayerImageManager()  = default;
    ~LayerImageManager() = default;

    // ---------------------------------------------------------------------
    // 8.3 — Create a full layer image from a directory
    // ---------------------------------------------------------------------
    // Walks sourceDir recursively, collects all files into a tar-like stream,
    // compresses with zstd, writes header + metadata + data + SHA-256 checksum.
    // metadata is updated in place with id/createdAt (if not set), fileCount,
    // uncompressedSize, compressedSize.
    DWORD CreateImage(const std::wstring& sourceDir,
                      const std::wstring& outputPath,
                      LayerMetadata& metadata,
                      int compressionLevel = 3);

    // ---------------------------------------------------------------------
    // 8.5 — Extract a layer image to a directory
    // ---------------------------------------------------------------------
    // Validates header, verifies SHA-256 checksum (if requested), decompresses
    // the data section, and extracts files to targetDir. Materializes any
    // metadata.whiteouts entries as .wh.* marker files.
    DWORD ExtractImage(const std::wstring& imagePath,
                       const std::wstring& targetDir,
                       bool verifyChecksum = true);

    // ---------------------------------------------------------------------
    // 8.6 — Read header and metadata only (no data decompression)
    // ---------------------------------------------------------------------
    DWORD GetImageInfo(const std::wstring& imagePath,
                       LayerImageHeader& header,
                       LayerMetadata& metadata);

    // ---------------------------------------------------------------------
    // Validate a layer image's full contents. Returns ERROR_SUCCESS iff
    // the header parses, the metadata JSON round-trips, AND the SHA-256
    // of the compressed data section matches header.checksum. Returns
    // ERROR_CRC on checksum mismatch. Does not decompress. This is what
    // a caller wants when they ask "is this .lmnt file intact?" -- the
    // older GetImageInfo-only check validated just the header bytes and
    // missed any corruption inside the data payload.
    DWORD ValidateImage(const std::wstring& imagePath);

    // ---------------------------------------------------------------------
    // 8.7 — Create a differential image (delta between source and base)
    // ---------------------------------------------------------------------
    // Compares sourceDir against baseDir. Archives only files that are new
    // or modified (different size or timestamp). Deleted files (in base but
    // not source) are recorded in metadata.whiteouts AND archived as explicit
    // whiteout marker entries using the `.wh.` prefix convention.
    DWORD CreateDifferentialImage(const std::wstring& sourceDir,
                                  const std::wstring& baseDir,
                                  const std::wstring& outputPath,
                                  LayerMetadata& metadata,
                                  int compressionLevel = 3);

    // ---------------------------------------------------------------------
    // 8.8 — Create/load a manifest listing multiple layer images
    // ---------------------------------------------------------------------
    // The manifest is a JSON file listing an ordered array of layer image
    // paths and their SHA-256 checksums (hex).
    static DWORD CreateManifest(const std::wstring& outputPath,
                                const std::vector<std::wstring>& layerImagePaths);

    static DWORD LoadManifest(const std::wstring& manifestPath,
                              LayerManifest& manifest);
};

} // namespace LayerMount::LayerImage
