#pragma once

#include "WindowsNtStatus.h"
#include "../abi/CapabilityGate.h"

namespace LayerMount {

// What the engine does with security on an overlay, decided once from
// LM_CAP_NTFS_ACLS. With the capability, reads, writes and creates use the
// security of the object in the upper. Without it, the upper keeps no
// security that a caller sets: a read returns a synthetic world-readable
// descriptor, a write succeeds and changes nothing, and a create applies
// no descriptor.
class SecurityPolicy {
public:
    explicit SecurityPolicy(const ::LayerMount::abi::CapabilityGate& capabilities) noexcept
        : ntfsAcls_(capabilities.HasNtfsAcls()) {}

    // False when GetSecurity answers with SyntheticSecurity instead of
    // the upper object's descriptor.
    bool ReadsUpperSecurity() const noexcept { return ntfsAcls_; }

    // False when SetSecurity succeeds without writing anything. Callers
    // set security after every create, so failing would break them.
    bool AppliesSecurityWrites() const noexcept { return ntfsAcls_; }

    // The descriptor a create writes to the new object: the caller's, or
    // none, so that the object keeps what it inherits from its parent.
    PSECURITY_DESCRIPTOR DescriptorForCreate(PSECURITY_DESCRIPTOR requested) const noexcept {
        // Without the capability a create follows the same rule as SetSecurity.
        return ntfsAcls_ ? requested : nullptr;
    }

    // Builds a descriptor with only the parts `effective` asks for:
    // owner World, group World, and a DACL that grants FILE_GENERIC_READ
    // to Everyone. It never builds a SACL, because such an upper has no
    // audit data. When isProbe is set, it only reports the size in
    // requiredBytes.
    static NTSTATUS SyntheticSecurity(SECURITY_INFORMATION effective,
                                      bool isProbe,
                                      PSECURITY_DESCRIPTOR sd,
                                      SIZE_T sdBytes,
                                      SIZE_T* requiredBytes);

private:
    bool ntfsAcls_;
};

}
