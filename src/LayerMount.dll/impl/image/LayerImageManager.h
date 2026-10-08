#pragma once

#include "LayerImageFormat.h"
#include "../HostPath.h"

#include <windows.h>
#include <string>
#include <vector>

namespace LayerMount::LayerImage {

struct ImageOutput {
    HostPath path;
    int compressionLevel;
};

class LayerImageManager {
public:
    LayerImageManager()  = default;
    ~LayerImageManager() = default;

    // Walks sourceDir recursively and packs every entry except the root
    // sidecar into a zstd-compressed archive, then writes the header, the
    // metadata, the data and its SHA-256. Sets metadata.id and createdAt when
    // they are empty, and sets fileCount, uncompressedSize and compressedSize.
    DWORD CreateImage(const HostPath& sourceDir,
                      const ImageOutput& output,
                      LayerMetadata& metadata);

    // Validates header, verifies SHA-256 checksum (if requested), decompresses
    // the data section, and extracts files to targetDir. Materializes any
    // metadata.whiteouts entries as .wh.* marker files.
    DWORD ExtractImage(const HostPath& imagePath,
                       const HostPath& targetDir,
                       bool verifyChecksum);

    // Reads the header and metadata only. Does not verify the checksum and
    // does not decompress.
    DWORD GetImageInfo(const HostPath& imagePath,
                       LayerImageHeader& header,
                       LayerMetadata& metadata);

    // Returns ERROR_SUCCESS iff the header parses, the metadata JSON parses,
    // AND the SHA-256 of the compressed data section matches header.checksum.
    // Returns ERROR_CRC on checksum mismatch. Does not decompress.
    DWORD ValidateImage(const HostPath& imagePath);

    // Compares sourceDir against baseDir, with paths matched case-insensitively.
    // Packs each source entry that base does not have, that changed between
    // file and directory, or that is a file with a different size or
    // last-write time. Each base entry, file or directory, that source does not
    // have goes into metadata.whiteouts and into the archive as a `.wh.`
    // whiteout marker.
    DWORD CreateDifferentialImage(const HostPath& sourceDir,
                                  const HostPath& baseDir,
                                  const ImageOutput& output,
                                  LayerMetadata& metadata);

    // The manifest is a JSON file listing an ordered array of layer image
    // paths and their SHA-256 checksums (hex). It stores each image path as
    // the caller gave it.
    static DWORD CreateManifest(const HostPath& outputPath,
                                const std::vector<HostPath>& layerImagePaths);

    static DWORD LoadManifest(const HostPath& manifestPath,
                              LayerManifest& manifest);
};

}
