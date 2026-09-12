using System;
using System.Collections.Generic;
using System.IO;

namespace LayerMount.TestShared;

/// <summary>
/// C# twin of the native TempLayerEnv: creates a unique directory tree
/// under %TEMP% with upper/, work/, and zero or more lowerN/ folders.
/// Tries to remove everything on <see cref="Dispose"/>. The cleanup stays
/// inside <see cref="Root"/> and does not follow a reparse point out of it.
/// It is best-effort: <see cref="Dispose"/> swallows a failure, so
/// <see cref="Root"/> can outlive it.
///
/// Lives in TestShared so consumer test projects across the engine and
/// host adapters can share one fixture.
/// </summary>
public sealed class TempLayerEnvironment : IDisposable
{
    public string Root { get; }
    public string Upper { get; }
    public string Work  { get; }
    public IReadOnlyList<string> Lowers => _lowers;

    private readonly List<string> _lowers = new();

    public TempLayerEnvironment(int lowerCount = 1)
    {
        Root  = Path.Combine(Path.GetTempPath(),
                             "LayerMountNet_" + Guid.NewGuid().ToString("N"));
        Upper = Path.Combine(Root, "upper");
        Work  = Path.Combine(Root, "work");
        Directory.CreateDirectory(Upper);
        Directory.CreateDirectory(Work);

        for (int i = 0; i < lowerCount; ++i)
        {
            var l = Path.Combine(Root, $"lower{i}");
            Directory.CreateDirectory(l);
            _lowers.Add(l);
        }
    }

    public string Lower(int index) => _lowers[index];

    public void WriteLowerFile(int index, string relative, string contents)
    {
        var path = Path.Combine(Lower(index), relative);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, contents);
    }

    public LayerMountConfig BuildConfig(
        HostCapabilities? capabilities = null)
    {
        return new LayerMountConfig
        {
            UpperPath     = Upper,
            WorkDirPath   = Work,
            LowerPaths    = _lowers,
            Capabilities  = capabilities ?? (HostCapabilities.Ads
                                             | HostCapabilities.ReparsePoints
                                             | HostCapabilities.SparseFiles
                                             | HostCapabilities.MultipleStreams
                                             | HostCapabilities.NtfsAcls),
        };
    }

    public void Dispose()
    {
        try
        {
            if (Directory.Exists(Root))
            {
                DirectoryTree.DeleteWithoutFollowingLinks(Root);
            }
        }
        catch
        {
            // Tests must not fail on cleanup; the OS will reclaim %TEMP%.
        }
    }

}
