#pragma once

#include <string>
#include <utility>

namespace LayerMount {

// A full path on a disk of the machine, outside the merged view. Text() is
// the path as the caller wrote it, for storage, keys and comparisons.
// ForWin32() is its extended form, for Win32, std::filesystem and fstream
// calls. Text() keeps an extended path that a caller gave, so code must not
// remove a prefix from it. The constructor does not check the form, so a
// HostPath can hold an empty or a relative path.
class HostPath {
public:
    HostPath() = default;
    explicit HostPath(std::wstring text) : text_(std::move(text)) {}

    const std::wstring& Text() const noexcept { return text_; }
    // Builds a new string on each call. A relative path resolves against the
    // current directory of the process. If GetFullPathNameW fails, the result
    // is Text() with no prefix.
    std::wstring ForWin32() const;

private:
    std::wstring text_;
};

}
