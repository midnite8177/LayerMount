#pragma once

#include "pch.h"

#include "LayerMount.h"
#include "Cache.h"
#include "CopyUp.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"

#include "AclTestHelpers.h"

#include <exception>
#include <functional>
#include <rpc.h>
#include <type_traits>
#include <winioctl.h>

namespace LayerMountTests {

namespace fs = std::filesystem;

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
        c.hostCapabilities = LM_CAP_ADS | LM_CAP_REPARSE_POINTS |
                             LM_CAP_SPARSE_FILES | LM_CAP_MULTIPLE_STREAMS |
                             LM_CAP_NTFS_ACLS;
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

// A FILETIME for noon UTC on the given day.
inline FILETIME MakeFileTime(WORD year, WORD month, WORD day) {
    SYSTEMTIME st{};
    st.wYear = year;
    st.wMonth = month;
    st.wDay = day;
    st.wHour = 12;
    FILETIME ft{};
    ::SystemTimeToFileTime(&st, &ft);
    return ft;
}

inline void StampFile(const std::wstring& path,
                      const FILETIME& creation,
                      const FILETIME& access,
                      const FILETIME& write) {
    HANDLE h = ::CreateFileW(path.c_str(),
                              FILE_WRITE_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    Microsoft::VisualStudio::CppUnitTestFramework::Assert::AreNotEqual<HANDLE>(
        INVALID_HANDLE_VALUE, h, L"StampFile: CreateFileW must succeed");
    ::SetFileTime(h, &creation, &access, &write);
    ::CloseHandle(h);
}

inline void GetTimes(const std::wstring& path,
                     FILETIME* creation, FILETIME* access, FILETIME* write) {
    HANDLE h = ::CreateFileW(path.c_str(),
                              FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    Microsoft::VisualStudio::CppUnitTestFramework::Assert::AreNotEqual<HANDLE>(
        INVALID_HANDLE_VALUE, h, L"GetTimes: CreateFileW must succeed");
    ::GetFileTime(h, creation, access, write);
    ::CloseHandle(h);
}

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

// Opens path through the mount and returns its first 64 bytes from the
// engine's Read. Fails the test when the open or the read fails.
inline std::string ReadThroughMount(::LayerMount::LayerMount& mount,
                                    const std::wstring& path) {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
    constexpr UINT64 fromStart = 0u;
    std::unique_ptr<::LayerMount::FileContext> ctx;
    ::LayerMount::InternalFileInfo info{};
    Assert::IsTrue(NT_SUCCESS(mount.Open(path, FILE_READ_DATA, kNoCreateOptions,
                                         kNoCallerPid, &ctx, &info)),
        L"ReadThroughMount: the open must succeed");
    char buffer[64] = {};
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

// A CopyUp and the objects it depends on, all built from a copy of
// layerConfig that the rig owns.
struct CopyUpRig {
    explicit CopyUpRig(LayerMount::LayerConfig layerConfig)
        : config(std::move(layerConfig)),
          cache(),
          whiteouts(config, &cache),
          resolver(config, whiteouts, cache),
          stats(),
          copyUp(config, resolver, whiteouts, cache, stats) {}

    LayerMount::LayerConfig      config;
    LayerMount::Cache            cache;
    LayerMount::WhiteoutManager  whiteouts;
    LayerMount::PathResolver     resolver;
    LayerMount::LayerMountStats  stats;
    LayerMount::CopyUp           copyUp;
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
