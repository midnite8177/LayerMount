// NOT installed. DLL consumers access this only indirectly through the
// public header.

#pragma once

#include "../public/LayerMount.h"

namespace LayerMount::abi::ErrorTls {

// Records the HRESULT + message for the current thread. `message` is
// copied; the caller's buffer need not outlive the call. Passing
// `message == nullptr` stores an empty string.
void Set(HRESULT hr, PCWSTR message) noexcept;

// Same as Set, but translates a narrow (UTF-8 or ASCII) string to wide
// for storage. Intended for catch blocks that pick up std::exception::what().
void SetFromNarrow(HRESULT hr, const char* utf8) noexcept;

void SetFillFailure(HRESULT hr, PCWSTR message) noexcept;

// Clears only the fill mark. The HRESULT and the message stay, so
// LayerMountGetLastErrorMessage still reads the previous failure.
void ClearFillFailure() noexcept;

bool LastFailureWasFill() noexcept;

// Clears the HRESULT, the message and the fill mark, and frees the
// message's buffer.
void Clear() noexcept;

// Reads the stored message using the two-call buffer pattern. `hr`
// filters: if it does not match the stored HRESULT the function returns
// S_OK with *requiredChars == 0 (caller asked about a different error
// than the one on record for this thread).
//
//   Sizing call:   buffer == nullptr, bufferChars == 0
//                  -> writes required chars (incl. NUL) to *requiredChars
//                  -> returns S_OK
//   Full call:     buffer != nullptr, bufferChars >= required
//                  -> copies message incl. NUL
//                  -> writes chars copied (incl. NUL) to *requiredChars
//                  -> returns S_OK
//   Short buffer:  bufferChars < required
//                  -> writes required to *requiredChars, leaves buffer untouched
//                  -> returns HRESULT_FROM_WIN32(ERROR_MORE_DATA)
HRESULT Last(HRESULT hr, PWSTR buffer, SIZE_T bufferChars, SIZE_T* requiredChars) noexcept;

} // namespace LayerMount::abi::ErrorTls
