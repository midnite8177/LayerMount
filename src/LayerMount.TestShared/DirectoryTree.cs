using System.Collections.Generic;
using System.IO;
using System.IO.Enumeration;

namespace LayerMount.TestShared;

/// <summary>
/// Removes a directory tree a test built.
/// </summary>
public static class DirectoryTree
{
    /// <summary>
    /// Deletes <paramref name="root"/> and everything under it. The walk
    /// stops at a directory reparse point and neither reads nor writes
    /// through one, so a junction inside the tree loses its link and keeps
    /// its target. Throws if the file system refuses the final delete.
    /// </summary>
    public static void DeleteWithoutFollowingLinks(string root)
    {
        // The order carries weight. A read-only file refuses to be deleted,
        // and a directory link left in place fails the recursive delete.
        ClearReadOnlyFlags(root);
        RemoveDirectoryLinks(root);
        Directory.Delete(root, recursive: true);
    }

    private static void ClearReadOnlyFlags(string root)
    {
        foreach (var file in FilesInside(root))
        {
            // A path-based SetAttributes call acts on a link's target. A
            // file reached through a junction may be outside root, and the
            // cleanup must not change it.
            try
            {
                var attributes = File.GetAttributes(file);
                File.SetAttributes(file, attributes & ~FileAttributes.ReadOnly);
            }
            catch { /* best-effort */ }
        }
    }

    private static void RemoveDirectoryLinks(string root)
    {
        var links = new List<string>(DirectoryLinksInside(root));
        foreach (var link in links)
        {
            // A recursive Directory.Delete removes a directory link and
            // keeps its target. On a mount-point reparse point it also
            // calls DeleteVolumeMountPoint. That call fails with
            // ERROR_INVALID_PARAMETER on a junction that names a directory
            // rather than a volume. The failure then stops the delete
            // before it reaches root. This non-recursive delete of the link
            // alone does not make that call.
            try { Directory.Delete(link); }
            catch { /* best-effort */ }
        }
    }

    private static IEnumerable<string> FilesInside(string root)
    {
        return EntriesInside(
            root,
            (ref FileSystemEntry entry) =>
                !entry.IsDirectory
                && (entry.Attributes & FileAttributes.ReparsePoint) == 0);
    }

    private static IEnumerable<string> DirectoryLinksInside(string root)
    {
        return EntriesInside(
            root,
            (ref FileSystemEntry entry) =>
                entry.IsDirectory
                && (entry.Attributes & FileAttributes.ReparsePoint) != 0);
    }

    private static IEnumerable<string> EntriesInside(
        string root, FileSystemEnumerable<string>.FindPredicate include)
    {
        return new FileSystemEnumerable<string>(
            root,
            (ref FileSystemEntry entry) => entry.ToFullPath(),
            WalkOptions())
        {
            ShouldIncludePredicate = include,
            ShouldRecursePredicate = (ref FileSystemEntry entry) =>
                (entry.Attributes & FileAttributes.ReparsePoint) == 0,
        };
    }

    private static EnumerationOptions WalkOptions()
    {
        return new EnumerationOptions
        {
            RecurseSubdirectories = true,
            // The default skips a hidden or system entry. An entry that the
            // cleanup must reach can carry either attribute.
            AttributesToSkip = 0,
        };
    }
}
