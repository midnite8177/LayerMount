// CapabilityGate.h -- Internal helper that wraps the LM_HOST_CAPABILITIES
// bitfield supplied at LayerMountCreate time and exposes a typed query API for
// engine code to gate optimized vs. fallback paths.
//
// Lives under abi/ because it interprets a public-header enum, but is freely
// included from impl/. Pure header; no separate .cpp file.

#pragma once

#include "../public/LayerMount.h"

namespace LayerMount::abi {

class CapabilityGate {
public:
    constexpr CapabilityGate() noexcept : bits_(0) {}
    constexpr explicit CapabilityGate(UINT32 bits) noexcept : bits_(bits) {}

    constexpr UINT32 Raw() const noexcept { return bits_; }

    constexpr bool HasAds() const noexcept              { return (bits_ & LM_CAP_ADS) != 0; }
    constexpr bool HasReparsePoints() const noexcept    { return (bits_ & LM_CAP_REPARSE_POINTS) != 0; }
    constexpr bool HasSparseFiles() const noexcept      { return (bits_ & LM_CAP_SPARSE_FILES) != 0; }
    constexpr bool HasMultipleStreams() const noexcept  { return (bits_ & LM_CAP_MULTIPLE_STREAMS) != 0; }
    constexpr bool HasNtfsAcls() const noexcept         { return (bits_ & LM_CAP_NTFS_ACLS) != 0; }
    constexpr bool IsCaseSensitive() const noexcept     { return (bits_ & LM_CAP_CASE_SENSITIVE) != 0; }

private:
    UINT32 bits_;
};

} // namespace LayerMount::abi
