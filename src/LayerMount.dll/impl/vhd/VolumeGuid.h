#pragma once

#include <windows.h>
#include <string>

namespace LayerMount::VHD {

// Given a VHD physical disk path (\\.\PhysicalDriveN), find the matching
// volume GUID path (\\?\Volume{GUID}\).
DWORD GetVolumeGuidForPhysicalDisk(const std::wstring& physicalDiskPath,
                                    std::wstring& outVolumeGuid);

// Given a volume GUID path, return the first mount point path (or empty).
DWORD GetMountPointForGuid(const std::wstring& volumeGuid,
                            std::wstring& outMountPoint);

// Given a VHD handle (from OpenVirtualDisk/CreateVirtualDisk after attach),
// find its volume GUID path. Combines GetVirtualDiskPhysicalPath +
// GetVolumeGuidForPhysicalDisk.
DWORD GetVolumeGuidForVHD(HANDLE vhdHandle, std::wstring& outVolumeGuid);

} // namespace LayerMount::VHD
