#include "LayerImageManager.h"

#include "nlohmann/json.hpp"
#include "zstd.h"

#include <bcrypt.h>
#include <objbase.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>

namespace LayerMount::LayerImage {

namespace fs = std::filesystem;

// ===========================================================================
// Status code mapping — BCrypt returns NTSTATUS, we return DWORD
// ===========================================================================

// BCrypt NTSTATUS values we care about. Defined here to avoid including
// <ntstatus.h>, which conflicts with <windows.h> without careful ordering.
namespace {
constexpr NTSTATUS kStatusSuccess          = 0x00000000L;
constexpr NTSTATUS kStatusInvalidParameter = static_cast<NTSTATUS>(0xC000000DL);
constexpr NTSTATUS kStatusInvalidHandle    = static_cast<NTSTATUS>(0xC0000008L);
constexpr NTSTATUS kStatusNoMemory         = static_cast<NTSTATUS>(0xC0000017L);
}

static DWORD NtStatusToDword(NTSTATUS status) {
    if (status == kStatusSuccess) return ERROR_SUCCESS;
    switch (status) {
        case kStatusInvalidParameter: return ERROR_INVALID_PARAMETER;
        case kStatusInvalidHandle:    return ERROR_INVALID_HANDLE;
        case kStatusNoMemory:         return ERROR_NOT_ENOUGH_MEMORY;
        default:                      return ERROR_INVALID_FUNCTION;
    }
}

// ===========================================================================
// String conversion helpers (UTF-16 <-> UTF-8)
// ===========================================================================

static std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int size = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                     static_cast<int>(wide.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string utf8(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                          static_cast<int>(wide.size()),
                          utf8.data(), size, nullptr, nullptr);
    return utf8;
}

static std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int size = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                     static_cast<int>(utf8.size()),
                                     nullptr, 0);
    if (size <= 0) return {};
    std::wstring wide(static_cast<size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                          static_cast<int>(utf8.size()),
                          wide.data(), size);
    return wide;
}

// ===========================================================================
// UUID generation — CoCreateGuid + StringFromGUID2
// ===========================================================================

static std::wstring GenerateId() {
    GUID guid{};
    HRESULT hr = ::CoCreateGuid(&guid);
    if (FAILED(hr)) return L"";

    wchar_t buf[40]{};
    int len = ::StringFromGUID2(guid, buf, 40);
    if (len == 0) return L"";

    std::wstring id(buf);
    if (id.size() >= 2 && id.front() == L'{' && id.back() == L'}') {
        id = id.substr(1, id.size() - 2);
    }
    return id;
}

// ===========================================================================
// ISO 8601 timestamp (UTC)
// ===========================================================================

static std::wstring GetCurrentTimestampISO8601() {
    SYSTEMTIME st{};
    ::GetSystemTime(&st);
    wchar_t buf[32]{};
    ::swprintf_s(buf, L"%04u-%02u-%02uT%02u:%02u:%02uZ",
                 st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond);
    return std::wstring(buf);
}

// ===========================================================================
// BCrypt SHA-256 — RAII context + three entry points
// ===========================================================================

namespace {
struct BcryptHashContext {
    BCRYPT_ALG_HANDLE  hAlg  = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    std::vector<uint8_t> hashObject;

    BcryptHashContext() = default;
    BcryptHashContext(const BcryptHashContext&) = delete;
    BcryptHashContext& operator=(const BcryptHashContext&) = delete;

    ~BcryptHashContext() {
        if (hHash) ::BCryptDestroyHash(hHash);
        if (hAlg)  ::BCryptCloseAlgorithmProvider(hAlg, 0);
    }
};
}

static DWORD InitBcryptSha256(BcryptHashContext& ctx) {
    NTSTATUS status = ::BCryptOpenAlgorithmProvider(
        &ctx.hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status != kStatusSuccess) return NtStatusToDword(status);

    DWORD hashObjSize = 0;
    DWORD cbData      = 0;
    status = ::BCryptGetProperty(ctx.hAlg, BCRYPT_OBJECT_LENGTH,
                                 reinterpret_cast<PUCHAR>(&hashObjSize),
                                 sizeof(hashObjSize), &cbData, 0);
    if (status != kStatusSuccess) return NtStatusToDword(status);

    ctx.hashObject.resize(hashObjSize);

    status = ::BCryptCreateHash(ctx.hAlg, &ctx.hHash,
                                ctx.hashObject.data(), hashObjSize,
                                nullptr, 0, 0);
    if (status != kStatusSuccess) return NtStatusToDword(status);

    return ERROR_SUCCESS;
}

static DWORD ComputeSHA256Stream(const std::wstring& filePath,
                                 uint64_t offset, uint64_t size,
                                 uint8_t outHash[32]) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file) return ERROR_FILE_NOT_FOUND;

    file.seekg(static_cast<std::streamoff>(offset));
    if (!file) return ERROR_SEEK;

    BcryptHashContext ctx;
    DWORD err = InitBcryptSha256(ctx);
    if (err != ERROR_SUCCESS) return err;

    constexpr size_t kChunkSize = 64 * 1024;
    std::vector<uint8_t> buffer(kChunkSize);

    uint64_t remaining = size;
    while (remaining > 0) {
        size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, kChunkSize));
        file.read(reinterpret_cast<char*>(buffer.data()),
                  static_cast<std::streamsize>(chunk));
        if (file.gcount() != static_cast<std::streamsize>(chunk)) {
            return ERROR_READ_FAULT;
        }
        NTSTATUS status = ::BCryptHashData(
            ctx.hHash, buffer.data(), static_cast<ULONG>(chunk), 0);
        if (status != kStatusSuccess) return NtStatusToDword(status);
        remaining -= chunk;
    }

    NTSTATUS status = ::BCryptFinishHash(ctx.hHash, outHash, 32, 0);
    return NtStatusToDword(status);
}

// Stream write + hash in a single pass.
static DWORD WriteDataWithHash(std::ofstream& output,
                               const uint8_t* data, size_t size,
                               uint8_t outHash[32]) {
    BcryptHashContext ctx;
    DWORD err = InitBcryptSha256(ctx);
    if (err != ERROR_SUCCESS) return err;

    constexpr size_t kChunkSize = 64 * 1024;
    size_t remaining = size;
    const uint8_t* ptr = data;

    while (remaining > 0) {
        size_t chunk = std::min(remaining, kChunkSize);

        NTSTATUS status = ::BCryptHashData(
            ctx.hHash, const_cast<PUCHAR>(ptr), static_cast<ULONG>(chunk), 0);
        if (status != kStatusSuccess) return NtStatusToDword(status);

        output.write(reinterpret_cast<const char*>(ptr),
                     static_cast<std::streamsize>(chunk));
        if (!output) return ERROR_WRITE_FAULT;

        ptr       += chunk;
        remaining -= chunk;
    }

    NTSTATUS status = ::BCryptFinishHash(ctx.hHash, outHash, 32, 0);
    return NtStatusToDword(status);
}

static std::wstring HashToHexString(const uint8_t hash[32]) {
    wchar_t buf[65]{};
    for (int i = 0; i < 32; ++i) {
        ::swprintf_s(&buf[i * 2], 3, L"%02x", hash[i]);
    }
    return std::wstring(buf, 64);
}

// ===========================================================================
// UTF-8 and archive-path validation
// ===========================================================================

static bool IsValidUtf8(const char* data, size_t len) {
    for (size_t i = 0; i < len; ) {
        uint8_t c = static_cast<uint8_t>(data[i]);
        if (c == 0) return false;           // reject embedded NULs
        size_t extra;
        if ((c & 0x80) == 0x00)      extra = 0;
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else return false;
        if (i + extra >= len) return false;
        for (size_t j = 1; j <= extra; ++j) {
            if ((static_cast<uint8_t>(data[i + j]) & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

// Component-aware containment check used by the zip-slip guard. Returns
// true iff `canon` either equals `root` or sits strictly under `root`
// at a path-component boundary.
//
// The previous guard collapsed to `canon.compare(0, root.size(), root) == 0`,
// a raw prefix-string compare. That accepts sibling paths whose names
// happen to start with the root's name (e.g., root = `C:\out`, canon =
// `C:\out2\file`) and silently lets extraction escape the target. Walking
// to the next character after the prefix and requiring a separator (or
// allowing a trailing-separator root) closes that hole.
static bool IsPathContainedIn(const std::wstring& canon,
                              const std::wstring& root) {
    if (canon == root) return true;
    if (root.empty()) return true;             // any path is "under" empty root
    if (canon.size() <= root.size()) return false;
    if (canon.compare(0, root.size(), root) != 0) return false;
    // If the root already ends with a separator (e.g., a drive root
    // `C:\`), prefix-equality at root.size() already implies a clean
    // component boundary.
    const wchar_t rootLast = root.back();
    if (rootLast == L'\\' || rootLast == L'/') return true;
    // Otherwise the very next character in canon must be a path separator
    // so we are not matching a sibling whose name extends the root's name.
    const wchar_t boundary = canon[root.size()];
    return boundary == L'\\' || boundary == L'/';
}

// Reject absolute paths, drive-qualified paths, and `..` traversal.
// Paths use forward slashes. Trailing slashes are allowed (for directories).
static bool IsValidArchivePath(const std::string& utf8Path) {
    if (utf8Path.empty()) return false;
    if (utf8Path.front() == '/' || utf8Path.front() == '\\') return false;
    if (utf8Path.size() >= 2 && utf8Path[1] == ':') return false;  // drive qualified

    // Walk each path component, reject "." and ".."
    size_t start = 0;
    for (size_t i = 0; i <= utf8Path.size(); ++i) {
        bool boundary = (i == utf8Path.size()) ||
                        utf8Path[i] == '/' || utf8Path[i] == '\\';
        if (boundary) {
            size_t len = i - start;
            if (len == 0) {
                if (i == utf8Path.size()) break;  // allow trailing slash
                return false;                     // empty component (e.g., "//")
            }
            std::string_view component(utf8Path.data() + start, len);
            if (component == "." || component == "..") return false;
            start = i + 1;
        }
    }
    return true;
}

// ===========================================================================
// zstd compression helpers
// ===========================================================================

static DWORD CompressBuffer(const std::vector<uint8_t>& input,
                            std::vector<uint8_t>& output,
                            int level) {
    size_t bound = ::ZSTD_compressBound(input.size());
    output.resize(bound);
    size_t result = ::ZSTD_compress(output.data(), bound,
                                    input.data(), input.size(), level);
    if (::ZSTD_isError(result)) return ERROR_INVALID_DATA;
    output.resize(result);
    return ERROR_SUCCESS;
}

// Upper bound for a single decompression (prevents DoS via a malformed frame
// reporting a huge content size). 4 GiB is well above any reasonable layer.
constexpr uint64_t kMaxDecompressedBytes = 4ULL * 1024 * 1024 * 1024;

static DWORD DecompressBuffer(const uint8_t* input, size_t inputSize,
                              uint64_t expectedSize,
                              std::vector<uint8_t>& output) {
    unsigned long long frameSize = ::ZSTD_getFrameContentSize(input, inputSize);
    if (frameSize == ZSTD_CONTENTSIZE_ERROR ||
        frameSize == ZSTD_CONTENTSIZE_UNKNOWN) {
        return ERROR_INVALID_DATA;
    }
    if (frameSize > kMaxDecompressedBytes) return ERROR_INVALID_DATA;
    if (expectedSize != 0 && frameSize != expectedSize) return ERROR_INVALID_DATA;

    output.resize(static_cast<size_t>(frameSize));
    size_t result = ::ZSTD_decompress(output.data(), output.size(),
                                      input, inputSize);
    if (::ZSTD_isError(result)) return ERROR_INVALID_DATA;
    if (result != frameSize) return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}

// ===========================================================================
// Archive buffer helpers
// ===========================================================================

static void AppendBytes(std::vector<uint8_t>& buf, const void* src, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(src);
    buf.insert(buf.end(), p, p + n);
}

// Convert a wstring path to UTF-8 with forward slashes, relative to base.
static std::string NormalizeRelativePath(const fs::path& absolute,
                                         const fs::path& base) {
    fs::path rel = fs::relative(absolute, base);
    std::wstring w = rel.wstring();
    std::replace(w.begin(), w.end(), L'\\', L'/');
    return WideToUtf8(w);
}

// Convert a logical path to its whiteout marker path:
//   "foo/bar.txt" -> "foo/.wh.bar.txt"
//   "bar.txt"     -> ".wh.bar.txt"
static std::wstring MakeWhiteoutMarkerPath(const std::wstring& logicalPath) {
    std::wstring path = logicalPath;
    std::replace(path.begin(), path.end(), L'\\', L'/');
    size_t slash = path.rfind(L'/');
    if (slash == std::wstring::npos) return L".wh." + path;
    return path.substr(0, slash + 1) + L".wh." + path.substr(slash + 1);
}

// Check if a filename has the .wh. prefix (used for whiteout detection).
static bool HasWhiteoutPrefix(const std::wstring& filename) {
    return filename.size() >= 4 && filename.compare(0, 4, L".wh.") == 0;
}

// Pack a single archive entry into the buffer: header + name + data.
static DWORD AppendFileEntry(std::vector<uint8_t>& archive,
                             const std::string& utf8Name,
                             const FileEntryHeader& entry,
                             const uint8_t* data,
                             uint64_t& uncompressedTotal) {
    if (utf8Name.empty()) return ERROR_INVALID_PARAMETER;
    if (utf8Name.size() > 0xFFFE) return ERROR_FILENAME_EXCED_RANGE;

    FileEntryHeader h = entry;
    h.nameLength = static_cast<uint16_t>(utf8Name.size());

    AppendBytes(archive, &h, sizeof(h));
    AppendBytes(archive, utf8Name.data(), utf8Name.size());
    if (!h.isDirectory && h.size > 0) {
        AppendBytes(archive, data, static_cast<size_t>(h.size));
    }
    uncompressedTotal = archive.size();
    return ERROR_SUCCESS;
}

// Append the end-of-archive marker.
static void AppendEndMarker(std::vector<uint8_t>& archive) {
    FileEntryHeader end{};
    end.nameLength = ARCHIVE_END_MARKER;
    AppendBytes(archive, &end, sizeof(end));
}

// Read a file into a buffer (used when archiving).
static DWORD ReadFileBytes(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return ERROR_ACCESS_DENIED;
    in.seekg(0, std::ios::end);
    std::streamoff sz = in.tellg();
    if (sz < 0) return ERROR_READ_FAULT;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    if (sz > 0) {
        in.read(reinterpret_cast<char*>(out.data()),
                static_cast<std::streamsize>(sz));
        if (!in) return ERROR_READ_FAULT;
    }
    return ERROR_SUCCESS;
}

// Fill an entry header from an on-disk file.
static DWORD FillFileEntry(const fs::path& path, FileEntryHeader& entry,
                           bool isDirectory) {
    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attrs)) {
        return ::GetLastError();
    }
    entry.attributes = attrs.dwFileAttributes;
    entry.modified   = (static_cast<uint64_t>(attrs.ftLastWriteTime.dwHighDateTime) << 32) |
                       attrs.ftLastWriteTime.dwLowDateTime;
    entry.isDirectory = isDirectory ? 1 : 0;
    if (isDirectory) {
        entry.size = 0;
    } else {
        entry.size = (static_cast<uint64_t>(attrs.nFileSizeHigh) << 32) |
                     attrs.nFileSizeLow;
    }
    return ERROR_SUCCESS;
}

// ===========================================================================
// Archive construction — walk a directory tree
// ===========================================================================

static DWORD BuildArchiveFromDirectory(const std::wstring& sourceDir,
                                       std::vector<uint8_t>& archive,
                                       uint64_t& fileCount,
                                       uint64_t& uncompressedTotal) {
    fs::path basePath(sourceDir);
    if (!fs::exists(basePath)) return ERROR_PATH_NOT_FOUND;

    std::error_code ec;
    auto it = fs::recursive_directory_iterator(
        basePath, fs::directory_options::none, ec);
    if (ec) return ERROR_ACCESS_DENIED;

    const auto end = fs::recursive_directory_iterator();
    while (it != end) {
        const auto& entry = *it;

        std::string utf8Name = NormalizeRelativePath(entry.path(), basePath);
        if (utf8Name.size() > 0xFFFE) return ERROR_FILENAME_EXCED_RANGE;

        FileEntryHeader h{};
        bool isDir = entry.is_directory(ec);
        if (ec) return ERROR_ACCESS_DENIED;

        DWORD err = FillFileEntry(entry.path(), h, isDir);
        if (err != ERROR_SUCCESS) return err;

        if (!isDir) {
            bool isWhiteout = HasWhiteoutPrefix(entry.path().filename().wstring());
            h.isWhiteout = isWhiteout ? 1 : 0;

            std::vector<uint8_t> fileBytes;
            if (h.size > 0) {
                err = ReadFileBytes(entry.path(), fileBytes);
                if (err != ERROR_SUCCESS) return err;
            }
            err = AppendFileEntry(archive, utf8Name, h,
                                  fileBytes.empty() ? nullptr : fileBytes.data(),
                                  uncompressedTotal);
            if (err != ERROR_SUCCESS) return err;
            ++fileCount;
        } else {
            err = AppendFileEntry(archive, utf8Name, h, nullptr,
                                  uncompressedTotal);
            if (err != ERROR_SUCCESS) return err;
        }

        it.increment(ec);
        if (ec) return ERROR_ACCESS_DENIED;
    }

    AppendEndMarker(archive);
    uncompressedTotal = archive.size();
    return ERROR_SUCCESS;
}

// ===========================================================================
// Directory scanning for differential comparison
// ===========================================================================

namespace {
struct FileSnapshot {
    std::wstring relativePath;   // forward-slash normalized, lower-cased key
    std::wstring displayPath;    // original-case relative path
    uint64_t size         = 0;
    uint64_t modifiedTime = 0;
    uint32_t attributes   = 0;
    bool     isDirectory  = false;
};
}

static std::wstring ToLower(const std::wstring& s) {
    std::wstring r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return r;
}

static DWORD ScanDirectory(const std::wstring& dir,
                           std::vector<FileSnapshot>& out) {
    fs::path base(dir);
    if (!fs::exists(base)) return ERROR_PATH_NOT_FOUND;

    std::error_code ec;
    auto it = fs::recursive_directory_iterator(
        base, fs::directory_options::none, ec);
    if (ec) return ERROR_ACCESS_DENIED;

    const auto end = fs::recursive_directory_iterator();
    while (it != end) {
        const auto& entry = *it;
        fs::path rel = fs::relative(entry.path(), base);
        std::wstring relStr = rel.wstring();
        std::replace(relStr.begin(), relStr.end(), L'\\', L'/');

        FileSnapshot snap;
        snap.displayPath  = relStr;
        snap.relativePath = ToLower(relStr);
        snap.isDirectory  = entry.is_directory(ec);
        if (ec) return ERROR_ACCESS_DENIED;

        WIN32_FILE_ATTRIBUTE_DATA attrs{};
        if (!::GetFileAttributesExW(entry.path().c_str(),
                                    GetFileExInfoStandard, &attrs)) {
            return ::GetLastError();
        }
        snap.attributes   = attrs.dwFileAttributes;
        snap.modifiedTime = (static_cast<uint64_t>(attrs.ftLastWriteTime.dwHighDateTime) << 32) |
                            attrs.ftLastWriteTime.dwLowDateTime;
        snap.size = snap.isDirectory
            ? 0
            : ((static_cast<uint64_t>(attrs.nFileSizeHigh) << 32) | attrs.nFileSizeLow);

        out.push_back(std::move(snap));

        it.increment(ec);
        if (ec) return ERROR_ACCESS_DENIED;
    }
    return ERROR_SUCCESS;
}

// ===========================================================================
// JSON metadata serialization
// ===========================================================================

static const char* CompressionTypeToString(CompressionType c) {
    switch (c) {
        case CompressionType::None: return "none";
        case CompressionType::Zstd: return "zstd";
        default:                    return "none";
    }
}

static CompressionType StringToCompressionType(const std::string& s) {
    if (s == "zstd") return CompressionType::Zstd;
    return CompressionType::None;
}

static nlohmann::json MetadataToJson(const LayerMetadata& meta) {
    nlohmann::json j;
    j["layerId"]     = WideToUtf8(meta.id);
    if (meta.parentId.empty()) {
        j["parentLayerId"] = nullptr;
    } else {
        j["parentLayerId"] = WideToUtf8(meta.parentId);
    }
    j["createdAt"]   = WideToUtf8(meta.createdAt);
    j["author"]      = WideToUtf8(meta.author);
    j["description"] = WideToUtf8(meta.description);

    nlohmann::json tags = nlohmann::json::array();
    for (const auto& t : meta.tags) tags.push_back(WideToUtf8(t));
    j["tags"] = tags;

    j["compressionType"]  = CompressionTypeToString(meta.compression);
    j["fileCount"]        = meta.fileCount;
    j["uncompressedSize"] = meta.uncompressedSize;
    j["compressedSize"]   = meta.compressedSize;

    nlohmann::json wl = nlohmann::json::array();
    for (const auto& w : meta.whiteouts) wl.push_back(WideToUtf8(w));
    j["whiteouts"] = wl;

    nlohmann::json labels = nlohmann::json::object();
    for (const auto& [k, v] : meta.labels) {
        labels[WideToUtf8(k)] = WideToUtf8(v);
    }
    j["labels"] = labels;

    return j;
}

static DWORD JsonToMetadata(const nlohmann::json& j, LayerMetadata& meta) {
    try {
        meta.id          = Utf8ToWide(j.value("layerId", ""));

        // parentLayerId: special-cased because j.value() throws on present-but-null
        if (j.contains("parentLayerId") && !j["parentLayerId"].is_null()) {
            meta.parentId = Utf8ToWide(j["parentLayerId"].get<std::string>());
        } else {
            meta.parentId.clear();
        }

        meta.createdAt   = Utf8ToWide(j.value("createdAt", ""));
        meta.author      = Utf8ToWide(j.value("author", ""));
        meta.description = Utf8ToWide(j.value("description", ""));

        meta.tags.clear();
        if (j.contains("tags") && j["tags"].is_array()) {
            for (const auto& t : j["tags"]) {
                if (t.is_string()) meta.tags.push_back(Utf8ToWide(t.get<std::string>()));
            }
        }

        meta.compression      = StringToCompressionType(
            j.value("compressionType", "none"));
        meta.fileCount        = j.value("fileCount", uint64_t{0});
        meta.uncompressedSize = j.value("uncompressedSize", uint64_t{0});
        meta.compressedSize   = j.value("compressedSize", uint64_t{0});

        meta.whiteouts.clear();
        if (j.contains("whiteouts") && j["whiteouts"].is_array()) {
            for (const auto& w : j["whiteouts"]) {
                if (w.is_string()) meta.whiteouts.push_back(Utf8ToWide(w.get<std::string>()));
            }
        }

        meta.labels.clear();
        if (j.contains("labels") && j["labels"].is_object()) {
            for (auto it = j["labels"].begin(); it != j["labels"].end(); ++it) {
                if (it.value().is_string()) {
                    meta.labels[Utf8ToWide(it.key())] =
                        Utf8ToWide(it.value().get<std::string>());
                }
            }
        }
    } catch (const nlohmann::json::exception&) {
        return ERROR_INVALID_DATA;
    }
    return ERROR_SUCCESS;
}

static std::string SerializeMetadata(const LayerMetadata& meta) {
    return MetadataToJson(meta).dump(2);
}

static DWORD DeserializeMetadata(const std::string& utf8, LayerMetadata& meta) {
    try {
        auto j = nlohmann::json::parse(utf8);
        return JsonToMetadata(j, meta);
    } catch (const nlohmann::json::exception&) {
        return ERROR_INVALID_DATA;
    }
}

// ===========================================================================
// Shared write path — compress archive, write header+metadata+data
// ===========================================================================

static DWORD WriteImageFile(const std::wstring& outputPath,
                            LayerMetadata& metadata,
                            const std::vector<uint8_t>& archive,
                            int compressionLevel) {
    // Ensure parent directory exists
    fs::path outPath(outputPath);
    if (outPath.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(outPath.parent_path(), ec);
        // Ignore error; the open below will fail if directory is unusable
    }

    // Compress archive
    std::vector<uint8_t> compressed;
    DWORD err = CompressBuffer(archive, compressed, compressionLevel);
    if (err != ERROR_SUCCESS) return err;

    metadata.compression      = CompressionType::Zstd;
    metadata.uncompressedSize = archive.size();
    metadata.compressedSize   = compressed.size();

    // Serialize metadata (UTF-8, null-terminated as required)
    std::string metadataJson = SerializeMetadata(metadata);
    metadataJson.push_back('\0');

    // Open output
    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    if (!out) return ERROR_ACCESS_DENIED;

    // Write placeholder header
    LayerImageHeader header{};
    header.version = LAYER_IMAGE_VERSION;
    header.flags   = static_cast<uint32_t>(CompressionType::Zstd);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!out) return ERROR_WRITE_FAULT;

    // Write metadata
    header.metadataOffset = sizeof(header);
    header.metadataSize   = metadataJson.size();
    out.write(metadataJson.data(),
              static_cast<std::streamsize>(metadataJson.size()));
    if (!out) return ERROR_WRITE_FAULT;

    // Write compressed data, streaming through SHA-256
    header.dataOffset = header.metadataOffset + header.metadataSize;
    header.dataSize   = compressed.size();
    err = WriteDataWithHash(out, compressed.data(), compressed.size(),
                            header.checksum);
    if (err != ERROR_SUCCESS) return err;

    // Seek back to offset 0, write final header
    out.seekp(0);
    if (!out) return ERROR_SEEK;
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!out) return ERROR_WRITE_FAULT;

    out.close();
    return ERROR_SUCCESS;
}

// ===========================================================================
// Archive extraction
// ===========================================================================

static DWORD ExtractArchive(const uint8_t* data, size_t dataSize,
                            const std::wstring& targetDir,
                            std::vector<std::wstring>& extractedWhiteouts) {
    std::error_code ec;
    fs::create_directories(targetDir, ec);

    fs::path targetRoot(targetDir);
    fs::path targetCanonical = fs::weakly_canonical(targetRoot, ec);
    if (ec) targetCanonical = targetRoot;

    size_t pos = 0;
    while (pos + sizeof(FileEntryHeader) <= dataSize) {
        FileEntryHeader entry;
        std::memcpy(&entry, data + pos, sizeof(FileEntryHeader));
        pos += sizeof(FileEntryHeader);

        if (entry.nameLength == ARCHIVE_END_MARKER) return ERROR_SUCCESS;
        if (entry.nameLength == 0) return ERROR_INVALID_DATA;
        if (pos + entry.nameLength > dataSize) return ERROR_INVALID_DATA;

        const char* namePtr = reinterpret_cast<const char*>(data + pos);
        if (!IsValidUtf8(namePtr, entry.nameLength)) return ERROR_INVALID_DATA;

        std::string utf8Name(namePtr, entry.nameLength);
        pos += entry.nameLength;

        if (!IsValidArchivePath(utf8Name)) return ERROR_BAD_PATHNAME;

        fs::path rel = fs::path(Utf8ToWide(utf8Name));
        fs::path target = targetRoot / rel;

        // Zip-slip guard: verify extraction stays within targetDir using
        // component-aware containment so a sibling whose name extends
        // targetCanonical's name (e.g., `C:\out` vs `C:\out2`) cannot
        // bypass the check.
        fs::path canon = fs::weakly_canonical(target, ec);
        if (ec) return ERROR_BAD_PATHNAME;
        if (!IsPathContainedIn(canon.wstring(), targetCanonical.wstring())) {
            return ERROR_BAD_PATHNAME;
        }

        if (entry.isDirectory) {
            if (pos + entry.size > dataSize) return ERROR_INVALID_DATA;
            fs::create_directories(target, ec);
        } else {
            if (pos + entry.size > dataSize) return ERROR_INVALID_DATA;
            // Ensure parent exists
            if (target.has_parent_path()) {
                fs::create_directories(target.parent_path(), ec);
            }
            std::ofstream out(target, std::ios::binary | std::ios::trunc);
            if (!out) return ERROR_ACCESS_DENIED;
            if (entry.size > 0) {
                out.write(reinterpret_cast<const char*>(data + pos),
                          static_cast<std::streamsize>(entry.size));
                if (!out) return ERROR_WRITE_FAULT;
            }
            out.close();

            // Restore attributes and timestamps. Previously all four calls
            // were fire-and-forget; a failure to apply readonly/hidden/
            // system bits or to set the modified time was indistinguishable
            // from a faithful extraction. Surface errors so callers don't
            // make cache/conflict decisions based on wrong metadata.
            if (!::SetFileAttributesW(target.c_str(), entry.attributes)) {
                return ::GetLastError();
            }
            HANDLE hFile = ::CreateFileW(
                target.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile == INVALID_HANDLE_VALUE) {
                return ::GetLastError();
            }
            FILETIME ft;
            ft.dwHighDateTime = static_cast<DWORD>(entry.modified >> 32);
            ft.dwLowDateTime  = static_cast<DWORD>(entry.modified & 0xFFFFFFFF);
            if (!::SetFileTime(hFile, nullptr, nullptr, &ft)) {
                const DWORD err = ::GetLastError();
                ::CloseHandle(hFile);
                return err;
            }
            if (!::CloseHandle(hFile)) {
                return ::GetLastError();
            }

            if (entry.isWhiteout) {
                // Track which archive paths we already wrote as whiteouts
                extractedWhiteouts.push_back(Utf8ToWide(utf8Name));
            }

            pos += entry.size;
        }
    }
    // Reached EOF without marker
    return ERROR_INVALID_DATA;
}

// ===========================================================================
// Read header + metadata section (shared by ExtractImage and GetImageInfo)
// ===========================================================================

static DWORD ReadHeaderAndMetadata(const std::wstring& imagePath,
                                   LayerImageHeader& header,
                                   LayerMetadata& metadata,
                                   std::ifstream& fileHandle) {
    fileHandle.open(imagePath, std::ios::binary);
    if (!fileHandle) return ERROR_FILE_NOT_FOUND;

    fileHandle.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!fileHandle || fileHandle.gcount() != sizeof(header)) {
        return ERROR_INVALID_DATA;
    }

    // Validate magic and version
    if (std::memcmp(header.magic, LAYER_IMAGE_MAGIC, 8) != 0) {
        return ERROR_INVALID_DATA;
    }
    if (header.version > LAYER_IMAGE_VERSION) return ERROR_REVISION_MISMATCH;

    // Sanity check offsets
    if (header.metadataOffset < sizeof(header)) return ERROR_INVALID_DATA;
    if (header.dataOffset < header.metadataOffset + header.metadataSize) {
        return ERROR_INVALID_DATA;
    }
    if (header.metadataOffset + header.metadataSize < header.metadataOffset) {
        return ERROR_INVALID_DATA;  // overflow
    }
    if (header.dataOffset + header.dataSize < header.dataOffset) {
        return ERROR_INVALID_DATA;  // overflow
    }

    // Read metadata JSON
    fileHandle.seekg(static_cast<std::streamoff>(header.metadataOffset));
    if (!fileHandle) return ERROR_SEEK;

    std::string metadataStr(static_cast<size_t>(header.metadataSize), '\0');
    fileHandle.read(metadataStr.data(),
                    static_cast<std::streamsize>(header.metadataSize));
    if (fileHandle.gcount() != static_cast<std::streamsize>(header.metadataSize)) {
        return ERROR_READ_FAULT;
    }
    // Strip trailing null(s) from the JSON string
    while (!metadataStr.empty() && metadataStr.back() == '\0') {
        metadataStr.pop_back();
    }

    return DeserializeMetadata(metadataStr, metadata);
}

// ===========================================================================
// Public API
// ===========================================================================

// 8.3 — CreateImage
DWORD LayerImageManager::CreateImage(const std::wstring& sourceDir,
                                     const std::wstring& outputPath,
                                     LayerMetadata& metadata,
                                     int compressionLevel) {
    if (!fs::exists(sourceDir)) return ERROR_PATH_NOT_FOUND;

    if (metadata.id.empty())        metadata.id        = GenerateId();
    if (metadata.createdAt.empty()) metadata.createdAt = GetCurrentTimestampISO8601();

    std::vector<uint8_t> archive;
    uint64_t fileCount         = 0;
    uint64_t uncompressedTotal = 0;
    DWORD err = BuildArchiveFromDirectory(sourceDir, archive,
                                          fileCount, uncompressedTotal);
    if (err != ERROR_SUCCESS) return err;

    metadata.fileCount = fileCount;

    err = WriteImageFile(outputPath, metadata, archive, compressionLevel);
    if (err != ERROR_SUCCESS) {
        std::error_code ec;
        fs::remove(outputPath, ec);
        return err;
    }
    return ERROR_SUCCESS;
}

// 8.5 — ExtractImage
DWORD LayerImageManager::ExtractImage(const std::wstring& imagePath,
                                      const std::wstring& targetDir,
                                      bool verifyChecksum) {
    LayerImageHeader header{};
    LayerMetadata    metadata;
    std::ifstream    file;
    DWORD err = ReadHeaderAndMetadata(imagePath, header, metadata, file);
    if (err != ERROR_SUCCESS) return err;

    if (verifyChecksum) {
        uint8_t computed[32]{};
        err = ComputeSHA256Stream(imagePath, header.dataOffset,
                                  header.dataSize, computed);
        if (err != ERROR_SUCCESS) return err;
        if (std::memcmp(computed, header.checksum, 32) != 0) return ERROR_CRC;
    }

    // Read compressed data
    file.seekg(static_cast<std::streamoff>(header.dataOffset));
    if (!file) return ERROR_SEEK;

    std::vector<uint8_t> compressed(static_cast<size_t>(header.dataSize));
    if (header.dataSize > 0) {
        file.read(reinterpret_cast<char*>(compressed.data()),
                  static_cast<std::streamsize>(header.dataSize));
        if (file.gcount() != static_cast<std::streamsize>(header.dataSize)) {
            return ERROR_READ_FAULT;
        }
    }
    file.close();

    // Decompress
    std::vector<uint8_t> decompressed;
    err = DecompressBuffer(compressed.data(), compressed.size(),
                           0 /* expectedSize unknown */, decompressed);
    if (err != ERROR_SUCCESS) return err;

    // Extract
    std::vector<std::wstring> extractedWhiteouts;
    err = ExtractArchive(decompressed.data(), decompressed.size(),
                         targetDir, extractedWhiteouts);
    if (err != ERROR_SUCCESS) return err;

    // Materialize metadata.whiteouts that weren't archived explicitly.
    // Apply the same validation + containment check ExtractArchive uses for
    // archived entries. A malicious image can list `..\outside` or `C:\esc`
    // in metadata.whiteouts, and `MakeWhiteoutMarkerPath` will happily
    // produce a path that resolves outside `targetDir` when concatenated.
    {
        std::unordered_map<std::wstring, bool> already;
        for (const auto& w : extractedWhiteouts) already[ToLower(w)] = true;

        std::error_code targetEc;
        fs::path targetRoot(targetDir);
        fs::path targetCanonical = fs::weakly_canonical(targetRoot, targetEc);
        if (targetEc) targetCanonical = targetRoot;
        const std::wstring rootStr = targetCanonical.wstring();

        for (const auto& logical : metadata.whiteouts) {
            std::string utf8Logical = WideToUtf8(logical);
            if (!IsValidArchivePath(utf8Logical)) return ERROR_BAD_PATHNAME;

            std::wstring marker = MakeWhiteoutMarkerPath(logical);
            if (already.count(ToLower(marker))) continue;

            fs::path target = fs::path(targetDir) / fs::path(marker);
            std::error_code ec;
            fs::path canon = fs::weakly_canonical(target, ec);
            if (ec) return ERROR_BAD_PATHNAME;
            // Component-aware containment (see IsPathContainedIn). A
            // malicious image listing `..\outside` or a sibling whose
            // name extends rootStr cannot land its whiteout marker
            // outside targetDir.
            if (!IsPathContainedIn(canon.wstring(), rootStr)) {
                return ERROR_BAD_PATHNAME;
            }

            if (target.has_parent_path()) {
                fs::create_directories(target.parent_path(), ec);
            }
            std::ofstream whFile(target, std::ios::binary | std::ios::trunc);
            if (!whFile) return ERROR_ACCESS_DENIED;
            whFile.close();
        }
    }

    return ERROR_SUCCESS;
}

// 8.6 — GetImageInfo
DWORD LayerImageManager::GetImageInfo(const std::wstring& imagePath,
                                      LayerImageHeader& header,
                                      LayerMetadata& metadata) {
    std::ifstream file;
    DWORD err = ReadHeaderAndMetadata(imagePath, header, metadata, file);
    file.close();
    return err;
}

DWORD LayerImageManager::ValidateImage(const std::wstring& imagePath) {
    LayerImageHeader header{};
    LayerMetadata    metadata;
    {
        std::ifstream file;
        DWORD err = ReadHeaderAndMetadata(imagePath, header, metadata, file);
        file.close();
        if (err != ERROR_SUCCESS) return err;
    }

    uint8_t computed[32]{};
    DWORD err = ComputeSHA256Stream(imagePath, header.dataOffset,
                                    header.dataSize, computed);
    if (err != ERROR_SUCCESS) return err;
    if (std::memcmp(computed, header.checksum, 32) != 0) return ERROR_CRC;
    return ERROR_SUCCESS;
}

// 8.7 — CreateDifferentialImage
DWORD LayerImageManager::CreateDifferentialImage(
    const std::wstring& sourceDir, const std::wstring& baseDir,
    const std::wstring& outputPath, LayerMetadata& metadata,
    int compressionLevel) {

    if (!fs::exists(sourceDir)) return ERROR_PATH_NOT_FOUND;
    if (!fs::exists(baseDir))   return ERROR_PATH_NOT_FOUND;

    std::vector<FileSnapshot> sourceFiles;
    std::vector<FileSnapshot> baseFiles;
    DWORD err = ScanDirectory(sourceDir, sourceFiles);
    if (err != ERROR_SUCCESS) return err;
    err = ScanDirectory(baseDir, baseFiles);
    if (err != ERROR_SUCCESS) return err;

    // Build lookup: relativePath (lowercased) -> FileSnapshot
    std::unordered_map<std::wstring, const FileSnapshot*> baseMap;
    for (const auto& s : baseFiles) baseMap[s.relativePath] = &s;

    std::unordered_map<std::wstring, const FileSnapshot*> sourceMap;
    for (const auto& s : sourceFiles) sourceMap[s.relativePath] = &s;

    // Classify
    std::vector<const FileSnapshot*> toArchive;  // new + modified (incl. dirs)
    for (const auto& s : sourceFiles) {
        auto it = baseMap.find(s.relativePath);
        if (it == baseMap.end()) {
            toArchive.push_back(&s);                       // new
        } else {
            const FileSnapshot* b = it->second;
            if (s.isDirectory != b->isDirectory) {
                toArchive.push_back(&s);                   // type change
            } else if (!s.isDirectory &&
                       (s.size != b->size || s.modifiedTime != b->modifiedTime)) {
                toArchive.push_back(&s);                   // modified
            }
        }
    }

    std::vector<std::wstring> deletedLogical;
    for (const auto& b : baseFiles) {
        if (sourceMap.find(b.relativePath) == sourceMap.end()) {
            deletedLogical.push_back(b.displayPath);
        }
    }

    // Populate metadata (id/createdAt default if unset, whiteouts)
    if (metadata.id.empty())        metadata.id        = GenerateId();
    if (metadata.createdAt.empty()) metadata.createdAt = GetCurrentTimestampISO8601();
    metadata.whiteouts = deletedLogical;

    // Build archive
    std::vector<uint8_t> archive;
    uint64_t fileCount         = 0;
    uint64_t uncompressedTotal = 0;

    fs::path basePath(sourceDir);
    for (const FileSnapshot* snap : toArchive) {
        fs::path abs = basePath / fs::path(snap->displayPath);

        FileEntryHeader h{};
        DWORD e = FillFileEntry(abs, h, snap->isDirectory);
        if (e != ERROR_SUCCESS) return e;

        std::string utf8Name = WideToUtf8(snap->displayPath);
        if (utf8Name.size() > 0xFFFE) return ERROR_FILENAME_EXCED_RANGE;

        if (snap->isDirectory) {
            e = AppendFileEntry(archive, utf8Name, h, nullptr, uncompressedTotal);
            if (e != ERROR_SUCCESS) return e;
        } else {
            h.isWhiteout = HasWhiteoutPrefix(fs::path(snap->displayPath).filename().wstring()) ? 1 : 0;
            std::vector<uint8_t> bytes;
            if (h.size > 0) {
                e = ReadFileBytes(abs, bytes);
                if (e != ERROR_SUCCESS) return e;
            }
            e = AppendFileEntry(archive, utf8Name, h,
                                bytes.empty() ? nullptr : bytes.data(),
                                uncompressedTotal);
            if (e != ERROR_SUCCESS) return e;
            ++fileCount;
        }
    }

    // Add explicit whiteout entries (materialized marker paths) for deletions
    for (const auto& logicalPath : deletedLogical) {
        std::wstring markerPath = MakeWhiteoutMarkerPath(logicalPath);
        std::string  utf8Marker = WideToUtf8(markerPath);
        if (utf8Marker.size() > 0xFFFE) return ERROR_FILENAME_EXCED_RANGE;

        FileEntryHeader h{};
        h.size        = 0;
        h.attributes  = FILE_ATTRIBUTE_NORMAL;
        h.modified    = 0;
        h.isDirectory = 0;
        h.isWhiteout  = 1;
        DWORD e = AppendFileEntry(archive, utf8Marker, h, nullptr, uncompressedTotal);
        if (e != ERROR_SUCCESS) return e;
        ++fileCount;
    }

    AppendEndMarker(archive);
    metadata.fileCount = fileCount;

    DWORD writeErr = WriteImageFile(outputPath, metadata, archive, compressionLevel);
    if (writeErr != ERROR_SUCCESS) {
        std::error_code ec;
        fs::remove(outputPath, ec);
        return writeErr;
    }
    return ERROR_SUCCESS;
}

// 8.8 — CreateManifest
DWORD LayerImageManager::CreateManifest(
    const std::wstring& outputPath,
    const std::vector<std::wstring>& layerImagePaths) {

    nlohmann::json j;
    j["schemaVersion"] = 1;
    nlohmann::json layers = nlohmann::json::array();

    for (const auto& imagePath : layerImagePaths) {
        std::ifstream file(imagePath, std::ios::binary);
        if (!file) return ERROR_FILE_NOT_FOUND;
        LayerImageHeader header{};
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (file.gcount() != sizeof(header)) return ERROR_INVALID_DATA;
        if (std::memcmp(header.magic, LAYER_IMAGE_MAGIC, 8) != 0) {
            return ERROR_INVALID_DATA;
        }

        nlohmann::json entry;
        entry["path"]   = WideToUtf8(imagePath);
        entry["sha256"] = WideToUtf8(HashToHexString(header.checksum));
        layers.push_back(entry);
    }
    j["layers"] = layers;

    fs::path outPath(outputPath);
    if (outPath.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(outPath.parent_path(), ec);
    }
    std::ofstream out(outputPath);
    if (!out) return ERROR_ACCESS_DENIED;
    out << j.dump(2);
    if (!out) return ERROR_WRITE_FAULT;
    return ERROR_SUCCESS;
}

// 8.8 — LoadManifest
DWORD LayerImageManager::LoadManifest(const std::wstring& manifestPath,
                                      LayerManifest& manifest) {
    std::ifstream in(manifestPath);
    if (!in) return ERROR_FILE_NOT_FOUND;

    nlohmann::json j;
    try {
        in >> j;
    } catch (const nlohmann::json::exception&) {
        return ERROR_INVALID_DATA;
    }

    int schemaVersion = j.value("schemaVersion", 0);
    if (schemaVersion < 1) return ERROR_INVALID_DATA;
    manifest.schemaVersion = static_cast<uint32_t>(schemaVersion);

    manifest.layers.clear();
    if (j.contains("layers") && j["layers"].is_array()) {
        for (const auto& entry : j["layers"]) {
            LayerManifestEntry e;
            e.imagePath   = Utf8ToWide(entry.value("path", ""));
            e.checksumHex = Utf8ToWide(entry.value("sha256", ""));
            manifest.layers.push_back(std::move(e));
        }
    }
    return ERROR_SUCCESS;
}

} // namespace LayerMount::LayerImage
