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
