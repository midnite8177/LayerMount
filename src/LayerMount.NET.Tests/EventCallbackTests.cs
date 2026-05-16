using System;
using System.Collections.Concurrent;
using LayerMount.Tests.Support;
using Xunit;

namespace LayerMount.Tests;

public sealed class EventCallbackTests
{
    [Fact]
    public void Event_FiresOnCopyUp()
    {
        using var env = new TempLayerEnvironment(1);
        env.WriteLowerFile(0, "promote.txt", "from-lower");
        using var mount = LayerMount.Create(env.BuildConfig());

        var events = new ConcurrentBag<LayerMountEventArgs>();
        mount.Event += (_, args) => events.Add(args);

        mount.EnsureInUpperLayer(@"\promote.txt");

        bool sawCopyUp = false;
        foreach (var e in events)
        {
            if (e.Type == LayerMountEventType.CopyUp) { sawCopyUp = true; break; }
        }
        Assert.True(sawCopyUp,
            "EnsureInUpperLayer on a lower file should surface LayerMountEventType.CopyUp to managed handlers");
    }

    [Fact]
    public void Event_Unsubscribe_StopsDelivery()
    {
        using var env = new TempLayerEnvironment(1);
        env.WriteLowerFile(0, "a.txt", "a");
        env.WriteLowerFile(0, "b.txt", "b");
        using var mount = LayerMount.Create(env.BuildConfig());

        int count = 0;
        EventHandler<LayerMountEventArgs> handler = (_, _) => System.Threading.Interlocked.Increment(ref count);
        mount.Event += handler;
        mount.EnsureInUpperLayer(@"\a.txt");
        int afterFirst = count;
        Assert.True(afterFirst > 0);

        mount.Event -= handler;
        mount.EnsureInUpperLayer(@"\b.txt");
        Assert.Equal(afterFirst, count);
    }
}
