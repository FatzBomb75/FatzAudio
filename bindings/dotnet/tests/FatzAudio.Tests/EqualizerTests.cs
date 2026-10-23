using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>Equalizer, presets and user DSP.</summary>
public class EqualizerTests
{
    private static AudioEngine CreateHeadless() => AudioEngine.Create(new AudioEngineOptions { NullOutput = true });

    [Fact]
    public void Presets_HaveTenOctaveBandsAndNames()
    {
        Assert.Equal(14, Equalizer.Presets.Count);
        foreach (var p in Equalizer.Presets)
        {
            var bands = Equalizer.GetPreset(p);
            Assert.Equal(10, bands.Length);
            Assert.All(bands, b => Assert.Equal(EqualizerBandType.Peak, b.Type));
            Assert.False(string.IsNullOrEmpty(Equalizer.GetPresetName(p)));
        }
        Assert.Equal("Rock", Equalizer.GetPresetName(EqualizerPreset.Rock));
        Assert.All(Equalizer.GetPreset(EqualizerPreset.Flat), b => Assert.Equal(0f, b.GainDb));
        Assert.Throws<ArgumentOutOfRangeException>(() => Equalizer.GetPreset((EqualizerPreset)99));
    }

    [Fact]
    public void SetEqualizer_RoundTripsAndClamps()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        using var s = engine.OpenMemory(TestAudio.SineWav(seconds: 1));
        s.SetEqualizer(EqualizerPreset.Rock);
        s.SetEqualizerGain(0, 40f);
        var bands = s.GetEqualizer();
        Assert.Equal(10, bands.Length);
        Assert.Equal(24f, bands[0].GainDb);
        Assert.Equal(1000f, bands[5].Frequency);
        var ex = Assert.Throws<FatzAudioException>(() => s.SetEqualizerGain(10, 1f));
        Assert.Equal(ErrorCode.InvalidArgument, ex.ErrorCode);
        Assert.True(s.EqualizerEnabled);
        s.EqualizerEnabled = false;
        Assert.False(s.EqualizerEnabled);
        s.SetEqualizer(Array.Empty<EqualizerBand>());
        Assert.Empty(s.GetEqualizer());
    }

    [Fact]
    public void AddDsp_RunsOnDecodeOnlyReads()
    {
        using var engine = CreateHeadless();
        using var s = engine.OpenMemory(TestAudio.SineWav(seconds: 1), new StreamOptions { DecodeOnly = true });
        int rate = 0;
        using (s.AddDsp((samples, channels, sampleRate) => { rate = sampleRate; samples.Fill(0.25f); }))
        {
            var buf = new float[1024];
            int n = s.Read(buf);
            Assert.True(n > 0);
            Assert.Equal(0.25f, buf[0]);
            Assert.Equal(44100, rate);
        }
        var after = new float[1024];
        Assert.True(s.Read(after) > 0);
        Assert.NotEqual(0.25f, after[1]);
    }
}
