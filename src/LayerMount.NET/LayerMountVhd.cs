// LayerMount.Vhd -- managed facade for VHD/VHDX primitives.
// Obtained via <see cref="LayerMount.Vhd"/>.

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using LayerMount.Interop;

namespace LayerMount;

public sealed class VhdApi
{
    private readonly LayerMount _owner;

    internal VhdApi(LayerMount owner) => _owner = owner;

    public unsafe VhdImage Create(
        string path,
        ulong sizeBytes,
        VhdKind kind = VhdKind.Dynamic,
        string? parentPath = null,
        bool readOnly = false,
        bool suppressDriveLetter = false,
        VhdAttachLifetime lifetime = VhdAttachLifetime.Permanent)
    {
        return CreateOrOpen(path, sizeBytes, kind, parentPath,
            readOnly, suppressDriveLetter, lifetime, create: true);
    }

    public unsafe VhdImage Open(
        string path,
        bool readOnly = false,
        bool suppressDriveLetter = false,
        VhdAttachLifetime lifetime = VhdAttachLifetime.Permanent)
    {
        return CreateOrOpen(path, 0, VhdKind.Dynamic, null,
            readOnly, suppressDriveLetter, lifetime, create: false);
    }

    private unsafe VhdImage CreateOrOpen(
        string path,
        ulong sizeBytes,
        VhdKind kind,
        string? parentPath,
        bool readOnly,
        bool suppressDriveLetter,
        VhdAttachLifetime lifetime,
        bool create)
    {
        ArgumentNullException.ThrowIfNull(path);

        IntPtr pathPtr = Marshal.StringToCoTaskMemUni(path);
        IntPtr parentPtr = parentPath != null
            ? Marshal.StringToCoTaskMemUni(parentPath)
            : IntPtr.Zero;

        try
        {
            LM_VHD_CONFIG config = default;
            config.structSize          = (uint)sizeof(LM_VHD_CONFIG);
            config.kind                = (uint)kind;
            config.sizeBytes           = sizeBytes;
            config.path                = pathPtr;
            config.parentPath          = parentPtr;
            config.readOnly            = readOnly ? 1 : 0;
            config.suppressDriveLetter = suppressDriveLetter ? 1 : 0;
            config.lifetime            = (uint)lifetime;

            IntPtr raw;
            using var lease = new SafeHandleLease(_owner.Handle);
            int hr = create
                ? NativeMethods.LayerMountVhdCreate(lease.Handle, &config, &raw)
                : NativeMethods.LayerMountVhdOpen(lease.Handle, &config, &raw);
            HResultGuard.ThrowIfFailed(hr,
                create ? nameof(NativeMethods.LayerMountVhdCreate) : nameof(NativeMethods.LayerMountVhdOpen));

            var safe = new VhdHandle();
            safe.SetRawHandle(raw);
            _owner.RegisterChild(safe);
            return new VhdImage(safe, path);
        }
        finally
        {
            if (pathPtr != IntPtr.Zero) Marshal.FreeCoTaskMem(pathPtr);
            if (parentPtr != IntPtr.Zero) Marshal.FreeCoTaskMem(parentPtr);
        }
    }

    public void Import(string directoryPath, string vhdPath, ulong sizeBytes)
    {
        ArgumentNullException.ThrowIfNull(directoryPath);
        ArgumentNullException.ThrowIfNull(vhdPath);
        using var lease = new SafeHandleLease(_owner.Handle);
        int hr = NativeMethods.LayerMountVhdImport(
            lease.Handle, directoryPath, vhdPath, sizeBytes);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountVhdImport));
    }

    public void Export(string vhdPath, string directoryPath)
    {
        ArgumentNullException.ThrowIfNull(vhdPath);
        ArgumentNullException.ThrowIfNull(directoryPath);
        using var lease = new SafeHandleLease(_owner.Handle);
        int hr = NativeMethods.LayerMountVhdExport(
            lease.Handle, vhdPath, directoryPath);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountVhdExport));
    }

    /// <summary>
    /// Lists the VHD layers recorded in the manifest JSON at
    /// <paramref name="manifestDir"/> (pass null for the current working
    /// directory). Returns an empty array if the manifest is absent.
    /// </summary>
    public unsafe IReadOnlyList<VhdLayerInfo> ListLayers(string? manifestDir = null)
    {
        IntPtr manifestDirPtr = manifestDir != null
            ? Marshal.StringToCoTaskMemUni(manifestDir)
            : IntPtr.Zero;

        try
        {
            using var lease = new SafeHandleLease(_owner.Handle);
            IntPtr h = lease.Handle;

            // First call: size probe.
            uint required = 0;
            uint written = 0;
            int hr = NativeMethods.LayerMountVhdListLayers(
                h, manifestDirPtr, null, 0, &written, &required);
            HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountVhdListLayers));
            if (required == 0) return Array.Empty<VhdLayerInfo>();

            // Second call: populate with per-entry buffers. Start with a
            // generous fixed cap; if any field overflowed (native ABI
            // sets that field's *Chars=0 and *Required > capacity) grow
            // and retry. The old implementation ignored *Required and
            // silently returned empty strings for long paths.
            int perStringCap = 512;
            int entryCount = (int)required;
            var entries = new LM_VHD_LAYER_INFO[entryCount];

            for (int pass = 0; pass < 3; pass++)
            {
                int totalChars = entryCount * 5 * perStringCap;
                IntPtr buf = Marshal.AllocCoTaskMem(totalChars * sizeof(char));

                try
                {
                    for (int i = 0; i < entryCount; i++)
                    {
                        IntPtr basePtr = buf + i * 5 * perStringCap * sizeof(char);
                        entries[i] = default;
                        entries[i].path             = basePtr + 0 * perStringCap * sizeof(char);
                        entries[i].pathChars        = (nuint)perStringCap;
                        entries[i].parentId         = basePtr + 1 * perStringCap * sizeof(char);
                        entries[i].parentIdChars    = (nuint)perStringCap;
                        entries[i].mountStatus      = basePtr + 2 * perStringCap * sizeof(char);
                        entries[i].mountStatusChars = (nuint)perStringCap;
                        entries[i].volumeGuid       = basePtr + 3 * perStringCap * sizeof(char);
                        entries[i].volumeGuidChars  = (nuint)perStringCap;
                        entries[i].createdAt        = basePtr + 4 * perStringCap * sizeof(char);
                        entries[i].createdAtChars   = (nuint)perStringCap;
                    }

                    fixed (LM_VHD_LAYER_INFO* ep = entries)
                    {
                        hr = NativeMethods.LayerMountVhdListLayers(
                            h, manifestDirPtr, ep, (uint)entryCount, &written, &required);
                        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountVhdListLayers));
                    }

                    int maxRequired = perStringCap;
                    for (int i = 0; i < written; i++)
                    {
                        if ((int)entries[i].pathRequired        > maxRequired) maxRequired = (int)entries[i].pathRequired;
                        if ((int)entries[i].parentIdRequired    > maxRequired) maxRequired = (int)entries[i].parentIdRequired;
                        if ((int)entries[i].mountStatusRequired > maxRequired) maxRequired = (int)entries[i].mountStatusRequired;
                        if ((int)entries[i].volumeGuidRequired  > maxRequired) maxRequired = (int)entries[i].volumeGuidRequired;
                        if ((int)entries[i].createdAtRequired   > maxRequired) maxRequired = (int)entries[i].createdAtRequired;
                    }
                    if (maxRequired > perStringCap)
                    {
                        perStringCap = maxRequired;
                        continue;
                    }

                    var result = new List<VhdLayerInfo>((int)written);
                    for (int i = 0; i < written; i++)
                    {
                        result.Add(ProjectLayerInfo(ref entries[i]));
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
                nameof(NativeMethods.LayerMountVhdListLayers));
            throw new InvalidOperationException("unreachable");
        }
        finally
        {
            if (manifestDirPtr != IntPtr.Zero) Marshal.FreeCoTaskMem(manifestDirPtr);
        }
    }

    private static unsafe VhdLayerInfo ProjectLayerInfo(ref LM_VHD_LAYER_INFO entry)
    {
        // Fixed-size id field; NUL-terminated.
        string id;
        fixed (char* idp = entry.id)
        {
            id = Marshal.PtrToStringUni((IntPtr)idp) ?? string.Empty;
        }

        return new VhdLayerInfo(
            id,
            (VhdLayerType)entry.type,
            ReadString(entry.path, entry.pathChars),
            ReadString(entry.parentId, entry.parentIdChars),
            ReadString(entry.mountStatus, entry.mountStatusChars),
            ReadString(entry.volumeGuid, entry.volumeGuidChars),
            ReadString(entry.createdAt, entry.createdAtChars));
    }

    private static string ReadString(IntPtr ptr, nuint charCount)
    {
        if (ptr == IntPtr.Zero || charCount == 0) return string.Empty;
        return Marshal.PtrToStringUni(ptr) ?? string.Empty;
    }

    public unsafe bool UnregisterLayer(string layerId, string? manifestDir = null)
    {
        ArgumentNullException.ThrowIfNull(layerId);
        IntPtr manifestDirPtr = manifestDir != null
            ? Marshal.StringToCoTaskMemUni(manifestDir)
            : IntPtr.Zero;

        try
        {
            using var lease = new SafeHandleLease(_owner.Handle);
            int removed = 0;
            int hr = NativeMethods.LayerMountVhdUnregisterLayer(
                lease.Handle, layerId, manifestDirPtr, &removed);
            HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountVhdUnregisterLayer));
            return removed != 0;
        }
        finally
        {
            if (manifestDirPtr != IntPtr.Zero) Marshal.FreeCoTaskMem(manifestDirPtr);
        }
    }

    public unsafe string GetLayerMetadataJson(string layerId, string? manifestDir = null)
    {
        ArgumentNullException.ThrowIfNull(layerId);
        IntPtr manifestDirPtr = manifestDir != null
            ? Marshal.StringToCoTaskMemUni(manifestDir)
            : IntPtr.Zero;

        try
        {
            using var lease = new SafeHandleLease(_owner.Handle);
            IntPtr h = lease.Handle;
            int hr = BufferHelpers.TryReadString(
                (char* buffer, nuint capacity, nuint* required) =>
                    NativeMethods.LayerMountVhdGetLayerMetadataJson(
                        h, manifestDirPtr, layerId, buffer, capacity, required),
                out string? result);
            HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountVhdGetLayerMetadataJson));
            return result ?? "{}";
        }
        finally
        {
            if (manifestDirPtr != IntPtr.Zero) Marshal.FreeCoTaskMem(manifestDirPtr);
        }
    }
}

public sealed record VhdLayerInfo(
    string Id,
    VhdLayerType Type,
    string Path,
    string ParentId,
    string MountStatus,
    string VolumeGuid,
    string CreatedAt);
