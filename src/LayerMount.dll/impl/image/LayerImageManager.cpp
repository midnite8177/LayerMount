#include "../WindowsNtStatus.h"
#include "../LayerMount.h"
#include "../NtStatusUtil.h"
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
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace LayerMount::LayerImage {

namespace fs = std::filesystem;

static DWORD NtStatusToDword(NTSTATUS status) {
    if (status == STATUS_SUCCESS) return ERROR_SUCCESS;
    switch (status) {
        case STATUS_INVALID_PARAMETER: return ERROR_INVALID_PARAMETER;
        case STATUS_INVALID_HANDLE:    return ERROR_INVALID_HANDLE;
        case STATUS_NO_MEMORY:         return ERROR_NOT_ENOUGH_MEMORY;
        default:                       return ERROR_INVALID_FUNCTION;
    }
}

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

static std::wstring GetCurrentTimestampISO8601() {
    SYSTEMTIME st{};
    ::GetSystemTime(&st);
    wchar_t buf[32]{};
    ::swprintf_s(buf, L"%04u-%02u-%02uT%02u:%02u:%02uZ",
                 st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond);
    return std::wstring(buf);
}

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
    if (status != STATUS_SUCCESS) return NtStatusToDword(status);

    DWORD hashObjSize = 0;
    DWORD cbData      = 0;
    status = ::BCryptGetProperty(ctx.hAlg, BCRYPT_OBJECT_LENGTH,
                                 reinterpret_cast<PUCHAR>(&hashObjSize),
                                 sizeof(hashObjSize), &cbData, 0);
    if (status != STATUS_SUCCESS) return NtStatusToDword(status);

    ctx.hashObject.resize(hashObjSize);

    status = ::BCryptCreateHash(ctx.hAlg, &ctx.hHash,
                                ctx.hashObject.data(), hashObjSize,
                                nullptr, 0, 0);
    if (status != STATUS_SUCCESS) return NtStatusToDword(status);

    return ERROR_SUCCESS;
}

static DWORD ComputeSHA256Stream(const HostPath& filePath,
                                 uint64_t offset, uint64_t size,
                                 uint8_t outHash[32]) {
    std::ifstream file(filePath.ForWin32(), std::ios::binary);
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
        if (status != STATUS_SUCCESS) return NtStatusToDword(status);
        remaining -= chunk;
    }

    NTSTATUS status = ::BCryptFinishHash(ctx.hHash, outHash, 32, 0);
    return NtStatusToDword(status);
}

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
        if (status != STATUS_SUCCESS) return NtStatusToDword(status);

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

static bool IsValidUtf8(const char* data, size_t len) {
    for (size_t i = 0; i < len; ) {
        uint8_t c = static_cast<uint8_t>(data[i]);
        if (c == 0) return false;
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

// Returns true when `canon` equals `root` or is under `root` at a path
// component boundary. A plain prefix compare would accept `C:\out2\file`
// under the root `C:\out`.
static bool IsPathContainedIn(const std::wstring& canon,
                              const std::wstring& root) {
    if (canon == root) return true;
    if (root.empty()) return true;
    if (canon.size() <= root.size()) return false;
    if (canon.compare(0, root.size(), root) != 0) return false;
    const wchar_t rootLast = root.back();
    if (rootLast == L'\\' || rootLast == L'/') return true;
    const wchar_t boundary = canon[root.size()];
    return boundary == L'\\' || boundary == L'/';
}

// Rejects an absolute path, an empty, `.` or `..` component, and a colon
// anywhere. A colon names a drive or an alternate data stream.
static bool IsValidArchivePath(const std::string& utf8Path) {
    if (utf8Path.empty()) return false;
    if (utf8Path.front() == '/' || utf8Path.front() == '\\') return false;
    if (utf8Path.find(':') != std::string::npos) return false;

    size_t start = 0;
    for (size_t i = 0; i <= utf8Path.size(); ++i) {
        bool boundary = (i == utf8Path.size()) ||
                        utf8Path[i] == '/' || utf8Path[i] == '\\';
        if (boundary) {
            size_t len = i - start;
            if (len == 0) return false;
            std::string_view component(utf8Path.data() + start, len);
            if (component == "." || component == "..") return false;
            start = i + 1;
        }
    }
    return true;
}

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

// The frame header gives the content size, and the frame comes from the
// image file, so the size is untrusted. The cap bounds the allocation that
// the size drives.
constexpr uint64_t kMaxDecompressedBytes = 4ULL * 1024 * 1024 * 1024;

static DWORD DecompressBuffer(const uint8_t* input, size_t inputSize,
                              std::vector<uint8_t>& output) {
    unsigned long long frameSize = ::ZSTD_getFrameContentSize(input, inputSize);
    if (frameSize == ZSTD_CONTENTSIZE_ERROR ||
        frameSize == ZSTD_CONTENTSIZE_UNKNOWN) {
        return ERROR_INVALID_DATA;
    }
    if (frameSize > kMaxDecompressedBytes) return ERROR_INVALID_DATA;

    output.resize(static_cast<size_t>(frameSize));
    size_t result = ::ZSTD_decompress(output.data(), output.size(),
                                      input, inputSize);
    if (::ZSTD_isError(result)) return ERROR_INVALID_DATA;
    if (result != frameSize) return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}

static void AppendBytes(std::vector<uint8_t>& buf, const void* src, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(src);
    buf.insert(buf.end(), p, p + n);
}

// Uses forward slashes, as an archive name does.
static std::wstring RelativeNameOf(const fs::path& absolute, const fs::path& base) {
    std::wstring name = fs::relative(absolute, base).wstring();
    std::replace(name.begin(), name.end(), L'\\', L'/');
    return name;
}

// Joins relativePath, an archive name or a whiteout marker path, onto root,
// an extended path. Windows does not read a forward slash in an extended
// path as a separator, so the join converts each one to a backslash.
static fs::path UnderExtendedRoot(const fs::path& root, const std::wstring& relativePath) {
    return root / fs::path(relativePath).make_preferred();
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

static bool HasWhiteoutPrefix(const std::wstring& filename) {
    return filename.size() >= 4 && filename.compare(0, 4, L".wh.") == 0;
}

static std::wstring ToLower(const std::wstring& s) {
    std::wstring r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return r;
}

// An unpack writes no entry whose first segment opens as the root sidecar,
// as the whiteout marker of the root sidecar, or as an existing entry in
// the target whose long name is the root sidecar.
static bool UnpackSkipsEntry(const std::wstring& relativePath,
                             const fs::path& targetRoot) {
    const std::wstring firstSegment = FirstSegmentAsOpened(relativePath);
    if (firstSegment.empty()) return false;
    if (IsRootSidecarPath(firstSegment)) return true;
    if (HasWhiteoutPrefix(firstSegment) &&
        IsRootSidecarPath(firstSegment.substr(std::wstring_view(kWhiteoutPrefix).size()))) {
        return true;
    }
    const std::optional<std::wstring> longName = ExistingLongName(targetRoot, firstSegment);
    return longName && FirstSegmentOpensAsSidecar(*longName);
}

// Moves `it` past the root sidecar and everything under it. Returns false
// and leaves `it` unchanged when `it` is not on the root sidecar.
static bool StepPastRootSidecar(fs::recursive_directory_iterator& it,
                                std::error_code& ec) {
    if (it.depth() != 0 ||
        !FirstSegmentOpensAsSidecar(it->path().filename().wstring())) {
        return false;
    }
    it.disable_recursion_pending();
    it.increment(ec);
    return true;
}

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

namespace {

class ArchiveBuilder {
public:
    DWORD AddDirectory(const std::wstring& relativePath, const FileEntryHeader& entry) {
        return Append(relativePath, entry, {});
    }

    // Reads the file at path now, and records the size of what it read, so a
    // file that changed size after the scan still gives a well-formed entry.
    DWORD AddFile(const fs::path& path, const std::wstring& relativePath,
                  FileEntryHeader entry) {
        std::vector<uint8_t> bytes;
        if (entry.size > 0) {
            const DWORD err = ReadFileBytes(path, bytes);
            if (err != ERROR_SUCCESS) return err;
        }
        entry.size = bytes.size();
        entry.isWhiteout = HasWhiteoutPrefix(fs::path(relativePath).filename().wstring()) ? 1 : 0;
        return AppendCounted(relativePath, entry, bytes);
    }

    DWORD AddWhiteoutMarker(const std::wstring& deletedPath) {
        FileEntryHeader entry{};
        entry.attributes = FILE_ATTRIBUTE_NORMAL;
        entry.isWhiteout = 1;
        return AppendCounted(MakeWhiteoutMarkerPath(deletedPath), entry, {});
    }

    uint64_t FileCount() const { return fileCount_; }

    // Appends the end marker and moves the archive out. The builder is empty
    // afterwards.
    std::vector<uint8_t> Finish() {
        FileEntryHeader end{};
        end.nameLength = ARCHIVE_END_MARKER;
        AppendBytes(archive_, &end, sizeof(end));
        return std::move(archive_);
    }

private:
    DWORD AppendCounted(const std::wstring& relativePath, const FileEntryHeader& entry,
                        const std::vector<uint8_t>& data) {
        const DWORD err = Append(relativePath, entry, data);
        if (err == ERROR_SUCCESS) ++fileCount_;
        return err;
    }

    DWORD Append(const std::wstring& relativePath, FileEntryHeader entry,
                 const std::vector<uint8_t>& data) {
        const std::string utf8Name = WideToUtf8(relativePath);
        if (utf8Name.empty()) return ERROR_INVALID_PARAMETER;
        if (utf8Name.size() > 0xFFFE) return ERROR_FILENAME_EXCED_RANGE;
        entry.nameLength = static_cast<uint16_t>(utf8Name.size());
        AppendBytes(archive_, &entry, sizeof(entry));
        AppendBytes(archive_, utf8Name.data(), utf8Name.size());
        AppendBytes(archive_, data.data(), data.size());
        return ERROR_SUCCESS;
    }

    std::vector<uint8_t> archive_;
    uint64_t fileCount_ = 0;
};

struct FileSnapshot {
    std::wstring lookupKey;
    std::wstring relativePath;
    FileEntryHeader entry;
};

}

// Lists every entry under dir except the root sidecar, in walk order. The
// walk starts on the extended form of dir, so each entry path that it gives
// is extended too, and an entry past MAX_PATH still opens.
static DWORD ScanDirectory(const HostPath& dir, std::vector<FileSnapshot>& out) {
    const fs::path base(dir.ForWin32());
    if (!fs::exists(base)) return ERROR_PATH_NOT_FOUND;

    std::error_code ec;
    auto it = fs::recursive_directory_iterator(
        base, fs::directory_options::none, ec);
    if (ec) return ERROR_ACCESS_DENIED;

    const auto end = fs::recursive_directory_iterator();
    while (it != end) {
        if (StepPastRootSidecar(it, ec)) {
            if (ec) return ERROR_ACCESS_DENIED;
            continue;
        }
        const bool isDirectory = it->is_directory(ec);
        if (ec) return ERROR_ACCESS_DENIED;

        FileSnapshot snap;
        snap.relativePath = RelativeNameOf(it->path(), base);
        snap.lookupKey    = ToLower(snap.relativePath);
        const DWORD err = FillFileEntry(it->path(), snap.entry, isDirectory);
        if (err != ERROR_SUCCESS) return err;
        out.push_back(std::move(snap));

        it.increment(ec);
        if (ec) return ERROR_ACCESS_DENIED;
    }
    return ERROR_SUCCESS;
}

static DWORD PackEntries(const HostPath& root,
                         const std::vector<const FileSnapshot*>& entries,
                         ArchiveBuilder& builder) {
    const fs::path base(root.ForWin32());
    for (const FileSnapshot* snap : entries) {
        const DWORD err = snap->entry.isDirectory
            ? builder.AddDirectory(snap->relativePath, snap->entry)
            : builder.AddFile(UnderExtendedRoot(base, snap->relativePath),
                              snap->relativePath, snap->entry);
        if (err != ERROR_SUCCESS) return err;
    }
    return ERROR_SUCCESS;
}

static bool Differs(const FileEntryHeader& source, const FileEntryHeader& base) {
    if (source.isDirectory != base.isDirectory) return true;
    return !source.isDirectory &&
           (source.size != base.size || source.modified != base.modified);
}

static std::vector<const FileSnapshot*> ChangedEntries(
    const std::vector<FileSnapshot>& source, const std::vector<FileSnapshot>& base) {
    std::unordered_map<std::wstring, const FileSnapshot*> baseByKey;
    for (const auto& b : base) baseByKey[b.lookupKey] = &b;

    std::vector<const FileSnapshot*> changed;
    for (const auto& s : source) {
        const auto match = baseByKey.find(s.lookupKey);
        if (match == baseByKey.end() || Differs(s.entry, match->second->entry)) {
            changed.push_back(&s);
        }
    }
    return changed;
}

static std::vector<std::wstring> DeletedPaths(
    const std::vector<FileSnapshot>& source, const std::vector<FileSnapshot>& base) {
    std::unordered_set<std::wstring> sourceKeys;
    for (const auto& s : source) sourceKeys.insert(s.lookupKey);

    std::vector<std::wstring> deleted;
    for (const auto& b : base) {
        if (sourceKeys.count(b.lookupKey) == 0) deleted.push_back(b.relativePath);
    }
    return deleted;
}

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

static DWORD WriteImageFile(const ImageOutput& output,
                            LayerMetadata& metadata,
                            const std::vector<uint8_t>& archive) {
    const fs::path outPath(output.path.ForWin32());
    if (outPath.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(outPath.parent_path(), ec);
        if (ec) return Win32FromErrorCode(ec);
    }

    std::vector<uint8_t> compressed;
    DWORD err = CompressBuffer(archive, compressed, output.compressionLevel);
    if (err != ERROR_SUCCESS) return err;

    metadata.compression      = CompressionType::Zstd;
    metadata.uncompressedSize = archive.size();
    metadata.compressedSize   = compressed.size();

    std::string metadataJson = SerializeMetadata(metadata);
    metadataJson.push_back('\0');

    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out) return ERROR_ACCESS_DENIED;

    LayerImageHeader header{};
    header.version = LAYER_IMAGE_VERSION;
    header.flags   = static_cast<uint32_t>(CompressionType::Zstd);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!out) return ERROR_WRITE_FAULT;

    header.metadataOffset = sizeof(header);
    header.metadataSize   = metadataJson.size();
    out.write(metadataJson.data(),
              static_cast<std::streamsize>(metadataJson.size()));
    if (!out) return ERROR_WRITE_FAULT;

    header.dataOffset = header.metadataOffset + header.metadataSize;
    header.dataSize   = compressed.size();
    err = WriteDataWithHash(out, compressed.data(), compressed.size(),
                            header.checksum);
    if (err != ERROR_SUCCESS) return err;

    out.seekp(0);
    if (!out) return ERROR_SEEK;
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!out) return ERROR_WRITE_FAULT;

    out.close();
    return ERROR_SUCCESS;
}

// Removes the partial image file when the write fails.
static DWORD WriteImage(const ImageOutput& output, LayerMetadata& metadata,
                        const std::vector<uint8_t>& archive) {
    const DWORD err = WriteImageFile(output, metadata, archive);
    if (err != ERROR_SUCCESS) {
        std::error_code ec;
        fs::remove(output.path.ForWin32(), ec);
    }
    return err;
}

// Writes the `entry.size` bytes at `fileData` to `target`, and then gives
// the file the attributes and the last-write time from `entry`.
static DWORD WriteExtractedFile(const fs::path& target,
                                const FileEntryHeader& entry,
                                const uint8_t* fileData) {
    std::error_code ec;
    if (target.has_parent_path()) {
        fs::create_directories(target.parent_path(), ec);
    }
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) return ERROR_ACCESS_DENIED;
    if (entry.size > 0) {
        out.write(reinterpret_cast<const char*>(fileData),
                  static_cast<std::streamsize>(entry.size));
        if (!out) return ERROR_WRITE_FAULT;
    }
    out.close();

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
    return ERROR_SUCCESS;
}

namespace {

struct ArchiveEntry {
    FileEntryHeader header;
    std::wstring name;
    const uint8_t* data;
};

struct UnpackTarget {
    fs::path root;
    std::wstring canonicalRoot;
};

}

// Reads the entry at `pos` and moves `pos` past it. Sets `entry` to
// std::nullopt at the end marker.
static DWORD ReadArchiveEntry(const std::vector<uint8_t>& archive, size_t& pos,
                              std::optional<ArchiveEntry>& entry) {
    if (pos + sizeof(FileEntryHeader) > archive.size()) return ERROR_INVALID_DATA;
    FileEntryHeader header;
    std::memcpy(&header, archive.data() + pos, sizeof(header));
    pos += sizeof(header);

    if (header.nameLength == ARCHIVE_END_MARKER) {
        entry.reset();
        return ERROR_SUCCESS;
    }
    if (header.nameLength == 0) return ERROR_INVALID_DATA;
    if (pos + header.nameLength > archive.size()) return ERROR_INVALID_DATA;

    const char* namePtr = reinterpret_cast<const char*>(archive.data() + pos);
    if (!IsValidUtf8(namePtr, header.nameLength)) return ERROR_INVALID_DATA;
    const std::string utf8Name(namePtr, header.nameLength);
    pos += header.nameLength;

    if (!IsValidArchivePath(utf8Name)) return ERROR_BAD_PATHNAME;
    if (header.size > archive.size() - pos) return ERROR_INVALID_DATA;

    const uint8_t* data = archive.data() + pos;
    if (!header.isDirectory) pos += static_cast<size_t>(header.size);
    entry = ArchiveEntry{header, Utf8ToWide(utf8Name), data};
    return ERROR_SUCCESS;
}

// Creates targetDir when it is missing. A target root that does not
// canonicalize is an error, because the containment check compares the root
// and each entry in the form that canonicalization gives.
static DWORD ResolveUnpackTarget(const HostPath& targetDir, UnpackTarget& target) {
    target.root = fs::path(targetDir.ForWin32());
    std::error_code ec;
    fs::create_directories(target.root, ec);
    const fs::path canonical = fs::weakly_canonical(target.root, ec);
    if (ec) return ERROR_BAD_PATHNAME;
    target.canonicalRoot = canonical.wstring();
    return ERROR_SUCCESS;
}

// Refuses a relativePath that canonicalizes outside the target root, such as
// `a/../../outside`.
static DWORD JoinUnderTarget(const UnpackTarget& target, const std::wstring& relativePath,
                             fs::path& path) {
    path = UnderExtendedRoot(target.root, relativePath);
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(path, ec);
    if (ec) return ERROR_BAD_PATHNAME;
    if (!IsPathContainedIn(canonical.wstring(), target.canonicalRoot)) {
        return ERROR_BAD_PATHNAME;
    }
    return ERROR_SUCCESS;
}

static DWORD ExtractArchive(const std::vector<uint8_t>& archive, const UnpackTarget& target,
                            std::vector<std::wstring>& extractedWhiteouts) {
    size_t pos = 0;
    while (true) {
        std::optional<ArchiveEntry> entry;
        DWORD err = ReadArchiveEntry(archive, pos, entry);
        if (err != ERROR_SUCCESS) return err;
        if (!entry) return ERROR_SUCCESS;
        if (UnpackSkipsEntry(entry->name, target.root)) continue;

        fs::path path;
        err = JoinUnderTarget(target, entry->name, path);
        if (err != ERROR_SUCCESS) return err;

        if (entry->header.isDirectory) {
            std::error_code ec;
            fs::create_directories(path, ec);
            continue;
        }
        err = WriteExtractedFile(path, entry->header, entry->data);
        if (err != ERROR_SUCCESS) return err;
        if (entry->header.isWhiteout) extractedWhiteouts.push_back(entry->name);
    }
}

// Writes a whiteout marker for each path in `whiteouts` that the archive
// did not already hold as a whiteout entry. A path in `whiteouts` gets the
// same path check and containment check as an archive entry, so an image
// that lists `../outside` writes no marker outside the target.
static DWORD MaterializeMetadataWhiteouts(const std::vector<std::wstring>& whiteouts,
                                          const std::vector<std::wstring>& extractedWhiteouts,
                                          const UnpackTarget& target) {
    std::unordered_set<std::wstring> already;
    for (const auto& w : extractedWhiteouts) already.insert(ToLower(w));

    for (const auto& logical : whiteouts) {
        if (!IsValidArchivePath(WideToUtf8(logical))) return ERROR_BAD_PATHNAME;

        const std::wstring marker = MakeWhiteoutMarkerPath(logical);
        if (already.count(ToLower(marker)) != 0) continue;
        if (UnpackSkipsEntry(marker, target.root)) continue;

        fs::path path;
        const DWORD err = JoinUnderTarget(target, marker, path);
        if (err != ERROR_SUCCESS) return err;

        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream whFile(path, std::ios::binary | std::ios::trunc);
        if (!whFile) return ERROR_ACCESS_DENIED;
    }
    return ERROR_SUCCESS;
}

static DWORD ReadHeaderAndMetadata(const HostPath& imagePath,
                                   LayerImageHeader& header,
                                   LayerMetadata& metadata,
                                   std::ifstream& fileHandle) {
    fileHandle.open(imagePath.ForWin32(), std::ios::binary);
    if (!fileHandle) return ERROR_FILE_NOT_FOUND;

    fileHandle.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!fileHandle || fileHandle.gcount() != sizeof(header)) {
        return ERROR_INVALID_DATA;
    }

    if (std::memcmp(header.magic, LAYER_IMAGE_MAGIC, 8) != 0) {
        return ERROR_INVALID_DATA;
    }
    if (header.version > LAYER_IMAGE_VERSION) return ERROR_REVISION_MISMATCH;

    if (header.metadataOffset < sizeof(header)) return ERROR_INVALID_DATA;
    if (header.dataOffset < header.metadataOffset + header.metadataSize) {
        return ERROR_INVALID_DATA;
    }
    if (header.metadataOffset + header.metadataSize < header.metadataOffset) {
        return ERROR_INVALID_DATA;
    }
    if (header.dataOffset + header.dataSize < header.dataOffset) {
        return ERROR_INVALID_DATA;
    }

    fileHandle.seekg(static_cast<std::streamoff>(header.metadataOffset));
    if (!fileHandle) return ERROR_SEEK;

    std::string metadataStr(static_cast<size_t>(header.metadataSize), '\0');
    fileHandle.read(metadataStr.data(),
                    static_cast<std::streamsize>(header.metadataSize));
    if (fileHandle.gcount() != static_cast<std::streamsize>(header.metadataSize)) {
        return ERROR_READ_FAULT;
    }
    while (!metadataStr.empty() && metadataStr.back() == '\0') {
        metadataStr.pop_back();
    }

    return DeserializeMetadata(metadataStr, metadata);
}

static void StampIdentity(LayerMetadata& metadata) {
    if (metadata.id.empty())        metadata.id        = GenerateId();
    if (metadata.createdAt.empty()) metadata.createdAt = GetCurrentTimestampISO8601();
}

DWORD LayerImageManager::CreateImage(const HostPath& sourceDir,
                                     const ImageOutput& output,
                                     LayerMetadata& metadata) {
    std::vector<FileSnapshot> sourceEntries;
    DWORD err = ScanDirectory(sourceDir, sourceEntries);
    if (err != ERROR_SUCCESS) return err;

    std::vector<const FileSnapshot*> everyEntry(sourceEntries.size());
    std::transform(sourceEntries.begin(), sourceEntries.end(), everyEntry.begin(),
                   [](const FileSnapshot& s) { return &s; });
    ArchiveBuilder builder;
    err = PackEntries(sourceDir, everyEntry, builder);
    if (err != ERROR_SUCCESS) return err;

    StampIdentity(metadata);
    metadata.fileCount = builder.FileCount();
    return WriteImage(output, metadata, builder.Finish());
}

DWORD LayerImageManager::ExtractImage(const HostPath& imagePath,
                                      const HostPath& targetDir,
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

    std::vector<uint8_t> archive;
    err = DecompressBuffer(compressed.data(), compressed.size(), archive);
    if (err != ERROR_SUCCESS) return err;

    UnpackTarget target;
    err = ResolveUnpackTarget(targetDir, target);
    if (err != ERROR_SUCCESS) return err;

    std::vector<std::wstring> extractedWhiteouts;
    err = ExtractArchive(archive, target, extractedWhiteouts);
    if (err != ERROR_SUCCESS) return err;

    return MaterializeMetadataWhiteouts(metadata.whiteouts, extractedWhiteouts, target);
}

DWORD LayerImageManager::GetImageInfo(const HostPath& imagePath,
                                      LayerImageHeader& header,
                                      LayerMetadata& metadata) {
    std::ifstream file;
    DWORD err = ReadHeaderAndMetadata(imagePath, header, metadata, file);
    file.close();
    return err;
}

DWORD LayerImageManager::ValidateImage(const HostPath& imagePath) {
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

DWORD LayerImageManager::CreateDifferentialImage(const HostPath& sourceDir,
                                                 const HostPath& baseDir,
                                                 const ImageOutput& output,
                                                 LayerMetadata& metadata) {
    std::vector<FileSnapshot> sourceEntries;
    DWORD err = ScanDirectory(sourceDir, sourceEntries);
    if (err != ERROR_SUCCESS) return err;
    std::vector<FileSnapshot> baseEntries;
    err = ScanDirectory(baseDir, baseEntries);
    if (err != ERROR_SUCCESS) return err;

    ArchiveBuilder builder;
    err = PackEntries(sourceDir, ChangedEntries(sourceEntries, baseEntries), builder);
    if (err != ERROR_SUCCESS) return err;
    const std::vector<std::wstring> deleted = DeletedPaths(sourceEntries, baseEntries);
    for (const auto& path : deleted) {
        err = builder.AddWhiteoutMarker(path);
        if (err != ERROR_SUCCESS) return err;
    }

    StampIdentity(metadata);
    metadata.whiteouts = deleted;
    metadata.fileCount = builder.FileCount();
    return WriteImage(output, metadata, builder.Finish());
}

DWORD LayerImageManager::CreateManifest(
    const HostPath& outputPath,
    const std::vector<HostPath>& layerImagePaths) {

    nlohmann::json j;
    j["schemaVersion"] = 1;
    nlohmann::json layers = nlohmann::json::array();

    for (const auto& imagePath : layerImagePaths) {
        std::ifstream file(imagePath.ForWin32(), std::ios::binary);
        if (!file) return ERROR_FILE_NOT_FOUND;
        LayerImageHeader header{};
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (file.gcount() != sizeof(header)) return ERROR_INVALID_DATA;
        if (std::memcmp(header.magic, LAYER_IMAGE_MAGIC, 8) != 0) {
            return ERROR_INVALID_DATA;
        }

        nlohmann::json entry;
        entry["path"]   = WideToUtf8(imagePath.Text());
        entry["sha256"] = WideToUtf8(HashToHexString(header.checksum));
        layers.push_back(entry);
    }
    j["layers"] = layers;

    const fs::path outPath(outputPath.ForWin32());
    if (outPath.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(outPath.parent_path(), ec);
        if (ec) return Win32FromErrorCode(ec);
    }
    std::ofstream out(outPath);
    if (!out) return ERROR_ACCESS_DENIED;
    out << j.dump(2);
    if (!out) return ERROR_WRITE_FAULT;
    return ERROR_SUCCESS;
}

DWORD LayerImageManager::LoadManifest(const HostPath& manifestPath,
                                      LayerManifest& manifest) {
    std::ifstream in(manifestPath.ForWin32());
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

}
