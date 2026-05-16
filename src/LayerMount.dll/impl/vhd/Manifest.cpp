#include "Manifest.h"
#include "VHDLayerManager.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <filesystem>
#include <sstream>

namespace LayerMount::VHD {

namespace {

std::wstring NormalizeManifestPath(const std::wstring& path) {
    std::wstring out;
    out.reserve(path.size());
    for (wchar_t c : path) {
        if (c == L'/') c = L'\\';
        out.push_back(static_cast<wchar_t>(::towlower(c)));
    }
    return out;
}

// FNV-1a 64-bit — stable across processes and CRT versions, unlike std::hash.
uint64_t Fnv1a64(const std::wstring& s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (wchar_t c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 0x100000001b3ULL;
    }
    return h;
}

std::wstring MutexNameForPath(const std::wstring& manifestPath) {
    wchar_t buf[64];
    swprintf_s(buf, 64, L"LayerMount.Manifest.%016llx",
               static_cast<unsigned long long>(
                   Fnv1a64(NormalizeManifestPath(manifestPath))));
    return buf;
}

} // namespace

// ===========================================================================
// ManifestLock — cross-process serialization of Load-mutate-Save sequences.
// ===========================================================================

ManifestLock::ManifestLock(const std::wstring& manifestPath, DWORD timeoutMs) {
    const std::wstring name = MutexNameForPath(manifestPath);
    handle_ = ::CreateMutexW(nullptr, FALSE, name.c_str());
    if (handle_ == nullptr) return;

    DWORD wait = ::WaitForSingleObject(handle_, timeoutMs);
    // WAIT_OBJECT_0 or WAIT_ABANDONED both mean we own it — abandoned means
    // the previous holder crashed without releasing, and the state is now ours.
    held_ = (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED);
}

ManifestLock::~ManifestLock() {
    if (held_ && handle_) {
        ::ReleaseMutex(handle_);
    }
    if (handle_) {
        ::CloseHandle(handle_);
    }
}

ManifestLock::ManifestLock(ManifestLock&& other) noexcept
    : handle_(other.handle_), held_(other.held_) {
    other.handle_ = nullptr;
    other.held_ = false;
}

ManifestLock& ManifestLock::operator=(ManifestLock&& other) noexcept {
    if (this != &other) {
        if (held_ && handle_) ::ReleaseMutex(handle_);
        if (handle_) ::CloseHandle(handle_);
        handle_ = other.handle_;
        held_   = other.held_;
        other.handle_ = nullptr;
        other.held_   = false;
    }
    return *this;
}

// ===========================================================================
// JSON serialization helpers
// ===========================================================================

static std::string LayerTypeToString(LayerType type) {
    switch (type) {
        case LayerType::Directory: return "directory";
        case LayerType::VHD:       return "vhd";
        case LayerType::VSS:       return "vss";
        default:                   return "directory";
    }
}

static LayerType StringToLayerType(const std::string& s) {
    if (s == "vhd") return LayerType::VHD;
    if (s == "vss") return LayerType::VSS;
    return LayerType::Directory;
}

static nlohmann::json LayerEntryToJson(const LayerEntry& entry) {
    nlohmann::json j;
    j["id"]          = WideToUtf8(entry.id);
    j["type"]        = LayerTypeToString(entry.type);
    j["path"]        = WideToUtf8(entry.path);
    j["parentId"]    = WideToUtf8(entry.parentId);
    j["mountStatus"] = WideToUtf8(entry.mountStatus);
    j["volumeGuid"]  = WideToUtf8(entry.volumeGuid);
    j["createdAt"]   = WideToUtf8(entry.createdAt);

    nlohmann::json meta = nlohmann::json::object();
    for (const auto& [k, v] : entry.metadata) {
        meta[WideToUtf8(k)] = WideToUtf8(v);
    }
    j["metadata"] = meta;

    return j;
}

static LayerEntry JsonToLayerEntry(const nlohmann::json& j) {
    LayerEntry entry;
    entry.id          = Utf8ToWide(j.value("id", ""));
    entry.type        = StringToLayerType(j.value("type", "directory"));
    entry.path        = Utf8ToWide(j.value("path", ""));
    entry.parentId    = Utf8ToWide(j.value("parentId", ""));
    // Transient fields reset on load
    entry.mountStatus = L"detached";
    entry.volumeGuid.clear();
    entry.createdAt   = Utf8ToWide(j.value("createdAt", ""));

    if (j.contains("metadata") && j["metadata"].is_object()) {
        for (auto& [k, v] : j["metadata"].items()) {
            if (v.is_string()) {
                entry.metadata[Utf8ToWide(k)] = Utf8ToWide(v.get<std::string>());
            }
        }
    }

    return entry;
}

// ===========================================================================
// Manifest::DefaultPath — canonical manifest location under a working dir
// ===========================================================================

std::wstring Manifest::DefaultPath(const std::wstring& workingDir) {
    return (std::filesystem::path(workingDir) / DefaultFileName()).wstring();
}

// ===========================================================================
// 4.9 — Manifest::Load
// ===========================================================================

DWORD Manifest::Load(const std::wstring& manifestPath) {
    // Use Win32 CreateFileW instead of std::ifstream so the specific open
    // failure survives: ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND,
    // ERROR_ACCESS_DENIED, ERROR_SHARING_VIOLATION, and transient I/O
    // errors are all distinct conditions the caller needs to distinguish.
    // The previous ifstream path collapsed every failure to
    // ERROR_FILE_NOT_FOUND, which caused lazy-load callers to treat a
    // temporarily-locked manifest as "missing" and recreate it on top.
    HANDLE fh = ::CreateFileW(manifestPath.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (fh == INVALID_HANDLE_VALUE) {
        DWORD err = ::GetLastError();
        return err ? err : ERROR_FILE_NOT_FOUND;
    }

    // Read the whole file. Manifests are small (O(KiB)), so a plain
    // read-all loop is fine. Keep reading until ReadFile reports EOF
    // (read == 0); bail out on any read failure with the concrete error.
    std::string body;
    for (;;) {
        char chunk[8192];
        DWORD read = 0;
        BOOL ok = ::ReadFile(fh, chunk, sizeof(chunk), &read, nullptr);
        if (!ok) {
            DWORD err = ::GetLastError();
            ::CloseHandle(fh);
            return err ? err : ERROR_READ_FAULT;
        }
        if (read == 0) break;
        body.append(chunk, read);
    }
    ::CloseHandle(fh);

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(body);
    } catch (const nlohmann::json::exception&) {
        return ERROR_INVALID_DATA;
    }

    // Check schema version
    int version = root.value("schemaVersion", 0);
    if (version < 1) return ERROR_INVALID_DATA;

    layers_.clear();

    if (root.contains("layers") && root["layers"].is_array()) {
        for (const auto& item : root["layers"]) {
            LayerEntry entry = JsonToLayerEntry(item);
            if (!entry.id.empty()) {
                layers_[entry.id] = std::move(entry);
            }
        }
    }

    return ERROR_SUCCESS;
}

// ===========================================================================
// 4.9 — Manifest::Save
// ===========================================================================

DWORD Manifest::Save(const std::wstring& manifestPath) const {
    nlohmann::json root;
    root["schemaVersion"] = 1;

    nlohmann::json layerArray = nlohmann::json::array();
    for (const auto& [id, entry] : layers_) {
        layerArray.push_back(LayerEntryToJson(entry));
    }
    root["layers"] = layerArray;

    // Ensure parent directory exists. The throwing overload of
    // create_directories can raise filesystem_error on access-denied /
    // path-too-long / device-not-ready conditions; since Save() returns
    // DWORD, that exception would tunnel out and violate the ABI's
    // exception-safe contract (managed callers would see an unexpected
    // crash rather than a structured Win32 error). Use the error_code
    // overload and translate failures back to Win32.
    std::filesystem::path parentDir = std::filesystem::path(manifestPath).parent_path();
    if (!parentDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parentDir, ec);
        if (ec) {
            if (ec.category() == std::system_category()) {
                return static_cast<DWORD>(ec.value());
            }
            return ERROR_PATH_NOT_FOUND;
        }
    }

    // Atomic write: stream to a sibling tmp file, then MoveFileExW with
    // REPLACE_EXISTING. Readers never observe a partial file.
    std::wostringstream tmpName;
    tmpName << manifestPath << L".tmp." << ::GetCurrentProcessId()
            << L"." << ::GetCurrentThreadId();
    const std::wstring tmpPath = tmpName.str();

    {
        std::ofstream file(tmpPath);
        if (!file.is_open()) return ERROR_ACCESS_DENIED;
        file << root.dump(2);
        if (!file.good()) {
            file.close();
            ::DeleteFileW(tmpPath.c_str());
            return ERROR_WRITE_FAULT;
        }
    }

    if (!::MoveFileExW(tmpPath.c_str(), manifestPath.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD err = ::GetLastError();
        ::DeleteFileW(tmpPath.c_str());
        return err ? err : ERROR_WRITE_FAULT;
    }
    return ERROR_SUCCESS;
}

// ===========================================================================
// 4.9 — Manifest mutation methods
// ===========================================================================

void Manifest::AddLayer(const LayerEntry& entry) {
    layers_[entry.id] = entry;
}

bool Manifest::RemoveLayer(const std::wstring& id) {
    return layers_.erase(id) > 0;
}

LayerEntry* Manifest::GetLayer(const std::wstring& id) {
    auto it = layers_.find(id);
    return it != layers_.end() ? &it->second : nullptr;
}

const LayerEntry* Manifest::GetLayer(const std::wstring& id) const {
    auto it = layers_.find(id);
    return it != layers_.end() ? &it->second : nullptr;
}

std::vector<const LayerEntry*> Manifest::ListLayers() const {
    std::vector<const LayerEntry*> result;
    result.reserve(layers_.size());
    for (const auto& [id, entry] : layers_) {
        result.push_back(&entry);
    }
    return result;
}

// ===========================================================================
// 4.10 — DetectOrphans / CleanupOrphans
// ===========================================================================

Manifest::OrphanReport Manifest::DetectOrphans(
    const std::wstring& layerDirectory) const {
    namespace fs = std::filesystem;
    OrphanReport report;

    // Check manifest entries whose files no longer exist
    for (const auto& [id, entry] : layers_) {
        if (!entry.path.empty() && !fs::exists(entry.path)) {
            report.missingVhds.push_back(id);
        }
    }

    // Check for .vhdx files in the directory with no manifest entry
    std::error_code ec;
    for (const auto& dirEntry : fs::directory_iterator(layerDirectory, ec)) {
        if (!dirEntry.is_regular_file(ec)) continue;

        auto ext = dirEntry.path().extension().wstring();
        for (auto& c : ext) c = static_cast<wchar_t>(::towlower(c));
        if (ext != L".vhdx" && ext != L".vhd") continue;

        std::wstring filePath = dirEntry.path().wstring();
        bool tracked = false;
        for (const auto& [id, entry] : layers_) {
            if (entry.path == filePath) {
                tracked = true;
                break;
            }
        }
        if (!tracked) {
            report.untrackedFiles.push_back(filePath);
        }
    }

    return report;
}

DWORD Manifest::CleanupOrphans(const std::wstring& layerDirectory, bool dryRun) {
    namespace fs = std::filesystem;

    OrphanReport report = DetectOrphans(layerDirectory);

    if (dryRun) return ERROR_SUCCESS;

    // Remove stale manifest entries
    for (const auto& id : report.missingVhds) {
        layers_.erase(id);
    }

    // Delete untracked files. Use a fresh error_code per iteration (the
    // previous version reused a single `ec` so an early success clobbered
    // a later failure flag, and the final return always reported SUCCESS
    // even when orphan removal left files on disk). Continue on error so
    // every file is at least attempted; return the first concrete failure
    // code to the caller so admins know cleanup was partial.
    DWORD firstFailure = ERROR_SUCCESS;
    for (const auto& filePath : report.untrackedFiles) {
        std::error_code ec;
        fs::remove(filePath, ec);
        if (ec && firstFailure == ERROR_SUCCESS) {
            // Map generic_category / system_category errors back to a Win32
            // code when possible; otherwise fall back to a file-write code
            // that still signals "cleanup failed".
            if (ec.category() == std::system_category()) {
                firstFailure = static_cast<DWORD>(ec.value());
            } else {
                firstFailure = ERROR_WRITE_FAULT;
            }
        }
    }
    return firstFailure;
}

} // namespace LayerMount::VHD
