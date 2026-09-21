#pragma once

// Shared scaffolding for AbiTests. Everything here sits on top of the public
// C ABI only -- no impl/ headers, no static lib. Tests build a config with
// TempLayerEnv + MakeConfig, drive the ABI, and let RAII clean up.

#include "pch.h"

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
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
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

// Read the whole file at `path` as bytes.
inline std::string ReadAllBytes(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

// Read the first 64 bytes of `fh` through the ABI and return them; the
// read's HRESULT lands in *outHr.
inline std::string ReadThroughHandle(LM_FILE_HANDLE fh, HRESULT* outHr) {
    char   buffer[64] = {};
    UINT32 transferred = 0;
    *outHr = ::LayerMountReadFile(fh, buffer, 0, sizeof(buffer), &transferred);
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

private:
    LM_CONFIG               cfg_{};
    std::wstring             upper_;
    std::wstring             work_;
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

// Admin-check: mount / VHD attach / VSS require elevation. Tests that need
// elevation call this as the first statement and Assert::Inconclusive via
// Logger + return if the process isn't elevated.
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
    LM_FILE_HANDLE fh = nullptr;
    LM_FILE_INFO   info{};
    Microsoft::VisualStudio::CppUnitTestFramework::Assert::AreEqual<HRESULT>(S_OK,
        ::LayerMountOpenFile(mount, relativePath, grantedAccess, 0u, 0u, &fh, &info));
    return fh;
}

// Sets only the end of file; attributes, times, and allocation size pass
// the ABI's leave-unchanged sentinels.
inline HRESULT SetFileSize(LM_FILE_HANDLE fh, UINT64 size, LM_FILE_INFO* outInfo) {
    return ::LayerMountSetFileInfo(
        fh,
        INVALID_FILE_ATTRIBUTES,
        /*creationTime*/   0u,
        /*lastAccessTime*/ 0u,
        /*lastWriteTime*/  0u,
        /*changeTime*/     0u,
        /*allocationSize*/ UINT64_MAX,
        /*fileSize*/       size,
        outInfo);
}

// Sets only the last write time; attributes, the other times, and both
// sizes pass the ABI's leave-unchanged sentinels.
inline HRESULT SetLastWriteTime(LM_FILE_HANDLE fh, UINT64 lastWriteTime, LM_FILE_INFO* outInfo) {
    return ::LayerMountSetFileInfo(
        fh,
        INVALID_FILE_ATTRIBUTES,
        /*creationTime*/   0u,
        /*lastAccessTime*/ 0u,
        lastWriteTime,
        /*changeTime*/     0u,
        /*allocationSize*/ UINT64_MAX,
        /*fileSize*/       UINT64_MAX,
        outInfo);
}

// The test fails with `message` unless a read open of `relativePath`
// reports file not found.
inline void AssertOpenFailsNotFound(LM_HANDLE mount,
                                    const wchar_t* relativePath,
                                    const wchar_t* message) {
    LM_FILE_HANDLE fh = nullptr;
    LM_FILE_INFO   info{};
    const HRESULT hr = ::LayerMountOpenFile(
        mount, relativePath, GENERIC_READ, 0u, 0u, &fh, &info);
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
