using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>Tempo, pitch and rate on decode-only streams (no audio hardware needed).</summary>
public class TempoTests
{
    private static readonly StreamOptions DecodeOnly = new() { DecodeOnly = true };

    private static AudioEngine CreateHeadless() => AudioEngine.Create(new AudioEngineOptions { NullOutput = true });

    private static long ReadAll(AudioStream s)
    {
        var buf = new float[4096 * s.Info.Format.Channels];
        long total = 0;
        int frames;
        while ((frames = s.Read(buf)) > 0) total += frames;
        return total;
    }

    [Fact]
    public void Tempo_ChangesTheLength()
    {
        using var engine = CreateHeadless();
        using var s = engine.OpenMemory(TestAudio.SineWav(seconds: 2), DecodeOnly);
        s.Tempo = 2f;
        Assert.Equal(2f, s.Tempo, 3);
        long frames = ReadAll(s);
        Assert.InRange(frames, 44100 - 64, 44100 + 64); // 2 s at double speed
    }

    [Fact]
    public void PitchAndRate_AreClamped()
    {
        using var engine = CreateHeadless();
        using var s = engine.OpenMemory(TestAudio.SineWav(), DecodeOnly);
        s.Pitch = 100f;
        Assert.Equal(36f, s.Pitch, 3);
        s.Rate = 50f;
        Assert.Equal(10f, s.Rate, 3);
        var ex = Assert.Throws<FatzAudioException>(() => s.Tempo = 0f);
        Assert.Equal(ErrorCode.InvalidArgument, ex.ErrorCode);
    }

    [Fact]
    public void TempoOptions_RoundTrip()
    {
        using var engine = CreateHeadless();
        using var s = engine.OpenMemory(TestAudio.SineWav(), DecodeOnly);
        Assert.Equal(new TempoOptions(), s.TempoOptions);
        s.TempoOptions = new TempoOptions { Quality = StretchQuality.Fast, KeepFormants = true, Latency = TimeSpan.FromMilliseconds(80) };
        Assert.Equal(StretchQuality.Fast, s.TempoOptions.Quality);
        Assert.True(s.TempoOptions.KeepFormants);
        Assert.Equal(80, s.TempoOptions.Latency.TotalMilliseconds);
        s.TempoOptions = null;
        Assert.Equal(new TempoOptions(), s.TempoOptions);
        Assert.Throws<ArgumentException>(() => s.TempoOptions = new TempoOptions { Quality = (StretchQuality)7 });
    }

    [Fact]
    public void PushStreams_TakeTempoAndRate()
    {
        using var engine = CreateHeadless();
        using var s = engine.CreatePushStream(AudioFormat.Float32(48000, 2));
        s.Tempo = 1.25f;
        s.Rate = 0.9f;
        Assert.Equal(1.25f, s.Tempo, 3);
        Assert.Equal(0.9f, s.Rate, 3);
    }
}
