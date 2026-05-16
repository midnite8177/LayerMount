#include "HandleTable.h"

// These includes surface the complete payload types so the unique_ptr
// destructors instantiated by the HandleRegistry singleton can see
// the full definitions. HandleTypes.h only forward-declares them to
// keep the ABI layer header-lean; the singleton lives here, so here
// is where we pay the full-include cost.
#include "../impl/LayerMount.h"
#include "../impl/vhd/VHDLayerManager.h"
#include "../impl/vss/VSSManager.h"
#include "../impl/image/LayerImageManager.h"

namespace LayerMount::abi {

HandleRegistry& Handles() noexcept
{
    // Meyers singleton -- thread-safe initialization guaranteed by C++11.
    // The registry has no destructor side effects beyond releasing the
    // unique_ptr payloads; running at static-destruction time is fine
    // because the DLL is unloaded after all consumer threads have
    // released their handles (or it's a process exit, in which case
    // the OS reclaims memory anyway).
    static HandleRegistry registry;
    return registry;
}

} // namespace LayerMount::abi
