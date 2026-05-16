// LayerMount.MountPoint -- managed wrappers around the host-helper mount-point
// primitives exported by LayerMount.dll (see public/LayerMount.h ::
// LayerMountPoint*). Lives inside the engine so every host adapter
// shares one path-validation + ownership-tracking implementation.
//
// All operations are stateless and host-agnostic, so they hang off
// LayerMount.MountPoint as a static class (no LM_HANDLE required).

using System;
using LayerMount.Interop;

namespace LayerMount;

public sealed partial class LayerMount
{
    /// <summary>
    /// Static helpers for validating, reserving, and releasing Windows
    /// mount-point directories. See <see cref="MountPointPrep"/> for the
    /// ownership token returned by <see cref="PrepareDirectory"/> /
    /// <see cref="CaptureIdentity"/>.
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
        /// Validates a directory mount-point and reserves the spot. The
        /// path must not already exist and must not be a reparse point;
        /// missing parent directories are created on demand. Returns a
        /// <see cref="MountPointPrep"/> token whose
        /// <see cref="MountPointPrep.DirectoryCreatedByUs"/> is TRUE so a
        /// later <see cref="ReleaseIfSafe"/> call can clean up safely.
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
            ArgumentNullException.ThrowIfNull(mountPoint);

            LM_MOUNT_POINT_PREP prep = default;
            int hr = NativeMethods.LayerMountPointCaptureIdentity(mountPoint, &prep);
            HResultGuard.ThrowIfFailed(hr,
                nameof(NativeMethods.LayerMountPointCaptureIdentity));
            return new MountPointPrep(in prep);
        }

        /// <summary>
        /// Best-effort: removes the mount-point directory iff
        /// <paramref name="prep"/>.DirectoryCreatedByUs is TRUE, the
        /// directory's current volume-serial + file-id match the captured
        /// values, and the directory is empty. Otherwise leaves the
        /// directory in place. Never fails — non-success HRESULTs are
        /// nevertheless surfaced via <see cref="LayerMountException"/> so
        /// callers can log them.
        /// </summary>
        public static unsafe void ReleaseIfSafe(string mountPoint, MountPointPrep prep)
        {
            ArgumentNullException.ThrowIfNull(mountPoint);

            LM_MOUNT_POINT_PREP native = prep.Native;
            int hr = NativeMethods.LayerMountPointReleaseIfSafe(mountPoint, &native);
            HResultGuard.ThrowIfFailed(hr,
                nameof(NativeMethods.LayerMountPointReleaseIfSafe));
        }
    }
}

/// <summary>
/// Opaque ownership token returned by <see cref="LayerMount.MountPoint.PrepareDirectory"/>
/// or <see cref="LayerMount.MountPoint.CaptureIdentity"/>. Round-trips through
/// <see cref="LayerMount.MountPoint.ReleaseIfSafe"/> byte-for-byte; the host
/// adapter holds onto it between Mount and Unmount.
/// </summary>
public unsafe readonly struct MountPointPrep
{
    private readonly LM_MOUNT_POINT_PREP _native;

    internal MountPointPrep(in LM_MOUNT_POINT_PREP native) => _native = native;

    internal LM_MOUNT_POINT_PREP Native => _native;

    /// <summary>
    /// TRUE iff this token came from <see cref="LayerMount.MountPoint.PrepareDirectory"/>
    /// (we created the directory and so are entitled to delete it on
    /// Release). FALSE for <see cref="LayerMount.MountPoint.CaptureIdentity"/>.
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
