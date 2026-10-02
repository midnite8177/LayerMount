#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace LayerMount::VHD {

// RAII wrapper for a named mutex that serializes reads and writes of the
// layer registry at a given path across processes. Take it before Load.
// Warning: a Win32 mutex lets the thread that owns it take it again, so it
// does not serialize two holders on the same thread.
class ManifestLock {
public:
    ManifestLock(const std::wstring& manifestPath, DWORD timeoutMs);
    ~ManifestLock();

    ManifestLock(const ManifestLock&) = delete;
    ManifestLock& operator=(const ManifestLock&) = delete;

    // True if the mutex was acquired (handle held + wait succeeded).
    bool Held() const noexcept { return held_; }

private:
    HANDLE handle_ = nullptr;
    bool   held_   = false;
};

// Storage backend type: directory, VHD, or VSS.
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
