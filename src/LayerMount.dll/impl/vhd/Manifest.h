#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace LayerMount::VHD {

// RAII wrapper for a named mutex that serializes manifest mutation for a
// given manifest path across processes. Acquire before Load-mutate-Save.
class ManifestLock {
public:
    ManifestLock() = default;
    explicit ManifestLock(const std::wstring& manifestPath, DWORD timeoutMs = 30000);
    ~ManifestLock();

    ManifestLock(const ManifestLock&) = delete;
    ManifestLock& operator=(const ManifestLock&) = delete;
    ManifestLock(ManifestLock&& other) noexcept;
    ManifestLock& operator=(ManifestLock&& other) noexcept;

    // True if the mutex was acquired (handle held + wait succeeded).
    bool Held() const noexcept { return held_; }

private:
    HANDLE handle_ = nullptr;
    bool   held_   = false;
};

// Storage backend type — per task spec: directory/vhd/vss
enum class LayerType { Directory, VHD, VSS };

struct LayerEntry {
    std::wstring id;
    LayerType type = LayerType::Directory;
    std::wstring path;
    std::wstring parentId;
    std::wstring mountStatus;   // "mounted" / "detached" / "unknown"
    std::wstring volumeGuid;    // Transient — reset on load
    std::wstring createdAt;     // ISO 8601
    std::map<std::wstring, std::wstring> metadata;
};

class Manifest {
public:
    Manifest() = default;

    // Canonical on-disk filename for a VHD layer manifest. Callers resolving
    // a default path from a working directory should use DefaultPath(dir).
    // Keeping this as a single source of truth avoids the historical drift
    // where writers used "layers.manifest.json" and `vhd list` read "layers.json".
    static const wchar_t* DefaultFileName() { return L"layers.manifest.json"; }
    static std::wstring DefaultPath(const std::wstring& workingDir);

    DWORD Load(const std::wstring& manifestPath);
    DWORD Save(const std::wstring& manifestPath) const;

    void AddLayer(const LayerEntry& entry);
    bool RemoveLayer(const std::wstring& id);
    LayerEntry* GetLayer(const std::wstring& id);
    const LayerEntry* GetLayer(const std::wstring& id) const;
    std::vector<const LayerEntry*> ListLayers() const;

    struct OrphanReport {
        std::vector<std::wstring> missingVhds;      // Manifest entries with no file
        std::vector<std::wstring> untrackedFiles;    // VHD files with no manifest entry
    };
    OrphanReport DetectOrphans(const std::wstring& layerDirectory) const;
    DWORD CleanupOrphans(const std::wstring& layerDirectory, bool dryRun);

private:
    std::map<std::wstring, LayerEntry> layers_;
};

} // namespace LayerMount::VHD
