// LayerMount.Vss -- managed facade for VSS snapshot primitives (FR-26, FR-27).
// Obtained via <see cref="LayerMount.Vss"/>. All entry points open a native
// ComScope internally; callers never have to CoInitializeEx.

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using LayerMount.Interop;

namespace LayerMount;

public sealed class VssApi
{
    private readonly LayerMount _owner;

    internal VssApi(LayerMount owner) => _owner = owner;

    public unsafe VssSnapshot CreateSnapshot(string volumePath, bool persistent = false)
    {
        ArgumentNullException.ThrowIfNull(volumePath);
        using var lease = new SafeHandleLease(_owner.Handle);

        // The native ABI guarantees buffer sizes these or larger are
        // always sufficient for a GUID-shaped id and a VSS device-path
        // string. Pass them straight in -- earlier probe-then-fill
        // loops created two snapshots for one logical request because
        // the probe call committed a snapshot before checking buffers.
        const int IdCap = 64;        // LM_VSS_ID_CHARS_REQUIRED
        const int DevicePathCap = 260; // LM_VSS_DEVICE_PATH_CHARS_REQUIRED
        char* idBuf = stackalloc char[IdCap];
        char* deviceBuf = stackalloc char[DevicePathCap];

        IntPtr snapshotHandle = IntPtr.Zero;
        nuint idRequired = 0;
        nuint deviceRequired = 0;

        int hr = NativeMethods.LayerMountVssCreateSnapshot(
            lease.Handle, volumePath, persistent ? 1 : 0, &snapshotHandle,
            idBuf, IdCap, &idRequired,
            deviceBuf, DevicePathCap, &deviceRequired);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountVssCreateSnapshot));

        string idStr = Marshal.PtrToStringUni((IntPtr)idBuf) ?? string.Empty;
        string devStr = Marshal.PtrToStringUni((IntPtr)deviceBuf) ?? string.Empty;
        var safe = new VssSnapshotHandle();
        safe.SetRawHandle(snapshotHandle);
        _owner.RegisterChild(safe);
        return new VssSnapshot(safe, idStr, devStr, persistent);
    }

    public void DeleteSnapshot(string snapshotId)
    {
        ArgumentNullException.ThrowIfNull(snapshotId);
        using var lease = new SafeHandleLease(_owner.Handle);
        int hr = NativeMethods.LayerMountVssDeleteSnapshot(
            lease.Handle, snapshotId);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountVssDeleteSnapshot));
    }

    public unsafe IReadOnlyList<VssSnapshotInfo> ListSnapshots()
    {
        using var lease = new SafeHandleLease(_owner.Handle);
        IntPtr h = lease.Handle;

        // Size probe.
        uint required = 0;
        uint written = 0;
        int hr = NativeMethods.LayerMountVssListSnapshots(h, null, 0, &written, &required);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountVssListSnapshots));
        if (required == 0) return Array.Empty<VssSnapshotInfo>();

        int entryCount = (int)required;
        int perStringCap = 260;
        var entries = new LM_VSS_SNAPSHOT_INFO[entryCount];

        // Retry loop: if any entry's volumePath or devicePath exceeded
        // the per-string cap, the native ABI sets *Chars = 0 and
        // *Required > capacity. Grow the cap to cover the worst case
        // and re-run. Ignoring the Required values (the old behavior)
        // silently dropped long fields to empty strings.
        for (int pass = 0; pass < 3; pass++)
        {
            int totalChars = entryCount * 2 * perStringCap;
            IntPtr buf = Marshal.AllocCoTaskMem(totalChars * sizeof(char));
            try
            {
                for (int i = 0; i < entryCount; i++)
                {
                    IntPtr basePtr = buf + i * 2 * perStringCap * sizeof(char);
                    entries[i] = default;
                    entries[i].volumePath = basePtr + 0 * perStringCap * sizeof(char);
                    entries[i].volumePathChars = (nuint)perStringCap;
                    entries[i].devicePath = basePtr + 1 * perStringCap * sizeof(char);
                    entries[i].devicePathChars = (nuint)perStringCap;
                }

                fixed (LM_VSS_SNAPSHOT_INFO* ep = entries)
                {
                    hr = NativeMethods.LayerMountVssListSnapshots(
                        h, ep, (uint)entryCount, &written, &required);
                    HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountVssListSnapshots));
                }

                // Check whether any entry reported an undersized buffer.
                int maxRequired = perStringCap;
                for (int i = 0; i < written; i++)
                {
                    if ((int)entries[i].volumePathRequired > maxRequired)
                        maxRequired = (int)entries[i].volumePathRequired;
                    if ((int)entries[i].devicePathRequired > maxRequired)
                        maxRequired = (int)entries[i].devicePathRequired;
                }
                if (maxRequired > perStringCap)
                {
                    // Grow and retry.
                    perStringCap = maxRequired;
                    continue;
                }

                var result = new List<VssSnapshotInfo>((int)written);
                for (int i = 0; i < written; i++)
                {
                    string id;
                    fixed (char* idp = entries[i].id)
                    {
                        id = Marshal.PtrToStringUni((IntPtr)idp) ?? string.Empty;
                    }
                    result.Add(new VssSnapshotInfo(
                        id,
                        entries[i].vssId,
                        Marshal.PtrToStringUni(entries[i].volumePath) ?? string.Empty,
                        Marshal.PtrToStringUni(entries[i].devicePath) ?? string.Empty,
                        entries[i].persistent != 0,
                        SafeFromFileTimeUtc((long)entries[i].createdAt)));
                }
                return result;
            }
            finally
            {
                Marshal.FreeCoTaskMem(buf);
            }
        }
        HResultGuard.ThrowIfFailed(
            unchecked((int)0x800700EAu) /* ERROR_MORE_DATA */,
            nameof(NativeMethods.LayerMountVssListSnapshots));
        throw new InvalidOperationException("unreachable");
    }

    public void Cleanup()
    {
        using var lease = new SafeHandleLease(_owner.Handle);
        int hr = NativeMethods.LayerMountVssCleanupSnapshots(lease.Handle);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountVssCleanupSnapshots));
    }

    // DateTime.FromFileTimeUtc throws ArgumentOutOfRangeException for
    // FILETIME values outside [0, 2650467743999999999]. A native side
    // returning 0 (snapshot whose createdAt was never populated), -1
    // (uninitialized memory), or a corrupted high word would otherwise
    // tunnel through the whole ListSnapshots call and surface as an
    // unhelpful exception with no diagnostic linkage to the bad entry.
    // Clamp to DateTime.MinValue so the caller sees a benign sentinel
    // and the rest of the list still parses.
    private static DateTime SafeFromFileTimeUtc(long ft)
    {
        try
        {
            return DateTime.FromFileTimeUtc(ft);
        }
        catch (ArgumentOutOfRangeException)
        {
            return DateTime.MinValue;
        }
    }
}

public sealed record VssSnapshotInfo(
    string Id,
    Guid VssId,
    string VolumePath,
    string DevicePath,
    bool Persistent,
    DateTime CreatedAtUtc);
