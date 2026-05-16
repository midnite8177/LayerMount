// LayerImage -- managed receipt for an <c>LM_IMAGE_HANDLE</c>.
//
// Returned from <see cref="LayerMount.Images.Pack"/> and <see cref="LayerMount.Images.PackDifferential"/>.
// Disposing closes the native handle; the on-disk .lmnt artifact persists
// independently.

using System;

namespace LayerMount;

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

    public void Dispose() => _handle.Dispose();
}
