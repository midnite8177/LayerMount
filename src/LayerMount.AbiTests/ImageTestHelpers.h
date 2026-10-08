#pragma once

#include "AbiTestFixture.h"

namespace LayerMountAbiTests {

inline constexpr INT32 kCompressionLevel = 3;
inline constexpr BOOL  kVerifyChecksum   = TRUE;
inline constexpr BOOL  kSkipChecksum     = FALSE;

inline constexpr UINT64 kImageHeaderSize        = 128;
inline constexpr UINT32 kImageFormatVersion     = 1;
inline constexpr UINT32 kZstdCompressionFlags   = 1;
inline constexpr size_t kArchiveEntryHeaderSize = 24;
inline constexpr UINT8  kFileEntry              = 0;
inline constexpr UINT8  kDirectoryEntry         = 1;
inline constexpr UINT8  kNotWhiteout            = 0;
inline constexpr UINT64 kNoModifiedTime         = 0;

inline constexpr UINT32 kZstdFrameMagic = 0xFD2FB528;
inline constexpr UINT8  kSingleSegmentWithOneByteContentSize = 0x20;
inline constexpr UINT32 kLastRawBlock = 1;
inline constexpr int    kBlockSizeShift = 3;
inline constexpr size_t kBlockHeaderBytes = 3;

inline std::string WideToNarrow(const std::wstring& ascii) {
    std::string narrow;
    for (const wchar_t c : ascii) narrow.push_back(static_cast<char>(c));
    return narrow;
}

inline UINT64 FileCountOf(LM_HANDLE mount, const std::wstring& imagePath) {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
    LM_IMAGE_METADATA metadata{};
    Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountImageGetMetadata(mount, imagePath.c_str(), &metadata),
        L"LayerMountImageGetMetadata");
    return metadata.fileCount;
}

template <typename T>
void AppendLittleEndian(std::string& out, T value) {
    char bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    out.append(bytes, sizeof(T));
}

inline std::string ArchiveEntryHeader(const std::string& path, UINT64 size, UINT32 attributes,
                                      UINT8 isDirectory) {
    std::string header;
    AppendLittleEndian<UINT16>(header, static_cast<UINT16>(path.size()));
    AppendLittleEndian<UINT64>(header, size);
    AppendLittleEndian<UINT32>(header, attributes);
    AppendLittleEndian<UINT64>(header, kNoModifiedTime);
    AppendLittleEndian<UINT8>(header, isDirectory);
    AppendLittleEndian<UINT8>(header, kNotWhiteout);
    return header;
}

inline std::string ArchiveDirectoryEntry(const std::string& path) {
    return ArchiveEntryHeader(path, 0, FILE_ATTRIBUTE_DIRECTORY, kDirectoryEntry) + path;
}

inline std::string ArchiveFileEntry(const std::string& path, const std::string& data) {
    return ArchiveEntryHeader(path, data.size(), FILE_ATTRIBUTE_NORMAL, kFileEntry) + path + data;
}

inline std::string ArchiveSentinel() {
    std::string sentinel;
    AppendLittleEndian<UINT16>(sentinel, 0xFFFF);
    sentinel.append(kArchiveEntryHeaderSize - sizeof(UINT16), '\0');
    return sentinel;
}

inline std::string RawZstdFrame(const std::string& content) {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
    Assert::IsTrue(content.size() <= MAXBYTE,
        L"The frame descriptor gives the content size one byte, so the content holds at most 255 bytes");
    std::string frame;
    AppendLittleEndian<UINT32>(frame, kZstdFrameMagic);
    AppendLittleEndian<UINT8>(frame, kSingleSegmentWithOneByteContentSize);
    AppendLittleEndian<UINT8>(frame, static_cast<UINT8>(content.size()));
    const UINT32 blockHeader =
        kLastRawBlock | (static_cast<UINT32>(content.size()) << kBlockSizeShift);
    frame.append(reinterpret_cast<const char*>(&blockHeader), kBlockHeaderBytes);
    return frame + content;
}

inline void WriteImageWithZeroChecksum(const std::wstring& imagePath,
                                       const std::string& metadataJson,
                                       const std::string& archive) {
    const std::string metadata = metadataJson + '\0';
    const std::string data = RawZstdFrame(archive);
    std::string image = std::string("OVLYIMG", 8);
    AppendLittleEndian<UINT32>(image, kImageFormatVersion);
    AppendLittleEndian<UINT32>(image, kZstdCompressionFlags);
    AppendLittleEndian<UINT64>(image, kImageHeaderSize);
    AppendLittleEndian<UINT64>(image, metadata.size());
    AppendLittleEndian<UINT64>(image, kImageHeaderSize + metadata.size());
    AppendLittleEndian<UINT64>(image, data.size());
    image.append(static_cast<size_t>(kImageHeaderSize) - image.size(), '\0');
    std::ofstream out(imagePath, std::ios::binary | std::ios::trunc);
    out << image << metadata << data;
}

}
