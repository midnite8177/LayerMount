using System;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Threading;
using Xunit;

namespace LayerMount.Tests;

public sealed class HandshakeReaderTests
{
    private const string MountedLine =
        "{\"status\":\"mounted\",\"instanceId\":\"inst-7\",\"mountPoint\":\"X:\\\\\"," +
        "\"controlPipe\":\"\\\\\\\\.\\\\pipe\\\\lm-7\",\"pid\":4242," +
        "\"processCreationTimeFiletime\":133000000000000000," +
        "\"processCreationTimeUtc\":\"2026-01-02T03:04:05.0000000Z\"," +
        "\"startedAtUtc\":\"2026-01-02T03:04:06.0000000Z\",\"host\":\"adapter-a\"}";

    [Fact]
    public void Parse_MountedLine_FillsEveryMountedField()
    {
        HandshakeResult r = HandshakeReader.Parse(MountedLine);

        var m = Assert.IsType<MountedHandshake>(r);
        Assert.Equal("mounted", m.StatusRaw);
        Assert.Equal("inst-7", m.InstanceId);
        Assert.Equal("X:\\", m.MountPoint);
        Assert.Equal("\\\\.\\pipe\\lm-7", m.ControlPipe);
        Assert.Equal(4242, m.Pid);
        Assert.Equal(133000000000000000L, m.ProcessCreationTimeFiletime);
        Assert.Equal(new DateTimeOffset(2026, 1, 2, 3, 4, 5, TimeSpan.Zero), m.ProcessCreationTimeUtc);
        Assert.Equal(new DateTimeOffset(2026, 1, 2, 3, 4, 6, TimeSpan.Zero), m.StartedAtUtc);
        Assert.Equal("adapter-a", m.Host);
        Assert.Equal(MountedLine, m.RawLine);
    }

    [Fact]
    public void Parse_MountedLineWithUnparseableTimestamp_GivesNullTimestamp()
    {
        const string line =
            "{\"status\":\"mounted\",\"instanceId\":\"inst-7\",\"mountPoint\":\"X:\\\\\"," +
            "\"pid\":4242,\"processCreationTimeUtc\":\"2026-01-02T03:04:05.0000000Z\"," +
            "\"startedAtUtc\":\"soon\",\"host\":\"adapter-a\"}";

        HandshakeResult r = HandshakeReader.Parse(line);

        var m = Assert.IsType<MountedHandshake>(r);
        Assert.Null(m.StartedAtUtc);
        Assert.Equal(new DateTimeOffset(2026, 1, 2, 3, 4, 5, TimeSpan.Zero), m.ProcessCreationTimeUtc);
        Assert.Equal("inst-7", m.InstanceId);
        Assert.Equal(4242, m.Pid);
        Assert.Equal("adapter-a", m.Host);
    }

    [Fact]
    public void Parse_FailedLine_FillsErrorCodeMessageAndHost()
    {
        const string line =
            "{\"status\":\"failed\",\"error\":\"mount_point_busy\"," +
            "\"message\":\"X: is already in use.\",\"host\":\"adapter-a\"}";

        HandshakeResult r = HandshakeReader.Parse(line);

        var f = Assert.IsType<FailedHandshake>(r);
        Assert.Equal("failed", f.StatusRaw);
        Assert.Equal("mount_point_busy", f.ErrorCode);
        Assert.Equal("X: is already in use.", f.Message);
        Assert.Equal("adapter-a", f.Host);
        Assert.Equal(line, f.RawLine);
    }

    [Fact]
    public void Parse_UnknownStatus_IsUnknownAndKeepsStatusRaw()
    {
        HandshakeResult r = HandshakeReader.Parse("{\"status\":\"starting\",\"host\":\"adapter-a\"}");

        var u = Assert.IsType<UnknownHandshake>(r);
        Assert.Equal("starting", u.StatusRaw);
        Assert.Equal("adapter-a", u.Host);
    }

    [Fact]
    public void Parse_AbsentStatus_IsUnknownWithEmptyStatusRaw()
    {
        HandshakeResult r = HandshakeReader.Parse("{\"pid\":1}");

        var u = Assert.IsType<UnknownHandshake>(r);
        Assert.Equal("", u.StatusRaw);
    }

    [Fact]
    public void Parse_MountedLineWithoutHost_GivesEmptyHost()
    {
        HandshakeResult r = HandshakeReader.Parse(
            "{\"status\":\"mounted\",\"instanceId\":\"inst-7\",\"mountPoint\":\"X:\\\\\",\"pid\":1}");

        var m = Assert.IsType<MountedHandshake>(r);
        Assert.NotNull(m.Host);
        Assert.Equal("", m.Host);
    }

    [Fact]
    public void Parse_NonJsonLine_ThrowsJsonException()
    {
        Assert.ThrowsAny<JsonException>(() => HandshakeReader.Parse("mounted at X:"));
    }

    [Fact]
    public void ReadLine_ReturnsParsedResultWhenALineIsAvailable()
    {
        using var reader = new StreamReader(
            new MemoryStream(Encoding.UTF8.GetBytes(MountedLine + "\n")), Encoding.UTF8);

        HandshakeResult? r = HandshakeReader.ReadLine(reader, TimeSpan.FromSeconds(5));

        var m = Assert.IsType<MountedHandshake>(r);
        Assert.Equal("inst-7", m.InstanceId);
        Assert.Equal(MountedLine, m.RawLine);
    }

    [Fact]
    public void ReadLine_ReturnsNullWhenNoLineArrivesBeforeTimeout()
    {
        using var release = new ManualResetEventSlim(false);
        using var released = new ManualResetEventSlim(false);
        using var reader = new StreamReader(new BlockingStream(release, released), Encoding.UTF8);

        HandshakeResult? r = HandshakeReader.ReadLine(reader, TimeSpan.FromMilliseconds(200));

        Assert.Null(r);
        release.Set();
        Assert.True(released.Wait(TimeSpan.FromSeconds(5)));
    }

    [Fact]
    public void ReadLine_ReturnsNullWhenTheStreamEndsWithoutALine()
    {
        using var reader = new StreamReader(new MemoryStream(), Encoding.UTF8);

        HandshakeResult? r = HandshakeReader.ReadLine(reader, TimeSpan.FromSeconds(5));

        Assert.Null(r);
    }

    private sealed class BlockingStream : Stream
    {
        private readonly ManualResetEventSlim _release;
        private readonly ManualResetEventSlim _released;

        public BlockingStream(ManualResetEventSlim release, ManualResetEventSlim released)
        {
            _release = release;
            _released = released;
        }

        public override int Read(byte[] buffer, int offset, int count)
        {
            _release.Wait();
            _released.Set();
            return 0;
        }

        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => false;
        public override long Length => throw new NotSupportedException();
        public override long Position
        {
            get => throw new NotSupportedException();
            set => throw new NotSupportedException();
        }
        public override void Flush() { }
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
    }
}
