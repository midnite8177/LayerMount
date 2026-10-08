#include "Manifest.h"
#include "VHDLayerManager.h"
#include "../NtStatusUtil.h"
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

// FNV-1a 64-bit. Unlike std::hash, it gives the same value in every process
// and CRT version.
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

}

ManifestLock::ManifestLock(const HostPath& manifestPath, DWORD timeoutMs) {
    const std::wstring name = MutexNameForPath(manifestPath.Text());
    handle_ = ::CreateMutexW(nullptr, FALSE, name.c_str());
    if (handle_ == nullptr) return;

    DWORD wait = ::WaitForSingleObject(handle_, timeoutMs);
    // WAIT_ABANDONED also gives ownership. It means the previous holder
    // ended without a release.
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

HostPath Manifest::DefaultPath(const HostPath& workingDir) {
    return HostPath((std::filesystem::path(workingDir.Text()) / DefaultFileName()).wstring());
}

DWORD Manifest::Load(const HostPath& manifestPath) {
    HANDLE fh = ::CreateFileW(manifestPath.ForWin32().c_str(),
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

    // Manifests are small (O(KiB)), so reading the whole file is fine.
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

DWORD Manifest::Save(const HostPath& manifestPath) const {
    nlohmann::json root;
    root["schemaVersion"] = 1;

    nlohmann::json layerArray = nlohmann::json::array();
    for (const auto& [id, entry] : layers_) {
        layerArray.push_back(LayerEntryToJson(entry));
    }
    root["layers"] = layerArray;

    const std::wstring manifestForWin32 = manifestPath.ForWin32();
    std::filesystem::path parentDir = std::filesystem::path(manifestForWin32).parent_path();
    if (!parentDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parentDir, ec);
        if (ec) return Win32FromErrorCode(ec);
    }

    std::wostringstream tmpName;
    tmpName << manifestForWin32 << L".tmp." << ::GetCurrentProcessId()
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

    if (!::MoveFileExW(tmpPath.c_str(), manifestForWin32.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD err = ::GetLastError();
        ::DeleteFileW(tmpPath.c_str());
        return err ? err : ERROR_WRITE_FAULT;
    }
    return ERROR_SUCCESS;
}

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

}
