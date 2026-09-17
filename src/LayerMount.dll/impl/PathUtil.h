// PathUtil.h -- Internal path-string helpers.
// VSS snapshot operations and VHD layer operations both need to strip a
// trailing backslash from a device or volume path. This function is the
// one implementation that both managers use.
//
// This function is internal only. Code outside LayerMount.dll does
// not include this header.

#pragma once

#include <string>

namespace LayerMount {

// Returns path with any single trailing backslash removed.
// Returns path unchanged if it does not end in a backslash.
std::wstring StripTrailingBackslash(const std::wstring& path);

}
