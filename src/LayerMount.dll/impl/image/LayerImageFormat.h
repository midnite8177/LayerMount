#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace LayerMount::LayerImage {

// ===========================================================================
// Constants
// ===========================================================================

constexpr uint8_t LAYER_IMAGE_MAGIC[8] = {'O', 'V', 'L', 'Y', 'I', 'M', 'G', '\0'};
constexpr uint32_t LAYER_IMAGE_VERSION = 1;
constexpr uint16_t ARCHIVE_END_MARKER = 0xFFFF;

// ===========================================================================
// CompressionType
// ===========================================================================

enum class CompressionType : uint32_t {
    None = 0,
    Zstd = 1,
    // Reserved for future use:
    // Gzip = 2,
    // Lz4  = 3,
};

// ===========================================================================
// LayerImageHeader — 128-byte packed binary header
// ===========================================================================
// Layout:
//   [0..7]    magic          "OVLYIMG\0"
//   [8..11]   version        format version (currently 1)
//   [12..15]  flags          low 4 bits = CompressionType
//   [16..23]  metadataOffset byte offset of JSON metadata section
//   [24..31]  metadataSize   byte length of JSON metadata section
//   [32..39]  dataOffset     byte offset of compressed data section
//   [40..47]  dataSize       byte length of compressed data section
//   [48..79]  checksum       SHA-256 of the data section only
//   [80..127] reserved       zero-filled padding
//
// All integer fields are little-endian (native x86-64).

#pragma pack(push, 1)
struct LayerImageHeader {
    uint8_t  magic[8]       = {'O', 'V', 'L', 'Y', 'I', 'M', 'G', '\0'};
    uint32_t version        = LAYER_IMAGE_VERSION;
    uint32_t flags          = 0;
    uint64_t metadataOffset = 0;
    uint64_t metadataSize   = 0;
    uint64_t dataOffset     = 0;
    uint64_t dataSize       = 0;
    uint8_t  checksum[32]   = {};   // SHA-256 of data section
    uint8_t  reserved[48]   = {};
};
#pragma pack(pop)

static_assert(sizeof(LayerImageHeader) == 128,
              "LayerImageHeader must be exactly 128 bytes");

// ===========================================================================
// FileEntryHeader — 24-byte packed archive entry
// ===========================================================================
// Each entry in the tar-like archive stream is:
//   1. FileEntryHeader  (24 bytes)
//   2. nameLength bytes of UTF-8 relative path (forward slashes)
//   3. For files: size bytes of file data
//
// End-of-archive marker: a full FileEntryHeader with nameLength = 0xFFFF
// and all other fields zeroed.
//
// Paths exceeding 65534 UTF-8 bytes must be rejected with
// ERROR_FILENAME_EXCED_RANGE.

#pragma pack(push, 1)
struct FileEntryHeader {
    uint16_t nameLength = 0;    // UTF-8 path length; 0xFFFF = end marker
    uint64_t size       = 0;    // uncompressed file size (0 for directories)
    uint32_t attributes = 0;    // Windows file attributes (DWORD)
    uint64_t modified   = 0;    // FILETIME packed as uint64
    uint8_t  isDirectory = 0;   // 1 = directory, 0 = file
    uint8_t  isWhiteout  = 0;   // 1 = whiteout marker (.wh.*)
};
#pragma pack(pop)

static_assert(sizeof(FileEntryHeader) == 24,
              "FileEntryHeader must be exactly 24 bytes");

// ===========================================================================
// LayerMetadata — in-memory metadata (serialized as JSON)
// ===========================================================================
// JSON schema:
// {
//   "layerId":          "UUID string",
//   "parentLayerId":    "UUID string or null",
//   "createdAt":        "ISO 8601 datetime (UTC)",
//   "author":           "string",
//   "description":      "string",
//   "tags":             ["string", ...],
//   "compressionType":  "none" | "zstd",
//   "fileCount":        uint64,
//   "uncompressedSize": uint64,
//   "compressedSize":   uint64,
//   "whiteouts":        ["relative/path", ...],
//   "labels":           {"key": "value", ...}
// }

struct LayerMetadata {
    std::wstring id;
    std::wstring parentId;      // empty = no parent (serialized as JSON null)
    std::wstring createdAt;     // ISO 8601 UTC
    std::wstring author;
    std::wstring description;
    std::vector<std::wstring> tags;
    CompressionType compression = CompressionType::Zstd;
    uint64_t fileCount          = 0;
    uint64_t uncompressedSize   = 0;
    uint64_t compressedSize     = 0;
    std::vector<std::wstring> whiteouts;
    std::map<std::wstring, std::wstring> labels;
};

// ===========================================================================
// LayerManifest — ordered list of layer images (similar to Docker manifest)
// ===========================================================================

struct LayerManifestEntry {
    std::wstring imagePath;
    std::wstring checksumHex;   // 64-char lowercase hex SHA-256
};

struct LayerManifest {
    uint32_t schemaVersion = 1;
    std::vector<LayerManifestEntry> layers;
};

} // namespace LayerMount::LayerImage
