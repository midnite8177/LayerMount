#include "SidecarMetadata.h"

#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <string>

#pragma comment(lib, "bcrypt.lib")

namespace LayerMount {

namespace {

constexpr const wchar_t* kSidecarSubdir = L"\\.overlay\\";
constexpr const wchar_t* kMetaSuffix    = L".meta.json";
constexpr const wchar_t* kOpaqueSuffix  = L".opaque";

// Wide / UTF-8 conversions duplicated from MetadataADS.cpp -- pulling
// them into a shared header would touch every TU in impl/ for one helper
// pair; better duplicated and kept local than dragged into the public
// engine header.
std::string WideToUtf8(const std::wstring& wide) {
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

std::wstring Utf8ToWide(const std::string& utf8) {
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

uint64_t FileTimeToUint64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

FILETIME Uint64ToFileTime(uint64_t val) {
    FILETIME ft;
    ft.dwLowDateTime  = static_cast<DWORD>(val & 0xFFFFFFFF);
    ft.dwHighDateTime = static_cast<DWORD>(val >> 32);
    return ft;
}

// SHA-1 of `data`. Returns empty on BCrypt failure. Uses BCRYPT_SHA1_ALGORITHM
// rather than a non-crypto hash because the task spec calls out SHA-1
// explicitly and because BCrypt is already linked (LayerImageManager uses
// SHA-256). Lowercase hex output, 40 chars.
std::wstring Sha1Hex(const std::string& data) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    NTSTATUS s = ::BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA1_ALGORITHM,
                                                nullptr, 0);
    if (s < 0) return {};
    s = ::BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    if (s < 0) {
        ::BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    s = ::BCryptHashData(hHash,
        reinterpret_cast<UCHAR*>(const_cast<char*>(data.data())),
        static_cast<ULONG>(data.size()), 0);
    if (s < 0) {
        ::BCryptDestroyHash(hHash);
        ::BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    UCHAR digest[20];
    s = ::BCryptFinishHash(hHash, digest, sizeof(digest), 0);
    ::BCryptDestroyHash(hHash);
    ::BCryptCloseAlgorithmProvider(hAlg, 0);
    if (s < 0) return {};

    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring out(40, L'0');
    for (int i = 0; i < 20; ++i) {
        out[i * 2]     = kHex[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = kHex[digest[i] & 0xF];
    }
    return out;
}

// Lowercase a wide string in place (NTFS case-insensitivity matches what
// PathResolver does so identical paths hash identically regardless of
// caller casing).
std::wstring LowerCase(std::wstring s) {
    if (!s.empty()) {
        ::CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    }
    return s;
}

// `<upper>\.overlay\<sha1(lowercase(filePath))>`
std::wstring SidecarBase(const std::wstring& filePath, const std::wstring& upperRoot) {
    std::wstring keyed = LowerCase(filePath);
    std::wstring hash = Sha1Hex(WideToUtf8(keyed));
    if (hash.empty()) return {};
    std::wstring base = upperRoot;
    base.append(kSidecarSubdir);
    base.append(hash);
    return base;
}

bool EnsureSidecarDir(const std::wstring& upperRoot) {
    std::wstring dir = upperRoot;
    dir.append(kSidecarSubdir);
    // Strip trailing slash for filesystem::create_directory
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) {
        dir.pop_back();
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return !ec;
}

nlohmann::json MetadataToJson(const LayerMountMetadata& m) {
    nlohmann::json j;
    j["opaque"]                = m.opaque;
    j["metacopy"]              = m.metacopy;
    j["redirect"]              = WideToUtf8(m.redirect);
    j["copyUpTimestamp"]       = FileTimeToUint64(m.copyUpTimestamp);
    j["originLayer"]           = WideToUtf8(m.originLayer);
    j["hasStableIndexNumber"]  = m.hasStableIndexNumber;
    j["stableIndexNumber"]     = m.stableIndexNumber;
    return j;
}

LayerMountMetadata JsonToMetadata(const nlohmann::json& j) {
    LayerMountMetadata m;
    m.opaque                = j.value("opaque", false);
    m.metacopy              = j.value("metacopy", false);
    m.redirect              = Utf8ToWide(j.value("redirect", std::string{}));
    m.copyUpTimestamp        = Uint64ToFileTime(j.value("copyUpTimestamp", uint64_t{0}));
    m.originLayer           = Utf8ToWide(j.value("originLayer", std::string{}));
    m.hasStableIndexNumber  = j.value("hasStableIndexNumber", false);
    m.stableIndexNumber     = j.value("stableIndexNumber", uint64_t{0});
    return m;
}

} // namespace

LayerMountMetadata SidecarMetadata::Read(const std::wstring& filePath,
                                      const std::wstring& upperRoot,
                                      bool* corrupted) {
    if (corrupted != nullptr) *corrupted = false;
    std::wstring base = SidecarBase(filePath, upperRoot);
    if (base.empty()) return {};
    std::wstring path = base + kMetaSuffix;

    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        // Benign absence: legitimately no sidecar present.
        // Anything else (sharing violation, ACL, etc.) is a real failure on
        // an apparently-existing sidecar.
        if (corrupted != nullptr &&
            err != ERROR_FILE_NOT_FOUND &&
            err != ERROR_PATH_NOT_FOUND) {
            *corrupted = true;
        }
        return {};
    }

    // GetFileSizeEx so a 4 GiB legitimate file isn't confused with
    // INVALID_FILE_SIZE (0xFFFFFFFF) -- sidecar metadata is small in
    // practice, but keep the error signal clean.
    LARGE_INTEGER liSize{};
    if (!::GetFileSizeEx(h, &liSize)) {
        ::CloseHandle(h);
        if (corrupted != nullptr) *corrupted = true;
        return {};
    }
    if (liSize.QuadPart == 0) {
        ::CloseHandle(h);
        // Zero-byte sidecar is effectively absent.
        return {};
    }
    if (liSize.QuadPart > 0xFFFFFFFFLL) {
        ::CloseHandle(h);
        if (corrupted != nullptr) *corrupted = true;
        return {};
    }
    const DWORD size = static_cast<DWORD>(liSize.QuadPart);

    std::string buffer(static_cast<size_t>(size), '\0');
    DWORD bytesRead = 0;
    BOOL ok = ::ReadFile(h, buffer.data(), size, &bytesRead, nullptr);
    ::CloseHandle(h);
    if (!ok || bytesRead == 0) {
        if (corrupted != nullptr) *corrupted = true;
        return {};
    }
    buffer.resize(bytesRead);

    try {
        return JsonToMetadata(nlohmann::json::parse(buffer));
    } catch (const nlohmann::json::exception&) {
        if (corrupted != nullptr) *corrupted = true;
        return {};
    }
}

bool SidecarMetadata::Write(const std::wstring& filePath,
                            const LayerMountMetadata& metadata,
                            const std::wstring& upperRoot) {
    if (!EnsureSidecarDir(upperRoot)) return false;
    std::wstring base = SidecarBase(filePath, upperRoot);
    if (base.empty()) return false;
    std::wstring path = base + kMetaSuffix;

    // Atomic write: stream to a unique sibling tmp file, then rename onto
    // the final path with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH.
    // The previous CREATE_ALWAYS-in-place pattern left a window where a
    // crash mid-write produced a partial / zero-byte sidecar visible to
    // SidecarMetadata::Read, which then reported the slot as corrupted
    // and (via the MetadataADS fallback / engine) could either drop or
    // resurrect the wrong metadata. The pid/tid suffix ensures concurrent
    // writers in the same process do not collide on the temp name.
    std::wstring tempPath = path + L".tmp." +
                            std::to_wstring(::GetCurrentProcessId()) + L"." +
                            std::to_wstring(::GetCurrentThreadId());

    HANDLE h = ::CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    std::string json = MetadataToJson(metadata).dump();
    DWORD written = 0;
    BOOL ok = ::WriteFile(h, json.c_str(), static_cast<DWORD>(json.size()),
                           &written, nullptr);
    ::CloseHandle(h);
    if (!ok || written != static_cast<DWORD>(json.size())) {
        ::DeleteFileW(tempPath.c_str());
        return false;
    }

    if (!::MoveFileExW(tempPath.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ::DeleteFileW(tempPath.c_str());
        return false;
    }
    return true;
}

bool SidecarMetadata::Remove(const std::wstring& filePath,
                             const std::wstring& upperRoot) {
    std::wstring base = SidecarBase(filePath, upperRoot);
    if (base.empty()) return false;
    std::wstring path = base + kMetaSuffix;
    if (::DeleteFileW(path.c_str())) return true;
    return ::GetLastError() == ERROR_FILE_NOT_FOUND;
}

bool SidecarMetadata::HasOpaque(const std::wstring& dirPath,
                                const std::wstring& upperRoot) {
    std::wstring base = SidecarBase(dirPath, upperRoot);
    if (base.empty()) return false;
    std::wstring path = base + kOpaqueSuffix;
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool SidecarMetadata::SetOpaque(const std::wstring& dirPath,
                                const std::wstring& upperRoot) {
    if (!EnsureSidecarDir(upperRoot)) return false;
    std::wstring base = SidecarBase(dirPath, upperRoot);
    if (base.empty()) return false;
    std::wstring path = base + kOpaqueSuffix;

    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ::CloseHandle(h);
    return true;
}

bool SidecarMetadata::RemoveOpaque(const std::wstring& dirPath,
                                   const std::wstring& upperRoot) {
    std::wstring base = SidecarBase(dirPath, upperRoot);
    // base.empty() means Sha1Hex failed -- there is no addressable sidecar
    // for this directory, so "nothing to remove" is the correct outcome.
    // Treating it as success also lets WhiteoutManager::RemoveOpaque report
    // success when a non-sidecar backend was used for the write (ADS), since
    // the sidecar path would then legitimately not exist.
    if (base.empty()) return true;
    std::wstring path = base + kOpaqueSuffix;
    if (::DeleteFileW(path.c_str())) return true;
    const DWORD err = ::GetLastError();
    // ERROR_FILE_NOT_FOUND: marker was never written (ADS backend was used).
    // ERROR_PATH_NOT_FOUND: the sidecar directory itself is absent -- again
    // a clean "nothing to remove" outcome, not a failure.
    return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
}

} // namespace LayerMount
