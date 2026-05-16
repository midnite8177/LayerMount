// LayerMount.Images -- managed facade for layer-image (.lmnt) primitives.
// Obtained via <see cref="LayerMount.Images"/>.

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using LayerMount.Interop;

namespace LayerMount;

public sealed class ImagesApi
{
    private readonly LayerMount _owner;

    internal ImagesApi(LayerMount owner) => _owner = owner;

    public unsafe LayerImage Pack(
        string sourceDir,
        string outputPath,
        int compressionLevel = 3,
        string? author = null,
        string? description = null)
    {
        return PackCore(sourceDir, baseDir: null, outputPath,
            compressionLevel, author, description);
    }

    public unsafe LayerImage PackDifferential(
        string sourceDir,
        string baseDir,
        string outputPath,
        int compressionLevel = 3,
        string? author = null,
        string? description = null)
    {
        ArgumentNullException.ThrowIfNull(baseDir);
        return PackCore(sourceDir, baseDir, outputPath,
            compressionLevel, author, description);
    }

    private unsafe LayerImage PackCore(
        string sourceDir,
        string? baseDir,
        string outputPath,
        int compressionLevel,
        string? author,
        string? description)
    {
        ArgumentNullException.ThrowIfNull(sourceDir);
        ArgumentNullException.ThrowIfNull(outputPath);

        IntPtr authorPtr = author != null
            ? Marshal.StringToCoTaskMemUni(author) : IntPtr.Zero;
        IntPtr descPtr = description != null
            ? Marshal.StringToCoTaskMemUni(description) : IntPtr.Zero;

        try
        {
            LM_IMAGE_PACK_OPTIONS options = default;
            options.structSize = (uint)sizeof(LM_IMAGE_PACK_OPTIONS);
            options.author = authorPtr;
            options.description = descPtr;

            using var lease = new SafeHandleLease(_owner.Handle);
            IntPtr h = lease.Handle;
            IntPtr imageHandle;
            int hr = baseDir != null
                ? NativeMethods.LayerMountImagePackDifferential(
                    h, sourceDir, baseDir, outputPath, compressionLevel,
                    &options, &imageHandle)
                : NativeMethods.LayerMountImagePack(
                    h, sourceDir, outputPath, compressionLevel,
                    &options, &imageHandle);
            HResultGuard.ThrowIfFailed(hr,
                baseDir != null
                    ? nameof(NativeMethods.LayerMountImagePackDifferential)
                    : nameof(NativeMethods.LayerMountImagePack));

            var safe = new ImageHandle();
            safe.SetRawHandle(imageHandle);
            _owner.RegisterChild(safe);
            return new LayerImage(safe, outputPath);
        }
        finally
        {
            if (authorPtr != IntPtr.Zero) Marshal.FreeCoTaskMem(authorPtr);
            if (descPtr != IntPtr.Zero) Marshal.FreeCoTaskMem(descPtr);
        }
    }

    public unsafe void CreateManifest(string outputPath, IReadOnlyList<string> imagePaths)
    {
        ArgumentNullException.ThrowIfNull(outputPath);
        ArgumentNullException.ThrowIfNull(imagePaths);

        int count = imagePaths.Count;
        IntPtr[] ptrs = new IntPtr[count];
        IntPtr arrayPtr = IntPtr.Zero;
        try
        {
            for (int i = 0; i < count; i++)
                ptrs[i] = Marshal.StringToCoTaskMemUni(imagePaths[i] ?? "");

            if (count > 0)
            {
                arrayPtr = Marshal.AllocCoTaskMem(IntPtr.Size * count);
                for (int i = 0; i < count; i++)
                    Marshal.WriteIntPtr(arrayPtr, i * IntPtr.Size, ptrs[i]);
            }

            using var lease = new SafeHandleLease(_owner.Handle);
            int hr = NativeMethods.LayerMountImageCreateManifest(
                lease.Handle,
                outputPath,
                (IntPtr*)arrayPtr,
                (uint)count);
            HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountImageCreateManifest));
        }
        finally
        {
            for (int i = 0; i < count; i++)
                if (ptrs[i] != IntPtr.Zero) Marshal.FreeCoTaskMem(ptrs[i]);
            if (arrayPtr != IntPtr.Zero) Marshal.FreeCoTaskMem(arrayPtr);
        }
    }

    public void Unpack(string imagePath, string targetDir, bool verifyChecksum = true)
    {
        ArgumentNullException.ThrowIfNull(imagePath);
        ArgumentNullException.ThrowIfNull(targetDir);
        using var lease = new SafeHandleLease(_owner.Handle);
        int hr = NativeMethods.LayerMountImageUnpack(
            lease.Handle, imagePath, targetDir, verifyChecksum ? 1 : 0);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountImageUnpack));
    }

    public void Validate(string imagePath)
    {
        ArgumentNullException.ThrowIfNull(imagePath);
        using var lease = new SafeHandleLease(_owner.Handle);
        int hr = NativeMethods.LayerMountImageValidate(
            lease.Handle, imagePath);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountImageValidate));
    }

    public unsafe IReadOnlyList<ImageManifestEntry> GetManifest(string imagePath)
    {
        ArgumentNullException.ThrowIfNull(imagePath);
        using var lease = new SafeHandleLease(_owner.Handle);
        IntPtr h = lease.Handle;

        LM_IMAGE_MANIFEST manifest = default;
        manifest.entryCount = 0;
        manifest.entries = IntPtr.Zero;

        int hr = NativeMethods.LayerMountImageGetManifest(h, imagePath, &manifest);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountImageGetManifest));

        nuint entriesNeeded = manifest.entriesRequired;
        if (entriesNeeded == 0) return Array.Empty<ImageManifestEntry>();

        int entryCount = (int)entriesNeeded;
        var entries = new LM_IMAGE_MANIFEST_ENTRY[entryCount];

        // Retry loop on per-entry imagePath overflow. The native ABI
        // signals per-entry overflow by setting imagePathChars=0 and
        // imagePathRequired > our capacity without failing the outer
        // call; a fixed PathCap silently truncated long image paths in
        // long-path manifests. After each fill call, inspect every
        // entry's imagePathRequired and rerun with a larger buffer
        // when any overflowed.
        int pathCap = 512;
        const int MaxPasses = 3;
        for (int pass = 0; pass < MaxPasses; pass++)
        {
            int totalChars = entryCount * pathCap;
            IntPtr buf = Marshal.AllocCoTaskMem(totalChars * sizeof(char));
            try
            {
                for (int i = 0; i < entryCount; i++)
                {
                    entries[i].imagePath = buf + i * pathCap * sizeof(char);
                    entries[i].imagePathChars = (nuint)pathCap;
                }

                fixed (LM_IMAGE_MANIFEST_ENTRY* ep = entries)
                {
                    manifest.entryCount = (uint)entryCount;
                    manifest.entries = (IntPtr)ep;
                    hr = NativeMethods.LayerMountImageGetManifest(h, imagePath, &manifest);
                    HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountImageGetManifest));
                }

                // Check for per-entry overflow: retry with the largest
                // reported required size if any entry overflowed.
                nuint maxRequired = (nuint)pathCap;
                for (int i = 0; i < manifest.entryCount; i++)
                {
                    if (entries[i].imagePathRequired > maxRequired)
                    {
                        maxRequired = entries[i].imagePathRequired;
                    }
                }
                if (maxRequired > (nuint)pathCap && pass + 1 < MaxPasses)
                {
                    // Bump cap and retry. Allocation is re-done in the
                    // next iteration so the `finally` releases the
                    // current `buf` cleanly.
                    pathCap = (int)maxRequired;
                    continue;
                }

                var result = new List<ImageManifestEntry>((int)manifest.entryCount);
                for (int i = 0; i < manifest.entryCount; i++)
                {
                    string path = Marshal.PtrToStringUni(entries[i].imagePath) ?? string.Empty;
                    string checksum;
                    fixed (char* cp = entries[i].checksumHex)
                    {
                        checksum = Marshal.PtrToStringUni((IntPtr)cp) ?? string.Empty;
                    }
                    result.Add(new ImageManifestEntry(path, checksum));
                }
                return result;
            }
            finally
            {
                Marshal.FreeCoTaskMem(buf);
            }
        }

        // Unreachable: the retry loop either returns the result or bumps
        // pathCap past the largest reported size within MaxPasses. This
        // return exists so the compiler is happy about definite paths.
        return Array.Empty<ImageManifestEntry>();
    }

    public unsafe ImageMetadata GetMetadata(string imagePath)
    {
        ArgumentNullException.ThrowIfNull(imagePath);
        using var lease = new SafeHandleLease(_owner.Handle);
        IntPtr h = lease.Handle;

        // Retry loop on per-field buffer overflow. The native ABI signals
        // per-field overflow by setting *Chars=0 and *Required > capacity
        // without failing the outer call; the old implementation
        // ignored those Required values and returned empty strings for
        // long author / description / id fields.
        int perStringCap = 512;
        for (int pass = 0; pass < 3; pass++)
        {
            IntPtr buf = Marshal.AllocCoTaskMem(5 * perStringCap * sizeof(char));
            try
            {
                LM_IMAGE_METADATA meta = default;
                meta.id               = buf + 0 * perStringCap * sizeof(char);
                meta.idChars          = (nuint)perStringCap;
                meta.parentId         = buf + 1 * perStringCap * sizeof(char);
                meta.parentIdChars    = (nuint)perStringCap;
                meta.createdAt        = buf + 2 * perStringCap * sizeof(char);
                meta.createdAtChars   = (nuint)perStringCap;
                meta.author           = buf + 3 * perStringCap * sizeof(char);
                meta.authorChars      = (nuint)perStringCap;
                meta.description      = buf + 4 * perStringCap * sizeof(char);
                meta.descriptionChars = (nuint)perStringCap;

                int hr = NativeMethods.LayerMountImageGetMetadata(h, imagePath, &meta);
                HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountImageGetMetadata));

                int maxRequired = perStringCap;
                if ((int)meta.idRequired          > maxRequired) maxRequired = (int)meta.idRequired;
                if ((int)meta.parentIdRequired    > maxRequired) maxRequired = (int)meta.parentIdRequired;
                if ((int)meta.createdAtRequired   > maxRequired) maxRequired = (int)meta.createdAtRequired;
                if ((int)meta.authorRequired      > maxRequired) maxRequired = (int)meta.authorRequired;
                if ((int)meta.descriptionRequired > maxRequired) maxRequired = (int)meta.descriptionRequired;
                if (maxRequired > perStringCap)
                {
                    perStringCap = maxRequired;
                    continue;
                }

                return new ImageMetadata(
                    Marshal.PtrToStringUni(meta.id) ?? string.Empty,
                    Marshal.PtrToStringUni(meta.parentId) ?? string.Empty,
                    Marshal.PtrToStringUni(meta.createdAt) ?? string.Empty,
                    Marshal.PtrToStringUni(meta.author) ?? string.Empty,
                    Marshal.PtrToStringUni(meta.description) ?? string.Empty,
                    (CompressionType)meta.compression,
                    meta.fileCount,
                    meta.uncompressedSize,
                    meta.compressedSize);
            }
            finally
            {
                Marshal.FreeCoTaskMem(buf);
            }
        }
        HResultGuard.ThrowIfFailed(
            unchecked((int)0x800700EAu) /* ERROR_MORE_DATA */,
            nameof(NativeMethods.LayerMountImageGetMetadata));
        throw new InvalidOperationException("unreachable");
    }
}

public sealed record ImageManifestEntry(string ImagePath, string ChecksumHex);

public sealed record ImageMetadata(
    string Id,
    string ParentId,
    string CreatedAt,
    string Author,
    string Description,
    CompressionType Compression,
    ulong FileCount,
    ulong UncompressedSize,
    ulong CompressedSize);
