using System;
using System.IO;
using System.Runtime.Versioning;
using System.Security.AccessControl;
using System.Security.Principal;

namespace LayerMount.Tests;

/// <summary>
/// Denies list access to Everyone on one directory, without inheritance,
/// and removes the deny rule on <see cref="Dispose"/>. Declare it after the
/// test's layer environment so the rule goes before the environment
/// removes its tree.
/// </summary>
[SupportedOSPlatform("windows")]
internal sealed class DirectoryListingDenied : IDisposable
{
    private readonly DirectoryInfo _directory;
    private readonly FileSystemAccessRule _rule;

    public DirectoryListingDenied(string path)
    {
        _directory = new DirectoryInfo(path);
        _rule = new FileSystemAccessRule(
            new SecurityIdentifier(WellKnownSidType.WorldSid, null),
            FileSystemRights.ListDirectory,
            InheritanceFlags.None,
            PropagationFlags.None,
            AccessControlType.Deny);
        DirectorySecurity security = _directory.GetAccessControl(AccessControlSections.Access);
        security.AddAccessRule(_rule);
        _directory.SetAccessControl(security);
    }

    public void Dispose()
    {
        DirectorySecurity security = _directory.GetAccessControl(AccessControlSections.Access);
        security.RemoveAccessRuleSpecific(_rule);
        _directory.SetAccessControl(security);
    }
}
