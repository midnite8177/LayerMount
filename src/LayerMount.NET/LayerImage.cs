using System;

namespace LayerMount;

/// <summary>
/// Managed receipt for an <c>LM_IMAGE_HANDLE</c>, returned from
/// <see cref="ImagesApi.Pack"/> and <see cref="ImagesApi.PackDifferential"/>.
/// This receipt holds only the native handle and the output path, no
/// other state.
/// </summary>
public sealed class LayerImage : IDisposable
{
    private readonly ImageHandle _handle;
    private readonly string _path;

    internal LayerImage(ImageHandle handle, string path)
    {
        _handle = handle;
        _path = path;
    }

    /// <summary>The filesystem path of the packed image on disk.</summary>
    public string Path => _path;

    /// <summary>True when the handle has been disposed or was never acquired.</summary>
    public bool IsClosed => _handle.IsClosed;

    /// <summary>
    /// Releases the native handle-table slot backing this image. The
    /// underlying <c>.lmnt</c> file on disk is unaffected.
    /// </summary>
    public void Dispose() => _handle.Dispose();
}
