#pragma once

#include "pch.h"

#include "LayerMount.h"
#include "Cache.h"
#include "CopyUp.h"
#include "DirectoryRename.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"

#include "AclTestHelpers.h"
#include "FileIdTestHelpers.h"
#include "FileTimeTestHelpers.h"

#include <cfapi.h>

#include <cstring>
#include <exception>
#include <functional>
#include <rpc.h>
#include <type_traits>
#include <winioctl.h>

namespace LayerMountTests {

namespace fs = std::filesystem;

using LayerMountTestShared::GetTimes;
using LayerMountTestShared::MakeFileTime;
using LayerMountTestShared::StampTimes;

// True when T, built from a LayerConfig followed by Rest, accepts a named
// config and refuses a temporary one.
template <class T, class... Rest>
constexpr bool RefusesTemporaryConfig =
    !std::is_constructible_v<T, LayerMount::LayerConfig, Rest...> &&
    !std::is_constructible_v<T, const LayerMount::LayerConfig, Rest...> &&
    std::is_constructible_v<T, LayerMount::LayerConfig&, Rest...> &&
    std::is_constructible_v<T, const LayerMount::LayerConfig&, Rest...>;

static_assert(RefusesTemporaryConfig<LayerMount::ConfigRef>,
    "ConfigRef keeps a reference to its LayerConfig");

inline std::wstring MakeUniqueTempRoot() {
    wchar_t tempBuf[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, tempBuf);

    UUID uuid = {};
    ::UuidCreate(&uuid);
    RPC_WSTR uuidStr = nullptr;
    ::UuidToStringW(&uuid, &uuidStr);

    std::wstring root = std::wstring(tempBuf) + L"LayerMountTests\\" +
                        reinterpret_cast<wchar_t*>(uuidStr);
    ::RpcStringFreeW(&uuidStr);

    std::error_code ec;
    fs::create_directories(root, ec);
    return root;
}

// Prefixes dirPath with \\?\ and appends one separator.
inline std::wstring ExtendedDirWithSeparator(const std::wstring& dirPath) {
    return L"\\\\?\\" + dirPath + L"\\";
}

inline bool IsElevated() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    BOOL ok = ::GetTokenInformation(token, TokenElevation, &elevation,
                                    sizeof(elevation), &returned);
    ::CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

// Logs a skip and returns from the enclosing TEST_METHOD when the process
// is not elevated.
#define UNIT_SKIP_IF_NOT_ADMIN()                                               \
    do {                                                                       \
        if (!::LayerMountTests::IsElevated()) {                                 \
            Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage(\
                L"[SKIP] Test requires administrator privileges");             \
            return;                                                            \
        }                                                                      \
    } while (0)

inline bool IsTempNtfs() {
    wchar_t tempBuf[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, tempBuf);
    std::wstring temp(tempBuf);
    std::wstring rootPath;
    if (temp.size() >= 3 && temp[1] == L':' && temp[2] == L'\\') {
        rootPath = temp.substr(0, 3);
    } else {
        rootPath = temp;
    }
    wchar_t fsName[16] = {};
    if (!::GetVolumeInformationW(rootPath.c_str(), nullptr, 0, nullptr, nullptr,
                                 nullptr, fsName, 16)) {
        return false;
    }
    return std::wstring(fsName) == L"NTFS";
}

#define UNIT_SKIP_IF_NOT_NTFS()                                                \
    do {                                                                       \
        if (!::LayerMountTests::IsTempNtfs()) {                                 \
            Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage(\
                L"[SKIP] Test requires NTFS %TEMP% for Alternate Data Streams");\
            return;                                                            \
        }                                                                      \
    } while (0)

// Fails the test unless %TEMP% is on NTFS. Call it from
// TEST_CLASS_INITIALIZE in a class that uses Alternate Data Streams.
inline void AssertTempIsNTFS() {
    wchar_t tempBuf[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, tempBuf);

    // GetVolumeInformationW takes a volume root such as "C:\", not a subdirectory.
    std::wstring temp(tempBuf);
    std::wstring rootPath;
    if (temp.size() >= 3 && temp[1] == L':' && temp[2] == L'\\') {
        rootPath = temp.substr(0, 3);
    } else {
        rootPath = temp;
    }

    wchar_t fsName[16] = {};
    ::GetVolumeInformationW(rootPath.c_str(), nullptr, 0, nullptr, nullptr,
                            nullptr, fsName, 16);

    Microsoft::VisualStudio::CppUnitTestFramework::Assert::AreEqual(
        std::wstring(L"NTFS"), std::wstring(fsName),
        L"Tests require %TEMP% to be on NTFS for Alternate Data Stream support");
}

using LayerMountTestShared::kDefaultHostCapabilities;
using LayerMountTestShared::kHostCapabilitiesWithoutAds;
using LayerMountTestShared::LinkOpen;
using LayerMountTestShared::NtfsFileIdOf;

// Runs body once with kDefaultHostCapabilities and once with
// kHostCapabilitiesWithoutAds, so a test covers the ADS store and the
// sidecar store.
inline void ForEachMetadataStore(const std::function<void(UINT32 hostCapabilities)>& body) {
    for (const UINT32 hostCapabilities : {kDefaultHostCapabilities, kHostCapabilitiesWithoutAds}) {
        body(hostCapabilities);
    }
}

// Creates upper, work and lowerN directories under a unique %TEMP%
// subdirectory, and deletes the tree on destruction.
class TempLayerEnvironment {
public:
    explicit TempLayerEnvironment(size_t lowerCount = 1)
        : root_(MakeUniqueTempRoot()) {
        upper_ = root_ + L"\\upper";
        work_  = root_ + L"\\work";

        std::error_code ec;
        fs::create_directories(upper_, ec);
        fs::create_directories(work_, ec);

        for (size_t i = 0; i < lowerCount; ++i) {
            std::wstring lower = root_ + L"\\lower" + std::to_wstring(i);
            fs::create_directories(lower, ec);
            lowers_.push_back(lower);
        }
    }

    ~TempLayerEnvironment() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    TempLayerEnvironment(const TempLayerEnvironment&) = delete;
    TempLayerEnvironment& operator=(const TempLayerEnvironment&) = delete;

    const std::wstring& Root()  const { return root_; }
    const std::wstring& Upper() const { return upper_; }
    const std::wstring& Work()  const { return work_; }
    const std::wstring& Lower(size_t i = 0) const { return lowers_.at(i); }
    size_t LowerCount() const { return lowers_.size(); }

    LayerMount::LayerConfig MakeConfig() const {
        LayerMount::LayerConfig c;
        c.upperPath   = upper_;
        c.workDirPath = work_;
        c.lowerPaths  = lowers_;
        c.hostCapabilities = kDefaultHostCapabilities;
        return c;
    }

    void WriteFile(const std::wstring& layer, const std::wstring& rel,
                   const std::string& content) const {
        std::wstring full = layer + L"\\" + rel;
        std::error_code ec;
        fs::create_directories(fs::path(full).parent_path(), ec);

        HANDLE h = ::CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        Microsoft::VisualStudio::CppUnitTestFramework::Assert::AreNotEqual(
            INVALID_HANDLE_VALUE, h, L"TempLayerEnvironment::WriteFile — CreateFileW failed");

        DWORD written = 0;
        ::WriteFile(h, content.data(), static_cast<DWORD>(content.size()),
                    &written, nullptr);
        ::CloseHandle(h);
    }

    void CreateDir(const std::wstring& layer, const std::wstring& rel) const {
        std::error_code ec;
        fs::create_directories(layer + L"\\" + rel, ec);
    }

    bool FileExists(const std::wstring& layer, const std::wstring& rel) const {
        return ::GetFileAttributesW((layer + L"\\" + rel).c_str())
               != INVALID_FILE_ATTRIBUTES;
    }

    std::string ReadFile(const std::wstring& layer, const std::wstring& rel) const {
        std::wstring full = layer + L"\\" + rel;
        HANDLE h = ::CreateFileW(full.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            return {};
        }

        LARGE_INTEGER sz = {};
        ::GetFileSizeEx(h, &sz);
        std::string buf(static_cast<size_t>(sz.QuadPart), '\0');
        DWORD read = 0;
        if (!buf.empty()) {
            ::ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr);
        }
        ::CloseHandle(h);
        buf.resize(read);
        return buf;
    }

private:
    std::wstring root_;
    std::wstring upper_;
    std::wstring work_;
    std::vector<std::wstring> lowers_;
};

inline bool FileTimesEqual(const FILETIME& a, const FILETIME& b) {
    return a.dwLowDateTime == b.dwLowDateTime &&
           a.dwHighDateTime == b.dwHighDateTime;
}

inline LONGLONG LogicalBytes(const std::wstring& path) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    Microsoft::VisualStudio::CppUnitTestFramework::Assert::AreNotEqual<HANDLE>(
        INVALID_HANDLE_VALUE, h, L"LogicalBytes: open failed");
    LARGE_INTEGER sz{};
    ::GetFileSizeEx(h, &sz);
    ::CloseHandle(h);
    return sz.QuadPart;
}

// The bytes the volume allocated for the file. GetCompressedFileSizeW
// reports the allocation of a sparse file, holes excluded. The flush before
// the query makes the report include the data still in the cache.
inline LONGLONG AllocatedBytes(const std::wstring& path) {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    Assert::AreNotEqual<HANDLE>(INVALID_HANDLE_VALUE, h, L"AllocatedBytes: open failed");
    Assert::IsTrue(::FlushFileBuffers(h) != FALSE, L"AllocatedBytes: flush failed");
    ::CloseHandle(h);
    DWORD high = 0;
    const DWORD low = ::GetCompressedFileSizeW(path.c_str(), &high);
    Assert::IsTrue(low != INVALID_FILE_SIZE || ::GetLastError() == NO_ERROR,
        L"AllocatedBytes: GetCompressedFileSizeW failed");
    return (static_cast<LONGLONG>(high) << 32) | low;
}

inline bool HasAttribute(const std::wstring& path, DWORD flag) {
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & flag) != 0;
}

// The name the file system stores for the last component of path, which
// keeps its case; empty when path does not exist.
inline std::wstring StoredLeafName(const std::wstring& path) {
    WIN32_FIND_DATAW fd{};
    HANDLE find = ::FindFirstFileW(path.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        return {};
    }
    ::FindClose(find);
    return fd.cFileName;
}

inline std::vector<std::wstring> EntriesUnder(const std::wstring& root) {
    std::vector<std::wstring> entries;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        entries.push_back(fs::relative(entry.path(), root).wstring());
    }
    return entries;
}

class LayerSnapshot {
public:
    explicit LayerSnapshot(const std::wstring& root)
        : root_(root), entries_(EntriesUnder(root)) {}

    void AssertUnchanged(const wchar_t* message) const {
        Microsoft::VisualStudio::CppUnitTestFramework::Assert::IsTrue(
            entries_ == EntriesUnder(root_), message);
    }

private:
    std::wstring root_;
    std::vector<std::wstring> entries_;
};

inline ::LayerMount::ScopedHandle HoldOpen(const std::wstring& path, DWORD shareMode) {
    ::LayerMount::ScopedHandle held(::CreateFileW(path.c_str(), GENERIC_READ, shareMode, nullptr,
                                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    Microsoft::VisualStudio::CppUnitTestFramework::Assert::IsTrue(
        held.IsValid(), (L"The test must hold " + path + L" open").c_str());
    return held;
}

// Creates the stream at streamPath (L"file:name") and holds it open with no
// sharing, so a copy that opens the stream fails with STATUS_SHARING_VIOLATION.
// Does not assert, so an event callback can call it; the handle is invalid on
// failure.
inline ::LayerMount::ScopedHandle OpenNewStreamExclusively(const std::wstring& streamPath) {
    return ::LayerMount::ScopedHandle(::CreateFileW(streamPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
}

inline ::LayerMount::ScopedHandle HoldNewStreamExclusively(const std::wstring& streamPath) {
    ::LayerMount::ScopedHandle held = OpenNewStreamExclusively(streamPath);
    Microsoft::VisualStudio::CppUnitTestFramework::Assert::IsTrue(
        held.IsValid(), (L"The test must hold " + streamPath + L" open with no sharing").c_str());
    return held;
}

// The layer-relative path of the whiteout marker that hides relativePath:
// L"a\\b\\c.txt" gives L"a\\b\\.wh.c.txt".
inline std::wstring WhiteoutMarkerPath(const std::wstring& relativePath) {
    const size_t separator = relativePath.find_last_of(L'\\');
    if (separator == std::wstring::npos) return L".wh." + relativePath;
    return relativePath.substr(0, separator + 1) + L".wh." + relativePath.substr(separator + 1);
}

// The layer-relative path of the marker file that makes relativeDirectory
// opaque: L"d" gives L"d\\.wh..wh..opq".
inline std::wstring OpaqueMarkerPath(const std::wstring& relativeDirectory) {
    return relativeDirectory + L"\\.wh..wh..opq";
}

// mklink /J needs no symbolic-link privilege.
inline bool CreateDirectoryJunction(const std::wstring& junction, const std::wstring& target) {
    const std::wstring command =
        L"cmd.exe /c mklink /J \"" + junction + L"\" \"" + target + L"\" >nul 2>&1";
    return _wsystem(command.c_str()) == 0;
}

inline bool CreateDirectorySymlink(const std::wstring& link, const std::wstring& target) {
    return ::CreateSymbolicLinkW(link.c_str(), target.c_str(),
        SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE;
}

inline bool CreateFileSymlink(const std::wstring& link, const std::wstring& target) {
    return ::CreateSymbolicLinkW(link.c_str(), target.c_str(),
        SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE;
}

using LinkCreator = bool (*)(const std::wstring& link, const std::wstring& target);

inline bool LinkCreatedOrSkipped(LinkCreator createLink,
                                 const std::wstring& link,
                                 const std::wstring& target) {
    if (createLink(link, target)) {
        return true;
    }
    Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage(
        (L"[SKIP] the test could not create the link " + link).c_str());
    return false;
}

// Makes the lower junction parent\link to the directory target under the
// environment root. Logs a skip and returns false when the junction cannot
// be created.
inline bool LowerJunctionCreatedOrSkipped(const TempLayerEnvironment& env,
                                          const std::wstring& parent) {
    env.WriteFile(env.Root(), L"target\\inside.txt", "inside");
    env.CreateDir(env.Lower(0), parent);
    return LinkCreatedOrSkipped(CreateDirectoryJunction,
                                env.Lower(0) + L"\\" + parent + L"\\link",
                                env.Root() + L"\\target");
}

// Creates "link" in the upper for LayerSource::Upper, or in the first lower
// for LayerSource::Lower, and a directory of the same name in the next layer
// down. The link targets a directory outside the layers. Logs a skip and
// returns false when the link cannot be created.
inline bool BuildLinkOverDirectory(const TempLayerEnvironment& env,
                                   LayerMount::LayerSource linkSource,
                                   LinkCreator createLink) {
    const bool linkInUpper = linkSource == LayerMount::LayerSource::Upper;
    const std::wstring& linkLayer = linkInUpper ? env.Upper() : env.Lower(0);
    const std::wstring& dirLayer = linkInUpper ? env.Lower(0) : env.Lower(1);
    env.WriteFile(env.Root(), L"target\\fromtarget.txt", "target");
    env.WriteFile(env.Root(), L"target\\sub\\fromtarget.txt", "target");
    env.WriteFile(dirLayer, L"link\\fromlower.txt", "lower");
    env.WriteFile(dirLayer, L"link\\sub\\fromlower.txt", "lower");
    return LinkCreatedOrSkipped(createLink, linkLayer + L"\\link", env.Root() + L"\\target");
}

// Denies accessMask to Everyone on a link itself, not on its target, and
// restores the link's DACL on destruction. The handle stays open, so the
// restore needs no open that the deny ACE can refuse. Declare it after the
// test's layer environment.
class LinkAccessDenied {
public:
    LinkAccessDenied(const std::wstring& link, DWORD accessMask) {
        using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
        link_ = ::CreateFileW(link.c_str(), READ_CONTROL | WRITE_DAC,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        Assert::IsTrue(link_ != INVALID_HANDLE_VALUE, L"The link opens for a DACL change");
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            ::GetSecurityInfo(link_, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, &originalDacl_, nullptr, &originalSd_),
            L"GetSecurityInfo reads the link's DACL");
        LayerMountTestShared::EveryoneSid everyone;
        Assert::IsNotNull(everyone.sid, L"The Everyone SID allocates");
        EXPLICIT_ACCESSW deny{};
        deny.grfAccessPermissions = accessMask;
        deny.grfAccessMode = DENY_ACCESS;
        deny.grfInheritance = NO_INHERITANCE;
        deny.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        deny.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        deny.Trustee.ptstrName = reinterpret_cast<LPWSTR>(everyone.sid);
        PACL deniedDacl = nullptr;
        Assert::AreEqual<DWORD>(ERROR_SUCCESS,
            ::SetEntriesInAclW(1, &deny, originalDacl_, &deniedDacl),
            L"The deny ACE merges into the link's DACL");
        const DWORD setResult = ::SetSecurityInfo(link_, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, deniedDacl, nullptr);
        ::LocalFree(deniedDacl);
        Assert::AreEqual<DWORD>(ERROR_SUCCESS, setResult, L"The link's DACL takes the deny ACE");
    }

    ~LinkAccessDenied() {
        ::SetSecurityInfo(link_, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, originalDacl_, nullptr);
        ::LocalFree(originalSd_);
        ::CloseHandle(link_);
    }

    LinkAccessDenied(const LinkAccessDenied&) = delete;
    LinkAccessDenied& operator=(const LinkAccessDenied&) = delete;

private:
    HANDLE link_ = INVALID_HANDLE_VALUE;
    PACL originalDacl_ = nullptr;
    PSECURITY_DESCRIPTOR originalSd_ = nullptr;
};

// Disables SE_RESTORE_NAME on the thread's impersonation token, which
// BackupPrivilegeDisabledOnThread provides. SE_RESTORE_NAME grants a
// backup-intent open SYNCHRONIZE past a deny ACE, since FILE_GENERIC_WRITE
// holds SYNCHRONIZE.
inline void DisableRestorePrivilegeOnThread() {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
    HANDLE token = nullptr;
    Assert::IsTrue(::OpenThreadToken(::GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                                     TRUE, &token) != FALSE,
        L"The thread holds an impersonation token");
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = 0;
    const bool disabled =
        ::LookupPrivilegeValueW(nullptr, SE_RESTORE_NAME, &privileges.Privileges[0].Luid) &&
        ::AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
    ::CloseHandle(token);
    Assert::IsTrue(disabled, L"SE_RESTORE_NAME is disabled on the thread's token");
}

// Returns the error of an open of the link itself with the access and flags
// that a reparse tag read uses, or ERROR_SUCCESS.
inline DWORD LinkTagOpenError(const std::wstring& link) {
    HANDLE entry = ::CreateFileW(link.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (entry == INVALID_HANDLE_VALUE) {
        return ::GetLastError();
    }
    ::CloseHandle(entry);
    return ERROR_SUCCESS;
}

inline void AssertStatus(NTSTATUS expected, NTSTATUS actual, const wchar_t* message) {
    Microsoft::VisualStudio::CppUnitTestFramework::Assert::AreEqual(
        static_cast<long>(expected), static_cast<long>(actual), message);
}

inline bool EnableCompression(const std::wstring& path) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    USHORT format = COMPRESSION_FORMAT_DEFAULT;
    DWORD bytesReturned = 0;
    const BOOL ok = ::DeviceIoControl(h, FSCTL_SET_COMPRESSION, &format, sizeof(format),
                                      nullptr, 0, &bytesReturned, nullptr);
    ::CloseHandle(h);
    return ok != FALSE;
}

// Encrypts the file or directory at path. Logs a skip with the error and
// returns false when EFS refuses.
inline bool EncryptedOrSkipped(const std::wstring& path) {
    if (::EncryptFileW(path.c_str())) {
        return true;
    }
    const DWORD err = ::GetLastError();
    Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage(
        (L"[SKIP] EncryptFileW on " + path + L" failed (error " + std::to_wstring(err) +
         L"). EFS unavailable or user has no cert.").c_str());
    return false;
}

// Sets the reparse point in buffer, a whole reparse data buffer, on the
// entry at path. Logs a skip and returns false when the reparse point
// cannot be set.
inline bool ReparseBufferSetOrSkipped(const std::wstring& path, const std::vector<BYTE>& buffer) {
    ::LayerMount::ScopedHandle handle(::CreateFileW(
        path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    DWORD returned = 0;
    const bool set = handle.IsValid() &&
                     ::DeviceIoControl(handle.Get(), FSCTL_SET_REPARSE_POINT,
                                       const_cast<BYTE*>(buffer.data()),
                                       static_cast<DWORD>(buffer.size()), nullptr, 0, &returned,
                                       nullptr) != FALSE;
    if (!set || !HasAttribute(path, FILE_ATTRIBUTE_REPARSE_POINT)) {
        Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage(
            (L"[SKIP] the test could not set a reparse tag on " + path).c_str());
        return false;
    }
    return true;
}

// Gives the file or directory at path a reparse point with a tag that is
// not a Microsoft tag and not a name surrogate, so a directory is not a
// link. No filter handles the tag, so a listing of a directory fails with
// ERROR_CANT_ACCESS_FILE. Logs a skip and returns false when the tag
// cannot be set.
inline bool NonLinkReparseTagSetOrSkipped(const std::wstring& path) {
    constexpr DWORD kDataLength = 4;
    std::vector<BYTE> buffer(REPARSE_GUID_DATA_BUFFER_HEADER_SIZE + kDataLength);
    auto* reparse = reinterpret_cast<REPARSE_GUID_DATA_BUFFER*>(buffer.data());
    reparse->ReparseTag = 0x00001234;
    reparse->ReparseDataLength = kDataLength;
    reparse->ReparseGuid = {0x6d1b5a8e, 0x2f4c, 0x4b7a, {0x9e, 0x31, 0x5c, 0x0d, 0x7a, 0x42, 0x18, 0x66}};
    return ReparseBufferSetOrSkipped(path, buffer);
}

// Gives the empty file at path a reparse point with the Microsoft reparse
// tag in tag and no reparse data. The header of a Microsoft reparse point
// has no GUID. Logs a skip and returns false when the tag cannot be set.
inline bool MicrosoftReparseTagSetOrSkipped(const std::wstring& path, DWORD tag) {
    constexpr size_t kHeaderSize = sizeof(DWORD) + 2 * sizeof(WORD);
    std::vector<BYTE> buffer(kHeaderSize);
    std::memcpy(buffer.data(), &tag, sizeof(tag));
    return ReparseBufferSetOrSkipped(path, buffer);
}

// Returns the reparse tag of the entry at path, or 0 when the entry is not
// a reparse point or the tag cannot be read.
inline DWORD ReparseTagOf(const std::wstring& path) {
    ::LayerMount::ScopedHandle handle(::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!handle.IsValid() ||
        !::GetFileInformationByHandleEx(handle.Get(), FileAttributeTagInfo, &info,
                                        sizeof(info)) ||
        (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        return 0;
    }
    return info.ReparseTag;
}

// The data the test sync provider serves for a cloud placeholder file,
// length bytes from offset. The byte at offset i is 1 + i % 251, so no byte
// is zero and each 4 KB block differs from the one before it.
inline std::string CloudProviderData(LONGLONG offset, size_t length) {
    std::string data(length, '\0');
    for (size_t i = 0; i < length; ++i) {
        data[i] = static_cast<char>(1 + (offset + static_cast<LONGLONG>(i)) % 251);
    }
    return data;
}

// A range of bytes in a file, length bytes from offset.
struct ByteRange {
    LONGLONG offset;
    LONGLONG length;
};

inline std::wstring HresultText(HRESULT result) {
    wchar_t code[16] = {};
    swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(result));
    return code;
}

// A test sync provider connected to a Windows Cloud Files sync root. It
// answers each data fetch with CloudProviderData for the fetched range. The
// destructor disconnects it.
class CloudProviderConnection {
public:
    // Takes ownership of key, a connection that CfConnectSyncRoot made.
    explicit CloudProviderConnection(CF_CONNECTION_KEY key) : key_(key) {}

    ~CloudProviderConnection() {
        ::CfDisconnectSyncRoot(key_);
    }

    CloudProviderConnection(const CloudProviderConnection&) = delete;
    CloudProviderConnection& operator=(const CloudProviderConnection&) = delete;

    // Connects a provider to the sync root at root and puts it in
    // connection. Returns the HRESULT of the connect and logs nothing.
    static HRESULT Connect(const std::wstring& root,
                           std::optional<CloudProviderConnection>& connection) {
        static const CF_CALLBACK_REGISTRATION callbacks[] = {
            {CF_CALLBACK_TYPE_FETCH_DATA, &CloudProviderConnection::ServeFetchData},
            CF_CALLBACK_REGISTRATION_END,
        };
        CF_CONNECTION_KEY key{};
        const HRESULT result = ::CfConnectSyncRoot(root.c_str(), callbacks, nullptr,
                                                   CF_CONNECT_FLAG_NONE, &key);
        if (SUCCEEDED(result)) {
            connection.emplace(key);
        }
        return result;
    }

private:
    static void CALLBACK ServeFetchData(const CF_CALLBACK_INFO* info,
                                        const CF_CALLBACK_PARAMETERS* parameters) {
        constexpr LONGLONG kChunk = 1024 * 1024;
        const LONGLONG fileSize = info->FileSize.QuadPart;
        LONGLONG offset = parameters->FetchData.RequiredFileOffset.QuadPart;
        const LONGLONG required = parameters->FetchData.RequiredLength.QuadPart;
        // CF_CALLBACK_PARAMETERS documents a fetch length of CF_EOF as "to end of file".
        const LONGLONG end =
            required == CF_EOF ? fileSize : (std::min)(fileSize, offset + required);
        while (offset < end) {
            const LONGLONG length = (std::min)(kChunk, end - offset);
            const std::string data = CloudProviderData(offset, static_cast<size_t>(length));
            const HRESULT result =
                TransferData(*info, STATUS_SUCCESS, data.data(), ByteRange{offset, length});
            if (FAILED(result)) {
                Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage(
                    (L"The test provider could not transfer the data at offset " +
                     std::to_wstring(offset) + L" (HRESULT " + HresultText(result) +
                     L"), so it fails the rest of the fetch").c_str());
                TransferData(*info, STATUS_CLOUD_FILE_UNSUCCESSFUL, nullptr,
                             ByteRange{offset, end - offset});
                return;
            }
            offset += length;
        }
    }

    // A transfer with a failure status fails each pending read of its
    // range with that status and ignores buffer. The status must be a
    // STATUS_CLOUD_FILE_* status (CF_OPERATION_PARAMETERS).
    static HRESULT TransferData(const CF_CALLBACK_INFO& info, NTSTATUS status,
                                const void* buffer, ByteRange range) {
        CF_OPERATION_INFO operation{};
        operation.StructSize = sizeof(operation);
        operation.Type = CF_OPERATION_TYPE_TRANSFER_DATA;
        operation.ConnectionKey = info.ConnectionKey;
        operation.TransferKey = info.TransferKey;
        CF_OPERATION_PARAMETERS transfer{};
        transfer.ParamSize = static_cast<ULONG>(
            FIELD_OFFSET(CF_OPERATION_PARAMETERS, TransferData) +
            sizeof(transfer.TransferData));
        transfer.TransferData.CompletionStatus = status;
        transfer.TransferData.Buffer = buffer;
        transfer.TransferData.Offset.QuadPart = range.offset;
        transfer.TransferData.Length.QuadPart = range.length;
        return ::CfExecute(&operation, &transfer);
    }

    CF_CONNECTION_KEY key_;
};

// Registers the directory at root as a Windows Cloud Files sync root and
// shows cloud placeholders to the calling thread as reparse points. A
// placeholder is a file or a directory. A cloud tag is not a name
// surrogate, so a placeholder is not a link. The sync root has full
// population, so a placeholder directory lists without a sync provider.
// The destructor disconnects the test provider, unregisters the sync root
// and restores the thread's placeholder mode. The unregister turns each
// placeholder directory back into a plain directory and removes each
// placeholder file that is still dehydrated in full or in part, so the
// tree deletes with no provider connected.
class CloudSyncRoot {
public:
    explicit CloudSyncRoot(std::wstring root) : root_(std::move(root)) {
        const auto setThreadMode = reinterpret_cast<SetPlaceholderMode>(::GetProcAddress(
            ::GetModuleHandleW(L"ntdll.dll"), "RtlSetThreadPlaceholderCompatibilityMode"));
        if (setThreadMode != nullptr) {
            // A negative return is an error code, not the previous mode.
            const CHAR previousMode = setThreadMode(kExposePlaceholders);
            if (previousMode >= 0) {
                restoreThreadMode_ = setThreadMode;
                previousThreadMode_ = previousMode;
            }
        }

        CF_SYNC_REGISTRATION registration{};
        registration.StructSize = sizeof(registration);
        registration.ProviderName = L"LayerMountTests";
        registration.ProviderVersion = L"1.0";
        registration.ProviderId =
            {0x5c3f1e2a, 0x7b4d, 0x4e8f, {0x9a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f, 0x60, 0x71}};
        CF_SYNC_POLICIES policies{};
        policies.StructSize = sizeof(policies);
        policies.Hydration.Primary = CF_HYDRATION_POLICY_FULL;
        policies.Population.Primary = CF_POPULATION_POLICY_ALWAYS_FULL;
        policies.InSync = CF_INSYNC_POLICY_NONE;
        policies.HardLink = CF_HARDLINK_POLICY_NONE;
        registerResult_ =
            ::CfRegisterSyncRoot(root_.c_str(), &registration, &policies, CF_REGISTER_FLAG_NONE);
    }

    ~CloudSyncRoot() {
        DisconnectProvider();
        if (SUCCEEDED(registerResult_)) {
            ::CfUnregisterSyncRoot(root_.c_str());
        }
        if (restoreThreadMode_ != nullptr) {
            restoreThreadMode_(previousThreadMode_);
        }
    }

    CloudSyncRoot(const CloudSyncRoot&) = delete;
    CloudSyncRoot& operator=(const CloudSyncRoot&) = delete;

    // Converts the file or directory at path, under the sync root, to an
    // in-sync placeholder. A converted file keeps its data. Logs a skip and
    // returns false when the platform refuses the sync root or the
    // conversion, or when path then shows no reparse attribute.
    bool PlaceholderMadeOrSkipped(const std::wstring& path) const {
        HRESULT result = registerResult_;
        if (SUCCEEDED(result)) {
            const ::LayerMount::ScopedHandle handle =
                OpenPlaceholder(path, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES);
            constexpr char kIdentity[] = "placeholder";
            result = handle.IsValid()
                ? ::CfConvertToPlaceholder(handle.Get(), kIdentity, sizeof(kIdentity),
                                           CF_CONVERT_FLAG_MARK_IN_SYNC, nullptr, nullptr)
                : HRESULT_FROM_WIN32(::GetLastError());
        }
        if (FAILED(result) || !HasAttribute(path, FILE_ATTRIBUTE_REPARSE_POINT)) {
            LogSkip(L"the test could not make a cloud placeholder of " + path, result);
            return false;
        }
        return true;
    }

    // Pins the placeholder file or directory at path, which gives it
    // FILE_ATTRIBUTE_PINNED. Logs a skip and returns false when the
    // platform refuses or path then shows no pinned attribute.
    bool PinnedOrSkipped(const std::wstring& path) const {
        const ::LayerMount::ScopedHandle handle =
            OpenPlaceholder(path, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES);
        const HRESULT result = handle.IsValid()
            ? ::CfSetPinState(handle.Get(), CF_PIN_STATE_PINNED, CF_SET_PIN_FLAG_NONE, nullptr)
            : HRESULT_FROM_WIN32(::GetLastError());
        if (FAILED(result) || !HasAttribute(path, FILE_ATTRIBUTE_PINNED)) {
            LogSkip(L"the test could not pin the cloud placeholder " + path, result);
            return false;
        }
        return true;
    }

    // Connects a CloudProviderConnection to the sync root. Logs a skip and
    // returns false when the platform refuses the sync root or the
    // connection.
    bool ProviderConnectedOrSkipped() {
        const HRESULT result = SUCCEEDED(registerResult_)
            ? CloudProviderConnection::Connect(root_, provider_)
            : registerResult_;
        if (FAILED(result)) {
            LogSkip(L"the test could not connect a sync provider to " + root_, result);
            return false;
        }
        return true;
    }

    void DisconnectProvider() {
        provider_.reset();
    }

    // Drops the local data of the in-sync placeholder file at path in
    // range, whose offset and length are multiples of 4 KB. Logs a skip
    // and returns false when the platform refuses the open or the
    // dehydration. Then asserts that the file is sparse, shows
    // FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS and keeps no dropped byte
    // allocated. No check reads the file, which would fetch its data.
    bool DehydratedOrSkipped(const std::wstring& path, ByteRange range) {
        using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
        const ::LayerMount::ScopedHandle handle = OpenPlaceholder(
            path, FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES);
        if (!handle.IsValid()) {
            LogSkip(L"the test could not open the cloud placeholder " + path,
                    HRESULT_FROM_WIN32(::GetLastError()));
            return false;
        }
        LARGE_INTEGER size{};
        Assert::IsTrue(::GetFileSizeEx(handle.Get(), &size) != FALSE,
            L"The test must read the size of the placeholder file");
        LARGE_INTEGER start{};
        start.QuadPart = range.offset;
        LARGE_INTEGER count{};
        count.QuadPart = range.length;
        const HRESULT result =
            ::CfDehydratePlaceholder(handle.Get(), start, count, CF_DEHYDRATE_FLAG_NONE, nullptr);
        if (FAILED(result)) {
            LogSkip(L"the test could not dehydrate the cloud placeholder " + path, result);
            return false;
        }
        Assert::IsTrue(HasAttribute(path, FILE_ATTRIBUTE_SPARSE_FILE),
            L"The dehydrated placeholder file must be sparse");
        Assert::IsTrue(HasAttribute(path, FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS),
            L"The dehydrated placeholder file must have FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS");
        Assert::IsTrue(AllocatedBytesWithoutFetch(path) <= size.QuadPart - range.length,
            L"The dehydrated placeholder file must keep no dropped byte allocated");
        return true;
    }

private:
    using SetPlaceholderMode = CHAR(NTAPI*)(CHAR);

    static LONGLONG AllocatedBytesWithoutFetch(const std::wstring& path) {
        DWORD high = 0;
        const DWORD low = ::GetCompressedFileSizeW(path.c_str(), &high);
        if (low == INVALID_FILE_SIZE && ::GetLastError() != NO_ERROR) {
            return LLONG_MAX;
        }
        return (static_cast<LONGLONG>(high) << 32) | low;
    }

    static constexpr CHAR kExposePlaceholders = 2;

    static ::LayerMount::ScopedHandle OpenPlaceholder(const std::wstring& path, DWORD access) {
        return ::LayerMount::ScopedHandle(::CreateFileW(
            path.c_str(), access,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    }

    static void LogSkip(const std::wstring& reason, HRESULT result) {
        Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage(
            (L"[SKIP] " + reason + L" (HRESULT " + HresultText(result) + L")").c_str());
    }

    std::wstring root_;
    HRESULT registerResult_ = E_FAIL;
    std::optional<CloudProviderConnection> provider_;
    SetPlaceholderMode restoreThreadMode_ = nullptr;
    CHAR previousThreadMode_ = 0;
};

// The layer of a CloudPlaceholderLayers that is the cloud sync root.
enum class SyncRootLayer { Lower, Upper };

// A one-lower layer environment whose lower or upper is a cloud sync root.
// syncRoot is built from a path of env, so env comes first, and the
// destruction in reverse order unregisters the sync root before the
// environment deletes the tree.
struct CloudPlaceholderLayers {
    explicit CloudPlaceholderLayers(SyncRootLayer layer)
        : syncRootPath(layer == SyncRootLayer::Upper ? env.Upper() : env.Lower(0))
        , syncRoot{syncRootPath} {}

    TempLayerEnvironment env{1};
    const std::wstring syncRootPath;
    CloudSyncRoot syncRoot;

    // Makes the existing directory at dir, relative to the sync root layer,
    // a placeholder. Logs a skip and returns false when the platform refuses.
    bool PlaceholderOrSkipped(const std::wstring& dir) {
        return syncRoot.PlaceholderMadeOrSkipped(syncRootPath + L"\\" + dir);
    }

    // Writes content to the file at file, then makes the directory at dir a
    // placeholder. Both paths are relative to the sync root layer. Logs a
    // skip and returns false when the platform refuses.
    bool PlaceholderWithFileOrSkipped(const std::wstring& dir,
                                      const std::wstring& file,
                                      const std::string& content) {
        env.WriteFile(syncRootPath, file, content);
        return PlaceholderOrSkipped(dir);
    }

    // Writes content to the file at file, relative to the sync root layer,
    // then makes that file an in-sync placeholder that keeps the content.
    // Logs a skip and returns false when the platform refuses.
    bool PlaceholderFileOrSkipped(const std::wstring& file, const std::string& content) {
        env.WriteFile(syncRootPath, file, content);
        return syncRoot.PlaceholderMadeOrSkipped(syncRootPath + L"\\" + file);
    }

    // Writes size bytes of CloudProviderData to the file at file, relative
    // to the sync root layer, makes that file an in-sync placeholder,
    // connects the test provider and drops the local data in dropped. Logs
    // a skip and returns false when the platform refuses.
    bool DehydratedPlaceholderFileOrSkipped(const std::wstring& file, size_t size,
                                            ByteRange dropped) {
        return PlaceholderFileOrSkipped(file, CloudProviderData(0, size)) &&
               syncRoot.ProviderConnectedOrSkipped() &&
               syncRoot.DehydratedOrSkipped(syncRootPath + L"\\" + file, dropped);
    }
};

// Returns up to length bytes at offset in the file at path, fewer when the
// file ends inside the range.
inline std::string ReadRange(const std::wstring& path, LONGLONG offset, DWORD length) {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    Assert::AreNotEqual<HANDLE>(INVALID_HANDLE_VALUE, h, L"ReadRange: open failed");
    LARGE_INTEGER pos{};
    pos.QuadPart = offset;
    Assert::IsTrue(::SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != FALSE);
    std::string buf(length, '\0');
    DWORD r = 0;
    Assert::IsTrue(::ReadFile(h, buf.data(), length, &r, nullptr) != FALSE);
    ::CloseHandle(h);
    buf.resize(r);
    return buf;
}

inline constexpr UINT32 kNoCreateOptions = 0u;
inline constexpr DWORD kNoCallerPid = 0u;
inline constexpr BOOLEAN kFailIfExists = FALSE;
inline constexpr BOOLEAN kReplaceIfExists = TRUE;
inline constexpr UINT64 kNoAllocationSize = 0u;
inline constexpr PSECURITY_DESCRIPTOR kDefaultSecurity = nullptr;

// Opens path for read through the mount, closes the handle, and returns
// the open's status.
inline NTSTATUS OpenThroughMount(::LayerMount::LayerMount& mount,
                                 const std::wstring& path) {
    std::unique_ptr<::LayerMount::FileContext> ctx;
    ::LayerMount::InternalFileInfo info{};
    const NTSTATUS status = mount.Open(path, FILE_READ_DATA,
                                       kNoCreateOptions, kNoCallerPid,
                                       &ctx, &info);
    if (ctx) mount.Close(ctx.get());
    return status;
}

// Creates path through the mount with full access, closes the handle, and
// returns the create's status. FILE_DIRECTORY_FILE in createOptions
// creates a directory.
inline NTSTATUS CreateThroughMount(::LayerMount::LayerMount& mount,
                                   const std::wstring& path,
                                   UINT32 createOptions) {
    std::unique_ptr<::LayerMount::FileContext> ctx;
    ::LayerMount::InternalFileInfo info{};
    const UINT32 attributes = (createOptions & FILE_DIRECTORY_FILE) != 0
        ? FILE_ATTRIBUTE_DIRECTORY
        : FILE_ATTRIBUTE_NORMAL;
    ::LayerMount::LayerMount::CreateRequest request{};
    request.relativePath = path;
    request.createOptions = createOptions;
    request.grantedAccess = FILE_ALL_ACCESS;
    request.fileAttributes = attributes;
    request.securityDescriptor = kDefaultSecurity;
    request.allocationSize = kNoAllocationSize;
    request.callerPid = kNoCallerPid;
    const NTSTATUS status = mount.Create(request, &ctx, &info);
    if (ctx) mount.Close(ctx.get());
    return status;
}

inline constexpr ULONG kReadThroughMountBytes = 64;

// Opens path through the mount and returns its first kReadThroughMountBytes
// bytes from the engine's Read. Fails the test when the open or the read fails.
inline std::string ReadThroughMount(::LayerMount::LayerMount& mount,
                                    const std::wstring& path) {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
    constexpr UINT64 fromStart = 0u;
    std::unique_ptr<::LayerMount::FileContext> ctx;
    ::LayerMount::InternalFileInfo info{};
    Assert::IsTrue(NT_SUCCESS(mount.Open(path, FILE_READ_DATA, kNoCreateOptions,
                                         kNoCallerPid, &ctx, &info)),
        L"ReadThroughMount: the open must succeed");
    char buffer[kReadThroughMountBytes] = {};
    ULONG transferred = 0;
    const NTSTATUS readStatus = mount.Read(ctx.get(), buffer, fromStart,
                                           sizeof(buffer), &transferred);
    mount.Close(ctx.get());
    Assert::IsTrue(NT_SUCCESS(readStatus), L"ReadThroughMount: the read must succeed");
    return std::string(buffer, transferred);
}

// Opens path through the mount and returns the file size that the open
// reports. Fails the test when the open fails.
inline UINT64 FileSizeThroughMount(::LayerMount::LayerMount& mount,
                                   const std::wstring& path) {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
    std::unique_ptr<::LayerMount::FileContext> ctx;
    ::LayerMount::InternalFileInfo info{};
    Assert::IsTrue(NT_SUCCESS(mount.Open(path, FILE_READ_DATA, kNoCreateOptions,
                                         kNoCallerPid, &ctx, &info)),
        L"FileSizeThroughMount: the open must succeed");
    mount.Close(ctx.get());
    return info.FileSize;
}

// Opens path through the mount for its attributes and returns the
// last-write time that the open reports. Fails the test when the open
// fails.
inline UINT64 LastWriteTimeThroughMount(::LayerMount::LayerMount& mount,
                                        const std::wstring& path) {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
    std::unique_ptr<::LayerMount::FileContext> ctx;
    ::LayerMount::InternalFileInfo info{};
    Assert::IsTrue(NT_SUCCESS(mount.Open(path, FILE_READ_ATTRIBUTES, kNoCreateOptions,
                                         kNoCallerPid, &ctx, &info)),
        L"LastWriteTimeThroughMount: the open must succeed");
    mount.Close(ctx.get());
    return info.LastWriteTime;
}

// Opens path through the mount for its attributes and returns the
// attribute bits that the open reports. Fails the test when the open
// fails.
inline UINT32 FileAttributesThroughMount(::LayerMount::LayerMount& mount,
                                         const std::wstring& path) {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
    std::unique_ptr<::LayerMount::FileContext> ctx;
    ::LayerMount::InternalFileInfo info{};
    Assert::IsTrue(NT_SUCCESS(mount.Open(path, FILE_READ_ATTRIBUTES, kNoCreateOptions,
                                         kNoCallerPid, &ctx, &info)),
        L"FileAttributesThroughMount: the open must succeed");
    mount.Close(ctx.get());
    return info.FileAttributes;
}

enum class LinkTarget { File, Directory };

// The target.txt file or the target directory under the environment root
// that a link test points its link at.
inline std::wstring LinkTargetPath(const TempLayerEnvironment& env, LinkTarget targetKind) {
    return env.Root() + (targetKind == LinkTarget::File ? L"\\target.txt" : L"\\target");
}

// Writes target.txt and target\inside.txt under the environment root and
// creates link to the one that targetKind names. Logs a skip and returns
// false when the link cannot be created.
inline bool LinkToTargetCreatedOrSkipped(const TempLayerEnvironment& env,
                                         LinkCreator createLink,
                                         LinkTarget targetKind,
                                         const std::wstring& link) {
    env.WriteFile(env.Root(), L"target.txt", "target");
    env.WriteFile(env.Root(), L"target\\inside.txt", "inside");
    return LinkCreatedOrSkipped(createLink, link, LinkTargetPath(env, targetKind));
}

// The IndexNumber that an open of path through the mount reports.
// FILE_OPEN_REPARSE_POINT in createOptions opens a link itself. Fails the
// test when the open fails.
inline UINT64 IndexNumberThroughMount(::LayerMount::LayerMount& mount,
                                      const std::wstring& path,
                                      UINT32 createOptions) {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
    std::unique_ptr<::LayerMount::FileContext> ctx;
    ::LayerMount::InternalFileInfo info{};
    Assert::IsTrue(NT_SUCCESS(mount.Open(path, FILE_READ_ATTRIBUTES, createOptions,
                                         kNoCallerPid, &ctx, &info)),
        (L"The open of " + path + L" through the mount must succeed").c_str());
    mount.Close(ctx.get());
    return info.IndexNumber;
}

struct CopyUpAndRenameRig {
    explicit CopyUpAndRenameRig(LayerMount::LayerConfig layerConfig)
        : config(std::move(layerConfig)),
          cache(),
          whiteouts(config, &cache),
          resolver(config, whiteouts, cache),
          stats(),
          copyUp(config, resolver, whiteouts, cache, stats),
          directoryRename(config, resolver, whiteouts, cache, copyUp) {}

    LayerMount::LayerConfig      config;
    LayerMount::Cache            cache;
    LayerMount::WhiteoutManager  whiteouts;
    LayerMount::PathResolver     resolver;
    LayerMount::LayerMountStats  stats;
    LayerMount::CopyUp           copyUp;
    LayerMount::DirectoryRename  directoryRename;
};

struct FreshThreadOutcome {
    DWORD              sehCode   = 0;
    DWORD              numParams = 0;
    std::wstring       message;
    std::exception_ptr cppException;
};

inline LONG CaptureFreshThreadFailure(EXCEPTION_POINTERS* ep,
                                      FreshThreadOutcome* out) noexcept {
    const EXCEPTION_RECORD& rec = *ep->ExceptionRecord;
    out->sehCode   = rec.ExceptionCode;
    out->numParams = rec.NumberParameters;
    if (rec.ExceptionCode == ERROR_ASSERT_FAILED && rec.NumberParameters >= 1
        && rec.ExceptionInformation[0] != 0) {
        out->message = reinterpret_cast<const wchar_t*>(rec.ExceptionInformation[0]);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// No objects with destructors in this function: __try forbids them (C2712).
inline void RunUnderSehCapture(const std::function<void()>* body,
                               FreshThreadOutcome* out) {
    __try {
        (*body)();
    } __except (CaptureFreshThreadFailure(GetExceptionInformation(), out)) {
    }
}

// Runs body on a new thread and fails the test with any failure it raised.
// The test host's thread already holds a COM apartment, so
// CoInitializeEx(COINIT_MULTITHREADED) fails there with RPC_E_CHANGED_MODE.
// The test framework raises an assertion failure as a structured exception
// (ERROR_ASSERT_FAILED); the worker copies its message while the raising
// frame is still alive.
inline void RunOnComFreeThread(std::function<void()> body) {
    namespace cuf = Microsoft::VisualStudio::CppUnitTestFramework;

    FreshThreadOutcome outcome;
    std::function<void()> guarded = [&] {
        try {
            body();
        } catch (...) {
            outcome.cppException = std::current_exception();
        }
    };
    std::thread worker([&] { RunUnderSehCapture(&guarded, &outcome); });
    worker.join();

    if (outcome.cppException) {
        std::rethrow_exception(outcome.cppException);
    }
    if (outcome.sehCode == ERROR_ASSERT_FAILED) {
        if (outcome.message.empty()) {
            outcome.message = L"assertion failed on the worker thread, message "
                              L"not available (parameters: "
                              + std::to_wstring(outcome.numParams) + L")";
        }
        cuf::Assert::Fail(outcome.message.c_str());
    }
    if (outcome.sehCode != 0) {
        wchar_t buf[80] = {};
        swprintf_s(buf, L"structured exception 0x%08X on the worker thread",
                   static_cast<unsigned>(outcome.sehCode));
        cuf::Assert::Fail(buf);
    }
}

}
