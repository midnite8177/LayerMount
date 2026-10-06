#pragma once

#include "HandleTable.h"
#include "../impl/LayerMount.h"

#include <cstdint>
#include <memory>
#include <string>

namespace LayerMount::abi {

struct OpenRequest {
    std::wstring relativePath;
    UINT32 grantedAccess;
    UINT32 createOptions;
    DWORD callerPid;
};

// Reserves a slot in files, then runs the engine's Create. A full table
// returns E_OUTOFMEMORY before the engine runs, so the overlay does not
// change. The reserved slot does not resolve until the engine call
// succeeds. On success *outEncoded is the installed handle and the mount's
// childCount counts it. On a failure the slot is free again; an exception
// from the engine frees the slot and propagates.
HRESULT CreateFileHandle(FileTable& files,
                         const std::shared_ptr<LayerMountHolder>& mountHolder,
                         const ::LayerMount::LayerMount::CreateRequest& request,
                         std::uint64_t* outEncoded,
                         ::LayerMount::InternalFileInfo* outInfo);

// The same as CreateFileHandle for the engine's Open, so a full table
// makes no copy-up.
HRESULT OpenFileHandle(FileTable& files,
                       const std::shared_ptr<LayerMountHolder>& mountHolder,
                       const OpenRequest& request,
                       std::uint64_t* outEncoded,
                       ::LayerMount::InternalFileInfo* outInfo);

}
