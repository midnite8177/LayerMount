using System;
using System.Runtime.InteropServices;
using LayerMount.Interop;
using Xunit;

namespace LayerMount.Tests;

/// <summary>
/// Pins the managed size of each blittable interop struct to the size the
/// native header produces, so a field added on one side fails here.
/// </summary>
public sealed class InteropStructSizeTests
{
    [Theory]
    [InlineData(typeof(LM_CONFIG), 64)]
    [InlineData(typeof(LM_FILE_INFO), 72)]
    [InlineData(typeof(LM_RESOLVED_PATH), 56)]
    [InlineData(typeof(LM_STATS), 72)]
    [InlineData(typeof(LM_EVENT), 40)]
    [InlineData(typeof(LM_VHD_CONFIG), 48)]
    [InlineData(typeof(LM_IMAGE_MANIFEST), 24)]
    [InlineData(typeof(LM_IMAGE_PACK_OPTIONS), 24)]
    [InlineData(typeof(LM_IMAGE_METADATA), 152)]
    public void ManagedStruct_HasNativeSize(Type structType, int nativeBytes)
    {
        Assert.Equal(nativeBytes, Marshal.SizeOf(structType));
    }
}
