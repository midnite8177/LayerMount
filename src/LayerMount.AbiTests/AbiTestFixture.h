#pragma once

// Shared scaffolding for AbiTests. Everything here sits on top of the public
// C ABI only -- no impl/ headers, no static lib. Tests build a config with
// TempLayerEnv + MakeConfig, drive the ABI, and let RAII clean up.

#include "pch.h"

namespace LayerMountAbiTests {

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
        auto path = std::filesystem::path(Lower(index)) / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

private:
    std::wstring              root_;
    std::wstring              upper_;
    std::wstring              work_;
    std::vector<std::wstring> lowers_;
};

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
        // HRESULT_FROM_NT(STATUS_OBJECT_NAME_NOT_FOUND) == 0xD0000034
        || hr == static_cast<HRESULT>(0xD0000034L)
        // HRESULT_FROM_NT(STATUS_OBJECT_PATH_NOT_FOUND) == 0xD000003A
        || hr == static_cast<HRESULT>(0xD000003AL);
}

} // namespace LayerMountAbiTests
