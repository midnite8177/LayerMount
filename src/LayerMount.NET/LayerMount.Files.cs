// LayerMount -- file-path and handle-based primitive operations.
//
// Partial class hosting the file-scoped API surface that routes to the
// shared <c>LM_HANDLE</c>. Handle-bound per-file ops live on
// <see cref="LayerMountFile"/>; everything below takes a relative path.

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using LayerMount.Interop;

namespace LayerMount;

public sealed partial class LayerMount
{
    /// <summary>Opens an existing file or directory. Returns null if the
    /// path resolves to a whiteout.</summary>
    public unsafe LayerMountFile OpenFile(
        string relativePath,
        uint grantedAccess,
        uint createOptions = 0,
        uint originatorPid = 0)
    {
        ArgumentNullException.ThrowIfNull(relativePath);
        using var lease = new SafeHandleLease(_handle);
        IntPtr fileHandle;
        LM_FILE_INFO info = default;
        int hr = NativeMethods.LayerMountOpenFile(
            lease.Handle, relativePath, grantedAccess, createOptions, originatorPid,
            &fileHandle, &info);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountOpenFile));

        var safe = new LayerMountFileHandle();
        safe.SetRawHandle(fileHandle);
        RegisterChild(safe);
        return new LayerMountFile(safe, FileInfoSnapshot.From(info));
    }

    /// <summary>Creates a new file or directory.</summary>
    public unsafe LayerMountFile CreateFile(
        string relativePath,
        uint createOptions,
        uint grantedAccess,
        uint fileAttributes,
        ulong allocationSize = 0,
        ReadOnlySpan<byte> securityDescriptor = default,
        uint originatorPid = 0)
    {
        ArgumentNullException.ThrowIfNull(relativePath);
        using var lease = new SafeHandleLease(_handle);
        IntPtr fileHandle;
        LM_FILE_INFO info = default;
        fixed (byte* sdp = securityDescriptor)
        {
            int hr = NativeMethods.LayerMountCreateFile(
                lease.Handle, relativePath, createOptions, grantedAccess, fileAttributes,
                sdp, (nuint)securityDescriptor.Length, allocationSize,
                originatorPid, &fileHandle, &info);
            HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountCreateFile));
        }

        var safe = new LayerMountFileHandle();
        safe.SetRawHandle(fileHandle);
        RegisterChild(safe);
        return new LayerMountFile(safe, FileInfoSnapshot.From(info));
    }

    public void DeleteFile(string relativePath)
    {
        ArgumentNullException.ThrowIfNull(relativePath);
        using var lease = new SafeHandleLease(_handle);
        int hr = NativeMethods.LayerMountDeleteFile(lease.Handle, relativePath);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountDeleteFile));
    }

    public void CheckCanDeleteFile(string relativePath)
    {
        ArgumentNullException.ThrowIfNull(relativePath);
        using var lease = new SafeHandleLease(_handle);
        int hr = NativeMethods.LayerMountCanDeleteFile(lease.Handle, relativePath);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountCanDeleteFile));
    }

    public void RenameFile(string oldRelativePath, string newRelativePath, bool replaceIfExists = false)
    {
        ArgumentNullException.ThrowIfNull(oldRelativePath);
        ArgumentNullException.ThrowIfNull(newRelativePath);
        using var lease = new SafeHandleLease(_handle);
        int hr = NativeMethods.LayerMountRenameFile(
            lease.Handle, oldRelativePath, newRelativePath,
            replaceIfExists ? 1 : 0);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountRenameFile));
    }

    public void CreateWhiteout(string relativePath, bool isDirectory = false)
    {
        ArgumentNullException.ThrowIfNull(relativePath);
        using var lease = new SafeHandleLease(_handle);
        int hr = NativeMethods.LayerMountCreateWhiteout(
            lease.Handle, relativePath, isDirectory ? 1 : 0);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountCreateWhiteout));
    }

    public void SetOpaque(string dirRelativePath)
    {
        ArgumentNullException.ThrowIfNull(dirRelativePath);
        using var lease = new SafeHandleLease(_handle);
        int hr = NativeMethods.LayerMountSetOpaque(lease.Handle, dirRelativePath);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountSetOpaque));
    }

    // ------------------------------------------------------------------
    // Security
    // ------------------------------------------------------------------

    public unsafe (uint Attributes, byte[] SecurityDescriptor) GetSecurity(string relativePath)
    {
        ArgumentNullException.ThrowIfNull(relativePath);
        using var lease = new SafeHandleLease(_handle);
        uint attributes = 0;
        nuint required = 0;

        int hr = NativeMethods.LayerMountGetSecurity(
            lease.Handle, relativePath, &attributes, null, 0, &required);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountGetSecurity));

        byte[] sd = new byte[(int)required];
        if (required > 0)
        {
            fixed (byte* p = sd)
            {
                nuint actual = 0;
                hr = NativeMethods.LayerMountGetSecurity(
                    lease.Handle, relativePath, &attributes, p, required, &actual);
                HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountGetSecurity));
            }
        }
        return (attributes, sd);
    }

    public unsafe void SetSecurity(
        string relativePath,
        uint securityInformation,
        ReadOnlySpan<byte> modificationDescriptor)
    {
        ArgumentNullException.ThrowIfNull(relativePath);
        using var lease = new SafeHandleLease(_handle);
        fixed (byte* p = modificationDescriptor)
        {
            int hr = NativeMethods.LayerMountSetSecurity(
                lease.Handle, relativePath, securityInformation,
                p, (nuint)modificationDescriptor.Length);
            HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountSetSecurity));
        }
    }

    // ------------------------------------------------------------------
    // Reparse points
    // ------------------------------------------------------------------

    public unsafe byte[] GetReparsePoint(string relativePath)
    {
        ArgumentNullException.ThrowIfNull(relativePath);
        using var lease = new SafeHandleLease(_handle);
        nuint required = 0;
        int hr = NativeMethods.LayerMountGetReparsePoint(lease.Handle, relativePath, null, 0, &required);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountGetReparsePoint));

        byte[] buffer = new byte[(int)required];
        if (required > 0)
        {
            fixed (byte* p = buffer)
            {
                nuint actual = 0;
                hr = NativeMethods.LayerMountGetReparsePoint(lease.Handle, relativePath, p, required, &actual);
                HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountGetReparsePoint));
            }
        }
        return buffer;
    }

    public unsafe void SetReparsePoint(string relativePath, ReadOnlySpan<byte> buffer)
    {
        ArgumentNullException.ThrowIfNull(relativePath);
        using var lease = new SafeHandleLease(_handle);
        fixed (byte* p = buffer)
        {
            int hr = NativeMethods.LayerMountSetReparsePoint(
                lease.Handle, relativePath, p, (nuint)buffer.Length);
            HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountSetReparsePoint));
        }
    }

    public unsafe void DeleteReparsePoint(string relativePath, ReadOnlySpan<byte> buffer)
    {
        ArgumentNullException.ThrowIfNull(relativePath);
        using var lease = new SafeHandleLease(_handle);
        fixed (byte* p = buffer)
        {
            int hr = NativeMethods.LayerMountDeleteReparsePoint(
                lease.Handle, relativePath, p, (nuint)buffer.Length);
            HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountDeleteReparsePoint));
        }
    }

    // ------------------------------------------------------------------
    // Merge directory
    // ------------------------------------------------------------------

    /// <summary>
    /// Merged view of a directory across the upper + lower layers. The
    /// callback fires once per entry in sorted order. Return
    /// <c>false</c> to abort enumeration.
    /// </summary>
    public unsafe void MergeDirectory(
        string dirRelativePath,
        Func<string, FileInfoSnapshot, bool> callback)
    {
        ArgumentNullException.ThrowIfNull(dirRelativePath);
        ArgumentNullException.ThrowIfNull(callback);

        // Pass the managed callback through a GCHandle in userContext; the
        // static [UnmanagedCallersOnly] trampoline below unwraps it.
        var gc = GCHandle.Alloc(callback, GCHandleType.Normal);
        try
        {
            using var lease = new SafeHandleLease(_handle);
            delegate* unmanaged[Stdcall]<char*, LM_FILE_INFO*, void*, int> trampoline =
                &MergeDirectoryTrampoline;
            int hr = NativeMethods.LayerMountMergeDirectory(
                lease.Handle, dirRelativePath, trampoline,
                (void*)GCHandle.ToIntPtr(gc));
            HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountMergeDirectory));
        }
        finally
        {
            if (gc.IsAllocated) gc.Free();
        }
    }

    /// <summary>
    /// Returns the named data streams attached to the file or directory
    /// at <paramref name="relativePath"/>. The main unnamed stream
    /// (<c>::$DATA</c>) and LayerMount's reserved metadata streams are
    /// filtered out — callers see only user-visible ADS in NTFS's
    /// native <c>:name:$DATA</c> form.
    /// </summary>
    /// <returns>
    /// An empty array if the path exists but carries no user-visible
    /// streams.
    /// </returns>
    /// <exception cref="LayerMountException">
    /// If the underlying native call returns a non-success HRESULT
    /// (most commonly file-not-found). Concurrent stream additions
    /// between the size probe and the fill call are retried up to
    /// <see cref="MaxStreamRetries"/> times before surfacing
    /// <c>ERROR_MORE_DATA</c>.
    /// </exception>
    public unsafe StreamInfo[] EnumerateStreams(string relativePath)
    {
        ArgumentNullException.ThrowIfNull(relativePath);
        using var lease = new SafeHandleLease(_handle);

        // Probe-then-fill against a possibly-changing stream set. A new
        // stream added between the probe and the fill makes the native
        // side return ERROR_MORE_DATA -- treat that as transient and
        // re-probe instead of throwing. Bounded so pathological churn
        // surfaces as a real failure rather than spinning forever.
        for (int attempt = 0; attempt < MaxStreamRetries; attempt++)
        {
            uint required = 0;
            int hrProbe = NativeMethods.LayerMountEnumerateStreams(
                lease.Handle, relativePath, null, 0, &required);
            HResultGuard.ThrowIfFailed(hrProbe, nameof(NativeMethods.LayerMountEnumerateStreams));

            if (required == 0)
            {
                return [];
            }

            LM_STREAM_INFO[] buffer = new LM_STREAM_INFO[required];
            uint written = 0;
            int hrFill;
            fixed (LM_STREAM_INFO* p = buffer)
            {
                hrFill = NativeMethods.LayerMountEnumerateStreams(
                    lease.Handle, relativePath, p, required, &written);
            }

            if (hrFill == HRESULT_E_MORE_DATA && attempt < MaxStreamRetries - 1)
            {
                // A stream was added between probe and fill. Re-probe.
                continue;
            }
            HResultGuard.ThrowIfFailed(hrFill, nameof(NativeMethods.LayerMountEnumerateStreams));

            StreamInfo[] result = new StreamInfo[written];
            for (uint i = 0; i < written; i++)
            {
                fixed (char* namePtr = buffer[i].streamName)
                {
                    string name = new(namePtr);
                    result[i] = new StreamInfo(
                        name,
                        buffer[i].streamSize,
                        buffer[i].allocationSize);
                }
            }
            return result;
        }

        // Exhausted retries: streams keep being added faster than we can
        // probe + allocate. Surface as a real ERROR_MORE_DATA.
        HResultGuard.ThrowIfFailed(HRESULT_E_MORE_DATA, nameof(NativeMethods.LayerMountEnumerateStreams));
        return [];
    }

    // Match BufferHelpers.MaxFillRetries and HRESULT_E_MORE_DATA so the
    // race-handling shape is uniform across the wrapper.
    private const int MaxStreamRetries = 5;
    private const int HRESULT_E_MORE_DATA = unchecked((int)0x800700EA);

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvStdcall) })]
    private static unsafe int MergeDirectoryTrampoline(
        char* name, LM_FILE_INFO* info, void* userContext)
    {
        try
        {
            if (userContext == null || name == null || info == null) return 0;
            var gc = GCHandle.FromIntPtr((IntPtr)userContext);
            if (!gc.IsAllocated) return 0;
            if (gc.Target is not Func<string, FileInfoSnapshot, bool> callback) return 0;

            string entryName = Marshal.PtrToStringUni((IntPtr)name) ?? string.Empty;
            bool cont = callback(entryName, FileInfoSnapshot.From(*info));
            // Return HRESULT; S_OK = keep going, E_ABORT = stop.
            return cont ? 0 : unchecked((int)0x80004004u); // E_ABORT
        }
        catch
        {
            return unchecked((int)0x80004005u); // E_FAIL
        }
    }
}
