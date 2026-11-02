using System.Runtime.InteropServices;
using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>Silence detection: a stream's edge scan, the analysis kind and its cache, and the detector for pushed PCM.</summary>
public class SilenceTests
{
    private const int Rate = 8000;
    private static readonly StreamOptions DecodeOnly = new() { DecodeOnly = true };

    /// <summary>Mono float samples, silent except a 440 Hz tone at 0.5 in each [on, off) span (seconds).</summary>
    private static float[] Tones(double seconds, params (double On, double Off)[] spans)
    {
        var x = new float[(int)Math.Round(seconds * Rate)];
        foreach (var (on, off) in spans)
        {
            int a = (int)Math.Round(on * Rate), b = (int)Math.Round(off * Rate);
            for (int i = a; i < b && i < x.Length; i++) x[i] = (float)(0.5 * Math.Sin(2 * Math.PI * 440 * (i - a) / Rate + 0.3));
        }
        return x;
    }

    private static byte[] FloatWav(float[] samples)
    {
        int dataBytes = samples.Length * 4;
        using var ms = new MemoryStream(44 + dataBytes);
        using var w = new BinaryWriter(ms);
        w.Write("RIFF"u8);
        w.Write(36 + dataBytes);
        w.Write("WAVE"u8);
        w.Write("fmt "u8);
        w.Write(16);
        w.Write((short)3);
        w.Write((short)1);
        w.Write(Rate);
        w.Write(Rate * 4);
        w.Write((short)4);
        w.Write((short)32);
        w.Write("data"u8);
        w.Write(dataBytes);
        foreach (float s in samples) w.Write(s);
        w.Flush();
        return ms.ToArray();
    }

    private static AudioEngine CreateHeadless(string? cache = null) =>
        AudioEngine.Create(new AudioEngineOptions { NullOutput = true, CacheDirectory = cache });

    [Fact]
    public void NativeStructs_MatchTheCLayout()
    {
        Assert.Equal(24, Marshal.SizeOf<Native.FaSilenceOptions>());
        Assert.Equal(64, Marshal.SizeOf<Native.FaSilence>());
        Assert.Equal(16, (int)Marshal.OffsetOf<Native.FaSilence>(nameof(Native.FaSilence.FirstAudible)));
        Assert.Equal(40, (int)Marshal.OffsetOf<Native.FaSilence>(nameof(Native.FaSilence.ThresholdDb)));
        Assert.Equal(56, (int)Marshal.OffsetOf<Native.FaSilence>(nameof(Native.FaSilence.MinSoundMs)));
        Assert.Equal(16, Marshal.SizeOf<Native.FaSilenceRegion>());
    }

    [Fact]
    public void DetectSilence_ShortFile_FindsEveryRegionAndSetsTheMarkers()
    {
        using var engine = CreateHeadless();
        using var song = engine.OpenMemory(FloatWav(Tones(11, (1.5, 3.5), (4.0, 5.0), (8.0, 9.0))), DecodeOnly);
        var s = song.DetectSilence();
        Assert.True(s.IsComplete);
        Assert.False(s.FromCache);
        Assert.Equal(Rate, s.SampleRate);
        Assert.Equal(11 * Rate, s.Length);
        Assert.Equal((long)(1.5 * Rate), s.FirstAudible);
        Assert.InRange(s.LastAudible!.Value, 9 * Rate - 2, 9 * Rate);
        Assert.Equal(TimeSpan.FromSeconds(1.5), s.FirstAudibleTime);
        Assert.Equal(3, s.Regions.Count);               // the 0.5 s gap is under a second
        Assert.Equal(new SilenceRegion(0, (long)(1.5 * Rate)), s.Regions[0]);
        Assert.Equal(8 * Rate, s.Regions[1].End);
        Assert.Equal(11 * Rate, s.Regions[2].End);

        song.Markers.CueIn = s.FirstAudibleTime;
        song.Markers.CueOut = s.LastAudibleTime;
        Assert.Equal(s.FirstAudibleTime, song.Markers.CueIn);
        Assert.Equal(s.LastAudibleTime, song.Markers.CueOut);

        // The settings that made it.
        Assert.Equal(SilenceMode.Rms, s.Mode);
        Assert.Equal(-50f, s.ThresholdDb);
        Assert.Equal(TimeSpan.FromSeconds(1), s.MinLength);
        Assert.Equal(TimeSpan.FromMilliseconds(50), s.Window);
        Assert.Equal(TimeSpan.FromMilliseconds(200), s.MinSound);

        // Shorter regions count with a shorter minimum.
        var fine = song.DetectSilence(new SilenceOptions { MinLength = TimeSpan.FromMilliseconds(400) });
        Assert.Equal(4, fine.Regions.Count);
    }

    [Fact]
    public void Rms_IgnoresAClickAndACountIn_PeakCountsThem()
    {
        // A lone full-scale click at 1 s, four 20 ms ticks from 2 s, the song from 4 to 6 s.
        var x = Tones(7, (4.0, 6.0));
        x[Rate] = 1f;
        for (int k = 0; k < 4; k++)
            for (int i = 0; i < Rate / 50; i++)
                x[(int)((2.0 + 0.5 * k) * Rate) + i] = (float)(0.5 * Math.Sin(2 * Math.PI * 1000 * i / Rate));

        using var rms = new SilenceDetector(AudioFormat.Float32(Rate, 1));
        rms.Feed(x);
        var r = rms.GetResult();
        Assert.Equal(TimeSpan.FromSeconds(4), r.FirstAudibleTime);
        Assert.Equal(TimeSpan.FromSeconds(6), r.LastAudibleTime);

        using var peak = new SilenceDetector(AudioFormat.Float32(Rate, 1), new SilenceOptions { Mode = SilenceMode.Peak });
        peak.Feed(x);
        var p = peak.GetResult();
        Assert.Equal(TimeSpan.FromSeconds(1), p.FirstAudibleTime);
        Assert.Equal(SilenceMode.Peak, p.Mode);
        Assert.Equal(TimeSpan.Zero, p.Window);

        var bad = Assert.Throws<FatzAudioException>(() => new SilenceDetector(AudioFormat.Float32(Rate, 1),
            new SilenceOptions { Window = TimeSpan.FromMilliseconds(5) }));
        Assert.Equal(ErrorCode.InvalidArgument, bad.ErrorCode);
    }

    [Fact]
    public void DetectSilence_LongFile_LeadingAndTrailingOnly_AnalysisHasAll_ThenFromCache()
    {
        string cache = Path.Combine(Path.GetTempPath(), $"fa-silence-{Guid.NewGuid():N}");
        try
        {
            using var engine = CreateHeadless(cache);
            using var song = engine.OpenMemory(FloatWav(Tones(110, (2, 50), (60, 95))), DecodeOnly);
            var edge = song.DetectSilence();
            Assert.False(edge.IsComplete);
            Assert.Equal(2, edge.Regions.Count);
            Assert.Equal(2L * Rate, edge.FirstAudible);

            var t = song.Analyze(AnalysisKinds.Silence);
            Assert.NotNull(t.Silence);
            Assert.True(t.Silence!.IsComplete);
            Assert.Equal(3, t.Silence.Regions.Count);
            Assert.Equal(edge.FirstAudible, t.Silence.FirstAudible);
            Assert.Equal(edge.LastAudible, t.Silence.LastAudible);

            var cached = song.DetectSilence();
            Assert.True(cached.IsComplete);
            Assert.True(cached.FromCache);
            Assert.Equal(t.Silence.Regions, cached.Regions);
            Assert.Equal(SilenceMode.Rms, cached.Mode);

            // A result made in one mode is not reused for the other.
            var peak = song.DetectSilence(new SilenceOptions { Mode = SilenceMode.Peak });
            Assert.False(peak.FromCache);
            Assert.Equal(SilenceMode.Peak, peak.Mode);
        }
        finally
        {
            if (Directory.Exists(cache)) Directory.Delete(cache, true);
        }
    }

    [Fact]
    public void Detector_MatchesTheStream_AndPushStreamsAreNotSeekable()
    {
        var x = Tones(11, (1.5, 3.5), (4.0, 5.0), (8.0, 9.0));
        using var engine = CreateHeadless();
        using var song = engine.OpenMemory(FloatWav(x), DecodeOnly);
        var want = song.DetectSilence();

        using var detector = new SilenceDetector(AudioFormat.Float32(Rate, 1));
        for (int at = 0; at < x.Length; at += 1234) detector.Feed(x.AsSpan(at, Math.Min(1234, x.Length - at)));
        var got = detector.GetResult();
        Assert.False(got.IsComplete);
        Assert.Equal(want.FirstAudible, got.FirstAudible);
        Assert.Equal(want.LastAudible, got.LastAudible);
        Assert.Equal(want.Regions, got.Regions);

        // 16-bit samples, through the same rule.
        using var d16 = new SilenceDetector(AudioFormat.Int16(Rate, 1));
        d16.Feed(x.Select(v => (short)Math.Round(v * 32767)).ToArray());
        Assert.Equal(want.FirstAudible, d16.GetResult().FirstAudible);
        Assert.Throws<InvalidOperationException>(() => d16.Feed(new float[2]));

        using var push = engine.CreatePushStream(AudioFormat.Float32(Rate, 1), DecodeOnly);
        var e = Assert.Throws<FatzAudioException>(() => push.DetectSilence());
        Assert.Equal(ErrorCode.NotSeekable, e.ErrorCode);
        var bad = Assert.Throws<FatzAudioException>(() => new SilenceDetector(AudioFormat.Float32(Rate, 1), new SilenceOptions { ThresholdDb = 3 }));
        Assert.Equal(ErrorCode.InvalidArgument, bad.ErrorCode);
    }
}
