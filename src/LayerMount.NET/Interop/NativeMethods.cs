// P/Invoke declarations for LayerMount.dll.
//
// Every entry mirrors a prototype in src/LayerMount.dll/public/LayerMount.h and
// a symbol in public/LayerMount.def. Declared with [LibraryImport] so the
// marshalling code is source-generated at compile time (AOT / trim
// safe). Calling convention is __stdcall (LM_CALL), applied via
// [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])].
//
// Convention:
//   - Handles are raw IntPtr; the managed SafeHandle layer sits above.
//   - PCWSTR input params are `string` (marshalled UTF-16, caller-owned
//     temp pinning handled by the source generator).
//   - PWSTR output buffers are `char*` + `nuint` capacity + `nuint*`
//     required, reflecting the two-call pattern.
//   - BYTE* buffers are `byte*` (raw).
//   - BOOL is `int` (0/1) for struct-field-equivalent blittability.
//   - SIZE_T is `nuint`.
//   - Callbacks are `delegate* unmanaged[Stdcall]<...>` function pointers
//     (AOT-safe; no marshal.GetFunctionPointerForDelegate calls).
//
// All methods in this file are internal.

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace LayerMount.Interop;

internal static unsafe partial class NativeMethods
{
    internal const string DllName = "LayerMount.dll";

    // --------------------------------------------------------------------
    // Lifecycle / diagnostics-lite
    // --------------------------------------------------------------------

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountGetVersion(
        uint* major, uint* minor, uint* patch, uint* abiVersion);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountGetLastErrorMessage(
        int hr, char* buffer, nuint bufferChars, nuint* requiredChars);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountCreate(
        LM_CONFIG* config, IntPtr* outHandle);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountCreateTransient(
        string workDir, uint hostCapabilities, IntPtr* outHandle);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountDestroy(IntPtr handle);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountHResultToNtStatus(int hr, int* outStatus);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountSetEventCallback(
        IntPtr handle,
        delegate* unmanaged[Stdcall]<LM_EVENT*, void*, void> callback,
        void* userContext);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountSetHostAttached(
        IntPtr handle, int attached);

    // --------------------------------------------------------------------
    // Path / volume
    // --------------------------------------------------------------------

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountResolvePath(
        IntPtr handle, string relativePath, LM_RESOLVED_PATH* outResolved);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountGetVolumeInfo(
        IntPtr handle, LM_VOLUME_INFO* outInfo);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountEnsureInUpperLayer(
        IntPtr handle, string relativePath);

    // --------------------------------------------------------------------
    // File primitives
    // --------------------------------------------------------------------

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountOpenFile(
        IntPtr         handle,
        string         relativePath,
        uint           grantedAccess,
        uint           createOptions,
        uint           originatorPid,
        IntPtr*        outFile,
        LM_FILE_INFO* outInfo);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountCreateFile(
        IntPtr         handle,
        string         relativePath,
        uint           createOptions,
        uint           grantedAccess,
        uint           fileAttributes,
        byte*          securityDescriptor,
        nuint          securityDescriptorBytes,
        ulong          allocationSize,
        uint           originatorPid,
        IntPtr*        outFile,
        LM_FILE_INFO* outInfo);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountCloseFile(IntPtr file);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountReadFile(
        IntPtr file,
        void*  buffer,
        ulong  offset,
        uint   length,
        uint   originatorPid,
        uint*  bytesTransferred);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountWriteFile(
        IntPtr         file,
        void*          buffer,
        ulong          offset,
        uint           length,
        int            writeToEnd,
        int            constrainedIo,
        uint           originatorPid,
        uint*          bytesTransferred,
        LM_FILE_INFO* outInfo);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountOverwriteFile(
        IntPtr         file,
        uint           fileAttributes,
        int            replaceAttributes,
        ulong          allocationSize,
        uint           originatorPid,
        LM_FILE_INFO* outInfo);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountFlushFile(
        IntPtr         file,
        uint           originatorPid,
        LM_FILE_INFO* outInfo);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountGetFileInfo(
        IntPtr file, LM_FILE_INFO* outInfo);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountSetFileInfo(
        IntPtr         file,
        uint           fileAttributes,
        ulong          creationTime,
        ulong          lastAccessTime,
        ulong          lastWriteTime,
        ulong          changeTime,
        ulong          allocationSize,
        ulong          fileSize,
        LM_FILE_INFO* outInfo);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountDeleteFile(
        IntPtr handle, string relativePath);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountCanDeleteFile(
        IntPtr handle, string relativePath);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountCanDeleteOpenFile(IntPtr file);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountDeleteOpenFile(IntPtr file);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountRenameFile(
        IntPtr handle,
        string oldRelativePath,
        string newRelativePath,
        int    replaceIfExists);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountRenameOpenFile(
        IntPtr file,
        string newRelativePath,
        int    replaceIfExists);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountUpdateOpenFilePath(
        IntPtr file,
        string newRelativePath);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountGetSecurity(
        IntPtr handle,
        string relativePath,
        uint*  outFileAttributes,
        byte*  securityDescriptor,
        nuint  securityDescriptorBytes,
        nuint* requiredBytes);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountSetSecurity(
        IntPtr handle,
        string relativePath,
        uint   securityInformation,
        byte*  modificationDescriptor,
        nuint  modificationDescriptorBytes);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountMergeDirectory(
        IntPtr handle,
        string dirRelativePath,
        delegate* unmanaged[Stdcall]<char*, LM_FILE_INFO*, void*, int> callback,
        void*  userContext);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountCreateWhiteout(
        IntPtr handle, string relativePath, int isDirectory);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountSetOpaque(
        IntPtr handle, string dirRelativePath);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountGetReparsePoint(
        IntPtr handle,
        string relativePath,
        byte*  buffer,
        nuint  bufferBytes,
        nuint* requiredBytes);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountSetReparsePoint(
        IntPtr handle,
        string relativePath,
        byte*  buffer,
        nuint  bufferBytes);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountDeleteReparsePoint(
        IntPtr handle,
        string relativePath,
        byte*  buffer,
        nuint  bufferBytes);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountEnumerateStreams(
        IntPtr           handle,
        string           relativePath,
        LM_STREAM_INFO* outBuffer,
        uint             bufferCapacity,
        uint*            outCount);

    // --------------------------------------------------------------------
    // VHD primitives
    // --------------------------------------------------------------------

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountVhdCreate(
        IntPtr mount, LM_VHD_CONFIG* config, IntPtr* outVhd);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountVhdOpen(
        IntPtr mount, LM_VHD_CONFIG* config, IntPtr* outVhd);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountVhdAttach(
        IntPtr vhd,
        char*  physicalPathBuffer,
        nuint  physicalPathChars,
        nuint* physicalPathRequired);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountVhdDetach(IntPtr vhd);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountVhdMerge(IntPtr childVhd);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountVhdImport(
        IntPtr mount,
        string directoryPath,
        string vhdPath,
        ulong  sizeBytes);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountVhdExport(
        IntPtr mount,
        string vhdPath,
        string directoryPath);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountVhdClose(IntPtr vhd);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountVhdGetVolumeGuid(
        IntPtr vhd,
        char*  buffer,
        nuint  bufferChars,
        nuint* requiredChars);

    // manifestDir is PCWSTR; may be NULL (use cwd). Use IntPtr so callers
    // can pass IntPtr.Zero for NULL without a special-case signature.
    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountVhdListLayers(
        IntPtr              mount,
        IntPtr              manifestDir,   // PCWSTR; IntPtr.Zero = NULL
        LM_VHD_LAYER_INFO* entries,
        uint                entriesCapacity,
        uint*               entriesWritten,
        uint*               entriesRequired);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountVhdUnregisterLayer(
        IntPtr mount,
        string layerId,
        IntPtr manifestDir,   // PCWSTR; IntPtr.Zero = NULL
        int*   outRemoved);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountVhdGetLayerMetadataJson(
        IntPtr mount,
        IntPtr manifestDir,   // PCWSTR; IntPtr.Zero = NULL
        string layerId,
        char*  buffer,
        nuint  bufferChars,
        nuint* requiredChars);

    // --------------------------------------------------------------------
    // VSS primitives
    // --------------------------------------------------------------------

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountVssCreateSnapshot(
        IntPtr  mount,
        string  volumePath,
        int     persistent,
        IntPtr* outSnapshot,
        char*   idBuffer,
        nuint   idBufferChars,
        nuint*  idRequired,
        char*   devicePathBuffer,
        nuint   devicePathBufferChars,
        nuint*  devicePathRequired);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountVssDeleteSnapshot(
        IntPtr mount, string snapshotId);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountVssListSnapshots(
        IntPtr                 mount,
        LM_VSS_SNAPSHOT_INFO* entries,
        uint                   entriesCapacity,
        uint*                  entriesWritten,
        uint*                  entriesRequired);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountVssCleanupSnapshots(IntPtr mount);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountVssCloseSnapshot(IntPtr snapshot);

    // --------------------------------------------------------------------
    // Layer image primitives
    // --------------------------------------------------------------------

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountImagePack(
        IntPtr                  mount,
        string                  sourceDir,
        string                  outputPath,
        int                     compressionLevel,
        LM_IMAGE_PACK_OPTIONS* options,
        IntPtr*                 outImage);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountImagePackDifferential(
        IntPtr                  mount,
        string                  sourceDir,
        string                  baseDir,
        string                  outputPath,
        int                     compressionLevel,
        LM_IMAGE_PACK_OPTIONS* options,
        IntPtr*                 outImage);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountImageCreateManifest(
        IntPtr  mount,
        string  outputPath,
        IntPtr* imagePaths,    // PCWSTR const* -- caller builds the char** block
        uint    imageCount);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountImageUnpack(
        IntPtr mount,
        string imagePath,
        string targetDir,
        int    verifyChecksum);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountImageValidate(
        IntPtr mount, string imagePath);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountImageGetManifest(
        IntPtr              mount,
        string              imagePath,
        LM_IMAGE_MANIFEST* manifest);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountImageGetMetadata(
        IntPtr              mount,
        string              imagePath,
        LM_IMAGE_METADATA* metadata);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountImageClose(IntPtr image);

    // --------------------------------------------------------------------
    // Diagnostics, eventing, process tracker
    // --------------------------------------------------------------------

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountGetStats(
        IntPtr handle, LM_STATS* outStats);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountProcessTrackerEnable(
        IntPtr handle, int enable);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountProcessTrackerSetRules(
        IntPtr handle, string rulesPath);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountProcessTrackerExportJson(
        IntPtr handle,
        char*  buffer,
        nuint  bufferChars,
        nuint* requiredChars);

    [LibraryImport(DllName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountProcessTrackerExportCsv(
        IntPtr handle,
        char*  buffer,
        nuint  bufferChars,
        nuint* requiredChars);

    // --------------------------------------------------------------------
    // Host-helper mount-point primitives. Promoted into LayerMount.dll
    // by commit 21190d2 ("refactor: promote WindowsMountPoint into
    // LayerMount.dll as host helper"); host-flavor-agnostic and stateless,
    // so they hang off LayerMount.MountPoint (static) rather than any
    // LM_HANDLE.
    // --------------------------------------------------------------------

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountPointIsDriveLetter(
        string mountPoint, int* outIsDriveLetter);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountPointPrepareDirectory(
        string mountPoint, LM_MOUNT_POINT_PREP* outPrep);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountPointCaptureIdentity(
        string mountPoint, LM_MOUNT_POINT_PREP* prep);

    [LibraryImport(DllName, StringMarshalling = StringMarshalling.Utf16)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvStdcall)])]
    internal static partial int LayerMountPointReleaseIfSafe(
        string mountPoint, LM_MOUNT_POINT_PREP* prep);
}
