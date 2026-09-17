#include "PathUtil.h"

namespace LayerMount {

std::wstring StripTrailingBackslash(const std::wstring& path)
{
    if (!path.empty() && path.back() == L'\\')
        return path.substr(0, path.size() - 1);
    return path;
}

}
