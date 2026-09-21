#pragma once

#include "pch.h"

#include "LayerMount.h"
#include "Cache.h"
#include "CopyUp.h"
#include "PathResolver.h"
#include "WhiteoutManager.h"

#include <exception>
#include <functional>
#include <rpc.h> // UuidCreate / UuidToStringW / RpcStringFreeW (Rpcrt4.lib — linked via vcxproj)
#include <winioctl.h>

namespace LayerMountTests {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Unique temp directory helper
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Admin / elevation guard — for unit tests that exercise elevated-only
// subsystems (VHD / VSS) pulled in from the former integration-test project.
// ---------------------------------------------------------------------------

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

// Early-return from a TEST_METHOD if the process is not elevated. Tests that
// exercise VHDLayerManager / VSSManager need admin because the underlying
// Win32 APIs (virtdisk, VSS) require SE_BACKUP/SE_RESTORE or full TCB.
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

// ---------------------------------------------------------------------------
// NTFS guard — call from TEST_CLASS_INITIALIZE in classes that exercise ADS
// ---------------------------------------------------------------------------

inline void AssertTempIsNTFS() {
    wchar_t tempBuf[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, tempBuf);

    // GetVolumeInformationW requires the root path — extract drive root (e.g., "C:\").
    std::wstring temp(tempBuf);
    std::wstring rootPath;
    if (temp.size() >= 3 && temp[1] == L':' && temp[2] == L'\\') {
        rootPath = temp.substr(0, 3);
    } else {
        rootPath = temp; // best-effort
    }

    wchar_t fsName[16] = {};
    ::GetVolumeInformationW(rootPath.c_str(), nullptr, 0, nullptr, nullptr,
                            nullptr, fsName, 16);

    Microsoft::VisualStudio::CppUnitTestFramework::Assert::AreEqual(
        std::wstring(L"NTFS"), std::wstring(fsName),
        L"Tests require %TEMP% to be on NTFS for Alternate Data Stream support");
}

// ---------------------------------------------------------------------------
// TempLayerEnvironment — RAII fixture: creates upper, work, lowerN dirs under
// a unique subdirectory of %TEMP%, cleans up on destruction.
// ---------------------------------------------------------------------------

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

    // --- accessors ---
    const std::wstring& Root()  const { return root_; }
    const std::wstring& Upper() const { return upper_; }
    const std::wstring& Work()  const { return work_; }
    const std::wstring& Lower(size_t i = 0) const { return lowers_.at(i); }
    size_t LowerCount() const { return lowers_.size(); }

    // NOTE: returns LayerConfig by value. Classes like WhiteoutManager/PathResolver
    // hold a `const LayerConfig&` member, so callers MUST store the result in a
    // local variable before constructing those classes — binding to a temporary
    // dangles and causes heisenbugs. Pattern:
    //     auto config = env.MakeConfig();
    //     WhiteoutManager wm(config, &cache);
    LayerMount::LayerConfig MakeConfig() const {
        LayerMount::LayerConfig c;
        c.upperPath   = upper_;
        c.workDirPath = work_;
        c.lowerPaths  = lowers_;
        // Unit tests here assume ADS-first metadata storage and the other
        // legacy-on-NTFS optimizations. `hostCapabilities` (default 0)
        // flips MetadataADS to sidecar-JSON when LM_CAP_ADS is clear.
        // Set the full legacy bitmask so tests see on-NTFS behavior;
        // capability-degradation paths are covered separately by
        // LayerMount.AbiTests::AbiCapabilityDegradationTests.
        c.hostCapabilities = LM_CAP_ADS | LM_CAP_REPARSE_POINTS |
                             LM_CAP_SPARSE_FILES | LM_CAP_MULTIPLE_STREAMS |
                             LM_CAP_NTFS_ACLS;
        return c;
    }

    // --- file helpers ---

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
        // Broad sharing mode: integration tests often inspect an upper layer
        // file while the overlay's internal ctx handle is still open on it
        // (host-adapter Close callbacks typically run asynchronously after
        // user-space completes a sync MoveFileExW / CreateFile call). The
        // overlay's internal handle
        // holds GENERIC_READ | GENERIC_WRITE with broad sharing; a reader
        // that asks for only FILE_SHARE_READ can race into a sharing
        // violation when the write-granting internal handle hasn't closed
        // yet. Including SHARE_WRITE | SHARE_DELETE here is compatible with
        // how every file-reading test uses this helper — it only widens
        // sharing against concurrent holders, not our own access.
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

// Sets the three file times through an attribute-only handle.
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

// True when the file or directory at path carries the attribute flag.
inline bool HasAttribute(const std::wstring& path, DWORD flag) {
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & flag) != 0;
}

// Set NTFS compression on the file or directory at path. Returns false when
// the open or the FSCTL fails.
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

// Read length bytes at offset from the file at path. The result is shorter
// when the file ends inside the range.
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

// ---------------------------------------------------------------------------
// A CopyUp with the objects it depends on, built in dependency order from
// one config. The members hold `const LayerConfig&`, so the rig owns the
// config they refer to.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Fresh-thread runner for subsystems that need a COM apartment of their own
// (VSS). The test host's thread already holds an apartment that
// CoInitializeEx(COINIT_MULTITHREADED) rejects with RPC_E_CHANGED_MODE, so a
// body that needs MTA runs on a new thread. An assertion failure inside the
// body is a structured exception (ERROR_ASSERT_FAILED) raised by the
// framework DLL. The worker catches it, copies the message while the raising
// frame is still alive, and the calling thread fails the test with it.
// ---------------------------------------------------------------------------

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

} // namespace LayerMountTests
