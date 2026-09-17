using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using LayerMount.Interop;

namespace LayerMount;

/// <summary>
/// Managed facade for layer-image (<c>.lmnt</c>) primitives: packing,
/// differential packing, manifest creation, unpacking, validation, and
/// metadata queries. Obtained via <see cref="LayerMount.Images"/>.
/// </summary>
public sealed class ImagesApi
{
    private readonly LayerMount _owner;

    internal ImagesApi(LayerMount owner) => _owner = owner;

    /// <summary>
    /// Packs <paramref name="sourceDir"/> into a <c>.lmnt</c> layer image at
    /// <paramref name="outputPath"/>, stamped per <paramref name="stampOptions"/>.
    /// </summary>
    /// <param name="sourceDir">Directory tree to pack.</param>
    /// <param name="outputPath">Destination path for the <c>.lmnt</c> file.</param>
    /// <param name="stampOptions">Compression level and metadata to stamp.</param>
    /// <returns>
    /// A <see cref="LayerImage"/> receipt for the packed image, holding
    /// only the native handle and the output path, no other state.
    /// </returns>
    /// <exception cref="ArgumentNullException">
    /// If <paramref name="sourceDir"/>, <paramref name="outputPath"/>, or
    /// <paramref name="stampOptions"/> is null.
    /// </exception>
    /// <exception cref="LayerMountException">
    /// If the underlying native call returns a non-success HRESULT.
    /// </exception>
    public unsafe LayerImage Pack(
        string sourceDir,
        string outputPath,
        ImageStampOptions stampOptions)
    {
        ArgumentNullException.ThrowIfNull(stampOptions);
        return PackCore(sourceDir, baseDir: null, outputPath, stampOptions);
    }

    /// <summary>
    /// Packs a differential layer image recording only the files in
    /// <paramref name="sourceDir"/> that are new or modified relative to
    /// <paramref name="baseDir"/>. Files deleted in <paramref name="sourceDir"/>
    /// relative to <paramref name="baseDir"/> get whiteout entries in the
    /// image metadata.
    /// </summary>
    /// <param name="sourceDir">Directory tree to pack.</param>
    /// <param name="baseDir">Directory tree the differential is computed against.</param>
    /// <param name="outputPath">Destination path for the <c>.lmnt</c> file.</param>
    /// <param name="stampOptions">Compression level and metadata to stamp.</param>
    /// <returns>
    /// A receipt handle for the packed image, with the same shape and
    /// lifetime as <see cref="Pack"/>.
    /// </returns>
    /// <exception cref="ArgumentNullException">
    /// If <paramref name="sourceDir"/>, <paramref name="baseDir"/>,
    /// <paramref name="outputPath"/>, or <paramref name="stampOptions"/> is null.
    /// </exception>
    /// <exception cref="LayerMountException">
    /// If the underlying native call returns a non-success HRESULT.
    /// </exception>
    public unsafe LayerImage PackDifferential(
        string sourceDir,
        string baseDir,
        string outputPath,
        ImageStampOptions stampOptions)
    {
        ArgumentNullException.ThrowIfNull(baseDir);
        ArgumentNullException.ThrowIfNull(stampOptions);
        return PackCore(sourceDir, baseDir, outputPath, stampOptions);
    }

    private unsafe LayerImage PackCore(
        string sourceDir,
        string? baseDir,
        string outputPath,
        ImageStampOptions stampOptions)
    {
        ArgumentNullException.ThrowIfNull(sourceDir);
        ArgumentNullException.ThrowIfNull(outputPath);

        string? author = stampOptions.Author;
        string? description = stampOptions.Description;

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
                    h, sourceDir, baseDir, outputPath, stampOptions.CompressionLevel,
                    &options, &imageHandle)
                : NativeMethods.LayerMountImagePack(
                    h, sourceDir, outputPath, stampOptions.CompressionLevel,
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

    /// <summary>
    /// Creates a multi-image manifest JSON file at <paramref name="outputPath"/>
    /// listing <paramref name="imagePaths"/> in order along with each
    /// image's SHA-256 checksum. Reads and hashes every listed image.
    /// </summary>
    /// <param name="outputPath">Destination path for the manifest JSON file.</param>
    /// <param name="imagePaths">
    /// Paths of the <c>.lmnt</c> images to list, in order. Each must name an
    /// existing layer image.
    /// </param>
    /// <exception cref="ArgumentNullException">
    /// If <paramref name="outputPath"/> or <paramref name="imagePaths"/> is null.
    /// </exception>
    /// <exception cref="LayerMountException">
    /// If the underlying native call returns a non-success HRESULT.
    /// </exception>
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

    /// <summary>
    /// Extracts the layer image at <paramref name="imagePath"/> into
    /// <paramref name="targetDir"/>, overwriting a file already there at
    /// the same relative path. Rejects the image if the data section's
    /// SHA-256 does not match the header's checksum.
    /// </summary>
    /// <param name="imagePath">Path of the <c>.lmnt</c> image to extract.</param>
    /// <param name="targetDir">Directory to extract into.</param>
    /// <exception cref="ArgumentNullException">
    /// If <paramref name="imagePath"/> or <paramref name="targetDir"/> is null.
    /// </exception>
    /// <exception cref="LayerMountException">
    /// If the underlying native call returns a non-success HRESULT, including
    /// when the checksum does not match.
    /// </exception>
    public void Unpack(string imagePath, string targetDir) =>
        UnpackCore(imagePath, targetDir, verifyChecksum: true);

    /// <summary>
    /// Extracts the layer image at <paramref name="imagePath"/> into
    /// <paramref name="targetDir"/>, overwriting a file already there at
    /// the same relative path, without checking the data section's SHA-256
    /// against the header's checksum. Use only when the caller has already
    /// established the image's integrity by other means; a corrupted or
    /// tampered image extracts with no signal.
    /// </summary>
    /// <param name="imagePath">Path of the <c>.lmnt</c> image to extract.</param>
    /// <param name="targetDir">Directory to extract into.</param>
    /// <exception cref="ArgumentNullException">
    /// If <paramref name="imagePath"/> or <paramref name="targetDir"/> is null.
    /// </exception>
    /// <exception cref="LayerMountException">
    /// If the underlying native call returns a non-success HRESULT.
    /// </exception>
    public void UnpackUnchecked(string imagePath, string targetDir) =>
        UnpackCore(imagePath, targetDir, verifyChecksum: false);

    private void UnpackCore(string imagePath, string targetDir, bool verifyChecksum)
    {
        ArgumentNullException.ThrowIfNull(imagePath);
        ArgumentNullException.ThrowIfNull(targetDir);
        using var lease = new SafeHandleLease(_owner.Handle);
        int hr = NativeMethods.LayerMountImageUnpack(
            lease.Handle, imagePath, targetDir, verifyChecksum ? 1 : 0);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountImageUnpack));
    }

    /// <summary>
    /// Validates the layer image at <paramref name="imagePath"/>: reads the
    /// header and metadata, then streams the data section to verify its
    /// SHA-256 against the header's checksum. Does not extract anything.
    /// </summary>
    /// <param name="imagePath">Path of the <c>.lmnt</c> image to validate.</param>
    /// <exception cref="ArgumentNullException">
    /// If <paramref name="imagePath"/> is null.
    /// </exception>
    /// <exception cref="LayerMountException">
    /// If the underlying native call returns a non-success HRESULT, including
    /// when the checksum does not match.
    /// </exception>
    public void Validate(string imagePath)
    {
        ArgumentNullException.ThrowIfNull(imagePath);
        using var lease = new SafeHandleLease(_owner.Handle);
        int hr = NativeMethods.LayerMountImageValidate(
            lease.Handle, imagePath);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountImageValidate));
    }

    /// <summary>
    /// Loads the multi-image manifest JSON at <paramref name="manifestPath"/>
    /// (a file written by <see cref="CreateManifest"/>, not a <c>.lmnt</c>
    /// image) and lists its entries. May issue more than one native call to
    /// size the per-entry path buffers.
    /// </summary>
    /// <param name="manifestPath">Path of the manifest JSON file to load.</param>
    /// <returns>The manifest's entries, in order.</returns>
    /// <exception cref="ArgumentNullException">
    /// If <paramref name="manifestPath"/> is null.
    /// </exception>
    /// <exception cref="LayerMountException">
    /// If the underlying native call returns a non-success HRESULT.
    /// </exception>
    public unsafe IReadOnlyList<ImageManifestEntry> GetManifest(string manifestPath)
    {
        ArgumentNullException.ThrowIfNull(manifestPath);
        using var lease = new SafeHandleLease(_owner.Handle);
        IntPtr h = lease.Handle;

        LM_IMAGE_MANIFEST manifest = default;
        manifest.entryCount = 0;
        manifest.entries = IntPtr.Zero;

        int hr = NativeMethods.LayerMountImageGetManifest(h, manifestPath, &manifest);
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
                    hr = NativeMethods.LayerMountImageGetManifest(h, manifestPath, &manifest);
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

    /// <summary>
    /// Reads the header and metadata of the layer image at
    /// <paramref name="imagePath"/>. Tags, whiteouts, and labels are not
    /// projected here. May issue more than one native call to size the
    /// per-field string buffers.
    /// </summary>
    /// <param name="imagePath">Path of the <c>.lmnt</c> image to read.</param>
    /// <returns>The image's metadata.</returns>
    /// <exception cref="ArgumentNullException">
    /// If <paramref name="imagePath"/> is null.
    /// </exception>
    /// <exception cref="LayerMountException">
    /// If the underlying native call returns a non-success HRESULT.
    /// </exception>
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

/// <summary>
/// Compression level and metadata to stamp on a packed layer image, for
/// <see cref="ImagesApi.Pack"/> and <see cref="ImagesApi.PackDifferential"/>.
/// </summary>
/// <param name="CompressionLevel">
/// zstd compression level. Accepted range is 1 to 22, or a negative
/// value for faster, lower compression. Defaults to 3.
/// </param>
/// <param name="Author">
/// Author to stamp in the image metadata. Null stamps an empty string.
/// </param>
/// <param name="Description">
/// Description to stamp in the image metadata. Null stamps an empty
/// string.
/// </param>
public sealed record ImageStampOptions(
    int CompressionLevel = 3,
    string? Author = null,
    string? Description = null);

/// <summary>One entry of a manifest listed by <see cref="ImagesApi.GetManifest"/>.</summary>
/// <param name="ImagePath">Path of the <c>.lmnt</c> image listed in the manifest.</param>
/// <param name="ChecksumHex">The image's SHA-256 checksum, hex-encoded.</param>
public sealed record ImageManifestEntry(string ImagePath, string ChecksumHex);

/// <summary>Metadata of a layer image, as returned by <see cref="ImagesApi.GetMetadata"/>.</summary>
/// <param name="Id">The image's identifier.</param>
/// <param name="ParentId">The parent image's identifier, or an empty string if there is none.</param>
/// <param name="CreatedAt">Creation timestamp, in ISO 8601 UTC.</param>
/// <param name="Author">The stamped author, or an empty string if none was stamped.</param>
/// <param name="Description">The stamped description, or an empty string if none was stamped.</param>
/// <param name="Compression">The compression type used for the image's data section.</param>
/// <param name="FileCount">Number of files recorded in the image.</param>
/// <param name="UncompressedSize">Total uncompressed size of the image's data, in bytes.</param>
/// <param name="CompressedSize">Total compressed size of the image's data, in bytes.</param>
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
