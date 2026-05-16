using Xunit;

namespace LayerMount.Tests;

public sealed class SmokeTests
{
    [Fact]
    public void BuildVerification()
    {
        Assert.True(true, ".NET test project build verification");
    }

    [Fact]
    public void LayerMountGetVersion_ReturnsSaneTuple()
    {
        var (major, minor, patch, abi) = LayerMount.GetVersion();
        Assert.Equal(1u, abi);
        // major/minor/patch live in LM_VER_* constants (0/1/0 as of 2026-04).
        // Keep the assertion loose: just insist they fit in uint and don't
        // surface as garbage.
        Assert.True(major <= 100 && minor <= 100 && patch <= 1000,
            $"Version tuple looks wrong: {major}.{minor}.{patch}");
    }
}
