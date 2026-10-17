using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>Scheduling, markers, crossfades, queues, buses and the channel matrix on the silent null output.</summary>
public class CrossfadeTests
{
    private static readonly TimeSpan Timeout = TimeSpan.FromSeconds(5);

    private static AudioEngine CreateHeadless() => AudioEngine.Create(new AudioEngineOptions { NullOutput = true });

    private static async Task WaitFor(Func<bool> condition)
    {
        var deadline = DateTime.UtcNow + Timeout;
        while (!condition() && DateTime.UtcNow < deadline) await Task.Delay(10);
        Assert.True(condition());
    }

    [Fact]
    public async Task DeviceTime_AdvancesWhileRunning()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        output.Start();
        var t0 = output.Time;
        await WaitFor(() => output.Time - t0 > TimeSpan.FromMilliseconds(100));
    }

    [Fact]
    public async Task PlayAt_IsScheduledThenStarts()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var s = engine.OpenMemory(TestAudio.SineWav(2));
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        s.Started += (_, _) => started.TrySetResult();
        s.PlayOn(output);
        s.Stop();
        s.PlayAt(output.Time + TimeSpan.FromMilliseconds(300));
        Assert.Equal(StreamState.Scheduled, s.State);
        await started.Task.WaitAsync(Timeout);
        Assert.Equal(StreamState.Playing, s.State);
        Assert.Throws<ArgumentOutOfRangeException>(() => s.PlayAt(TimeSpan.FromSeconds(-1)));
    }

    [Fact]
    public async Task CrossfadeTo_FadesOutAndEndsTheCurrentStream()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var a = engine.OpenMemory(TestAudio.SineWav(3));
        using var b = engine.OpenMemory(TestAudio.SineWav(3));
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var ended = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        b.Started += (_, _) => started.TrySetResult();
        a.Ended += (_, _) => ended.TrySetResult();
        a.Volume = 0.8f;
        a.PlayOn(output);
        await a.CrossfadeTo(b, TimeSpan.FromMilliseconds(200)).WaitAsync(Timeout);
        await started.Task.WaitAsync(Timeout);
        await ended.Task.WaitAsync(Timeout);
        Assert.Equal(StreamState.Ended, a.State);
        Assert.Equal(StreamState.Playing, b.State);
        Assert.Same(output, b.Device);           // b joined a's device
        Assert.Equal(0.8f, a.Volume, 3);         // the user's volume is untouched
    }

    [Fact]
    public async Task QueueNext_StartsTheNextStreamAtTheEnd()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var a = engine.OpenMemory(TestAudio.SineWav(0.3));
        using var b = engine.OpenMemory(TestAudio.SineWav(2));
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        b.Started += (_, _) => started.TrySetResult();
        a.PlayOn(output);
        a.QueueNext(b);
        Assert.Same(b, a.Next);
        Assert.Equal(StreamState.Scheduled, b.State);
        await started.Task.WaitAsync(Timeout);
        await WaitFor(() => a.State == StreamState.Ended);
        Assert.Equal(StreamState.Playing, b.State);
        Assert.Null(a.Next);
    }

    [Fact]
    public void QueueNext_RejectsPlayingStreamAndClearNextStops()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var a = engine.OpenMemory(TestAudio.SineWav(2));
        using var b = engine.OpenMemory(TestAudio.SineWav(2));
        using var c = engine.OpenMemory(TestAudio.SineWav(2));
        a.PlayOn(output);
        b.PlayOn(output);
        Assert.Equal(ErrorCode.Busy, Assert.Throws<FatzAudioException>(() => a.QueueNext(b)).ErrorCode);
        a.QueueNext(c, TimeSpan.FromSeconds(1), Curve.SCurve);
        a.ClearNext();
        Assert.Equal(StreamState.Stopped, c.State);
        Assert.Null(a.Next);
    }

    [Fact]
    public async Task CueOut_EndsPlaybackThereAndRaisesMarkers()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var s = engine.OpenMemory(TestAudio.SineWav(3));
        var markers = new List<MarkerKind>();
        var ended = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        s.MarkerReached += (_, e) => { lock (markers) markers.Add(e.Marker!.Value); };
        s.Ended += (_, _) => ended.TrySetResult();
        s.Markers.CueIn = TimeSpan.FromSeconds(1);
        Assert.Equal(1.0, s.Position.TotalSeconds, 3);   // stopped = at the cue-in
        s.Markers.CueOut = TimeSpan.FromSeconds(1.2);
        s.Markers.FadeInCurve = Curve.Linear;
        Assert.Equal(Curve.Linear, s.Markers.FadeInCurve);
        Assert.Equal(Curve.EqualPower, s.Markers.FadeOutCurve);
        s.PlayOn(output);
        await ended.Task.WaitAsync(Timeout);
        Assert.Equal(1.2, s.Position.TotalSeconds, 3);
        lock (markers)
        {
            Assert.Contains(MarkerKind.CueIn, markers);
            Assert.Contains(MarkerKind.CueOut, markers);
        }
        Assert.Null(s.Markers.Get(MarkerKind.MixPoint));
        Assert.Throws<ArgumentOutOfRangeException>(() => s.Markers.Set(MarkerKind.MixPoint, TimeSpan.FromSeconds(-1)));
    }

    [Fact]
    public void MixBus_HasVolumeAndPanAndReleasesItsStreams()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var s = engine.OpenMemory(TestAudio.SineWav(2));
        var bus = output.CreateBus();
        bus.Volume = 0.5f;
        bus.Pan = -0.25f;
        Assert.Equal(0.5f, bus.Volume, 3);
        Assert.Equal(-0.25f, bus.Pan, 3);
        bus.Ramp(AudioParameter.Volume, 1f, TimeSpan.FromMilliseconds(50), Curve.EqualPower, output.Time + TimeSpan.FromMilliseconds(20));
        s.PlayOn(bus);
        Assert.Same(bus, s.Bus);
        Assert.Same(output, s.Device);
        bus.Dispose();
        Assert.Equal(StreamState.Playing, s.State);   // continues on the master mix
        Assert.Null(s.Bus);                             // the disposed bus is no longer its route
        Assert.Throws<ObjectDisposedException>(() => bus.Volume = 1f);
        Assert.Throws<ObjectDisposedException>(() => s.PlayOn(bus));
        s.PlayOn(output);
        Assert.Null(s.Bus);
    }

    [Fact]
    public void SetMatrix_ValidatesItsShape()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var s = engine.OpenMemory(TestAudio.SineWav(2));   // mono
        s.PlayOn(output);
        var ex = Assert.Throws<FatzAudioException>(() => s.SetMatrix(new float[,] { { 1f, 0f }, { 0f, 1f } }));
        Assert.Equal(ErrorCode.InvalidArgument, ex.ErrorCode);
        s.SetMatrix(new float[,] { { 0f, 1f } });                 // right speaker only
        s.ResetMatrix();
        Assert.Equal(StreamState.Playing, s.State);
    }

    [Fact]
    public async Task ScheduledRamp_CompletesAfterItsStart()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var s = engine.OpenMemory(TestAudio.SineWav(3));
        s.PlayOn(output);
        var start = output.Time + TimeSpan.FromMilliseconds(200);
        await s.RampAsync(AudioParameter.Volume, 0.25f, TimeSpan.FromMilliseconds(100), Curve.Points(0f, 0.8f, 1f), startAt: start)
            .WaitAsync(Timeout);
        Assert.True(output.Time >= start + TimeSpan.FromMilliseconds(100));
        Assert.Equal(0.25f, s.Volume, 3);
    }

    [Fact]
    public void CurvePoints_Validates()
    {
        Assert.Throws<ArgumentException>(() => Curve.Points(1f));
        Assert.Throws<ArgumentException>(() => Curve.Points(0f, float.NaN));
        Assert.Throws<ArgumentException>(() => Curve.Points(new float[Curve.MaxPoints + 1]));
        var c = Curve.Points(0f, 0.5f, 1f);
        Assert.Equal([0f, 0.5f, 1f], c.PointValues);
        Assert.Equal(c, Curve.Points(0f, 0.5f, 1f));
        Assert.NotEqual(c, Curve.Points(0f, 0.6f, 1f));
    }
}
