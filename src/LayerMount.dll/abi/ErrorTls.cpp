#include "ErrorTls.h"

#include <cstring>
#include <string>

namespace LayerMount::abi::ErrorTls {

namespace {

thread_local HRESULT      g_lastHr{S_OK};
thread_local std::wstring g_lastMessage;

std::wstring WidenUtf8(const char* utf8) noexcept
{
    if (utf8 == nullptr || *utf8 == '\0') {
        return {};
    }
    const int narrowLen = static_cast<int>(std::strlen(utf8));
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8, narrowLen,
                                             nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out;
    try {
        out.resize(static_cast<size_t>(needed));
    } catch (...) {
        return {};
    }
    ::MultiByteToWideChar(CP_UTF8, 0, utf8, narrowLen, out.data(), needed);
    return out;
}

} // namespace

void Set(HRESULT hr, PCWSTR message) noexcept
{
    g_lastHr = hr;
    try {
        if (message == nullptr) {
            g_lastMessage.clear();
        } else {
            g_lastMessage.assign(message);
        }
    } catch (...) {
        // std::wstring::assign can throw std::bad_alloc. Fall back to an
        // empty message rather than propagating -- this function is
        // noexcept and runs from catch blocks.
        g_lastMessage.clear();
    }
}

void SetFromNarrow(HRESULT hr, const char* utf8) noexcept
{
    g_lastHr = hr;
    try {
        g_lastMessage = WidenUtf8(utf8);
    } catch (...) {
        g_lastMessage.clear();
    }
}

void Clear() noexcept
{
    g_lastHr = S_OK;
    try {
        g_lastMessage.clear();
        g_lastMessage.shrink_to_fit();
    } catch (...) {
        // Nothing we can do; keep whatever state we have.
    }
}

HRESULT Last(HRESULT hr, PWSTR buffer, SIZE_T bufferChars, SIZE_T* requiredChars) noexcept
{
    if (requiredChars == nullptr) {
        return E_POINTER;
    }

    // Filter by HRESULT: if the caller is asking about a different error
    // than the last one on this thread, report "no message on record".
    if (hr != g_lastHr) {
        *requiredChars = 0;
        if (buffer != nullptr && bufferChars > 0) {
            buffer[0] = L'\0';
        }
        return S_OK;
    }

    const SIZE_T required = g_lastMessage.size() + 1; // incl. NUL
    *requiredChars = required;

    if (buffer == nullptr || bufferChars == 0) {
        return S_OK; // sizing call
    }
    if (bufferChars < required) {
        return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    }

    if (!g_lastMessage.empty()) {
        std::memcpy(buffer, g_lastMessage.data(),
                    g_lastMessage.size() * sizeof(wchar_t));
    }
    buffer[g_lastMessage.size()] = L'\0';
    return S_OK;
}

} // namespace LayerMount::abi::ErrorTls
