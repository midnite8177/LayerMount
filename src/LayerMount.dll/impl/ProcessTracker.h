#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <chrono>
#include <cstdint>

namespace LayerMount {

namespace abi { class EventEmitter; }

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

enum class OperationType : uint8_t {
    Create,
    Open,
    Read,
    Write,
    Overwrite,
    Delete,
    Rename,
    GetInfo,
    SetInfo,
    SetSize,
    GetSecurity,
    SetSecurity,
    ReadDirectory,
    Flush,
    Cleanup,
    Close
};

enum class ExportFormat {
    JSON,
    CSV
};

// ---------------------------------------------------------------------------
// Data structs
// ---------------------------------------------------------------------------

struct ProcessInfo {
    DWORD pid = 0;
    std::wstring name;          // e.g., L"notepad.exe"
    std::wstring exePath;       // e.g., L"C:\\Windows\\notepad.exe"
    std::wstring commandLine;   // Full command line (best-effort)
    std::wstring user;          // e.g., L"DOMAIN\\Username" or SID string
    bool elevated = false;
};

struct AccessLogEntry {
    DWORD pid = 0;
    std::wstring processName;
    std::wstring filePath;      // Relative path within overlay
    OperationType operation = OperationType::Open;
    std::chrono::system_clock::time_point timestamp;
    bool allowed = true;
    std::wstring reason;        // Why denied, or matched rule description
};

struct AccessRule {
    std::wstring processNamePattern;    // Wildcard: "*", "notepad.exe", "*shell.exe"
    std::wstring pathPattern;           // Wildcard: "*", "*.secret", "data\\*"
    bool allowRead = true;
    bool allowWrite = true;
    bool allowExecute = true;
    bool allowDelete = true;
};

// ---------------------------------------------------------------------------
// ProcessTracker class
// ---------------------------------------------------------------------------

class ProcessTracker {
public:
    explicit ProcessTracker(size_t logCapacity = 10000);
    ~ProcessTracker();

    // Event emitter. When set, CheckAccess emits LM_EVT_ACCESS_DENIED
    // on every denied decision so hosts can surface audit events in
    // real time. Safe to call concurrently; atomicity of the single
    // pointer write is guaranteed on x64.
    void SetEventEmitter(::LayerMount::abi::EventEmitter* events) noexcept {
        events_ = events;
    }

    ProcessTracker(const ProcessTracker&) = delete;
    ProcessTracker& operator=(const ProcessTracker&) = delete;

    // --- Process resolution (6.2) ---
    ProcessInfo ResolveProcess(DWORD pid) const;

    // --- Access control (6.4) ---
    bool LoadRules(const std::wstring& configPath);
    bool CheckAccess(DWORD pid, const std::wstring& relativePath, OperationType op);

    // --- Logging (6.3) ---
    void LogAccess(DWORD pid, const std::wstring& relativePath, OperationType op);
    std::vector<AccessLogEntry> GetRecentEntries(size_t count) const;

    // --- Export (6.5) ---
    bool ExportLog(const std::wstring& filePath, ExportFormat format) const;

    // Render the log as an in-memory UTF-8 string for callers (ABI /
    // P-Invoke) that want to consume it without round-tripping through
    // disk. Pretty-printed JSON (2-space indent) or RFC-4180 CSV with a
    // header row. Same content that ExportLog writes.
    std::string ExportLogAsJson() const;
    std::string ExportLogAsCsv()  const;

private:
    // --- Wildcard matching ---
    static bool WildcardMatch(const std::wstring& pattern, const std::wstring& text);

    // --- Operation classification ---
    static bool IsReadOp(OperationType op);
    static bool IsWriteOp(OperationType op);
    static bool IsDeleteOp(OperationType op);
    static bool IsExecuteOp(OperationType op);
    static const wchar_t* OperationName(OperationType op);

    // --- Internal helpers ---
    void AddLogEntry(AccessLogEntry entry);
    ProcessInfo ResolveProcessUncached(DWORD pid) const;

    // --- Process info cache ---
    struct CachedProcessInfo {
        ProcessInfo info;
        std::chrono::steady_clock::time_point expiresAt;
    };
    mutable std::shared_mutex processInfoMutex_;
    mutable std::unordered_map<DWORD, CachedProcessInfo> processInfoCache_;
    static constexpr std::chrono::seconds kProcessInfoTTL{30};

    // --- Access log (circular buffer) ---
    mutable std::mutex logMutex_;
    std::vector<AccessLogEntry> logBuffer_;
    size_t logHead_ = 0;
    size_t logCount_ = 0;
    size_t logCapacity_;

    // --- Rules ---
    mutable std::shared_mutex rulesMutex_;
    std::vector<AccessRule> rules_;

    // --- Event emitter for LM_EVT_ACCESS_DENIED ---
    ::LayerMount::abi::EventEmitter* events_ = nullptr;
};

} // namespace LayerMount
