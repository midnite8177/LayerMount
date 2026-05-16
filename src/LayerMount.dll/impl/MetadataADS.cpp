#include "MetadataADS.h"
#include "SidecarMetadata.h"
#include "../abi/CapabilityGate.h"

#include <nlohmann/json.hpp>

namespace LayerMount {

// ---------------------------------------------------------------------------
// JSON serialization helpers for LayerMountMetadata
// ---------------------------------------------------------------------------

// FILETIME <-> uint64_t conversion for JSON storage
static uint64_t FileTimeToUint64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

static FILETIME Uint64ToFileTime(uint64_t val) {
    FILETIME ft;
    ft.dwLowDateTime  = static_cast<DWORD>(val & 0xFFFFFFFF);
    ft.dwHighDateTime = static_cast<DWORD>(val >> 32);
    return ft;
}

// Convert wstring to UTF-8 for JSON storage
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

// Convert UTF-8 to wstring
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

// ---------------------------------------------------------------------------
// MetadataADS implementation
// ---------------------------------------------------------------------------

namespace {

// Should the dispatcher prefer sidecar over ADS for this call?
inline bool UseSidecarFor(const LayerConfig* config) {
    if (config == nullptr) return false;
    return !abi::CapabilityGate(config->hostCapabilities).HasAds();
}

// Is the metadata effectively empty? Used by Read to decide whether to
// fall through to the sidecar. A default-constructed LayerMountMetadata
// has opaque=false, metacopy=false, empty redirect, zero timestamp,
// empty originLayer, hasStableIndexNumber=false, stableIndexNumber=0.
inline bool IsDefaultMetadata(const LayerMountMetadata& m) {
    return !m.opaque && !m.metacopy && m.redirect.empty()
        && m.copyUpTimestamp.dwLowDateTime == 0
        && m.copyUpTimestamp.dwHighDateTime == 0
        && m.originLayer.empty()
        && !m.hasStableIndexNumber
        && m.stableIndexNumber == 0;
}

LayerMountMetadata ReadAdsOnly(const std::wstring& filePath, bool* corrupted) {
    if (corrupted != nullptr) *corrupted = false;
    std::wstring adsPath = filePath + kLayerMountADSStream;

    // Permissive share mode: the base file may already be open with
    // GENERIC_WRITE or FILE_SHARE_DELETE from the overlay engine, and
    // ADS opens inherit the base's sharing constraints. A restrictive
    // share here would make ReadLayerMountMetadata spuriously observe the
    // metadata as absent (returns defaults) whenever the base file is
    // being concurrently written or marked for delete.
    // FILE_FLAG_BACKUP_SEMANTICS honors SE_BACKUP_NAME so a DENY-READ
    // inherited ACE on the base file can't hide overlay metadata the
    // engine itself wrote. Symmetric with WriteAdsOnly below.
    HANDLE h = CreateFileW(
        adsPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);

    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        // ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND mean the metadata stream
        // legitimately isn't there -- benign, caller should see defaults.
        // Any other error (sharing violation, ACL denial, etc.) is a real
        // corruption-class failure: the stream may exist but we cannot read
        // it. Callers that care (resolution-critical paths) can check the
        // corrupted out-param and treat defaults as unsafe.
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
        // Corrupted or non-JSON data -- surface to caller via out-param.
        if (corrupted != nullptr) *corrupted = true;
        return {};
    }
}

bool WriteAdsOnly(const std::wstring& filePath, const LayerMountMetadata& metadata) {
    std::wstring adsPath = filePath + kLayerMountADSStream;

    // Permissive share mode -- see ReadAdsOnly for rationale.
    // FILE_FLAG_BACKUP_SEMANTICS honors SE_BACKUP_NAME / SE_RESTORE_NAME
    // (enabled in EnsureCopyUpPrivileges). Without it, an upper file that
    // inherited a DENY-WRITE ACE from its parent would refuse the
    // `:overlay` stream open even though the process holds the backup
    // privileges, and the metadata write would fail -- breaking copy-up
    // of files under restrictive DACLs.
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
    // Also OK if stream didn't exist
    return GetLastError() == ERROR_FILE_NOT_FOUND;
}

bool HasOpaqueAdsOnly(const std::wstring& directoryPath) {
    std::wstring adsPath = directoryPath + kOpaqueADSStream;
    return GetFileAttributesW(adsPath.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool SetOpaqueAdsOnly(const std::wstring& directoryPath) {
    std::wstring adsPath = directoryPath + kOpaqueADSStream;

    // Permissive share mode -- see ReadAdsOnly for rationale.
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
    // Not-found is "nothing to remove" in both flavors. The ADS stream path
    // contains `:overlay.opaque`; DeleteFileW can surface either
    // FILE_NOT_FOUND (base file exists, stream does not) or PATH_NOT_FOUND
    // (base file / directory does not exist). Treat both as benign so we
    // don't report failure when the sidecar backend was the one used.
    return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
}

} // namespace

// ---------------------------------------------------------------------------
// Public dispatcher API. Each routes between ADS and SidecarMetadata
// based on the optional LayerConfig (FR-15).
// ---------------------------------------------------------------------------

LayerMountMetadata MetadataADS::ReadLayerMountMetadata(const std::wstring& filePath,
                                                 const LayerConfig* config,
                                                 bool* corrupted) {
    if (corrupted != nullptr) *corrupted = false;
    bool adsCorrupted = false;
    LayerMountMetadata fromAds = ReadAdsOnly(filePath, &adsCorrupted);

    // ADS-corruption case must NOT fall back to the sidecar. ReadAdsOnly
    // sets adsCorrupted when the stream exists (or apparently exists --
    // sharing violation, ACL denial, multi-GB size, malformed JSON) but
    // cannot be parsed into usable metadata. Falling through to the
    // sidecar in that case can resurrect older sidecar metadata (from
    // before an ADS-mode write took over) and produce an outdated
    // resolution decision. Reserve sidecar fallback for ADS-absent --
    // i.e., the cooperative dispatcher case where a host flipped from
    // sidecar to ADS and we want to honor prior state.
    if (adsCorrupted) {
        if (corrupted != nullptr) *corrupted = true;
        return fromAds;
    }

    if (config == nullptr) {
        return fromAds;
    }
    if (!IsDefaultMetadata(fromAds)) {
        return fromAds;
    }
    // ADS genuinely absent (not corrupted) -- try sidecar transparently.
    // This holds even when LM_CAP_ADS is set: a host that flipped from
    // sidecar to ADS still gets prior state honored.
    bool sidecarCorrupted = false;
    LayerMountMetadata fromSidecar = SidecarMetadata::Read(
        filePath, config->upperPath, &sidecarCorrupted);
    if (sidecarCorrupted && corrupted != nullptr) *corrupted = true;
    return fromSidecar;
}

bool MetadataADS::WriteLayerMountMetadata(const std::wstring& filePath,
                                       const LayerMountMetadata& metadata,
                                       const LayerConfig* config) {
    if (UseSidecarFor(config)) {
        return SidecarMetadata::Write(filePath, metadata, config->upperPath);
    }
    return WriteAdsOnly(filePath, metadata);
}

bool MetadataADS::RemoveLayerMountMetadata(const std::wstring& filePath,
                                        const LayerConfig* config) {
    bool adsOk = RemoveAdsOnly(filePath);
    if (config == nullptr) {
        return adsOk;
    }
    bool sidecarOk = SidecarMetadata::Remove(filePath, config->upperPath);
    return adsOk && sidecarOk;
}

bool MetadataADS::HasOpaqueADS(const std::wstring& directoryPath,
                               const LayerConfig* config) {
    if (HasOpaqueAdsOnly(directoryPath)) return true;
    if (config == nullptr) return false;
    return SidecarMetadata::HasOpaque(directoryPath, config->upperPath);
}

bool MetadataADS::SetOpaqueADS(const std::wstring& directoryPath,
                               const LayerConfig* config) {
    if (UseSidecarFor(config)) {
        return SidecarMetadata::SetOpaque(directoryPath, config->upperPath);
    }
    return SetOpaqueAdsOnly(directoryPath);
}

bool MetadataADS::RemoveOpaqueADS(const std::wstring& directoryPath,
                                  const LayerConfig* config) {
    bool adsOk = RemoveOpaqueAdsOnly(directoryPath);
    if (config == nullptr) {
        return adsOk;
    }
    bool sidecarOk = SidecarMetadata::RemoveOpaque(directoryPath, config->upperPath);
    return adsOk && sidecarOk;
}

} // namespace LayerMount
