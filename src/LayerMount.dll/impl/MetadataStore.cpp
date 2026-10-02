#include "MetadataStore.h"
#include "SidecarMetadata.h"
#include "../abi/CapabilityGate.h"

#include <nlohmann/json.hpp>

namespace LayerMount {

static uint64_t FileTimeToUint64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

static FILETIME Uint64ToFileTime(uint64_t val) {
    FILETIME ft;
    ft.dwLowDateTime  = static_cast<DWORD>(val & 0xFFFFFFFF);
    ft.dwHighDateTime = static_cast<DWORD>(val >> 32);
    return ft;
}

static std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                    static_cast<int>(wide.size()),
                                    nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                        static_cast<int>(wide.size()),
                        utf8.data(), size, nullptr, nullptr);
    return utf8;
}

static std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                    static_cast<int>(utf8.size()),
                                    nullptr, 0);
    if (size <= 0) return {};
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                        static_cast<int>(utf8.size()),
                        wide.data(), size);
    return wide;
}

static nlohmann::json MetadataToJson(const LayerMountMetadata& m) {
    nlohmann::json j;
    j["opaque"]          = m.opaque;
    j["metacopy"]        = m.metacopy;
    j["redirect"]        = WideToUtf8(m.redirect);
    j["copyUpTimestamp"]  = FileTimeToUint64(m.copyUpTimestamp);
    j["originLayer"]     = WideToUtf8(m.originLayer);
    j["hasStableIndexNumber"] = m.hasStableIndexNumber;
    j["stableIndexNumber"] = m.stableIndexNumber;
    return j;
}

static LayerMountMetadata JsonToMetadata(const nlohmann::json& j) {
    LayerMountMetadata m;
    m.opaque          = j.value("opaque", false);
    m.metacopy        = j.value("metacopy", false);
    m.redirect        = Utf8ToWide(j.value("redirect", std::string{}));
    m.copyUpTimestamp  = Uint64ToFileTime(j.value("copyUpTimestamp", uint64_t{0}));
    m.originLayer     = Utf8ToWide(j.value("originLayer", std::string{}));
    m.hasStableIndexNumber = j.value("hasStableIndexNumber", false);
    m.stableIndexNumber = j.value("stableIndexNumber", uint64_t{0});
    return m;
}

namespace {

inline bool UseSidecarFor(const LayerConfig* config) {
    if (config == nullptr) return false;
    return !abi::CapabilityGate(config->hostCapabilities).HasAds();
}

inline bool IsDefaultMetadata(const LayerMountMetadata& m) {
    return !m.opaque && !m.metacopy && m.redirect.empty()
        && m.copyUpTimestamp.dwLowDateTime == 0
        && m.copyUpTimestamp.dwHighDateTime == 0
        && m.originLayer.empty()
        && !m.hasStableIndexNumber
        && m.stableIndexNumber == 0;
}

// Opens the metadata stream for read. The share mode is permissive
// because the engine can hold the base file open with GENERIC_WRITE or
// FILE_SHARE_DELETE, and an ADS open inherits the base's sharing
// constraints. A restrictive share here makes ReadLayerMountMetadata
// see the metadata as absent and return defaults while another handle
// writes the base file or marks it for delete.
// FILE_FLAG_BACKUP_SEMANTICS honors SE_BACKUP_NAME so a DENY-READ
// inherited ACE on the base file cannot hide overlay metadata the
// engine itself wrote.
HANDLE OpenAdsForRead(const std::wstring& adsPath, DWORD desiredAccess) {
    return CreateFileW(
        adsPath.c_str(),
        desiredAccess,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
}

// SetFileTime with a last access time of -1 tells NTFS to not update
// the file's last access time for operations on that handle (the
// SetFileTime remarks). A volume or an ACL can refuse the
// FILE_WRITE_ATTRIBUTES access the mark needs. The read then falls back
// to a plain open, because a refused read would make the engine see its
// own metadata as absent.
HANDLE OpenAdsForReadWithAccessTimeKeptWhereAllowed(const std::wstring& adsPath) {
    HANDLE h = OpenAdsForRead(adsPath, GENERIC_READ | FILE_WRITE_ATTRIBUTES);
    if (h != INVALID_HANDLE_VALUE) {
        const FILETIME keepAccessTime{0xFFFFFFFF, 0xFFFFFFFF};
        SetFileTime(h, nullptr, &keepAccessTime, nullptr);
        return h;
    }
    const DWORD err = ::GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
        return INVALID_HANDLE_VALUE;
    }
    return OpenAdsForRead(adsPath, GENERIC_READ);
}

LayerMountMetadata ReadAdsOnly(const std::wstring& filePath, bool* corrupted) {
    if (corrupted != nullptr) *corrupted = false;
    std::wstring adsPath = filePath + kLayerMountADSStream;

    HANDLE h = OpenAdsForReadWithAccessTimeKeptWhereAllowed(adsPath);

    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (corrupted != nullptr &&
            err != ERROR_FILE_NOT_FOUND &&
            err != ERROR_PATH_NOT_FOUND) {
            *corrupted = true;
        }
        return {};
    }

    // Use GetFileSizeEx so a legitimate 4 GiB file is not confused with
    // INVALID_FILE_SIZE (0xFFFFFFFF). LayerMount metadata is never that large
    // in practice, but using GetFileSizeEx keeps the error signal clean.
    LARGE_INTEGER liSize{};
    if (!GetFileSizeEx(h, &liSize)) {
        CloseHandle(h);
        if (corrupted != nullptr) *corrupted = true;
        return {};
    }
    if (liSize.QuadPart == 0) {
        CloseHandle(h);
        // Zero-byte stream is effectively absent (nothing to parse).
        return {};
    }
    if (liSize.QuadPart > 0xFFFFFFFFLL) {
        // LayerMount metadata streams are small; a multi-GB stream here is
        // corruption, not legitimate content.
        CloseHandle(h);
        if (corrupted != nullptr) *corrupted = true;
        return {};
    }
    const DWORD fileSize = static_cast<DWORD>(liSize.QuadPart);

    std::string buffer(static_cast<size_t>(fileSize), '\0');
    DWORD bytesRead = 0;
    BOOL ok = ReadFile(h, buffer.data(), fileSize, &bytesRead, nullptr);
    CloseHandle(h);

    if (!ok || bytesRead == 0) {
        if (corrupted != nullptr) *corrupted = true;
        return {};
    }

    buffer.resize(bytesRead);

    try {
        nlohmann::json j = nlohmann::json::parse(buffer);
        return JsonToMetadata(j);
    }
    catch (const nlohmann::json::exception&) {
        if (corrupted != nullptr) *corrupted = true;
        return {};
    }
}

bool WriteAdsOnly(const std::wstring& filePath, const LayerMountMetadata& metadata) {
    std::wstring adsPath = filePath + kLayerMountADSStream;

    // Permissive share mode; see OpenAdsForRead for the reason.
    // FILE_FLAG_BACKUP_SEMANTICS honors SE_BACKUP_NAME / SE_RESTORE_NAME
    // (enabled in EnableFileSystemPrivileges). Without it, an upper file that
    // inherited a DENY-WRITE ACE from its parent would refuse the
    // `:overlay` stream open even though the process holds the backup
    // privileges. The metadata write, and with it the copy-up of a file
    // under a restrictive DACL, would then fail.
    HANDLE h = CreateFileW(
        adsPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);

    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    std::string json = MetadataToJson(metadata).dump();

    DWORD bytesWritten = 0;
    BOOL ok = WriteFile(h, json.c_str(), static_cast<DWORD>(json.size()),
                        &bytesWritten, nullptr);
    CloseHandle(h);

    return ok && bytesWritten == static_cast<DWORD>(json.size());
}

bool RemoveAdsOnly(const std::wstring& filePath) {
    std::wstring adsPath = filePath + kLayerMountADSStream;
    if (DeleteFileW(adsPath.c_str())) {
        return true;
    }
    return GetLastError() == ERROR_FILE_NOT_FOUND;
}

bool HasOpaqueAdsOnly(const std::wstring& directoryPath) {
    std::wstring adsPath = directoryPath + kOpaqueADSStream;
    return GetFileAttributesW(adsPath.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool SetOpaqueAdsOnly(const std::wstring& directoryPath) {
    std::wstring adsPath = directoryPath + kOpaqueADSStream;

    // Permissive share mode; see OpenAdsForRead for the reason.
    HANDLE h = CreateFileW(
        adsPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    const char marker = 'y';
    DWORD bytesWritten = 0;
    BOOL ok = WriteFile(h, &marker, 1, &bytesWritten, nullptr);
    CloseHandle(h);

    return ok && bytesWritten == 1;
}

bool RemoveOpaqueAdsOnly(const std::wstring& directoryPath) {
    std::wstring adsPath = directoryPath + kOpaqueADSStream;
    if (DeleteFileW(adsPath.c_str())) {
        return true;
    }
    const DWORD err = GetLastError();
    // DeleteFileW returns ERROR_FILE_NOT_FOUND when the directory exists
    // without the stream, and ERROR_PATH_NOT_FOUND when the directory does
    // not exist. Both mean nothing to remove, which is the normal case when
    // the sidecar store holds the marker.
    return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
}

}

LayerMountMetadata MetadataStore::ReadLayerMountMetadata(const std::wstring& filePath,
                                                         const LayerConfig* config) {
    bool adsCorrupted = false;
    LayerMountMetadata fromAds = ReadAdsOnly(filePath, &adsCorrupted);

    // A corrupted ADS must not fall back to the sidecar. ReadAdsOnly sets
    // adsCorrupted when the stream exists, or seems to exist after a sharing
    // violation, an ACL denial, a multi-GB size or malformed JSON, but does
    // not parse into usable metadata. A fallback there can bring back older
    // sidecar metadata from before an ADS write took over. The fallback is
    // only for an absent ADS, and it runs even when LM_CAP_ADS is set, so a
    // host that changed from the sidecar to the ADS keeps its earlier state.
    if (adsCorrupted) {
        return fromAds;
    }

    if (config == nullptr) {
        return fromAds;
    }
    if (!IsDefaultMetadata(fromAds)) {
        return fromAds;
    }
    return SidecarMetadata::Read(filePath, config->upperPath);
}

bool MetadataStore::WriteLayerMountMetadata(const std::wstring& filePath,
                                            const LayerMountMetadata& metadata,
                                            const LayerConfig* config) {
    if (UseSidecarFor(config)) {
        return SidecarMetadata::Write(filePath, metadata, config->upperPath);
    }
    return WriteAdsOnly(filePath, metadata);
}

bool MetadataStore::RemoveLayerMountMetadata(const std::wstring& filePath,
                                             const LayerConfig* config) {
    bool adsOk = RemoveAdsOnly(filePath);
    if (config == nullptr) {
        return adsOk;
    }
    bool sidecarOk = SidecarMetadata::Remove(filePath, config->upperPath);
    return adsOk && sidecarOk;
}

bool MetadataStore::HasOpaqueMetadata(const std::wstring& directoryPath,
                                      const LayerConfig* config) {
    if (HasOpaqueAdsOnly(directoryPath)) return true;
    if (config == nullptr) return false;
    return SidecarMetadata::HasOpaque(directoryPath, config->upperPath);
}

bool MetadataStore::SetOpaqueMetadata(const std::wstring& directoryPath,
                                      const LayerConfig* config) {
    if (UseSidecarFor(config)) {
        return SidecarMetadata::SetOpaque(directoryPath, config->upperPath);
    }
    return SetOpaqueAdsOnly(directoryPath);
}

bool MetadataStore::RemoveOpaqueMetadata(const std::wstring& directoryPath,
                                         const LayerConfig* config) {
    bool adsOk = RemoveOpaqueAdsOnly(directoryPath);
    if (config == nullptr) {
        return adsOk;
    }
    bool sidecarOk = SidecarMetadata::RemoveOpaque(directoryPath, config->upperPath);
    return adsOk && sidecarOk;
}

}
