#pragma once

// Shared scaffolding for AbiTests. Everything here sits on top of the public
// C ABI only -- no impl/ headers, no static lib. Tests build a config with
// TempLayerEnv + MakeConfig, drive the ABI, and let RAII clean up.

#include "pch.h"

#include "AclTestHelpers.h"
#include "LongPathTestHelpers.h"

#include <algorithm>
#include <iterator>

namespace LayerMountAbiTests {

// The engine stages a metacopy shell only for a lower file larger than
// 1 MiB, so a 2 MiB lower file stages a shell.
constexpr UINT64 kAboveMetacopyThresholdBytes = 2ull * 1024 * 1024;

constexpr UINT32 kAttributeOnlyAccess = FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES;

// -----------------------------------------------------------------------------
// TempLayerEnv -- creates %TEMP%\LayerMountAbi_<uuid>\{upper,work,lowerN}\ and
// removes the tree on destruction. Mirrors the shape of the existing unit /
// integration fixtures without depending on them.
// -----------------------------------------------------------------------------
class TempLayerEnv {
public:
    explicit TempLayerEnv(size_t lowerCount = 1) {
        wchar_t tempDir[MAX_PATH] = {};
        ::GetTempPathW(MAX_PATH, tempDir);
        GUID g{};
        (void)::CoCreateGuid(&g);
        wchar_t guidBuf[64] = {};
        ::StringFromGUID2(g, guidBuf, 64);
        root_ = std::wstring(tempDir) + L"LayerMountAbi_" + guidBuf;

        std::filesystem::create_directories(root_);
        upper_ = root_ + L"\\upper";
        work_  = root_ + L"\\work";
        std::filesystem::create_directories(upper_);
        std::filesystem::create_directories(work_);
        for (size_t i = 0; i < lowerCount; ++i) {
            std::wstring l = root_ + L"\\lower" + std::to_wstring(i);
            std::filesystem::create_directories(l);
            lowers_.push_back(std::move(l));
        }
    }

    ~TempLayerEnv() {
        LayerMountTestShared::RemoveTreeOfAnyDepth(root_);
    }

    TempLayerEnv(const TempLayerEnv&) = delete;
    TempLayerEnv& operator=(const TempLayerEnv&) = delete;

    const std::wstring& Root()  const noexcept { return root_; }
    const std::wstring& Upper() const noexcept { return upper_; }
    const std::wstring& Work()  const noexcept { return work_; }
    const std::wstring& Lower(size_t i) const { return lowers_.at(i); }
    size_t LowerCount() const noexcept { return lowers_.size(); }

    // Write a blob into lowerN\<relativePath>. Creates intermediate dirs.
    void WriteLowerFile(size_t index, const std::wstring& relative,
                        const std::string& contents) const {
        WriteFileUnder(Lower(index), relative, contents);
    }

    // Write a blob into upper\<relativePath>. Creates intermediate dirs.
    void WriteUpperFile(const std::wstring& relative,
                        const std::string& contents) const {
        WriteFileUnder(Upper(), relative, contents);
    }

private:
    static void WriteFileUnder(const std::wstring& root, const std::wstring& relative,
                               const std::string& contents) {
        auto path = std::filesystem::path(root) / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    std::wstring              root_;
    std::wstring              upper_;
    std::wstring              work_;
    std::vector<std::wstring> lowers_;
};

// The staging area the engine keeps in the work directory at workDir.
inline std::wstring StagingArea(const std::wstring& workDir) {
    return workDir + L"\\work";
}

// Write `len` bytes to the named data stream at `path` on the host file,
// outside the engine.
inline void WriteRawStream(const std::wstring& path, const char* data, DWORD len) {
    HANDLE h = ::CreateFileW(path.c_str(),
        GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
    Assert::IsTrue(h != INVALID_HANDLE_VALUE, L"open ADS for write");
    DWORD written = 0;
    BOOL ok = ::WriteFile(h, data, len, &written, nullptr);
    ::CloseHandle(h);
    Assert::IsTrue(ok != FALSE,        L"WriteFile to ADS succeeded");
    Assert::AreEqual<DWORD>(len, written, L"WriteFile wrote full payload");
}

// Read the whole file at `path` as bytes.
inline std::string ReadAllBytes(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

// The names of the entries directly in `directory`, in sorted order.
inline std::vector<std::wstring> SortedNamesIn(const std::wstring& directory) {
    std::vector<std::wstring> names;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        names.push_back(entry.path().filename().wstring());
    }
    std::sort(names.begin(), names.end());
    return names;
}

// Writes `text` to the file at `path`. Makes the parent directories first.
inline void WriteText(const std::wstring& path, const std::string& text) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

inline std::wstring LastErrorMessage(HRESULT hr) {
    std::vector<wchar_t> message(4096);
    SIZE_T required = 0;
    if (FAILED(::LayerMountGetLastErrorMessage(hr, message.data(), message.size(), &required))) {
        return {};
    }
    return message.data();
}

inline std::wstring FileNameOf(const std::wstring& path) {
    return std::filesystem::path(path).filename().wstring();
}

inline std::wstring ShortPathOf(const std::wstring& path) {
    const DWORD needed = ::GetShortPathNameW(path.c_str(), nullptr, 0);
    if (needed == 0) return path;
    std::wstring shortPath(needed, L'\0');
    const DWORD written = ::GetShortPathNameW(path.c_str(), shortPath.data(), needed);
    shortPath.resize(written);
    return shortPath;
}

// Enables SE_RESTORE_NAME for the life of the object, and then puts back the
// state the token had before.
class EnabledRestorePrivilege {
public:
    EnabledRestorePrivilege() {
        if (!::OpenProcessToken(::GetCurrentProcess(),
                                TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token_)) {
            token_ = nullptr;
            return;
        }
        TOKEN_PRIVILEGES wanted{};
        wanted.PrivilegeCount = 1;
        wanted.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (!::LookupPrivilegeValueW(nullptr, SE_RESTORE_NAME, &wanted.Privileges[0].Luid)) {
            return;
        }
        DWORD previousSize = 0;
        held_ = ::AdjustTokenPrivileges(token_, FALSE, &wanted, sizeof(previous_), &previous_,
                                        &previousSize) != FALSE
             && ::GetLastError() == ERROR_SUCCESS;
    }

    ~EnabledRestorePrivilege() {
        if (token_ == nullptr) return;
        if (held_) ::AdjustTokenPrivileges(token_, FALSE, &previous_, 0, nullptr, nullptr);
        ::CloseHandle(token_);
    }

    EnabledRestorePrivilege(const EnabledRestorePrivilege&) = delete;
    EnabledRestorePrivilege& operator=(const EnabledRestorePrivilege&) = delete;

    bool Held() const noexcept { return held_; }

private:
    HANDLE           token_ = nullptr;
    TOKEN_PRIVILEGES previous_{};
    bool             held_ = false;
};

inline bool GiveShortName(const std::wstring& path, PCWSTR shortName) {
    const EnabledRestorePrivilege privilege;
    if (!privilege.Held()) return false;
    HANDLE handle = ::CreateFileW(path.c_str(), DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const BOOL named = ::SetFileShortNameW(handle, shortName);
    ::CloseHandle(handle);
    return named != FALSE;
}

// The short name of the entry at `path`. A volume that makes no short names
// gives the entry none, so the function then sets `OVRLAY~1`. It returns no
// value when the volume refuses short names.
inline std::optional<std::wstring> ShortNameOf(const std::wstring& path) {
    const std::wstring longName = FileNameOf(path);
    std::wstring shortName = FileNameOf(ShortPathOf(path));
    if (_wcsicmp(shortName.c_str(), longName.c_str()) != 0) return shortName;
    if (!GiveShortName(path, L"OVRLAY~1")) return std::nullopt;
    shortName = FileNameOf(ShortPathOf(path));
    if (_wcsicmp(shortName.c_str(), longName.c_str()) != 0) return shortName;
    return std::nullopt;
}

constexpr DWORD kCurrentProcessOriginator = 0u;

constexpr UINT32 kNoCreateOptions = 0u;

constexpr UINT32 kKeepStoredAttributes = INVALID_FILE_ATTRIBUTES;
constexpr UINT64 kKeepStoredTime       = 0u;
constexpr UINT64 kKeepStoredSize       = UINT64_MAX;

struct OpenedFile {
    LM_FILE_HANDLE handle = nullptr;
    LM_FILE_INFO   info{};
};

inline HRESULT OpenOverlayFileAs(LM_HANDLE mount, PCWSTR relativePath,
                                 UINT32 grantedAccess, UINT32 createOptions,
                                 DWORD originatorPid, OpenedFile& out) {
    return ::LayerMountOpenFile(mount, relativePath, grantedAccess, createOptions,
                                originatorPid, &out.handle, &out.info);
}

inline HRESULT OpenOverlayFile(LM_HANDLE mount, PCWSTR relativePath,
                               UINT32 grantedAccess, UINT32 createOptions,
                               OpenedFile& out) {
    return OpenOverlayFileAs(mount, relativePath, grantedAccess, createOptions,
                             kCurrentProcessOriginator, out);
}

inline HRESULT CreateOverlayFileWithDescriptor(LM_HANDLE mount, PCWSTR relativePath,
                                               UINT32 grantedAccess, UINT32 createOptions,
                                               UINT32 fileAttributes,
                                               const BYTE* securityDescriptor,
                                               SIZE_T securityDescriptorBytes,
                                               OpenedFile& out) {
    constexpr UINT64 allocationSize = 0u;
    return ::LayerMountCreateFile(mount, relativePath, createOptions, grantedAccess,
                                  fileAttributes, securityDescriptor,
                                  securityDescriptorBytes, allocationSize,
                                  kCurrentProcessOriginator, &out.handle, &out.info);
}

inline HRESULT CreateOverlayFile(LM_HANDLE mount, PCWSTR relativePath,
                                 UINT32 grantedAccess, UINT32 createOptions,
                                 UINT32 fileAttributes, OpenedFile& out) {
    const BYTE*      securityDescriptor      = nullptr;
    constexpr SIZE_T securityDescriptorBytes = 0u;
    return CreateOverlayFileWithDescriptor(mount, relativePath, grantedAccess,
                                           createOptions, fileAttributes,
                                           securityDescriptor,
                                           securityDescriptorBytes, out);
}

inline HRESULT ReadFromStart(LM_FILE_HANDLE fh, void* buffer, UINT32 length,
                             UINT32* outCount) {
    constexpr UINT64 offset = 0u;
    return ::LayerMountReadFile(fh, buffer, offset, length, outCount);
}

inline HRESULT WriteFromStart(LM_FILE_HANDLE fh, const void* buffer, UINT32 length,
                              UINT32* outCount, LM_FILE_INFO* outInfo) {
    constexpr UINT64 offset        = 0u;
    constexpr BOOL   writeToEnd    = FALSE;
    constexpr BOOL   constrainedIo = FALSE;
    return ::LayerMountWriteFile(fh, buffer, offset, length, writeToEnd, constrainedIo,
                                 outCount, outInfo);
}

inline HRESULT OverwriteAddingAttributes(LM_FILE_HANDLE fh, UINT32 fileAttributes,
                                         UINT64 allocationSize, LM_FILE_INFO* outInfo) {
    constexpr BOOL replaceAttributes = FALSE;
    return ::LayerMountOverwriteFile(fh, fileAttributes, replaceAttributes,
                                     allocationSize, outInfo);
}

// Each default is the value LayerMountSetFileInfo reads as "keep the stored value".
struct FileInfoChange {
    UINT32 fileAttributes = kKeepStoredAttributes;
    UINT64 creationTime   = kKeepStoredTime;
    UINT64 lastAccessTime = kKeepStoredTime;
    UINT64 lastWriteTime  = kKeepStoredTime;
    UINT64 changeTime     = kKeepStoredTime;
    UINT64 allocationSize = kKeepStoredSize;
    UINT64 fileSize       = kKeepStoredSize;
};

inline HRESULT SetFileInfo(LM_FILE_HANDLE fh, const FileInfoChange& change,
                           LM_FILE_INFO* outInfo) {
    return ::LayerMountSetFileInfo(
        fh,
        change.fileAttributes,
        change.creationTime,
        change.lastAccessTime,
        change.lastWriteTime,
        change.changeTime,
        change.allocationSize,
        change.fileSize,
        outInfo);
}

// Read the first 64 bytes of `fh` through the ABI and return them; the
// read's HRESULT lands in *outHr.
inline std::string ReadThroughHandle(LM_FILE_HANDLE fh, HRESULT* outHr) {
    char   buffer[64] = {};
    UINT32 transferred = 0;
    *outHr = ReadFromStart(fh, buffer, sizeof(buffer), &transferred);
    return std::string(buffer, transferred);
}

// Write `rulesJson` as the process-tracker rules file under the
// environment root and return its path.
inline std::wstring WriteTrackerRules(const TempLayerEnv& env,
                                      const std::string& rulesJson) {
    const std::wstring rulesPath = env.Root() + L"\\rules.json";
    std::ofstream f(rulesPath, std::ios::binary | std::ios::trunc);
    f.write(rulesJson.data(), static_cast<std::streamsize>(rulesJson.size()));
    return rulesPath;
}

// Write a lower file above the metacopy threshold, filled with 'L', and
// return its content. An open for data access of this file through the
// mount stages a metacopy shell on the upper.
inline std::string WriteLargeLowerFile(const TempLayerEnv& env,
                                       size_t lowerIndex,
                                       const std::wstring& relative) {
    const std::string content(
        static_cast<size_t>(kAboveMetacopyThresholdBytes), 'L');
    env.WriteLowerFile(lowerIndex, relative, content);
    return content;
}

// -----------------------------------------------------------------------------
// ConfigBuilder -- owns the string storage that LM_CONFIG's PCWSTR fields
// point into. LayerMountCreate copies every field synchronously, but we still
// keep the strings alive until LayerMountCreate returns.
// -----------------------------------------------------------------------------
struct ConfigBuilder {
    explicit ConfigBuilder(const TempLayerEnv& env,
                           UINT32 capabilities =
                               LM_CAP_ADS | LM_CAP_REPARSE_POINTS |
                               LM_CAP_SPARSE_FILES | LM_CAP_MULTIPLE_STREAMS |
                               LM_CAP_NTFS_ACLS)
        : upper_(env.Upper()), work_(env.Work()) {
        for (size_t i = 0; i < env.LowerCount(); ++i) {
            lowerStorage_.push_back(env.Lower(i));
        }
        lowerPointers_.reserve(lowerStorage_.size());
        for (const auto& s : lowerStorage_) {
            lowerPointers_.push_back(s.c_str());
        }

        std::memset(&cfg_, 0, sizeof(cfg_));
        cfg_.structSize            = sizeof(LM_CONFIG);
        cfg_.abiVersion            = LM_ABI_VERSION;
        cfg_.hostCapabilities      = capabilities;
        cfg_.accessLogCapacity     = 256;
        cfg_.pathCacheCapacity     = 256;
        cfg_.enableProcessTracking = FALSE;
        cfg_.lowerPathCount        = static_cast<UINT32>(lowerPointers_.size());
        cfg_.upperPath             = upper_.c_str();
        cfg_.workDirPath           = work_.c_str();
        cfg_.processRulesPath      = nullptr;
        cfg_.lowerPaths            = lowerPointers_.empty()
                                         ? nullptr
                                         : lowerPointers_.data();
    }

    ConfigBuilder(const ConfigBuilder&) = delete;
    ConfigBuilder& operator=(const ConfigBuilder&) = delete;

    LM_CONFIG*       Ptr()       noexcept { return &cfg_; }
    const LM_CONFIG* Ptr() const noexcept { return &cfg_; }

    void SetCapabilities(UINT32 caps) noexcept { cfg_.hostCapabilities = caps; }
    void SetAbiVersion(UINT32 v)      noexcept { cfg_.abiVersion = v; }
    void SetStructSize(UINT32 s)      noexcept { cfg_.structSize = s; }
    void SetUpperPath(PCWSTR p)       noexcept { cfg_.upperPath = p; }

    void EnableProcessTrackingWithRules(const std::wstring& rulesPath) {
        rules_ = rulesPath;
        cfg_.enableProcessTracking = TRUE;
        cfg_.processRulesPath      = rules_.c_str();
    }

private:
    LM_CONFIG               cfg_{};
    std::wstring             upper_;
    std::wstring             work_;
    std::wstring             rules_;
    std::vector<std::wstring> lowerStorage_;
    std::vector<PCWSTR>      lowerPointers_;
};

// -----------------------------------------------------------------------------
// LayerMountHolder -- RAII wrapper around LM_HANDLE.
// -----------------------------------------------------------------------------
class LayerMountHolder {
public:
    LayerMountHolder() = default;

    explicit LayerMountHolder(LM_HANDLE h) : handle_(h) {}

    ~LayerMountHolder() {
        if (handle_) {
            (void)::LayerMountDestroy(handle_);
        }
    }

    LayerMountHolder(const LayerMountHolder&)            = delete;
    LayerMountHolder& operator=(const LayerMountHolder&) = delete;

    LayerMountHolder(LayerMountHolder&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    LayerMountHolder& operator=(LayerMountHolder&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_       = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    LM_HANDLE  Get()     const noexcept { return handle_; }
    LM_HANDLE* AddressOf()     noexcept { return &handle_; }

    LM_HANDLE Release() noexcept {
        LM_HANDLE h = handle_;
        handle_      = nullptr;
        return h;
    }

    void Reset() noexcept {
        if (handle_) {
            (void)::LayerMountDestroy(handle_);
            handle_ = nullptr;
        }
    }

    explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    LM_HANDLE handle_ = nullptr;
};

// -----------------------------------------------------------------------------
// VhdHandleHolder -- RAII wrapper around LM_VHD_HANDLE.
// -----------------------------------------------------------------------------
class VhdHandleHolder {
public:
    VhdHandleHolder() = default;

    explicit VhdHandleHolder(LM_VHD_HANDLE h) : handle_(h) {}

    ~VhdHandleHolder() {
        if (handle_) {
            (void)::LayerMountVhdClose(handle_);
        }
    }

    VhdHandleHolder(const VhdHandleHolder&)            = delete;
    VhdHandleHolder& operator=(const VhdHandleHolder&) = delete;

    VhdHandleHolder(VhdHandleHolder&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    VhdHandleHolder& operator=(VhdHandleHolder&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_       = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    LM_VHD_HANDLE  Get()     const noexcept { return handle_; }
    LM_VHD_HANDLE* AddressOf()     noexcept { return &handle_; }

    LM_VHD_HANDLE Release() noexcept {
        LM_VHD_HANDLE h = handle_;
        handle_         = nullptr;
        return h;
    }

    void Reset() noexcept {
        if (handle_) {
            (void)::LayerMountVhdClose(handle_);
            handle_ = nullptr;
        }
    }

    explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    LM_VHD_HANDLE handle_ = nullptr;
};

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

// Convenience: build an overlay from env's defaults and assert creation.
inline LayerMountHolder CreateLayerMount(const TempLayerEnv& env,
                                   UINT32 caps =
                                       LM_CAP_ADS | LM_CAP_REPARSE_POINTS |
                                       LM_CAP_SPARSE_FILES | LM_CAP_MULTIPLE_STREAMS |
                                       LM_CAP_NTFS_ACLS) {
    ConfigBuilder b(env, caps);
    LM_HANDLE h = nullptr;
    HRESULT hr = ::LayerMountCreate(b.Ptr(), &h);
    Microsoft::VisualStudio::CppUnitTestFramework::Assert::AreEqual<HRESULT>(
        S_OK, hr, L"LayerMountCreate should succeed for the default env config");
    Microsoft::VisualStudio::CppUnitTestFramework::Assert::IsNotNull(
        h, L"LayerMountCreate should return a non-null handle");
    return LayerMountHolder(h);
}

inline LayerMountHolder CreateTransient(const std::wstring& upper) {
    LM_HANDLE handle = nullptr;
    Microsoft::VisualStudio::CppUnitTestFramework::Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountCreateTransient(upper.c_str(), LM_CAP_NONE, &handle),
        L"LayerMountCreateTransient");
    return LayerMountHolder(handle);
}

inline LM_HANDLE DestroyedMountHandle(const TempLayerEnv& env) {
    LayerMountHolder mount = CreateLayerMount(env);
    const LM_HANDLE stale = mount.Get();
    Microsoft::VisualStudio::CppUnitTestFramework::Assert::AreEqual<HRESULT>(
        S_OK, ::LayerMountDestroy(mount.Release()));
    return stale;
}

inline void WriteThroughOverlay(LM_HANDLE mount, PCWSTR relativePath, const std::string& text) {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
    OpenedFile created;
    Assert::AreEqual<HRESULT>(S_OK,
        CreateOverlayFile(mount, relativePath, GENERIC_READ | GENERIC_WRITE,
                          kNoCreateOptions, FILE_ATTRIBUTE_NORMAL, created),
        L"LayerMountCreateFile");
    UINT32 written = 0;
    LM_FILE_INFO info{};
    Assert::AreEqual<HRESULT>(S_OK,
        WriteFromStart(created.handle, text.data(), static_cast<UINT32>(text.size()),
                       &written, &info),
        L"LayerMountWriteFile");
    Assert::AreEqual<HRESULT>(S_OK, ::LayerMountCloseFile(created.handle));
}

inline LM_VHD_CONFIG ProcessScopedVhdConfig(const std::wstring& vhdPath) {
    LM_VHD_CONFIG cfg{};
    cfg.structSize          = sizeof(cfg);
    cfg.kind                = LM_VHD_KIND_DYNAMIC;
    cfg.path                = vhdPath.c_str();
    cfg.suppressDriveLetter = TRUE;
    cfg.lifetime            = LM_VHD_ATTACH_PROCESS_SCOPED;
    return cfg;
}

// Returns the root of the attached VHD's volume, which ends in a backslash.
inline std::wstring AttachedVhdVolumeRoot(LM_VHD_HANDLE vhd) {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
    // The volume can show up after the attach returns.
    wchar_t volumeGuid[MAX_PATH] = {};
    SIZE_T required = 0;
    HRESULT hr = E_FAIL;
    for (int attempt = 0; attempt < 40 && FAILED(hr); ++attempt) {
        hr = ::LayerMountVhdGetVolumeGuid(vhd, volumeGuid, MAX_PATH, &required);
        if (FAILED(hr)) {
            ::Sleep(250);
        }
    }
    Assert::AreEqual<HRESULT>(S_OK, hr, L"LayerMountVhdGetVolumeGuid finds the VHD's volume");
    std::wstring volumeRoot = volumeGuid;
    if (volumeRoot.empty() || volumeRoot.back() != L'\\') {
        volumeRoot += L'\\';
    }
    return volumeRoot;
}

// Opens the VHD that `cfg` names, attaches it and returns the root of its
// volume, which ends in a backslash. A ProcessScoped attach ends when *vhd
// closes.
inline std::wstring AttachVhdVolumeWithConfig(LM_HANDLE mount, const LM_VHD_CONFIG& cfg,
                                              LM_VHD_HANDLE* vhd) {
    using Microsoft::VisualStudio::CppUnitTestFramework::Assert;
    Assert::AreEqual<HRESULT>(S_OK, ::LayerMountVhdOpen(mount, &cfg, vhd),
        L"LayerMountVhdOpen opens the VHD");
    wchar_t physicalPath[MAX_PATH] = {};
    SIZE_T required = 0;
    Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountVhdAttach(*vhd, physicalPath, MAX_PATH, &required),
        L"LayerMountVhdAttach attaches the VHD");
    return AttachedVhdVolumeRoot(*vhd);
}

inline std::wstring AttachVhdVolumeReadOnly(LM_HANDLE mount, const std::wstring& vhdPath,
                                            LM_VHD_HANDLE* vhd) {
    LM_VHD_CONFIG cfg = ProcessScopedVhdConfig(vhdPath);
    cfg.readOnly = TRUE;
    return AttachVhdVolumeWithConfig(mount, cfg, vhd);
}

inline std::wstring AttachVhdVolumeWritable(LM_HANDLE mount, const std::wstring& vhdPath,
                                            LM_VHD_HANDLE* vhd) {
    LM_VHD_CONFIG cfg = ProcessScopedVhdConfig(vhdPath);
    cfg.readOnly = FALSE;
    return AttachVhdVolumeWithConfig(mount, cfg, vhd);
}

// Mount, VHD attach and VSS need elevation. A skip macro below logs and
// returns, so a skipped test reports a pass.
inline bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elev{};
    DWORD cb = sizeof(elev);
    BOOL ok = ::GetTokenInformation(token, TokenElevation, &elev, cb, &cb);
    ::CloseHandle(token);
    return ok != FALSE && elev.TokenIsElevated != 0;
}

#define ABI_SKIP_IF_NOT_ADMIN()                                                  \
    do {                                                                         \
        if (!::LayerMountAbiTests::IsProcessElevated()) {                         \
            Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage( \
                L"Skipping: requires elevation");                                \
            return;                                                              \
        }                                                                        \
    } while (0)

// The structSize that reaches the end of `field`, computed apart from the
// DLL's own check.
#define ABI_SIZE_THROUGH(StructType, field) \
    static_cast<UINT32>(offsetof(StructType, field) + sizeof(StructType::field))

#define ABI_SKIP_IF_NO_SECURITY_PRIVILEGE()                                      \
    do {                                                                         \
        if (!::LayerMountTestShared::TryEnablePrivilege(SE_SECURITY_NAME)) {     \
            Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage( \
                L"Skipping: requires SE_SECURITY_NAME");                         \
            return;                                                              \
        }                                                                        \
    } while (0)

#define ABI_SKIP_IF_SECURITY_PRIVILEGE_HELD()                                    \
    do {                                                                         \
        if (::LayerMountTestShared::TryEnablePrivilege(SE_SECURITY_NAME)) {      \
            Microsoft::VisualStudio::CppUnitTestFramework::Logger::WriteMessage( \
                L"Skipping: requires a token without SE_SECURITY_NAME");         \
            return;                                                              \
        }                                                                        \
    } while (0)

// NT-status-derived HRESULTs (ERROR_FILE_NOT_FOUND vs STATUS_OBJECT_NAME_NOT_FOUND)
// produce different HRESULT encodings depending on which Win32 boundary the
// engine used. These helpers cover both forms.
inline bool IsFileNotFoundHr(HRESULT hr) noexcept {
    return hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
        || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)
        || hr == HRESULT_FROM_NT(STATUS_OBJECT_NAME_NOT_FOUND)
        || hr == HRESULT_FROM_NT(STATUS_OBJECT_PATH_NOT_FOUND);
}

// Open `relativePath` through the C ABI with `grantedAccess` and no create
// options. The test fails if the open does not return S_OK.
inline LM_FILE_HANDLE OpenWithAccess(LM_HANDLE mount,
                                     const wchar_t* relativePath,
                                     UINT32 grantedAccess) {
    OpenedFile opened;
    Microsoft::VisualStudio::CppUnitTestFramework::Assert::AreEqual<HRESULT>(S_OK,
        OpenOverlayFile(mount, relativePath, grantedAccess, kNoCreateOptions, opened));
    return opened.handle;
}

inline HRESULT SetFileSize(LM_FILE_HANDLE fh, UINT64 size, LM_FILE_INFO* outInfo) {
    FileInfoChange change;
    change.fileSize = size;
    return SetFileInfo(fh, change, outInfo);
}

// Sets the last access time and the last write time; a zero time keeps
// the stored value, as the ABI defines.
inline HRESULT SetFileTimes(LM_FILE_HANDLE fh, UINT64 lastAccessTime, UINT64 lastWriteTime,
                            LM_FILE_INFO* outInfo) {
    FileInfoChange change;
    change.lastAccessTime = lastAccessTime;
    change.lastWriteTime  = lastWriteTime;
    return SetFileInfo(fh, change, outInfo);
}

// The test fails with `message` unless a read open of `relativePath`
// reports file not found.
inline void AssertOpenFailsNotFound(LM_HANDLE mount,
                                    const wchar_t* relativePath,
                                    const wchar_t* message) {
    OpenedFile opened;
    const HRESULT hr = OpenOverlayFile(
        mount, relativePath, GENERIC_READ, kNoCreateOptions, opened);
    Microsoft::VisualStudio::CppUnitTestFramework::Assert::IsTrue(
        IsFileNotFoundHr(hr), message);
}

// -----------------------------------------------------------------------------
// Run |fn| on a thread with no COM apartment and return its HRESULT. The test
// host's own thread already holds an apartment that the VSS shims'
// COINIT_MULTITHREADED scope rejects with RPC_E_CHANGED_MODE, so a shim called
// from it never reaches the code under test. Assertions stay on the calling
// thread: |fn| only returns the HRESULT.
// -----------------------------------------------------------------------------
template <typename Fn>
HRESULT OnFreshThread(Fn&& fn) {
    HRESULT hr = E_FAIL;
    std::thread worker([&] { hr = fn(); });
    worker.join();
    return hr;
}

} // namespace LayerMountAbiTests
