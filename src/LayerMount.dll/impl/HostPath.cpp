#include "HostPath.h"
#include "LayerPath.h"

namespace LayerMount {

std::wstring HostPath::ForWin32() const {
    return WithExtendedPrefix(text_);
}

}
