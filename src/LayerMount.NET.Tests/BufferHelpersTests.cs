using System.Collections.Generic;
using LayerMount.Interop;
using Xunit;

namespace LayerMount.Tests;

public sealed class BufferHelpersTests
{
    [Theory]
    [InlineData(BufferHelpers.StackThreshold - 1)]
    [InlineData(BufferHelpers.StackThreshold + 1)]
    public unsafe void TryReadString_ResultShrinksBetweenProbeAndFill_ReturnsOnlyTheFilledChars(
        int probedChars)
    {
        const string filled = "abc";
        int calls = 0;

        int hr = BufferHelpers.TryReadString(
            (char* buffer, nuint bufferChars, nuint* requiredChars) =>
            {
                calls++;
                if (buffer == null)
                {
                    *requiredChars = (nuint)(probedChars + 1);
                    return 0;
                }
                for (int i = 0; i < filled.Length; i++)
                {
                    buffer[i] = filled[i];
                }
                buffer[filled.Length] = '\0';
                *requiredChars = (nuint)(filled.Length + 1);
                return 0;
            },
            out string? result);

        Assert.Equal(0, hr);
        Assert.Equal(2, calls);
        Assert.Equal("abc", result);
    }

    [Fact]
    public unsafe void TryReadBytes_ResultShrinksBetweenProbeAndFill_ReturnsOnlyTheFilledBytes()
    {
        byte[] filled = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
        int calls = 0;

        int hr = BufferHelpers.TryReadBytes(
            (byte* buffer, nuint bufferBytes, nuint* requiredBytes) =>
            {
                calls++;
                if (buffer == null)
                {
                    *requiredBytes = 16;
                    return 0;
                }
                for (int i = 0; i < filled.Length; i++)
                {
                    buffer[i] = filled[i];
                }
                *requiredBytes = (nuint)filled.Length;
                return 0;
            },
            out byte[]? result);

        Assert.Equal(0, hr);
        Assert.Equal(2, calls);
        Assert.Equal(new byte[] { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 }, result);
    }

    [Fact]
    public unsafe void TryReadBytes_ResultGrowsBetweenProbeAndFill_RetriesWithTheLargerSize()
    {
        var fillCapacities = new List<nuint>();

        int hr = BufferHelpers.TryReadBytes(
            (byte* buffer, nuint bufferBytes, nuint* requiredBytes) =>
            {
                if (buffer == null)
                {
                    *requiredBytes = 4;
                    return 0;
                }
                fillCapacities.Add(bufferBytes);
                if (bufferBytes < 6)
                {
                    *requiredBytes = 6;
                    return BufferHelpers.HRESULT_E_MORE_DATA;
                }
                for (int i = 0; i < 6; i++)
                {
                    buffer[i] = (byte)(i + 1);
                }
                *requiredBytes = 6;
                return 0;
            },
            out byte[]? result);

        Assert.Equal(0, hr);
        Assert.Equal(new nuint[] { 4, 6 }, fillCapacities);
        Assert.Equal(new byte[] { 1, 2, 3, 4, 5, 6 }, result);
    }

    [Fact]
    public unsafe void TryReadString_ResultShrinksToEmptyBetweenProbeAndFill_ReturnsEmptyString()
    {
        int hr = BufferHelpers.TryReadString(
            (char* buffer, nuint bufferChars, nuint* requiredChars) =>
            {
                if (buffer == null)
                {
                    *requiredChars = 8;
                    return 0;
                }
                buffer[0] = '\0';
                *requiredChars = 1;
                return 0;
            },
            out string? result);

        Assert.Equal(0, hr);
        Assert.Equal(string.Empty, result);
    }

    [Fact]
    public unsafe void TryReadBytes_FillReportsMoreThanTheBuffer_ReturnsTheWholeBuffer()
    {
        int hr = BufferHelpers.TryReadBytes(
            (byte* buffer, nuint bufferBytes, nuint* requiredBytes) =>
            {
                if (buffer == null)
                {
                    *requiredBytes = 4;
                    return 0;
                }
                for (int i = 0; i < 4; i++)
                {
                    buffer[i] = (byte)(i + 1);
                }
                *requiredBytes = 10;
                return 0;
            },
            out byte[]? result);

        Assert.Equal(0, hr);
        Assert.Equal(new byte[] { 1, 2, 3, 4 }, result);
    }
}
