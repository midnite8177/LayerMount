// AbiGuard.h -- Internal header. Wraps every ABI export body in a
// try/catch that translates C++ exceptions to HRESULT and records a
// human-readable message via ErrorTls (FR-9, FR-10).
//
// Usage:
//
//   LM_API HRESULT LM_CALL LayerMountFoo(LM_HANDLE h, int x, int* out)
//   {
//       if (h == nullptr || out == nullptr) return E_POINTER;
//       LM_ABI_BEGIN();
//       // ... implementation that may throw ...
//       return S_OK;
//       LM_ABI_END();
//   }
//
// Parameter validation for trivial cases (null handles, null out-params)
// should run before LM_ABI_BEGIN so the happy path does not pay the
// try-setup cost. Everything else lives inside the guard.
//
// NOT installed. Consumers of LayerMount.dll do not see this header.

#pragma once

#include "ErrorTls.h"

#include <exception>
#include <new>
#include <stdexcept>
#include <system_error>

namespace LayerMount::abi {

// Internal exception type for impl code that wants to carry a precise
// HRESULT + wide-string message across to the ABI edge. Catching this
// specifically (before std::exception) lets the guard preserve the
// original HRESULT instead of collapsing to E_FAIL.
class LayerMountAbiException : public std::exception {
public:
    LayerMountAbiException(HRESULT hr, std::wstring message) noexcept
        : hr_(hr), message_(std::move(message)) {}

    HRESULT Hr() const noexcept { return hr_; }
    const wchar_t* Message() const noexcept { return message_.c_str(); }

    // exception::what() returns narrow; callers should prefer Message().
    const char* what() const noexcept override { return "LayerMountAbiException"; }

private:
    HRESULT      hr_;
    std::wstring message_;
};

} // namespace LayerMount::abi

// Paired macros. LM_ABI_BEGIN opens the try block; LM_ABI_END closes it
// and supplies the catch ladder. Any return statement inside the body
// must come before LM_ABI_END.
#define LM_ABI_BEGIN() try {

#define LM_ABI_END() \
    } catch (const ::LayerMount::abi::LayerMountAbiException& _ovl_e) { \
        ::LayerMount::abi::ErrorTls::Set(_ovl_e.Hr(), _ovl_e.Message()); \
        return _ovl_e.Hr(); \
    } catch (const std::bad_alloc&) { \
        ::LayerMount::abi::ErrorTls::Set(E_OUTOFMEMORY, L"Out of memory"); \
        return E_OUTOFMEMORY; \
    } catch (const std::system_error& _ovl_e) { \
        const int _ovl_code = _ovl_e.code().value(); \
        HRESULT _ovl_hr = (_ovl_e.code().category() == std::system_category()) \
            ? HRESULT_FROM_WIN32(static_cast<DWORD>(_ovl_code)) \
            : E_FAIL; \
        ::LayerMount::abi::ErrorTls::SetFromNarrow(_ovl_hr, _ovl_e.what()); \
        return _ovl_hr; \
    } catch (const std::invalid_argument& _ovl_e) { \
        ::LayerMount::abi::ErrorTls::SetFromNarrow(E_INVALIDARG, _ovl_e.what()); \
        return E_INVALIDARG; \
    } catch (const std::out_of_range& _ovl_e) { \
        ::LayerMount::abi::ErrorTls::SetFromNarrow(E_INVALIDARG, _ovl_e.what()); \
        return E_INVALIDARG; \
    } catch (const std::exception& _ovl_e) { \
        ::LayerMount::abi::ErrorTls::SetFromNarrow(E_FAIL, _ovl_e.what()); \
        return E_FAIL; \
    } catch (...) { \
        ::LayerMount::abi::ErrorTls::Set(E_UNEXPECTED, L"Unexpected C++ exception"); \
        return E_UNEXPECTED; \
    }
