using System;
using LayerMount.Interop;

namespace LayerMount;

public sealed partial class LayerMount
{
    /// <summary>
    /// Static helpers for validating, reserving, and releasing Windows
    /// mount-point directories. See <see cref="MountPointPrep"/> for the
    /// ownership token returned by <see cref="PrepareDirectory"/>,
    /// <see cref="CaptureIdentity"/> and <see cref="ClaimAfterMount"/>.
    /// </summary>
    public static class MountPoint
    {
        /// <summary>
        /// Returns true iff <paramref name="mountPoint"/> is a drive-letter
        /// form ("X:" or "X:\"). Used by host adapters to choose between
        /// the drive-letter mount path and the directory mount path.
        /// </summary>
        public static unsafe bool IsDriveLetter(string mountPoint)
        {
            ArgumentNullException.ThrowIfNull(mountPoint);

            int result = 0;
            int hr = NativeMethods.LayerMountPointIsDriveLetter(mountPoint, &result);
            HResultGuard.ThrowIfFailed(hr,
                nameof(NativeMethods.LayerMountPointIsDriveLetter));
            return result != 0;
        }

        /// <summary>
        /// Validates a directory mount-point. The path must not already
        /// exist and must not be a reparse point; missing parent
        /// directories are created on demand. It does not create the leaf,
        /// because some host adapters cannot mount on a directory that
        /// already exists. Returns a
        /// <see cref="MountPointPrep"/> token whose
        /// <see cref="MountPointPrep.DirectoryCreatedByUs"/> is FALSE.
        /// </summary>
        /// <exception cref="LayerMountException">
        /// Thrown when the path already exists, is a reparse point, has an
        /// uncreatable parent, or fails any other native validation. The
        /// HRESULT is preserved on <see cref="Exception.HResult"/>.
        /// </exception>
        public static unsafe MountPointPrep PrepareDirectory(string mountPoint)
        {
            ArgumentNullException.ThrowIfNull(mountPoint);

            LM_MOUNT_POINT_PREP prep = default;
            int hr = NativeMethods.LayerMountPointPrepareDirectory(mountPoint, &prep);
            HResultGuard.ThrowIfFailed(hr,
                nameof(NativeMethods.LayerMountPointPrepareDirectory));
            return new MountPointPrep(in prep);
        }

        /// <summary>
        /// Captures the volume-serial + file-id of an existing mount-point
        /// directory. Returns a token whose
        /// <see cref="MountPointPrep.DirectoryCreatedByUs"/> is FALSE so a
        /// subsequent <see cref="ReleaseIfSafe"/> call leaves the directory
        /// in place even if it happens to be empty. Native semantics: if
        /// the directory cannot be opened the captured identity stays zero,
        /// which makes the matching ReleaseIfSafe call a no-op as well.
        /// </summary>
        public static unsafe MountPointPrep CaptureIdentity(string mountPoint)
        {
            LM_MOUNT_POINT_PREP prep = CaptureNative(mountPoint);
            return new MountPointPrep(in prep);
        }

        /// <summary>
        /// Captures the identity of the mount-point directory and claims
        /// ownership of it. A host adapter calls this after its mount call
        /// succeeds, because a successful mount means the directory is new.
        /// A claim before the mount would let another process put its own
        /// directory at the path before the mount, and the claim would then
        /// cover that directory. Call this only for a path that
        /// <see cref="PrepareDirectory"/> validated as absent: a later
        /// <see cref="ReleaseIfSafe"/> deletes a claimed directory if it is
        /// empty, also when the caller did not create it.
        /// The claim needs a nonzero volume serial. If the native capture
        /// cannot open the directory, the identity stays zero, the returned
        /// token has <see cref="MountPointPrep.DirectoryCreatedByUs"/> FALSE,
        /// and a later <see cref="ReleaseIfSafe"/> leaves the directory in place.
        /// A drive-letter mount point gets the same unclaimed token with a
        /// zero identity, because it has no directory to release.
        /// </summary>
        /// <exception cref="LayerMountException">
        /// Thrown when the native capture fails. The HRESULT is preserved on
        /// <see cref="Exception.HResult"/>.
        /// </exception>
        public static unsafe MountPointPrep ClaimAfterMount(string mountPoint)
        {
            if (IsDriveLetter(mountPoint))
            {
                return default;
            }

            LM_MOUNT_POINT_PREP prep = CaptureNative(mountPoint);
            if (prep.volumeSerial != 0)
            {
                prep.directoryCreatedByUs = 1;
            }
            return new MountPointPrep(in prep);
        }

        /// <summary>
        /// Removes the mount-point directory iff
        /// <paramref name="prep"/>.DirectoryCreatedByUs is TRUE, the
        /// directory's current volume-serial + file-id match the captured
        /// values, and the directory is empty. Otherwise leaves the
        /// directory in place. Throws <see cref="LayerMountException"/>
        /// when the directory cannot be opened, read, or removed.
        /// </summary>
        public static unsafe void ReleaseIfSafe(string mountPoint, MountPointPrep prep)
        {
            ArgumentNullException.ThrowIfNull(mountPoint);

            LM_MOUNT_POINT_PREP native = prep.Native;
            int hr = NativeMethods.LayerMountPointReleaseIfSafe(mountPoint, &native);
            HResultGuard.ThrowIfFailed(hr,
                nameof(NativeMethods.LayerMountPointReleaseIfSafe));
        }

        private static unsafe LM_MOUNT_POINT_PREP CaptureNative(string mountPoint)
        {
            ArgumentNullException.ThrowIfNull(mountPoint);

            LM_MOUNT_POINT_PREP prep = default;
            int hr = NativeMethods.LayerMountPointCaptureIdentity(mountPoint, &prep);
            HResultGuard.ThrowIfFailed(hr,
                nameof(NativeMethods.LayerMountPointCaptureIdentity));
            return prep;
        }
    }
}

/// <summary>
/// Opaque ownership token returned by <see cref="LayerMount.MountPoint.PrepareDirectory"/>,
/// <see cref="LayerMount.MountPoint.CaptureIdentity"/> or
/// <see cref="LayerMount.MountPoint.ClaimAfterMount"/>. Round-trips through
/// <see cref="LayerMount.MountPoint.ReleaseIfSafe"/> byte-for-byte; the host
/// adapter holds onto it between Mount and Unmount.
/// </summary>
public unsafe readonly struct MountPointPrep
{
    private readonly LM_MOUNT_POINT_PREP _native;

    internal MountPointPrep(in LM_MOUNT_POINT_PREP native) => _native = native;

    internal LM_MOUNT_POINT_PREP Native => _native;

    /// <summary>
    /// TRUE when the host adapter claimed ownership of the directory through
    /// <see cref="LayerMount.MountPoint.ClaimAfterMount"/> after its mount
    /// call succeeded. <see cref="LayerMount.MountPoint.PrepareDirectory"/>
    /// and <see cref="LayerMount.MountPoint.CaptureIdentity"/> return FALSE.
    /// </summary>
    public bool DirectoryCreatedByUs => _native.directoryCreatedByUs != 0;

    /// <summary>FILE_ID_INFO::VolumeSerialNumber of the mount-point directory.</summary>
    public ulong VolumeSerial => _native.volumeSerial;

    /// <summary>
    /// Returns a 16-byte copy of the FILE_ID_128 captured at prep time.
    /// Returned as a fresh array each call so callers can hold onto the
    /// bytes without lifetime concerns.
    /// </summary>
    public byte[] GetFileId()
    {
        var copy = new byte[16];
        fixed (byte* src = _native.fileId)
        {
            for (int i = 0; i < 16; i++) copy[i] = src[i];
        }
        return copy;
    }
}
