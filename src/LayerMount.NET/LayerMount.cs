// LayerMount -- top-level managed wrapper over an <c>LM_HANDLE</c>.
//
// Constructed via <see cref="Create"/>. Owns the underlying SafeHandle,
// manages the native event-callback subscription, and exposes the four
// subsystem APIs (<see cref="ProcessTracker"/>, <see cref="Vhd"/>,
// <see cref="Vss"/>, <see cref="Images"/>) as sub-objects that internally
// route to the parent overlay handle.
//
// Dispose contract: consumers should call <see cref="Dispose"/> (e.g.
// via <c>using</c>) to release the native instance deterministically.
// If Dispose is skipped the finalizer makes a best-effort cleanup pass:
// it clears the native event-callback slot (which drains any in-flight
// native emits via EventEmitter::Clear) and frees the self GCHandle, so
// the contained SafeHandle can then release the native overlay. The
// GCHandle is Weak rather than Normal -- a strong handle would root
// the LayerMount to itself and block collection forever. The event
// trampoline checks GCHandle.Target for null before dispatching, so
// a collected-but-not-yet-finalized LayerMount silently drops events.

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using LayerMount.Interop;

namespace LayerMount;

public sealed partial class LayerMount : IDisposable
{
    private readonly LayerMountHandle _handle;
    private GCHandle _selfGc;
    private bool _disposed;

    // Child registry: weak refs to the SafeHandle wrappers we produced via
    // OpenFile/CreateFile/Vhd.Create/Vhd.Open/Vss.CreateSnapshot/Images.*.
    // Walked and disposed in DisposeCore so a caller who forgot to close a
    // child does not strand its native handle-table slot. Weak so child
    // wrappers remain collectible during the LayerMount's lifetime.
    private readonly object _childrenLock = new();
    private List<WeakReference<SafeHandle>>? _children = new();

    /// <summary>
    /// Fires when the native engine emits a warning, copy-up, whiteout, or
    /// access-denied event (FR-31). Delivered on an arbitrary DLL-internal
    /// thread; handlers must not throw or call back into the DLL
    /// synchronously on the same overlay.
    /// </summary>
    public event EventHandler<LayerMountEventArgs>? Event;

    /// <summary>Process-tracking subsystem (FR-32).</summary>
    public ProcessTrackerApi ProcessTracker { get; }

    /// <summary>VHD/VHDX primitives (FR-23..FR-25, FR-35).</summary>
    public VhdApi Vhd { get; }

    /// <summary>VSS snapshot primitives (FR-26, FR-27).</summary>
    public VssApi Vss { get; }

    /// <summary>Layer image (.lmnt) primitives (FR-28, FR-35).</summary>
    public ImagesApi Images { get; }

    private unsafe LayerMount(LayerMountHandle handle)
    {
        _handle = handle;
        // Weak so the LayerMount can be collected even if the caller
        // forgets Dispose. EventTrampoline tolerates a null Target.
        _selfGc = GCHandle.Alloc(this, GCHandleType.Weak);

        // Register the native event trampoline. Failure leaves the
        // overlay usable without events; caller is informed via exception.
        delegate* unmanaged[Stdcall]<LM_EVENT*, void*, void> trampoline =
            &EventTrampoline.NativeEventCallback;
        using (var lease = new SafeHandleLease(handle))
        {
            int hr = NativeMethods.LayerMountSetEventCallback(
                lease.Handle,
                trampoline,
                (void*)GCHandle.ToIntPtr(_selfGc));
            if (hr < 0)
            {
                _selfGc.Free();
                handle.Dispose();
                HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountSetEventCallback));
            }
        }

        ProcessTracker = new ProcessTrackerApi(this);
        Vhd = new VhdApi(this);
        Vss = new VssApi(this);
        Images = new ImagesApi(this);
    }

    internal LayerMountHandle Handle => _handle;

    // Called by factory methods (OpenFile/CreateFile/Vhd.Create/Vss.Create/
    // Images.Pack/etc.) immediately after a successful native creation, so
    // the LayerMount can deterministically close still-live children when its
    // own Dispose runs. If the LayerMount has already been disposed by the
    // time registration reaches us (concurrent-Dispose race), we close the
    // orphaned child here so its native slot does not strand, then throw
    // ObjectDisposedException -- the caller's factory call completed
    // natively but the overlay is no longer a valid parent.
    internal void RegisterChild(SafeHandle child)
    {
        lock (_childrenLock)
        {
            if (_disposed || _children is null)
            {
                child.Dispose();
                throw new ObjectDisposedException(nameof(LayerMount));
            }
            _children.Add(new WeakReference<SafeHandle>(child));
        }
    }

    internal void RaiseEvent(LayerMountEventArgs args)
    {
        try { Event?.Invoke(this, args); }
        catch { /* host handler owns its own errors */ }
    }

    // ------------------------------------------------------------------
    // Static helpers
    // ------------------------------------------------------------------

    /// <summary>
    /// Translates a native LayerMount HRESULT to the NTSTATUS the host
    /// should report to the kernel / filesystem filter. Mirrors the
    /// native <c>LayerMountHResultToNtStatus</c> export -- use this rather
    /// than a hand-rolled table so managed hosts pick up new mappings
    /// (including NT-facility HRESULTs like STATUS_DIRECTORY_NOT_EMPTY
    /// or STATUS_OBJECT_NAME_COLLISION) as the engine's ABI evolves.
    /// </summary>
    public static unsafe int HResultToNtStatus(int hr)
    {
        int status = 0;
        NativeMethods.LayerMountHResultToNtStatus(hr, &status);
        return status;
    }

    /// <summary>
    /// Returns the loaded DLL's version triple plus ABI version. Does not
    /// require an overlay instance.
    /// </summary>
    public static unsafe (uint Major, uint Minor, uint Patch, uint AbiVersion) GetVersion()
    {
        uint major = 0, minor = 0, patch = 0, abi = 0;
        int hr = NativeMethods.LayerMountGetVersion(&major, &minor, &patch, &abi);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountGetVersion));
        return (major, minor, patch, abi);
    }

    /// <summary>
    /// Short-lived overlay for CLI subcommands (vhd / vss / layer / image)
    /// that need a valid <c>LM_HANDLE</c> to drive the engine's primitives
    /// without mounting a filesystem. Equivalent to <see cref="Create"/>
    /// with upperPath = workDir, no lower layers, no process tracking.
    /// The default capability set covers the features typical host adapters
    /// expose; callers can override per call.
    /// </summary>
    public static unsafe LayerMount CreateTransient(
        string workDir,
        HostCapabilities caps = HostCapabilities.Ads
                              | HostCapabilities.ReparsePoints
                              | HostCapabilities.SparseFiles
                              | HostCapabilities.MultipleStreams
                              | HostCapabilities.NtfsAcls)
    {
        ArgumentException.ThrowIfNullOrEmpty(workDir);

        IntPtr rawHandle;
        int hr = NativeMethods.LayerMountCreateTransient(workDir, (uint)caps, &rawHandle);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountCreateTransient));

        var safe = new LayerMountHandle();
        safe.SetRawHandle(rawHandle);
        return new LayerMount(safe);
    }

    public static unsafe LayerMount Create(LayerMountConfig config)
    {
        ArgumentNullException.ThrowIfNull(config);
        if (string.IsNullOrEmpty(config.UpperPath))
            throw new ArgumentException("UpperPath is required.", nameof(config));
        if (string.IsNullOrEmpty(config.WorkDirPath))
            throw new ArgumentException("WorkDirPath is required.", nameof(config));

        IntPtr upperPtr = Marshal.StringToCoTaskMemUni(config.UpperPath);
        IntPtr workPtr  = Marshal.StringToCoTaskMemUni(config.WorkDirPath);
        IntPtr rulesPtr = !string.IsNullOrEmpty(config.ProcessRulesPath)
            ? Marshal.StringToCoTaskMemUni(config.ProcessRulesPath)
            : IntPtr.Zero;

        int lowerCount = config.LowerPaths.Count;
        IntPtr[] lowerStringPtrs = new IntPtr[lowerCount];
        IntPtr lowerArray = IntPtr.Zero;

        try
        {
            for (int i = 0; i < lowerCount; i++)
                lowerStringPtrs[i] = Marshal.StringToCoTaskMemUni(config.LowerPaths[i]);

            if (lowerCount > 0)
            {
                lowerArray = Marshal.AllocCoTaskMem(IntPtr.Size * lowerCount);
                for (int i = 0; i < lowerCount; i++)
                    Marshal.WriteIntPtr(lowerArray, i * IntPtr.Size, lowerStringPtrs[i]);
            }

            LM_CONFIG native = default;
            native.structSize            = (uint)sizeof(LM_CONFIG);
            native.abiVersion            = 1u;
            native.hostCapabilities      = (uint)config.Capabilities;
            native.accessLogCapacity     = config.AccessLogCapacity;
            native.pathCacheCapacity     = config.PathCacheCapacity;
            native.enableProcessTracking = config.EnableProcessTracking ? 1 : 0;
            native.lowerPathCount        = (uint)lowerCount;
            native.upperPath             = upperPtr;
            native.workDirPath           = workPtr;
            native.processRulesPath      = rulesPtr;
            native.lowerPaths            = lowerArray;

            IntPtr rawHandle;
            int hr = NativeMethods.LayerMountCreate(&native, &rawHandle);
            HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountCreate));

            var safe = new LayerMountHandle();
            safe.SetRawHandle(rawHandle);
            return new LayerMount(safe);
        }
        finally
        {
            if (upperPtr != IntPtr.Zero) Marshal.FreeCoTaskMem(upperPtr);
            if (workPtr  != IntPtr.Zero) Marshal.FreeCoTaskMem(workPtr);
            if (rulesPtr != IntPtr.Zero) Marshal.FreeCoTaskMem(rulesPtr);
            for (int i = 0; i < lowerCount; i++)
                if (lowerStringPtrs[i] != IntPtr.Zero)
                    Marshal.FreeCoTaskMem(lowerStringPtrs[i]);
            if (lowerArray != IntPtr.Zero) Marshal.FreeCoTaskMem(lowerArray);
        }
    }

    // ------------------------------------------------------------------
    // Path / volume / stats
    // ------------------------------------------------------------------

    /// <summary>
    /// Resolves a relative path to its absolute location inside the
    /// upper/lower layer that services it. Returns layer-source metadata
    /// alongside the absolute path.
    /// </summary>
    public unsafe ResolvedPath ResolvePath(string relativePath)
    {
        ArgumentNullException.ThrowIfNull(relativePath);

        using var lease = new SafeHandleLease(_handle);

        // First probe for required size, then allocate and probe again.
        // Use the struct-based two-call pattern: pass absolutePath=null on
        // the sizing call, then fill and call again.
        LM_RESOLVED_PATH probe = default;
        probe.absolutePath = IntPtr.Zero;
        probe.absolutePathChars = 0;
        int hr = NativeMethods.LayerMountResolvePath(lease.Handle, relativePath, &probe);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountResolvePath));

        nuint required = probe.absolutePathRequired;
        string absolutePath = string.Empty;
        if (required > 1)
        {
            int charCount = checked((int)required);
            char[] buffer = new char[charCount];
            fixed (char* bp = buffer)
            {
                LM_RESOLVED_PATH call = probe;
                call.absolutePath = (IntPtr)bp;
                call.absolutePathChars = required;
                hr = NativeMethods.LayerMountResolvePath(lease.Handle, relativePath, &call);
                HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountResolvePath));
                absolutePath = new string(buffer, 0, charCount - 1);
                probe = call;
            }
        }

        return new ResolvedPath(
            absolutePath,
            (LayerSource)probe.source,
            probe.lowerIndex,
            probe.isWhiteout != 0,
            probe.attributes);
    }

    public unsafe VolumeInfo GetVolumeInfo()
    {
        using var lease = new SafeHandleLease(_handle);
        LM_VOLUME_INFO info = default;
        int hr = NativeMethods.LayerMountGetVolumeInfo(lease.Handle, &info);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountGetVolumeInfo));

        string label;
        int labelChars = (int)(info.volumeLabelLength / sizeof(char));
        if (labelChars <= 0)
        {
            label = string.Empty;
        }
        else
        {
            label = new string(info.volumeLabel, 0, labelChars);
        }

        return new VolumeInfo(info.totalSize, info.freeSize, label);
    }

    public void EnsureInUpperLayer(string relativePath)
    {
        ArgumentNullException.ThrowIfNull(relativePath);
        using var lease = new SafeHandleLease(_handle);
        int hr = NativeMethods.LayerMountEnsureInUpperLayer(lease.Handle, relativePath);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountEnsureInUpperLayer));
    }

    public unsafe LayerMountStats GetStats()
    {
        using var lease = new SafeHandleLease(_handle);
        LM_STATS stats = default;
        int hr = NativeMethods.LayerMountGetStats(lease.Handle, &stats);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountGetStats));
        return new LayerMountStats(
            stats.cacheHits, stats.cacheMisses, stats.copyUpCount,
            stats.readCount, stats.writeCount, stats.activeHandles,
            stats.bytesRead, stats.bytesWritten,
            stats.cleanupMetadataFailureCount);
    }

    /// <summary>
    /// Mark this overlay as "attached" to a host adapter (FR-13). While
    /// the flag is TRUE, <see cref="Dispose"/> refuses to release the
    /// instance and throws <see cref="LayerMountException"/>. General
    /// consumers should leave this untouched; host adapters toggle it
    /// around Mount/Unmount.
    /// </summary>
    public void SetHostAttached(bool attached)
    {
        using var lease = new SafeHandleLease(_handle);
        int hr = NativeMethods.LayerMountSetHostAttached(lease.Handle, attached ? 1 : 0);
        HResultGuard.ThrowIfFailed(hr, nameof(NativeMethods.LayerMountSetHostAttached));
    }

    // ------------------------------------------------------------------
    // Disposal
    // ------------------------------------------------------------------

    /// <summary>
    /// Deterministically releases this overlay and any open child handles.
    /// </summary>
    /// <remarks>
    /// On <c>Dispose</c>, closes in order:
    /// <list type="number">
    ///   <item>Native event callback (drains in-flight emits).</item>
    ///   <item>Any still-live <see cref="LayerMountFile"/>, <see cref="VssSnapshot"/>,
    ///         <see cref="VhdImage"/>, and <see cref="LayerImage"/> this overlay
    ///         produced, in that order. Children the caller already disposed
    ///         are skipped; children still held by the caller are closed on
    ///         the caller's behalf and subsequent use throws
    ///         <see cref="LayerMountInvalidHandleException"/>.</item>
    ///   <item>The native overlay instance.</item>
    /// </list>
    /// Deterministic child closure only occurs on explicit <c>Dispose</c>.
    /// If the <see cref="LayerMount"/> is collected without being disposed,
    /// each child <see cref="SafeHandle"/>'s critical finalizer still
    /// releases its own native slot, but the order is undefined. A factory
    /// call (<see cref="OpenFile"/>, <see cref="CreateFile"/>,
    /// <c>Vhd.Create</c>, etc.) that races with <c>Dispose</c> may observe
    /// <see cref="ObjectDisposedException"/> even when the native creation
    /// succeeded; the child's native slot is reclaimed before the throw.
    ///
    /// <para>
    /// If a host adapter is still attached when <c>Dispose</c> runs, the
    /// native release fails with <c>E_ILLEGAL_METHOD_CALL</c> (FR-13) and
    /// the SafeHandle's <c>ReleaseHandle</c> diagnostics surface the leak.
    /// The wrapper does not silently force-clear the host-attached flag;
    /// host adapters must run their unmount sequence (which clears the
    /// flag) before <c>Dispose</c>. This preserves the invariant that an
    /// overlay still serving live host callbacks cannot be torn down.
    /// </para>
    /// </remarks>
    public unsafe void Dispose()
    {
        DisposeCore(fromFinalizer: false);
        GC.SuppressFinalize(this);
    }

    ~LayerMount()
    {
        // Best-effort cleanup when the caller forgot Dispose. Clearing
        // the native callback drains any in-flight Emit through
        // EventEmitter::Clear so we can safely free the GCHandle
        // afterward. The SafeHandle's own finalizer runs after ours and
        // releases the native LM_HANDLE.
        DisposeCore(fromFinalizer: true);
    }

    private unsafe void DisposeCore(bool fromFinalizer)
    {
        if (_disposed) return;
        _disposed = true;

        // Clear the native callback slot first. LayerMountSetEventCallback(null)
        // invokes EventEmitter::Clear which drains concurrent Emits and
        // returns only after no in-flight trampoline calls remain, so it
        // is then safe to free the GCHandle.
        try
        {
            if (!_handle.IsInvalid && !_handle.IsClosed)
            {
                // Cannot use SafeHandleLease from the finalizer path
                // safely -- DangerousAddRef on a handle that is itself
                // being finalized risks throwing. Call directly; the
                // SafeHandle's own finalizer runs after ours.
                NativeMethods.LayerMountSetEventCallback(
                    _handle.DangerousGetHandle(), null, null);
            }
        }
        catch
        {
            if (!fromFinalizer) throw;
            // Finalizers must never propagate exceptions.
        }

        // Detach the child registry under lock, then walk outside it so
        // a child's Dispose path can safely re-enter RegisterChild (which
        // now sees _disposed and closes+throws). Skipped from the
        // finalizer: the child SafeHandles have their own critical
        // finalizers; touching them from our regular finalizer would
        // race their ReleaseHandle for a double native close.
        List<WeakReference<SafeHandle>>? childSnapshot;
        lock (_childrenLock)
        {
            childSnapshot = _children;
            _children = null;
        }

        if (!fromFinalizer && childSnapshot is not null)
        {
            // Type-ordered: files first so VSS/VHD teardown does not trip
            // over pinned file handles; VSS before VHD because a snapshot
            // can sit on a VHD-backed volume; images last (output
            // artifacts, no cross-child dependency). The ordering is
            // preserved even for children that don't interact today, so a
            // future child type with an inter-child dependency slots in
            // without a silent ordering bug.
            DisposeChildrenOfType<LayerMountFileHandle>(childSnapshot);
            DisposeChildrenOfType<VssSnapshotHandle>(childSnapshot);
            DisposeChildrenOfType<VhdHandle>(childSnapshot);
            DisposeChildrenOfType<ImageHandle>(childSnapshot);
        }

        // The host-attached flag is intentionally NOT force-cleared here.
        // FR-13 makes the flag a hard invariant: while a host adapter is
        // attached, LayerMountDestroy must refuse the release. Silently
        // clearing the flag from generic disposal would let a caller (or
        // a using-block unwind) tear down an overlay that is still serving
        // live host-adapter callbacks, which is exactly the use-after-free
        // the flag exists to prevent. If a caller disposes an overlay
        // whose host adapter was never unmounted, the underlying
        // ReleaseHandle returns E_ILLEGAL_METHOD_CALL and the leak is
        // surfaced via SafeHandle release diagnostics rather than papered
        // over.

        if (_selfGc.IsAllocated) _selfGc.Free();

        if (!fromFinalizer)
        {
            _handle.Dispose();
        }
        // From a finalizer, the SafeHandle's own critical finalizer
        // will release the native handle. Calling Dispose here would
        // double-release if it races with finalization.
    }

    private static void DisposeChildrenOfType<T>(
        List<WeakReference<SafeHandle>> snapshot) where T : SafeHandle
    {
        foreach (var weak in snapshot)
        {
            if (!weak.TryGetTarget(out var child))
            {
                continue;
            }
            if (child is not T)
            {
                continue;
            }
            try
            {
                child.Dispose();
            }
            catch
            {
                // SafeHandle.Dispose should not throw; native HR<0 is
                // already logged via ReleaseDiagnostics. Swallow so one
                // bad child does not block the rest of the walk.
            }
        }
    }
}

// ----------------------------------------------------------------------
// Projection types for GetVolumeInfo / ResolvePath / GetStats
// ----------------------------------------------------------------------

public sealed record ResolvedPath(
    string AbsolutePath,
    LayerSource Source,
    int LowerIndex,
    bool IsWhiteout,
    uint Attributes);

public sealed record VolumeInfo(
    ulong TotalSize,
    ulong FreeSize,
    string VolumeLabel);

public sealed record LayerMountStats(
    ulong CacheHits,
    ulong CacheMisses,
    ulong CopyUpCount,
    ulong ReadCount,
    ulong WriteCount,
    ulong ActiveHandles,
    ulong BytesRead,
    ulong BytesWritten,
    ulong CleanupMetadataFailureCount);
