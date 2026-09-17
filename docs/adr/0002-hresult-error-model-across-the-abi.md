# Every ABI call returns an HRESULT

The C ABI must serve C, C++, and P/Invoke callers, and C++ exceptions cannot cross it. Every exported function returns an HRESULT, keeps a per-thread last-error message, and never throws. Engine-specific failures use `FACILITY_ITF` codes `0xB000..0xBFFF`; a host adapter must not emit codes in that range, so that a caller can tell an engine failure from a host failure without a second lookup.
