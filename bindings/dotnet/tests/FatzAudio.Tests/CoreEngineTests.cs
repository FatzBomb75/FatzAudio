using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>Decoder registry, custom I/O and playback on the silent null output (works without audio hardware).</summary>
public class CoreEngineTests
{
    private static readonly StreamOptions DecodeOnly = new() { DecodeOnly = true };

    private static AudioEngine CreateHeadless() => AudioEngine.Create(new AudioEngineOptions { NullOutput = true });

    [Fact]
    public void SupportedFormats_ListsBuiltInDecoders()
    {
        var formats = AudioEngine.SupportedFormats;
        Assert.Contains(formats, f => f.Name == "wav" && f.Extensions.Contains("wav"));
        Assert.Contains(formats, f => f.Name == "flac");
        Assert.Contains(formats, f => f.Name == "mp3" && f.MimeTypes.Contains("audio/mpeg"));
        Assert.All(formats, f => Assert.False(string.IsNullOrEmpty(f.Description)));
    }

    [Fact]
    public void OpenMemory_DetectsFormatFromContent()
    {
        using var engine = CreateHeadless();
        using var s = engine.OpenMemory(TestAudio.SineWav(), DecodeOnly);
        Assert.Equal("pcm", s.Info.Codec);
        Assert.Equal("wav", s.Info.Container);
    }

    [Fact]
    public void OpenMemory_Garbage_ThrowsUnsupportedFormat()
    {
        using var engine = CreateHeadless();
        var junk = new byte[3000];
        for (int i = 0; i < junk.Length; i++) junk[i] = (byte)(i * 7 + 3);
        var ex = Assert.Throws<FatzAudioException>(() => engine.OpenMemory(junk));
        Assert.Equal(ErrorCode.UnsupportedFormat, ex.ErrorCode);
    }

    [Fact]
    public void OpenStream_Failure_LeavesSourceOpen()
    {
        using var engine = CreateHeadless();
        var source = new MemoryStream(new byte[512]);
        Assert.Throws<FatzAudioException>(() => engine.OpenStream(source));
        Assert.True(source.CanRead); // ownership only passes on success
    }

    [Fact]
    public void Markers_UnsetIsNull()
    {
        using var engine = CreateHeadless();
        using var s = engine.OpenMemory(TestAudio.SineWav());
        Assert.Null(s.Markers.MixPoint);
        s.Markers.MixPoint = TimeSpan.FromSeconds(0.5);
        Assert.Equal(0.5, s.Markers.MixPoint!.Value.TotalSeconds, 3);
        s.Markers.MixPoint = null;
        Assert.Null(s.Markers.MixPoint);
    }

    [Fact]
    public void Device_StartStop_AreIdempotent()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        Assert.False(output.IsStarted);
        output.Stop();
        output.Start();
        output.Start();
        Assert.True(output.IsStarted);
        output.Stop();
        output.Stop();
        Assert.False(output.IsStarted);
    }

    [Fact]
    public async Task PlayOn_KeepsFormatAndPosition_AcrossDevices()
    {
        using var engine = CreateHeadless();
        using var a = engine.OpenOutput();
        using var b = engine.OpenOutput();
        using var s = engine.OpenMemory(TestAudio.SineWav(seconds: 10, sampleRate: 22050));

        s.Position = TimeSpan.FromSeconds(1);
        s.PlayOn(a);
        Assert.Equal(22050, s.Info.Format.SampleRate); // source format, not the device's
        Assert.True(s.Position >= TimeSpan.FromSeconds(1));

        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(3);
        while (s.Position < TimeSpan.FromSeconds(1.1) && DateTime.UtcNow < deadline) await Task.Delay(10);
        s.Pause();
        var paused = s.Position;
        Assert.True(paused >= TimeSpan.FromSeconds(1.1));

        s.PlayOn(b);           // move, then pause again to compare
        s.Pause();
        Assert.Same(b, s.Device);
        Assert.InRange((s.Position - paused).TotalSeconds, 0, 0.5);

        s.Detach();
        Assert.Null(s.Device);
        var detached = s.Position;
        await Task.Delay(50);
        Assert.Equal(detached, s.Position);
    }

    [Fact]
    public async Task RequestSeek_ReportsTargetAtOnceAndPlaysFromIt()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var s = engine.OpenMemory(TestAudio.SineWav(seconds: 10));
        s.PlayOn(output);
        for (int i = 1; i <= 8; i++) s.RequestSeek(TimeSpan.FromSeconds(i * 0.5));   // a drag
        Assert.Equal(TimeSpan.FromSeconds(4), s.Position);
        Assert.InRange(s.Clock.TotalSeconds, 3.999, 4.001);

        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(3);
        while (s.Position < TimeSpan.FromSeconds(4.1) && DateTime.UtcNow < deadline) await Task.Delay(10);
        Assert.InRange(s.Position.TotalSeconds, 4.1, 6.0);   // playing on from the target
        Assert.Throws<ArgumentOutOfRangeException>(() => s.RequestSeek(TimeSpan.FromSeconds(-1)));
        var ex = Assert.Throws<FatzAudioException>(() => s.RequestSeek(TimeSpan.FromSeconds(60)));
        Assert.Equal(ErrorCode.InvalidArgument, ex.ErrorCode);
    }

    [Fact]
    public async Task Playback_ReachesEnd()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var s = engine.OpenMemory(TestAudio.SineWav(seconds: 0.1));
        s.PlayOn(output);
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(3);
        while (s.State != StreamState.Ended && DateTime.UtcNow < deadline) await Task.Delay(10);
        Assert.Equal(StreamState.Ended, s.State);
    }
}

/// <summary>Decode-ahead playback, ramps and events on the silent null output.</summary>
public class PlaybackTests
{
    private static AudioEngine CreateHeadless() => AudioEngine.Create(new AudioEngineOptions { NullOutput = true });

    [Fact]
    public async Task Ended_EventIsRaised()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var s = engine.OpenMemory(TestAudio.SineWav(seconds: 0.1));
        var ended = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        s.Ended += (_, _) => ended.TrySetResult();
        Assert.True(s.EventsSupported);
        s.PlayOn(output);
        await ended.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(StreamState.Ended, s.State);
    }

    [Fact]
    public async Task Loop_TurnedOnWhilePlaying_RaisesLooped()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var s = engine.OpenMemory(TestAudio.SineWav(seconds: 0.3));
        var looped = new TaskCompletionSource<StreamEventArgs>(TaskCreationOptions.RunContinuationsAsynchronously);
        s.Looped += (_, e) => looped.TrySetResult(e);
        Assert.False(s.Loop);
        s.PlayOn(output);
        s.Loop = true;
        Assert.True(s.Loop);
        var e = await looped.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(0, e.Value);
        Assert.True(e.Frame > 0);
        Assert.NotEqual(StreamState.Ended, s.State);
        s.Loop = false;
        Assert.False(s.Loop);
    }

    [Fact]
    public async Task AddPositionEvent_RaisesPositionReachedOnce()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var s = engine.OpenMemory(TestAudio.SineWav(seconds: 0.5));
        var reached = new List<long>();
        var ended = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        s.PositionReached += (_, e) => { lock (reached) reached.Add(e.Frame); };
        s.Ended += (_, _) => ended.TrySetResult();
        s.AddPositionEvent(TimeSpan.FromSeconds(0.1));
        s.AddPositionEvent(TimeSpan.FromSeconds(0.2));
        s.AddPositionEvent(TimeSpan.FromSeconds(0.4));
        s.ClearPositionEvents();
        s.AddPositionEvent(TimeSpan.FromSeconds(0.3));
        s.AddPositionEvent(TimeSpan.FromSeconds(0.1));
        s.PlayOn(output);
        await ended.Task.WaitAsync(TimeSpan.FromSeconds(5));   // events arrive in order: both came before ENDED
        lock (reached) Assert.Equal(new long[] { 4410, 13230 }, reached);
    }

    [Fact]
    public void Reverse_ReadsBackwards()
    {
        using var engine = CreateHeadless();
        using var s = engine.OpenMemory(TestAudio.SineWav(seconds: 1), new StreamOptions { DecodeOnly = true });
        int channels = s.Info.Format.Channels;
        var forward = new float[1000 * channels];
        s.PositionFrames = 1000;
        Assert.Equal(1000, s.Read(forward));
        Assert.False(s.Reverse);
        s.Reverse = true;
        Assert.True(s.Reverse);
        Assert.Equal(2000, s.PositionFrames);
        var back = new float[1000 * channels];
        Assert.Equal(1000, s.Read(back));
        Assert.Equal(1000, s.PositionFrames);
        for (int i = 0; i < 1000; i++)
            for (int c = 0; c < channels; c++)
                Assert.Equal(forward[(999 - i) * channels + c], back[i * channels + c]);
        s.Reverse = false;
        Assert.False(s.Reverse);
    }

    [Fact]
    public void Muted_KeepsVolume()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var s = engine.OpenMemory(TestAudio.SineWav(seconds: 1));
        s.Volume = 0.7f;
        s.Muted = true;
        Assert.True(s.Muted);
        Assert.Equal(0.7f, s.Volume, 3);
        s.Muted = false;
        Assert.False(s.Muted);
        output.Muted = true;
        Assert.True(output.Muted);
        output.Muted = false;
        Assert.False(output.Muted);
    }

    [Fact]
    public async Task RampAsync_CompletesWhenTheRampEnds()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var s = engine.OpenMemory(TestAudio.SineWav(seconds: 5));
        s.PlayOn(output);
        var sw = System.Diagnostics.Stopwatch.StartNew();
        await s.RampAsync(AudioParameter.Volume, 0f, TimeSpan.FromMilliseconds(200), Curve.EqualPower)
               .WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(sw.Elapsed >= TimeSpan.FromMilliseconds(150), $"completed after {sw.Elapsed}");
        Assert.Equal(0f, s.Volume, 3);
    }

    [Fact]
    public async Task RampAsync_CompletesWhenReplaced()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var s = engine.OpenMemory(TestAudio.SineWav(seconds: 5));
        s.PlayOn(output);
        var slow = s.RampAsync(AudioParameter.Volume, 0f, TimeSpan.FromSeconds(30));
        await Task.Delay(50);
        s.Volume = 0.5f;                       // replaces the running ramp
        await slow.WaitAsync(TimeSpan.FromSeconds(5));
        await Task.Delay(50);
        Assert.Equal(0.5f, s.Volume, 3);
    }

    [Fact]
    public async Task Ramp_WaitsForPlayback()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var s = engine.OpenMemory(TestAudio.SineWav(seconds: 5));
        s.Volume = 0f;
        var fadeIn = s.RampAsync(AudioParameter.Volume, 1f, TimeSpan.FromMilliseconds(100));
        await Task.Delay(200);
        Assert.False(fadeIn.IsCompleted);      // not playing yet: the fade has not started
        s.PlayOn(output);
        await fadeIn.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(1f, s.Volume, 3);
    }

    [Fact]
    public async Task Pause_FreezesPositionAndClock()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var s = engine.OpenMemory(TestAudio.SineWav(seconds: 5), new StreamOptions { BufferLength = TimeSpan.FromMilliseconds(200) });
        s.PlayOn(output);
        await Task.Delay(150);
        s.Pause();
        var pos = s.Position;
        Assert.True(pos > TimeSpan.Zero);
        await Task.Delay(100);
        Assert.Equal(pos, s.Position);
        Assert.True(s.Clock <= pos);           // clock = position minus device latency
    }

    [Fact]
    public async Task Dispose_CancelsPendingRamp()
    {
        using var engine = CreateHeadless();
        var s = engine.OpenMemory(TestAudio.SineWav(seconds: 5));
        var ramp = s.RampAsync(AudioParameter.Volume, 0f, TimeSpan.FromSeconds(1));
        s.Dispose();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => ramp);
    }
}
