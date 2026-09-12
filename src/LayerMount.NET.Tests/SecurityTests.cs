using System;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Security.AccessControl;
using LayerMount.Tests.Support;
using Xunit;

namespace LayerMount.Tests;

[SupportedOSPlatform("windows")]
public sealed class SecurityTests
{
    private const uint FILE_ATTRIBUTE_NORMAL = 0x00000080u;
    private const uint GENERIC_READ          = 0x80000000u;
    private const uint GENERIC_WRITE         = 0x40000000u;

    private const uint OWNER_SECURITY_INFORMATION = 0x1u;
    private const uint GROUP_SECURITY_INFORMATION = 0x2u;
    private const uint DACL_SECURITY_INFORMATION  = 0x4u;

    [Fact]
    public void GetSecurity_OnUpperLayerFile_ReturnsNonEmptyValidDescriptor()
    {
        using var fixture = new MountFixture();
        CreatePlainFile(fixture.Mount, @"\plain.txt");

        var (attributes, sd) = fixture.Mount.GetSecurity(@"\plain.txt");

        Assert.NotEmpty(sd);
        var raw = new RawSecurityDescriptor(sd, 0);
        Assert.NotNull(raw.GetSddlForm(AccessControlSections.All));
        Assert.True(attributes != 0);
    }

    [Fact]
    public void GetSecurity_SddlMatchesUnderlyingFile()
    {
        using var fixture = new MountFixture();
        CreatePlainFile(fixture.Mount, @"\match.txt");

        var (_, sd) = fixture.Mount.GetSecurity(@"\match.txt");
        string fromMount = new RawSecurityDescriptor(sd, 0).GetSddlForm(AccessControlSections.All);

        byte[] direct = ReadWin32Security(
            System.IO.Path.Combine(fixture.Upper, "match.txt"),
            OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION);
        string fromDisk = new RawSecurityDescriptor(direct, 0).GetSddlForm(AccessControlSections.All);

        Assert.Equal(fromDisk, fromMount);
    }

    [Fact]
    public void GetSecurity_ManyDistinctSidAces_RoundTripsWhole()
    {
        using var fixture = new MountFixture();
        CreatePlainFile(fixture.Mount, @"\manyaces.txt");

        string upperPath = System.IO.Path.Combine(fixture.Upper, "manyaces.txt");
        var sddl = new System.Text.StringBuilder("D:");
        string[] wellKnownSids =
        {
            "S-1-1-0",   // Everyone
            "S-1-5-11",  // Authenticated Users
            "S-1-5-4",   // Interactive
            "S-1-5-6",   // Service
            "S-1-5-2",   // Network
        };
        foreach (string sid in wellKnownSids)
        {
            sddl.Append($"(A;;FR;;;{sid})");
        }
        SetWin32SecurityFromSddl(upperPath, sddl.ToString());

        var (_, sd) = fixture.Mount.GetSecurity(@"\manyaces.txt");
        var raw = new RawSecurityDescriptor(sd, 0);

        RawAcl? dacl = raw.DiscretionaryAcl;
        Assert.NotNull(dacl);
        Assert.Equal(wellKnownSids.Length, dacl!.Count);
        foreach (string sid in wellKnownSids)
        {
            bool found = false;
            for (int i = 0; i < dacl.Count; i++)
            {
                if (dacl[i] is KnownAce ace && ace.SecurityIdentifier.Value == sid)
                {
                    found = true;
                    break;
                }
            }
            Assert.True(found, $"ACE for {sid} missing from the round-tripped descriptor");
        }
    }

    [Fact]
    public void GetSecurity_DaclOnlyOverload_OmitsOwnerAndGroup()
    {
        using var fixture = new MountFixture();
        CreatePlainFile(fixture.Mount, @"\daclonly.txt");

        var (_, sd) = fixture.Mount.GetSecurity(@"\daclonly.txt", DACL_SECURITY_INFORMATION);
        var raw = new RawSecurityDescriptor(sd, 0);

        Assert.Null(raw.Owner);
        Assert.Null(raw.Group);
        Assert.NotNull(raw.DiscretionaryAcl);
    }

    [Fact]
    public unsafe void GetSecurity_ReturnedArray_HoldsExactlyOneDescriptor()
    {
        using var fixture = new MountFixture();
        CreatePlainFile(fixture.Mount, @"\exact.txt");

        var (_, sd) = fixture.Mount.GetSecurity(@"\exact.txt");

        uint descriptorLength;
        fixed (byte* p = sd)
        {
            Assert.True(IsValidSecurityDescriptor(p),
                "GetSecurityDescriptorLength reads the header of a self-relative descriptor, "
                    + "so the length assertion below means nothing unless the bytes are one");
            descriptorLength = GetSecurityDescriptorLength(p);
        }
        Assert.Equal((int)descriptorLength, sd.Length);
    }

    private sealed class MountFixture : IDisposable
    {
        private readonly TempLayerEnvironment _env;

        public MountFixture()
        {
            _env = new TempLayerEnvironment(0);
            try
            {
                Mount = LayerMount.Create(_env.BuildConfig());
            }
            catch
            {
                _env.Dispose();
                throw;
            }
        }

        public LayerMount Mount { get; }

        public string Upper => _env.Upper;

        public void Dispose()
        {
            Mount.Dispose();
            _env.Dispose();
        }
    }

    private static void CreatePlainFile(LayerMount mount, string relativePath)
    {
        using var file = mount.CreateFile(
            relativePath, createOptions: 0u,
            grantedAccess: GENERIC_READ | GENERIC_WRITE,
            fileAttributes: FILE_ATTRIBUTE_NORMAL);
    }

    private static unsafe byte[] ReadWin32Security(string path, uint securityInformation)
    {
        uint size = 0;
        NativeGetFileSecurity(path, securityInformation, null, 0, ref size);
        byte[] buffer = new byte[size];
        fixed (byte* p = buffer)
        {
            if (!NativeGetFileSecurity(path, securityInformation, p, size, ref size))
            {
                throw new InvalidOperationException(
                    $"GetFileSecurityW failed with {Marshal.GetLastWin32Error()}");
            }
        }
        return buffer;
    }

    private static unsafe void SetWin32SecurityFromSddl(string path, string sddl)
    {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl, 1u, out IntPtr sd, out _))
        {
            throw new InvalidOperationException(
                $"ConvertStringSecurityDescriptorToSecurityDescriptorW failed with {Marshal.GetLastWin32Error()}");
        }
        try
        {
            if (!NativeSetFileSecurity(path, DACL_SECURITY_INFORMATION, sd))
            {
                throw new InvalidOperationException(
                    $"SetFileSecurityW failed with {Marshal.GetLastWin32Error()}");
            }
        }
        finally
        {
            LocalFree(sd);
        }
    }

    [DllImport("advapi32.dll", EntryPoint = "GetFileSecurityW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern unsafe bool NativeGetFileSecurity(
        string fileName, uint requestedInformation, byte* buffer, uint bufferLength, ref uint lengthNeeded);

    [DllImport("advapi32.dll", EntryPoint = "GetSecurityDescriptorLength")]
    private static extern unsafe uint GetSecurityDescriptorLength(byte* securityDescriptor);

    [DllImport("advapi32.dll", EntryPoint = "IsValidSecurityDescriptor")]
    private static extern unsafe bool IsValidSecurityDescriptor(byte* securityDescriptor);

    [DllImport("advapi32.dll", EntryPoint = "SetFileSecurityW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool NativeSetFileSecurity(
        string fileName, uint securityInformation, IntPtr securityDescriptor);

    [DllImport("advapi32.dll", EntryPoint = "ConvertStringSecurityDescriptorToSecurityDescriptorW", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool ConvertStringSecurityDescriptorToSecurityDescriptorW(
        string sddl, uint sddlRevision, out IntPtr securityDescriptor, out uint securityDescriptorSize);

    [DllImport("kernel32.dll")]
    private static extern IntPtr LocalFree(IntPtr handle);
}
