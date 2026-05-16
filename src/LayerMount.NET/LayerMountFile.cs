// LayerMountFile -- managed wrapper over an <c>LM_FILE_HANDLE</c>.
//
// Obtained from <see cref="LayerMount.OpenFile"/> and
// <see cref="LayerMount.CreateFile"/>. Exposes the handle-bound file
// primitives: Read / Write / Overwrite / Flush / GetFileInfo /
// SetFileInfo.

using System;
using LayerMount.Interop;

namespace LayerMount;

public sealed class LayerMountFile : IDisposable
{
    private readonly LayerMountFileHandle _handle;

    internal LayerMountFile(LayerMountFileHandle handle, FileInfoSnapshot initialInfo)
    {
        _handle = handle;
        Info = initialInfo;
    }

    /// <summary>The most recent file info snapshot observed for this
    /// handle. Updated by <see cref="Write"/>, <see cref="Overwrite"/>,
    /// <see cref="Flush"/>, and <see cref="SetFileInfo"/>.</summary>
    public FileInfoSnapshot Info { get; private set; }

    public bool IsClosed => _handle.IsClosed;

    /// <summary>
    /// Reads up to <paramref name="buffer"/>.Length bytes starting at
    /// <paramref name="offset"/>. Returns the number of bytes actually
    /// read (may be less than the buffer). Returns 0 at EOF.
    /// </summary>
    public unsafe uint Read(long offset, Span<byte> buffer, uint originatorPid = 0)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(offset);
        if (buffer.IsEmpty) return 0;
        using var lease = new SafeHandleLease(_handle);
        uint bytesRead = 0;
        fixed (byte* p = buffer)
        {
            int hr = NativeMethods.LayerMountReadFile(
                lease.Handle, p, (ulong)offset, (uint)buffer.Length, originatorPid, &bytesRead);
            HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountReadFile));
        }
        return bytesRead;
    }

    /// <summary>
    /// Writes the buffer starting at <paramref name="offset"/>.
    /// <paramref name="writeToEnd"/> appends regardless of offset;
    /// <paramref name="constrainedIo"/> truncates the write to fit the
    /// current EOF.
    /// </summary>
    public unsafe uint Write(
        long offset,
        ReadOnlySpan<byte> buffer,
        bool writeToEnd = false,
        bool constrainedIo = false,
        uint originatorPid = 0)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(offset);
        using var lease = new SafeHandleLease(_handle);
        uint bytesWritten = 0;
        LM_FILE_INFO info = default;
        fixed (byte* p = buffer)
        {
            int hr = NativeMethods.LayerMountWriteFile(
                lease.Handle, p, (ulong)offset, (uint)buffer.Length,
                writeToEnd ? 1 : 0, constrainedIo ? 1 : 0,
                originatorPid, &bytesWritten, &info);
            HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountWriteFile));
        }
        Info = FileInfoSnapshot.From(info);
        return bytesWritten;
    }

    /// <summary>
    /// CREATE_ALWAYS-style truncation. Replaces or ORs file attributes
    /// per <paramref name="replaceAttributes"/> and deletes non-overlay
    /// ADS streams.
    /// </summary>
    public unsafe void Overwrite(
        uint fileAttributes,
        bool replaceAttributes,
        ulong allocationSize,
        uint originatorPid = 0)
    {
        using var lease = new SafeHandleLease(_handle);
        LM_FILE_INFO info = default;
        int hr = NativeMethods.LayerMountOverwriteFile(
            lease.Handle, fileAttributes, replaceAttributes ? 1 : 0,
            allocationSize, originatorPid, &info);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountOverwriteFile));
        Info = FileInfoSnapshot.From(info);
    }

    public unsafe void Flush(uint originatorPid = 0)
    {
        using var lease = new SafeHandleLease(_handle);
        LM_FILE_INFO info = default;
        int hr = NativeMethods.LayerMountFlushFile(lease.Handle, originatorPid, &info);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountFlushFile));
        Info = FileInfoSnapshot.From(info);
    }

    public unsafe FileInfoSnapshot GetFileInfo()
    {
        using var lease = new SafeHandleLease(_handle);
        LM_FILE_INFO info = default;
        int hr = NativeMethods.LayerMountGetFileInfo(lease.Handle, &info);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountGetFileInfo));
        Info = FileInfoSnapshot.From(info);
        return Info;
    }

    /// <summary>
    /// Updates file metadata. Pass <c>UINT_MAX</c> for
    /// <paramref name="fileAttributes"/> (i.e. <c>INVALID_FILE_ATTRIBUTES</c>)
    /// to leave attributes unchanged; pass <c>0</c> for individual
    /// timestamps to leave them unchanged; pass <c>UINT64_MAX</c> for
    /// <paramref name="allocationSize"/> / <paramref name="fileSize"/>
    /// to leave sizes unchanged.
    /// </summary>
    public unsafe void SetFileInfo(
        uint fileAttributes = 0xFFFFFFFFu,
        ulong creationTime = 0,
        ulong lastAccessTime = 0,
        ulong lastWriteTime = 0,
        ulong changeTime = 0,
        ulong allocationSize = ulong.MaxValue,
        ulong fileSize = ulong.MaxValue)
    {
        using var lease = new SafeHandleLease(_handle);
        LM_FILE_INFO info = default;
        int hr = NativeMethods.LayerMountSetFileInfo(
            lease.Handle, fileAttributes, creationTime, lastAccessTime,
            lastWriteTime, changeTime, allocationSize, fileSize, &info);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountSetFileInfo));
        Info = FileInfoSnapshot.From(info);
    }

    /// <summary>
    /// Records a path-based rename that affected this handle without issuing
    /// a rename at the engine level. Hosts call this after they've observed a
    /// concurrent path-based rename (e.g. MoveFileExW renaming <c>a.txt</c>
    /// while a delete-on-close handle on <c>a.txt</c> is still open) so
    /// subsequent handle-bound operations target the current name.
    /// </summary>
    public void UpdatePathAfterRename(string newRelativePath)
    {
        ArgumentNullException.ThrowIfNull(newRelativePath);
        using var lease = new SafeHandleLease(_handle);
        int hr = NativeMethods.LayerMountUpdateOpenFilePath(lease.Handle, newRelativePath);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountUpdateOpenFilePath));
    }

    public void Dispose() => _handle.Dispose();
}

/// <summary>
/// Immutable managed projection of a native <c>LM_FILE_INFO</c>.
/// Timestamps are FILETIME (100ns ticks since 1601-01-01 UTC).
/// </summary>
public readonly struct FileInfoSnapshot
{
    public uint FileAttributes { get; }
    public uint ReparseTag { get; }
    public ulong AllocationSize { get; }
    public ulong FileSize { get; }
    public ulong CreationTime { get; }
    public ulong LastAccessTime { get; }
    public ulong LastWriteTime { get; }
    public ulong ChangeTime { get; }
    public ulong IndexNumber { get; }
    public uint HardLinks { get; }
    public uint EaSize { get; }

    internal FileInfoSnapshot(
        uint fileAttributes, uint reparseTag,
        ulong allocationSize, ulong fileSize,
        ulong creationTime, ulong lastAccessTime,
        ulong lastWriteTime, ulong changeTime,
        ulong indexNumber, uint hardLinks, uint eaSize)
    {
        FileAttributes = fileAttributes;
        ReparseTag = reparseTag;
        AllocationSize = allocationSize;
        FileSize = fileSize;
        CreationTime = creationTime;
        LastAccessTime = lastAccessTime;
        LastWriteTime = lastWriteTime;
        ChangeTime = changeTime;
        IndexNumber = indexNumber;
        HardLinks = hardLinks;
        EaSize = eaSize;
    }

    internal static FileInfoSnapshot From(LM_FILE_INFO info) => new(
        info.fileAttributes, info.reparseTag,
        info.allocationSize, info.fileSize,
        info.creationTime, info.lastAccessTime,
        info.lastWriteTime, info.changeTime,
        info.indexNumber, info.hardLinks, info.eaSize);

    public DateTime CreationTimeUtc   => FileTimeToDateTime(CreationTime);
    public DateTime LastAccessTimeUtc => FileTimeToDateTime(LastAccessTime);
    public DateTime LastWriteTimeUtc  => FileTimeToDateTime(LastWriteTime);
    public DateTime ChangeTimeUtc     => FileTimeToDateTime(ChangeTime);

    // Clamp out-of-range FILETIME values to DateTime.MinValue rather than
    // throwing from a property getter. Corrupt native metadata, a `ulong.MaxValue`
    // sentinel, or a zero-initialized field would otherwise throw
    // ArgumentOutOfRangeException from DateTime.FromFileTimeUtc and break the
    // caller's enumeration or stat callback mid-loop. MinValue is a clear
    // "unknown/invalid" marker that consumers can detect explicitly.
    private static DateTime FileTimeToDateTime(ulong fileTime)
    {
        // DateTime.FromFileTimeUtc accepts ticks in [0, DateTime.MaxValue.ToFileTimeUtc()].
        // MaxValue.ToFileTimeUtc() is 2650467743999999999 (~ 9999-12-31).
        // Treat values at or above that bound -- and the negative range after
        // signed cast -- as invalid.
        const long kMaxFileTimeUtc = 2650467743999999999L;
        if (fileTime == 0) return DateTime.MinValue;
        long signed = (long)fileTime;
        if (signed < 0 || signed > kMaxFileTimeUtc) return DateTime.MinValue;
        try
        {
            return DateTime.FromFileTimeUtc(signed);
        }
        catch (ArgumentOutOfRangeException)
        {
            // Belt-and-braces in case the .NET bound shifts in a future version.
            return DateTime.MinValue;
        }
    }
}
