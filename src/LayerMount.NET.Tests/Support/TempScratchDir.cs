using System;
using System.IO;

namespace LayerMount.Tests;

/// <summary>
/// A unique empty directory under %TEMP% for one test. The tag goes in the
/// directory name, so a leftover directory names the suite that made it.
/// </summary>
internal sealed class TempScratchDir : IDisposable
{
    public string Root { get; }

    public TempScratchDir(string tag)
    {
        Root = Path.Combine(
            Path.GetTempPath(), "LayerMount" + tag + "_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Root);
    }

    public string Sub(string name) => Path.Combine(Root, name);

    public void Dispose()
    {
        try { DirectoryTree.DeleteWithoutFollowingLinks(Root); }
        catch { /* best-effort */ }
    }
}
