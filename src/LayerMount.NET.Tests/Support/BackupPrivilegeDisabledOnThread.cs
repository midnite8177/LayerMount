using System;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using Xunit;

namespace LayerMount.Tests;

/// <summary>
/// Makes the calling thread impersonate a copy of the process token with
/// SeBackupPrivilege disabled, and reverts on <see cref="Dispose"/>. A
/// directory scan opens with backup intent, so a deny rule does not stop
/// the scan while SeBackupPrivilege is enabled.
/// </summary>
[SupportedOSPlatform("windows")]
internal sealed class BackupPrivilegeDisabledOnThread : IDisposable
{
    private const int SecurityImpersonation = 2;
    private const uint TOKEN_ADJUST_PRIVILEGES = 0x0020;
    private const uint TOKEN_QUERY = 0x0008;

    public BackupPrivilegeDisabledOnThread()
    {
        Assert.True(ImpersonateSelf(SecurityImpersonation),
            "ImpersonateSelf gives the thread a copy of the process token");
        if (!DisableBackupPrivilegeOnThread())
        {
            RevertToSelf();
            Assert.Fail("SeBackupPrivilege is disabled on the thread's token");
        }
    }

    public void Dispose()
    {
        RevertToSelf();
    }

    private static bool DisableBackupPrivilegeOnThread()
    {
        if (!OpenThreadToken(GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                             true, out IntPtr token))
        {
            return false;
        }
        try
        {
            if (!LookupPrivilegeValueW(null, "SeBackupPrivilege", out LUID luid))
            {
                return false;
            }
            var privileges = new TOKEN_PRIVILEGES
            {
                PrivilegeCount = 1,
                Luid = luid,
                Attributes = 0,
            };
            return AdjustTokenPrivileges(token, false, ref privileges,
                (uint)Marshal.SizeOf<TOKEN_PRIVILEGES>(), IntPtr.Zero, IntPtr.Zero);
        }
        finally
        {
            CloseHandle(token);
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct LUID
    {
        public uint LowPart;
        public int HighPart;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct TOKEN_PRIVILEGES
    {
        public uint PrivilegeCount;
        public LUID Luid;
        public uint Attributes;
    }

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool ImpersonateSelf(int impersonationLevel);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool RevertToSelf();

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool OpenThreadToken(
        IntPtr threadHandle, uint desiredAccess, bool openAsSelf, out IntPtr tokenHandle);

    [DllImport("kernel32.dll")]
    private static extern IntPtr GetCurrentThread();

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool LookupPrivilegeValueW(string? systemName, string name, out LUID luid);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool AdjustTokenPrivileges(
        IntPtr tokenHandle, bool disableAllPrivileges, ref TOKEN_PRIVILEGES newState,
        uint bufferLength, IntPtr previousState, IntPtr returnLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);
}
