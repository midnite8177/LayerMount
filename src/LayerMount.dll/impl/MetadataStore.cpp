#include "MetadataStore.h"
#include "LayerPath.h"
#include "SidecarMetadata.h"
#include "../abi/CapabilityGate.h"

#include <nlohmann/json.hpp>

#include <functional>

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
// FILE_FLAG_OPEN_REPARSE_POINT keeps the stream on a link itself. Without
// it, the open of a file symlink's stream reaches the target's stream, and
// the open of a directory link's stream fails with ERROR_DIRECTORY.
HANDLE OpenAdsForRead(const std::wstring& adsPath, DWORD desiredAccess) {
    return CreateFileW(
        adsPath.c_str(),
        desiredAccess,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
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
    // FILE_FLAG_OPEN_REPARSE_POINT writes the stream onto a link itself; see
    // OpenAdsForRead.
    HANDLE h = CreateFileW(
        adsPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
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

// DeleteFileW opens its path with FILE_OPEN_REPARSE_POINT, so on a link it
// removes the link's own stream and leaves the target's.
bool RemoveAdsOnly(const std::wstring& filePath) {
    std::wstring adsPath = filePath + kLayerMountADSStream;
    if (DeleteFileW(adsPath.c_str())) {
        return true;
    }
    return GetLastError() == ERROR_FILE_NOT_FOUND;
}

// GetFileAttributesW does not follow a link, so on a link it finds the
// link's own stream.
bool HasOpaqueAdsOnly(const std::wstring& directoryPath) {
    std::wstring adsPath = directoryPath + kOpaqueADSStream;
    return GetFileAttributesW(adsPath.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool SetOpaqueAdsOnly(const std::wstring& directoryPath) {
    std::wstring adsPath = directoryPath + kOpaqueADSStream;

    // Permissive share mode and FILE_FLAG_OPEN_REPARSE_POINT; see
    // OpenAdsForRead for the reasons.
    HANDLE h = CreateFileW(
        adsPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
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

    // On an ADS host the sidecar can hold a record from before the ADS write,
    // so a corrupt ADS returns the default record. A host without ADS writes
    // the sidecar, so its record is current.
    if (adsCorrupted && !UseSidecarFor(config)) {
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

namespace {

// Calls visit with the path of each entry below dirPath. Enters a
// subdirectory, but not a junction or a directory symbolic link, whose
// sidecar records are its own and not those of its target's entries. The
// find data carries the reparse tag, so the walk reads no tag of its own.
void ForEachEntryBelow(const std::wstring& dirPath,
                       const std::function<void(const std::wstring&)>& visit) {
    WIN32_FIND_DATAW fd{};
    HANDLE find = ::FindFirstFileW(JoinDirPath(dirPath, L"*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring childPath = JoinDirPath(dirPath, name);
        visit(childPath);
        const bool isLink = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
                            IsReparseTagNameSurrogate(fd.dwReserved0);
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && !isLink) {
            ForEachEntryBelow(childPath, visit);
        }
    } while (::FindNextFileW(find, &fd));
    ::FindClose(find);
}

// Whether the entry at path is a directory that is not a junction or a
// directory symbolic link. An entry whose reparse tag cannot be read counts
// as a link, so no walk goes into it.
bool IsDirectoryButNotLink(const std::wstring& path) {
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return false;
    }
    bool isLink = false;
    return NT_SUCCESS(IsDirectoryLink(path, attrs, &isLink)) && !isLink;
}

}

void MetadataStore::MoveSidecarRecords(const std::wstring& from,
                                       const std::wstring& to,
                                       const LayerConfig& config) {
    if (!UseSidecarFor(&config)) {
        return;
    }
    SidecarMetadata::Move(from, to, config.upperPath);
    if (!IsDirectoryButNotLink(to)) {
        return;
    }
    ForEachEntryBelow(to, [&](const std::wstring& movedPath) {
        SidecarMetadata::Move(from + movedPath.substr(to.size()), movedPath,
                              config.upperPath);
    });
}

std::vector<std::wstring> MetadataStore::ListSidecarKeyedEntries(const std::wstring& path,
                                                                 const LayerConfig& config) {
    std::vector<std::wstring> entries;
    if (!UseSidecarFor(&config)) {
        return entries;
    }
    entries.push_back(path);
    if (IsDirectoryButNotLink(path)) {
        ForEachEntryBelow(path, [&](const std::wstring& entryPath) {
            entries.push_back(entryPath);
        });
    }
    return entries;
}

void MetadataStore::RemoveSidecarRecordsOfGoneEntries(const std::vector<std::wstring>& entries,
                                                      const LayerConfig& config) {
    for (const std::wstring& entryPath : entries) {
        if (::GetFileAttributesW(entryPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        const DWORD probeErr = ::GetLastError();
        if (probeErr != ERROR_FILE_NOT_FOUND && probeErr != ERROR_PATH_NOT_FOUND) {
            continue;
        }
        SidecarMetadata::Remove(entryPath, config.upperPath);
        SidecarMetadata::RemoveOpaque(entryPath, config.upperPath);
    }
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
