#include "ProcessTracker.h"
#include "../abi/EventEmitter.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sddl.h>
#include <winternl.h>

#include <nlohmann/json.hpp>

#pragma comment(lib, "advapi32.lib")

namespace LayerMount {

// ---------------------------------------------------------------------------
// Local RAII handle wrapper (avoids coupling to CopyUp.h's ScopedHandle)
// ---------------------------------------------------------------------------

class ProcHandle {
public:
    explicit ProcHandle(HANDLE h = nullptr) noexcept : h_(h) {}
    ~ProcHandle() { if (h_) CloseHandle(h_); }
    ProcHandle(const ProcHandle&) = delete;
    ProcHandle& operator=(const ProcHandle&) = delete;
    ProcHandle(ProcHandle&& other) noexcept : h_(other.h_) { other.h_ = nullptr; }
    ProcHandle& operator=(ProcHandle&& other) noexcept {
        if (this != &other) { if (h_) CloseHandle(h_); h_ = other.h_; other.h_ = nullptr; }
        return *this;
    }
    HANDLE Get() const noexcept { return h_; }
    explicit operator bool() const noexcept { return h_ != nullptr; }
private:
    HANDLE h_;
};

// ---------------------------------------------------------------------------
// UTF-8 / Wide string conversion (local helpers, same pattern as MetadataADS)
// ---------------------------------------------------------------------------

static std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
        static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr);
    return result;
}

static std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
        static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
        static_cast<int>(utf8.size()), result.data(), size);
    return result;
}

// ---------------------------------------------------------------------------
// ISO 8601 timestamp formatting
// ---------------------------------------------------------------------------

static std::string FormatTimestamp(std::chrono::system_clock::time_point tp) {
    auto tt = std::chrono::system_clock::to_time_t(tp);
    struct tm tmBuf = {};
    gmtime_s(&tmBuf, &tt);

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        tp.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(&tmBuf, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}

// ---------------------------------------------------------------------------
// CSV field escaping (RFC 4180)
// ---------------------------------------------------------------------------

static std::string CsvEscape(const std::string& field) {
    // Neutralize spreadsheet formula injection: any field whose first
    // character is =, +, -, @, or a control character that some tools
    // treat as a SYLK/cell-formula lead gets prefixed with an
    // apostrophe so the spreadsheet treats it as literal text. The
    // process tracker's CSV output includes attacker-influenced paths
    // and process names, so a crafted filename like =HYPERLINK(...)
    // could execute on import.
    bool needsFormulaEscape = false;
    if (!field.empty()) {
        char f = field.front();
        if (f == '=' || f == '+' || f == '-' || f == '@' ||
            f == '\t' || f == '\r')
        {
            needsFormulaEscape = true;
        }
    }

    bool needsQuoting = needsFormulaEscape;
    for (char c : field) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needsQuoting = true;
            break;
        }
    }
    if (!needsQuoting) return field;

    std::string result = "\"";
    if (needsFormulaEscape) {
        result += '\'';
    }
    for (char c : field) {
        if (c == '"') result += "\"\"";
        else result += c;
    }
    result += '"';
    return result;
}

// ---------------------------------------------------------------------------
// NtQueryInformationProcess — dynamic load from ntdll.dll
// ---------------------------------------------------------------------------

// We need this for command line extraction. winternl.h provides an incomplete
// PROCESS_BASIC_INFORMATION — the PEB pointer is what we need.
typedef NTSTATUS(NTAPI* NtQueryInformationProcessFn)(
    HANDLE ProcessHandle,
    PROCESSINFOCLASS ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength,
    PULONG ReturnLength);

static NtQueryInformationProcessFn GetNtQueryInformationProcess() {
    // C++11 magic statics: initialization of a function-local static is
    // guaranteed thread-safe and runs exactly once. The previous version
    // used two separate statics (`fn` + `loaded`) plus an unsynchronized
    // body, so two threads racing into the body could both observe
    // `loaded == false`, both call GetModuleHandleW + GetProcAddress, and
    // race on the assignment to `fn` -- benign in practice (idempotent),
    // but undefined-behavior-prone and visible to TSAN. Folding the load
    // into a single immediately-invoked-lambda initializer makes the
    // race-free init explicit.
    static NtQueryInformationProcessFn fn = []() -> NtQueryInformationProcessFn {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return nullptr;
#pragma warning(push)
#pragma warning(disable: 4191) // unsafe function pointer cast
        return reinterpret_cast<NtQueryInformationProcessFn>(
            GetProcAddress(ntdll, "NtQueryInformationProcess"));
#pragma warning(pop)
    }();
    return fn;
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

ProcessTracker::ProcessTracker(size_t logCapacity)
    : logCapacity_(logCapacity) {
    logBuffer_.resize(logCapacity_);
}

ProcessTracker::~ProcessTracker() = default;

// ---------------------------------------------------------------------------
// Operation classification helpers
// ---------------------------------------------------------------------------

bool ProcessTracker::IsReadOp(OperationType op) {
    switch (op) {
    case OperationType::Open:
    case OperationType::Read:
    case OperationType::GetInfo:
    case OperationType::GetSecurity:
    case OperationType::ReadDirectory:
        return true;
    default:
        return false;
    }
}

bool ProcessTracker::IsWriteOp(OperationType op) {
    switch (op) {
    case OperationType::Create:
    case OperationType::Write:
    case OperationType::Overwrite:
    case OperationType::SetInfo:
    case OperationType::SetSize:
    case OperationType::SetSecurity:
    case OperationType::Flush:
    case OperationType::Rename:
        return true;
    default:
        return false;
    }
}

bool ProcessTracker::IsDeleteOp(OperationType op) {
    return op == OperationType::Delete;
}

bool ProcessTracker::IsExecuteOp(OperationType op) {
    (void)op;
    return false; // Reserved for future use
}

const wchar_t* ProcessTracker::OperationName(OperationType op) {
    switch (op) {
    case OperationType::Create:        return L"Create";
    case OperationType::Open:          return L"Open";
    case OperationType::Read:          return L"Read";
    case OperationType::Write:         return L"Write";
    case OperationType::Overwrite:     return L"Overwrite";
    case OperationType::Delete:        return L"Delete";
    case OperationType::Rename:        return L"Rename";
    case OperationType::GetInfo:       return L"GetInfo";
    case OperationType::SetInfo:       return L"SetInfo";
    case OperationType::SetSize:       return L"SetSize";
    case OperationType::GetSecurity:   return L"GetSecurity";
    case OperationType::SetSecurity:   return L"SetSecurity";
    case OperationType::ReadDirectory: return L"ReadDirectory";
    case OperationType::Flush:         return L"Flush";
    case OperationType::Cleanup:       return L"Cleanup";
    case OperationType::Close:         return L"Close";
    default:                           return L"Unknown";
    }
}

// ---------------------------------------------------------------------------
// Wildcard matching (case-insensitive)
// ---------------------------------------------------------------------------

bool ProcessTracker::WildcardMatch(const std::wstring& pattern, const std::wstring& text) {
    if (pattern == L"*") return true;
    if (pattern.empty()) return text.empty();

    // Use CharLowerBuffW (Win32 path-appropriate ordinal casefolding) instead
    // of the locale-sensitive C towlower. towlower depends on the C locale
    // and produces wrong results for Turkish dotted/dotless I, ß↔SS, and
    // other compatibility forms -- causing process rules to be bypassed or
    // over-applied for non-ASCII paths depending on the host's locale.
    std::wstring lowerPattern = pattern;
    std::wstring lowerText = text;
    if (!lowerPattern.empty()) {
        ::CharLowerBuffW(lowerPattern.data(),
                         static_cast<DWORD>(lowerPattern.size()));
    }
    if (!lowerText.empty()) {
        ::CharLowerBuffW(lowerText.data(),
                         static_cast<DWORD>(lowerText.size()));
    }

    // No wildcard: exact match
    if (lowerPattern.find(L'*') == std::wstring::npos) {
        return lowerPattern == lowerText;
    }

    size_t starPos = lowerPattern.find(L'*');
    bool starAtStart = (starPos == 0);
    bool starAtEnd = (starPos == lowerPattern.size() - 1);

    if (starAtStart && starAtEnd && lowerPattern.size() == 1) {
        return true; // Already handled above, but be safe
    }

    if (starAtStart && starAtEnd) {
        // *middle* — contains
        std::wstring middle = lowerPattern.substr(1, lowerPattern.size() - 2);
        return lowerText.find(middle) != std::wstring::npos;
    }

    if (starAtStart) {
        // *suffix — ends with
        std::wstring suffix = lowerPattern.substr(1);
        if (lowerText.size() < suffix.size()) return false;
        return lowerText.compare(lowerText.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    if (starAtEnd) {
        // prefix* — starts with
        std::wstring prefix = lowerPattern.substr(0, starPos);
        if (lowerText.size() < prefix.size()) return false;
        return lowerText.compare(0, prefix.size(), prefix) == 0;
    }

    // Single * in middle not supported — treat as no match
    return false;
}

// ---------------------------------------------------------------------------
// Circular buffer log helpers
// ---------------------------------------------------------------------------

void ProcessTracker::AddLogEntry(AccessLogEntry entry) {
    std::lock_guard<std::mutex> lock(logMutex_);
    logBuffer_[logHead_] = std::move(entry);
    logHead_ = (logHead_ + 1) % logCapacity_;
    if (logCount_ < logCapacity_) {
        ++logCount_;
    }
}

// ---------------------------------------------------------------------------
// 6.2 — Process resolution
// ---------------------------------------------------------------------------

ProcessInfo ProcessTracker::ResolveProcessUncached(DWORD pid) const {
    ProcessInfo info;
    info.pid = pid;

    // --- Name and executable path ---
    ProcHandle proc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!proc) {
        info.name = L"<unknown>";
        return info;
    }

    WCHAR exePathBuf[MAX_PATH] = {};
    DWORD exePathLen = MAX_PATH;
    if (QueryFullProcessImageNameW(proc.Get(), 0, exePathBuf, &exePathLen)) {
        info.exePath = exePathBuf;
        // Extract basename
        size_t lastSlash = info.exePath.find_last_of(L'\\');
        if (lastSlash != std::wstring::npos) {
            info.name = info.exePath.substr(lastSlash + 1);
        } else {
            info.name = info.exePath;
        }
    } else {
        info.name = L"<unknown>";
    }

    // --- Command line (best-effort via NtQueryInformationProcess) ---
    auto ntQuery = GetNtQueryInformationProcess();
    if (ntQuery) {
        // Need elevated access for reading process memory
        ProcHandle procVm(OpenProcess(
            PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid));
        if (procVm) {
            PROCESS_BASIC_INFORMATION pbi = {};
            ULONG retLen = 0;
            NTSTATUS status = ntQuery(procVm.Get(), ProcessBasicInformation,
                &pbi, sizeof(pbi), &retLen);

            if (NT_SUCCESS(status) && pbi.PebBaseAddress) {
                // Read PEB to get ProcessParameters pointer
                // PEB.ProcessParameters is at offset 0x20 (x64)
                PVOID paramsPtr = nullptr;
                SIZE_T bytesRead = 0;
                BYTE* pebAddr = reinterpret_cast<BYTE*>(pbi.PebBaseAddress);
                if (ReadProcessMemory(procVm.Get(), pebAddr + 0x20,
                        &paramsPtr, sizeof(paramsPtr), &bytesRead) && paramsPtr) {
                    // RTL_USER_PROCESS_PARAMETERS.CommandLine is a UNICODE_STRING
                    // at offset 0x70 (x64)
                    UNICODE_STRING cmdLineUs = {};
                    BYTE* paramsAddr = reinterpret_cast<BYTE*>(paramsPtr);
                    if (ReadProcessMemory(procVm.Get(), paramsAddr + 0x70,
                            &cmdLineUs, sizeof(cmdLineUs), &bytesRead) &&
                        cmdLineUs.Buffer && cmdLineUs.Length > 0) {
                        USHORT charCount = cmdLineUs.Length / sizeof(WCHAR);
                        std::wstring cmdLine(charCount, L'\0');
                        if (ReadProcessMemory(procVm.Get(), cmdLineUs.Buffer,
                                cmdLine.data(),
                                static_cast<SIZE_T>(cmdLineUs.Length), &bytesRead)) {
                            info.commandLine = std::move(cmdLine);
                        }
                    }
                }
            }
        }
    }

    // --- User and elevation (share the same token) ---
    ProcHandle procToken;
    {
        HANDLE rawToken = nullptr;
        ProcHandle procForToken(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid));
        if (!procForToken) {
            // Fall back to limited access
            procForToken = ProcHandle(OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        }
        if (procForToken && OpenProcessToken(procForToken.Get(), TOKEN_QUERY, &rawToken)) {
            procToken = ProcHandle(rawToken);
        }
    }

    if (procToken) {
        // --- User ---
        DWORD tokenUserSize = 0;
        GetTokenInformation(procToken.Get(), TokenUser, nullptr, 0, &tokenUserSize);
        if (tokenUserSize > 0) {
            std::vector<BYTE> tokenUserBuf(tokenUserSize);
            if (GetTokenInformation(procToken.Get(), TokenUser,
                    tokenUserBuf.data(), tokenUserSize, &tokenUserSize)) {
                auto* tokenUser = reinterpret_cast<TOKEN_USER*>(tokenUserBuf.data());
                WCHAR nameBuf[256] = {};
                WCHAR domainBuf[256] = {};
                DWORD nameLen = 256, domainLen = 256;
                SID_NAME_USE sidUse;
                if (LookupAccountSidW(nullptr, tokenUser->User.Sid,
                        nameBuf, &nameLen, domainBuf, &domainLen, &sidUse)) {
                    info.user = std::wstring(domainBuf) + L"\\" + nameBuf;
                } else {
                    // Fallback: SID string
                    LPWSTR sidStr = nullptr;
                    if (ConvertSidToStringSidW(tokenUser->User.Sid, &sidStr)) {
                        info.user = sidStr;
                        LocalFree(sidStr);
                    }
                }
            }
        }

        // --- Elevation ---
        TOKEN_ELEVATION elevation = {};
        DWORD elevSize = sizeof(elevation);
        if (GetTokenInformation(procToken.Get(), TokenElevation,
                &elevation, sizeof(elevation), &elevSize)) {
            info.elevated = (elevation.TokenIsElevated != 0);
        }
    }

    return info;
}

ProcessInfo ProcessTracker::ResolveProcess(DWORD pid) const {
    auto now = std::chrono::steady_clock::now();

    // Check cache (shared lock)
    {
        std::shared_lock<std::shared_mutex> lock(processInfoMutex_);
        auto it = processInfoCache_.find(pid);
        if (it != processInfoCache_.end() && it->second.expiresAt > now) {
            return it->second.info;
        }
    }

    // Cache miss or expired — resolve and cache (exclusive lock)
    ProcessInfo info = ResolveProcessUncached(pid);

    {
        std::unique_lock<std::shared_mutex> lock(processInfoMutex_);
        CachedProcessInfo cached;
        cached.info = info;
        cached.expiresAt = now + kProcessInfoTTL;
        processInfoCache_[pid] = std::move(cached);
    }

    return info;
}

// ---------------------------------------------------------------------------
// 6.3 — Access logging
// ---------------------------------------------------------------------------

void ProcessTracker::LogAccess(DWORD pid, const std::wstring& relativePath, OperationType op) {
    ProcessInfo pInfo = ResolveProcess(pid);

    AccessLogEntry entry;
    entry.pid = pid;
    entry.processName = pInfo.name;
    entry.filePath = relativePath;
    entry.operation = op;
    entry.timestamp = std::chrono::system_clock::now();
    entry.allowed = true;
    AddLogEntry(std::move(entry));
}

std::vector<AccessLogEntry> ProcessTracker::GetRecentEntries(size_t count) const {
    std::lock_guard<std::mutex> lock(logMutex_);

    size_t actual = (std::min)(count, logCount_);
    std::vector<AccessLogEntry> result;
    result.reserve(actual);

    if (actual == 0) return result;

    // Start index: the oldest entry we want (most recent N entries)
    size_t start = (logHead_ + logCapacity_ - actual) % logCapacity_;
    for (size_t i = 0; i < actual; ++i) {
        size_t idx = (start + i) % logCapacity_;
        result.push_back(logBuffer_[idx]);
    }

    return result;
}

// ---------------------------------------------------------------------------
// 6.4 — Access rules
// ---------------------------------------------------------------------------

bool ProcessTracker::LoadRules(const std::wstring& configPath) {
    // Open via std::filesystem::path (wide) so non-ASCII directory
    // names round-trip correctly. Constructing ifstream from a
    // narrow UTF-8 path used the active code page on MSVC, which
    // silently failed for paths like C:\Users\Jos\u00e9\rules.json
    // on non-UTF-8 system locales. Named-variable form avoids the
    // most-vexing-parse trap with a braced path temporary.
    std::filesystem::path fsPath{configPath};
    std::ifstream file(fsPath);
    if (!file.is_open()) {
        return false;
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const nlohmann::json::exception&) {
        return false;
    }

    if (!j.contains("rules") || !j["rules"].is_array()) {
        return false;
    }

    std::vector<AccessRule> newRules;
    for (const auto& ruleJson : j["rules"]) {
        AccessRule rule;
        rule.processNamePattern = Utf8ToWide(
            ruleJson.value("processName", std::string("*")));
        rule.pathPattern = Utf8ToWide(
            ruleJson.value("pathPattern", std::string("*")));
        rule.allowRead = ruleJson.value("allowRead", true);
        rule.allowWrite = ruleJson.value("allowWrite", true);
        rule.allowExecute = ruleJson.value("allowExecute", true);
        rule.allowDelete = ruleJson.value("allowDelete", true);
        newRules.push_back(std::move(rule));
    }

    // Swap under exclusive lock
    {
        std::unique_lock<std::shared_mutex> lock(rulesMutex_);
        rules_ = std::move(newRules);
    }

    return true;
}

bool ProcessTracker::CheckAccess(DWORD pid, const std::wstring& relativePath, OperationType op) {
    // Resolve process info (acquires/releases processInfoMutex_)
    ProcessInfo pInfo = ResolveProcess(pid);

    bool allowed = true;
    std::wstring reason;

    // Evaluate rules (acquires/releases rulesMutex_)
    {
        std::shared_lock<std::shared_mutex> lock(rulesMutex_);
        for (const auto& rule : rules_) {
            if (!WildcardMatch(rule.processNamePattern, pInfo.name)) continue;
            if (!WildcardMatch(rule.pathPattern, relativePath)) continue;

            // First matching rule wins
            if (IsReadOp(op) && !rule.allowRead) {
                allowed = false;
                reason = L"Denied by rule: " + rule.processNamePattern +
                         L" / " + rule.pathPattern;
            } else if (IsWriteOp(op) && !rule.allowWrite) {
                allowed = false;
                reason = L"Denied by rule: " + rule.processNamePattern +
                         L" / " + rule.pathPattern;
            } else if (IsDeleteOp(op) && !rule.allowDelete) {
                allowed = false;
                reason = L"Denied by rule: " + rule.processNamePattern +
                         L" / " + rule.pathPattern;
            } else if (IsExecuteOp(op) && !rule.allowExecute) {
                allowed = false;
                reason = L"Denied by rule: " + rule.processNamePattern +
                         L" / " + rule.pathPattern;
            }
            break; // First match wins regardless
        }
    }

    // Log the access (acquires/releases logMutex_)
    AccessLogEntry entry;
    entry.pid = pid;
    entry.processName = pInfo.name;
    entry.filePath = relativePath;
    entry.operation = op;
    entry.timestamp = std::chrono::system_clock::now();
    entry.allowed = allowed;
    entry.reason = std::move(reason);
    const std::wstring denyReason = entry.reason;  // copy for event emit
    AddLogEntry(std::move(entry));

    // Surface denied accesses through the host event callback as soon
    // as the logging side-effect lands. hr is E_ACCESSDENIED so
    // consumers that bucket by hr (e.g. counting access-denied events)
    // get a stable classification. pid carries the accessing process so
    // hosts can correlate with their own audit tooling.
    if (!allowed && events_ != nullptr) {
        events_->Emit(LM_EVT_ACCESS_DENIED, E_ACCESSDENIED,
                      relativePath.c_str(),
                      denyReason.empty() ? nullptr : denyReason.c_str(),
                      pid);
    }

    return allowed;
}

// ---------------------------------------------------------------------------
// 6.5 — Log export
// ---------------------------------------------------------------------------

std::string ProcessTracker::ExportLogAsJson() const {
    std::vector<AccessLogEntry> entries = GetRecentEntries(logCapacity_);

    nlohmann::json j;
    j["exportedAt"] = FormatTimestamp(std::chrono::system_clock::now());
    j["entryCount"] = entries.size();

    nlohmann::json jEntries = nlohmann::json::array();
    for (const auto& e : entries) {
        nlohmann::json je;
        je["pid"] = e.pid;
        je["processName"] = WideToUtf8(e.processName);
        je["filePath"] = WideToUtf8(e.filePath);
        je["operation"] = WideToUtf8(OperationName(e.operation));
        je["timestamp"] = FormatTimestamp(e.timestamp);
        je["allowed"] = e.allowed;
        je["reason"] = WideToUtf8(e.reason);
        jEntries.push_back(std::move(je));
    }
    j["entries"] = std::move(jEntries);

    return j.dump(2);
}

std::string ProcessTracker::ExportLogAsCsv() const {
    std::vector<AccessLogEntry> entries = GetRecentEntries(logCapacity_);

    std::ostringstream out;
    // RFC 4180 header + rows.
    out << "Timestamp,PID,ProcessName,FilePath,Operation,Allowed,Reason\n";
    for (const auto& e : entries) {
        out << CsvEscape(FormatTimestamp(e.timestamp)) << ','
            << e.pid << ','
            << CsvEscape(WideToUtf8(e.processName)) << ','
            << CsvEscape(WideToUtf8(e.filePath)) << ','
            << CsvEscape(WideToUtf8(OperationName(e.operation))) << ','
            << (e.allowed ? "true" : "false") << ','
            << CsvEscape(WideToUtf8(e.reason)) << '\n';
    }
    return out.str();
}

bool ProcessTracker::ExportLog(const std::wstring& filePath, ExportFormat format) const {
    const std::string content = (format == ExportFormat::JSON)
        ? ExportLogAsJson()
        : ExportLogAsCsv();

    // Narrow-path std::ofstream on Windows interprets the path through the
    // active code page, not UTF-8, so non-ASCII target paths would fail or
    // land at a mojibake location. Use CreateFileW + WriteFile so any valid
    // Windows wide path round-trips correctly and writes are verified.
    HANDLE h = ::CreateFileW(filePath.c_str(),
                             GENERIC_WRITE,
                             FILE_SHARE_READ,
                             nullptr,
                             CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    const char* data = content.data();
    size_t remaining = content.size();
    while (remaining > 0) {
        const DWORD chunk = (remaining > 0xFFFFFFFFu)
            ? 0xFFFFFFFFu
            : static_cast<DWORD>(remaining);
        DWORD written = 0;
        if (!::WriteFile(h, data, chunk, &written, nullptr) || written == 0) {
            ::CloseHandle(h);
            ::DeleteFileW(filePath.c_str());
            return false;
        }
        data      += written;
        remaining -= written;
    }
    if (!::CloseHandle(h)) {
        // The file bytes may or may not be persisted; err on the side of
        // failure so callers don't see a successful export for an export
        // that did not complete cleanly.
        return false;
    }
    return true;
}

} // namespace LayerMount
