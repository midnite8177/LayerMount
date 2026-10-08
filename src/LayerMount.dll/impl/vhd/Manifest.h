#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <memory>

#include "../HostPath.h"

namespace LayerMount::VHD {

// RAII wrapper for a named mutex that serializes reads and writes of the
// layer registry at a given path across processes. Take it before Load.
// Warning: a Win32 mutex lets the thread that owns it take it again, so it
// does not serialize two holders on the same thread.
class ManifestLock {
public:
    ManifestLock(const HostPath& manifestPath, DWORD timeoutMs);
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

    static const wchar_t* DefaultFileName() { return L"layers.manifest.json"; }
    // Joins the file name onto workingDir.Text(), so the result keeps the
    // form the caller gave.
    static HostPath DefaultPath(const HostPath& workingDir);

    // When the file does not open, the result is the Win32 error of the open,
    // so a caller can tell a missing registry from a locked or unreadable one.
    DWORD Load(const HostPath& manifestPath);
    DWORD Save(const HostPath& manifestPath) const;

    void AddLayer(const LayerEntry& entry);
    bool RemoveLayer(const std::wstring& id);
    LayerEntry* GetLayer(const std::wstring& id);
    const LayerEntry* GetLayer(const std::wstring& id) const;
    std::vector<const LayerEntry*> ListLayers() const;

private:
    std::map<std::wstring, LayerEntry> layers_;
};

}
