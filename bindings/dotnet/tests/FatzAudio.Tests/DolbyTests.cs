using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>Dolby Digital (AC-3) and Dolby Digital Plus (E-AC-3) decode natively, with the dynamic range modes.</summary>
public class DolbyTests
{
    private static AudioEngine CreateHeadless() => AudioEngine.Create(new AudioEngineOptions { NullOutput = true });

    private static string FindTestFile(string relative)
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
        {
            string p = Path.Combine(dir.FullName, relative);
            if (File.Exists(p)) return p;
        }
        throw new FileNotFoundException(relative);
    }

    private static float[] ReadAll(AudioStream s)
    {
        var all = new List<float>();
        var buf = new float[4096 * s.Info.Format.Channels];
        int frames;
        while ((frames = s.Read(buf)) > 0) all.AddRange(buf.AsSpan(0, frames * s.Info.Format.Channels).ToArray());
        return all.ToArray();
    }

    [Fact]
    public void Ac3AndEac3_DecodeNatively()
    {
        using var engine = CreateHeadless();
        foreach (var (file, codec) in new[] { ("tests/data/tone.ac3", "ac3"), ("tests/data/tone.eac3", "eac3") })
        {
            using var s = engine.OpenFile(FindTestFile(file), new StreamOptions { DecodeOnly = true });
            Assert.Equal(codec, s.Info.Codec);
            Assert.Equal(48000, s.Info.Format.SampleRate);
            Assert.Equal(2, s.Info.Format.Channels);
            Assert.Equal(24576, ReadAll(s).Length / 2);
        }
    }

    [Fact]
    public void DynamicRange_OptionAndProperty()
    {
        using var engine = CreateHeadless();
        string path = FindTestFile("tests/data/dolby/stereo.ec3");
        using var full = engine.OpenFile(path, new StreamOptions { DecodeOnly = true });
        Assert.Equal(DynamicRange.Full, full.DynamicRange);
        using var night = engine.OpenFile(path, new StreamOptions { DecodeOnly = true, DynamicRange = DynamicRange.Night });
        Assert.Equal(DynamicRange.Night, night.DynamicRange);
        float[] a = ReadAll(full), b = ReadAll(night);
        Assert.Equal(a.Length, b.Length);
        Assert.Contains(a.Zip(b), p => Math.Abs(p.First - p.Second) > 1e-3f);

        full.DynamicRange = DynamicRange.Normal;
        Assert.Equal(DynamicRange.Normal, full.DynamicRange);
        Assert.Throws<ArgumentException>(() => full.DynamicRange = (DynamicRange)7);
    }

    [Fact]
    public void DolbyDownmix_OptionAndProperty()
    {
        using var engine = CreateHeadless();
        using var output = engine.OpenOutput();
        string path = FindTestFile("tests/data/dolby/mix_alt.ac3");
        using var auto = engine.OpenFile(path);
        Assert.Equal(DolbyDownmix.Auto, auto.DolbyDownmix);
        using var engineMix = engine.OpenFile(path, new StreamOptions { DolbyDownmix = DolbyDownmix.Engine });
        Assert.Equal(DolbyDownmix.Engine, engineMix.DolbyDownmix);

        // While playing, also to and from the engine's downmix (the chain is rebuilt).
        auto.PlayOn(output);
        auto.DolbyDownmix = DolbyDownmix.LtRt;
        Assert.Equal(DolbyDownmix.LtRt, auto.DolbyDownmix);
        auto.DolbyDownmix = DolbyDownmix.Engine;
        Assert.Equal(DolbyDownmix.Engine, auto.DolbyDownmix);
        auto.DolbyDownmix = DolbyDownmix.LoRo;
        Assert.Equal(DolbyDownmix.LoRo, auto.DolbyDownmix);
        Assert.Throws<ArgumentException>(() => auto.DolbyDownmix = (DolbyDownmix)9);
    }

    [Fact]
    public async Task Dialnorm_CountsAsLoudnessWhenEnabled()
    {
        using var engine = CreateHeadless();
        engine.SetDialnorm(true);
        using var s = engine.OpenFile(FindTestFile("tests/data/dolby/mix_alt.ac3"));
        Loudness l = s.GetLoudness();
        for (int i = 0; i < 400 && !l.IsFinal; i++)
        {
            await Task.Delay(10);
            l = s.GetLoudness();
        }
        Assert.True(l.IsFinal);
        Assert.Equal(LoudnessSource.Dialnorm, l.Source);
        Assert.Equal(-20.0, l.IntegratedLufs);
    }
}
