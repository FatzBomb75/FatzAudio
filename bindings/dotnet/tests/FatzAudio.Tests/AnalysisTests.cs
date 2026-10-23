using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>Track analysis: beat grids, keys, the waveform overview, and a stream's beat grid, on audio built in code.</summary>
public class AnalysisTests
{
    private const int Rate = 44100;
    private static readonly StreamOptions DecodeOnly = new() { DecodeOnly = true };

    private static AudioEngine CreateHeadless(string? cache = null) =>
        AudioEngine.Create(new AudioEngineOptions { NullOutput = true, MixSampleRate = Rate, CacheDirectory = cache });

    /// <summary>A mono 32-bit float WAV of kick drums: 150 Hz sweeping down to 60 Hz with a fast decay, every <paramref name="beatSeconds"/>.</summary>
    private static byte[] KickWav(double seconds, double beatSeconds = 0.5)
    {
        int frames = (int)(seconds * Rate);
        var samples = new float[frames];
        int kickLength = (int)(0.25 * Rate);
        for (double t0 = 0; t0 < seconds; t0 += beatSeconds)
        {
            int start = (int)Math.Round(t0 * Rate);
            double phase = 0;
            for (int i = 0; i < kickLength && start + i < frames; i++)
            {
                double t = (double)i / Rate;
                double freq = 60 + 90 * Math.Exp(-t * 40);
                phase += 2 * Math.PI * freq / Rate;
                samples[start + i] += (float)(Math.Sin(phase) * Math.Exp(-t * 14) * 0.9);
            }
        }
        return FloatWav(samples);
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
        w.Write((short)3);          // IEEE float
        w.Write((short)1);          // mono
        w.Write(Rate);
        w.Write(Rate * 4);          // byte rate
        w.Write((short)4);          // block align
        w.Write((short)32);
        w.Write("data"u8);
        w.Write(dataBytes);
        foreach (float s in samples) w.Write(s);
        w.Flush();
        return ms.ToArray();
    }

    private static string TempFile(byte[] data, string extension = "wav")
    {
        string path = Path.Combine(Path.GetTempPath(), $"fa-analysis-{Guid.NewGuid():N}.{extension}");
        File.WriteAllBytes(path, data);
        return path;
    }

    [Fact]
    public void NativeStructs_MatchTheCLayout()
    {
        // sizeof / offsetof of the structs in fatzaudio.h on a 64-bit target.
        Assert.Equal(48, System.Runtime.InteropServices.Marshal.SizeOf<Native.FaBeatGrid>());
        Assert.Equal(8, (int)System.Runtime.InteropServices.Marshal.OffsetOf<Native.FaBeatGrid>(nameof(Native.FaBeatGrid.Bpm)));
        Assert.Equal(24, System.Runtime.InteropServices.Marshal.SizeOf<Native.FaTempoSegment>());
        Assert.Equal(28, System.Runtime.InteropServices.Marshal.SizeOf<Native.FaKeyInfo>());
        Assert.Equal(24, (int)System.Runtime.InteropServices.Marshal.OffsetOf<Native.FaKeyInfo>(nameof(Native.FaKeyInfo.Camelot)));
        Assert.Equal(24, System.Runtime.InteropServices.Marshal.SizeOf<Native.FaAnalysisInfo>());
        Assert.Equal(20, System.Runtime.InteropServices.Marshal.SizeOf<Native.FaAnalysisOptions>());
        Assert.Equal(16, System.Runtime.InteropServices.Marshal.SizeOf<Native.FaWaveformPoint>());
    }

    // ---- keys ------------------------------------------------------------------

    [Theory]
    [InlineData(0, false, "C", "8B")]
    [InlineData(9, true, "Am", "8A")]
    [InlineData(6, true, "F#m", "11A")]
    [InlineData(10, false, "Bb", "6B")]
    [InlineData(3, true, "Ebm", "2A")]
    public void FromTonic_HasNameAndCamelot(int tonic, bool minor, string name, string camelot)
    {
        var k = MusicalKey.FromTonic(tonic, minor);
        Assert.Equal(tonic, k.Tonic);
        Assert.Equal(minor, k.IsMinor);
        Assert.Equal(1f, k.Confidence);
        Assert.Equal(name, k.Name);
        Assert.Equal(camelot, k.Camelot);
        Assert.Equal($"{name} ({camelot})", k.ToString());
    }

    [Fact]
    public void FromTonic_RejectsTonicsOutsideTheOctave()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => MusicalKey.FromTonic(-1, false));
        Assert.Throws<ArgumentOutOfRangeException>(() => MusicalKey.FromTonic(12, true));
    }

    [Fact]
    public void Transpose_MovesTheTonicAndKeepsTheMode()
    {
        var c = MusicalKey.FromTonic(0, false);
        var d = c.Transpose(2);
        Assert.Equal(2, d.Tonic);
        Assert.False(d.IsMinor);
        Assert.Equal("D", d.Name);
        Assert.Equal("10B", d.Camelot);

        var fsm = MusicalKey.FromTonic(6, true);
        Assert.Equal("Gm", fsm.Transpose(1).Name);
        Assert.Equal("Em", fsm.Transpose(-2).Name);
        Assert.Equal(fsm, fsm.Transpose(12));
        Assert.Equal(fsm, fsm.Transpose(-24));
        Assert.Equal((int)((6L + int.MaxValue) % 12), fsm.Transpose(int.MaxValue).Tonic);
        Assert.Equal((int)(((6L + int.MinValue) % 12 + 12) % 12), fsm.Transpose(int.MinValue).Tonic);
        Assert.Equal(c, c.Transpose(0));
        Assert.NotEqual(c, d);
    }

    // ---- the grid's arithmetic ------------------------------------------------------

    [Fact]
    public void ConstantGrid_ConvertsBetweenBeatsAndFrames()
    {
        var grid = BeatGrid.Constant(120, 10_000, Rate);
        Assert.Equal(BeatGridFlags.Constant | BeatGridFlags.Edited, grid.Flags);
        Assert.Equal(BeatGridFlags.Constant | BeatGridFlags.Edited, new BeatGrid(120, 10_000, Rate).Flags);
        var seg = Assert.Single(grid.Segments);
        Assert.Equal(new TempoSegment(10_000, 120, 0), seg);
        Assert.Equal(TimeSpan.FromSeconds(10_000.0 / Rate), grid.FirstBeatTime);

        Assert.Equal(10_000, grid.BeatToFrame(0));
        Assert.Equal(10_000 + Rate / 2, grid.BeatToFrame(1));
        Assert.Equal(10_000 + Rate * 5, grid.BeatToFrame(10));
        Assert.Equal(10_000 + Rate / 4, grid.BeatToFrame(0.5));
        Assert.Equal(10_000 - Rate / 2, grid.BeatToFrame(-1));   // the grid goes on backwards from the first beat

        Assert.Equal(0.0, grid.FrameToBeat(10_000), 9);
        Assert.Equal(1.0, grid.FrameToBeat(10_000 + Rate / 2), 9);
        Assert.Equal(-1.0, grid.FrameToBeat(10_000 - Rate / 2), 9);
        Assert.Equal(2.5, grid.FrameToBeat(10_000 + Rate * 5 / 4), 9);
        foreach (double beat in new[] { -3.0, 0, 0.25, 7, 123.5 })
            Assert.Equal(beat, grid.FrameToBeat(grid.BeatToFrame(beat)), 3);
    }

    [Fact]
    public void TempoMap_ConvertsAcrossSegments()
    {
        // 120 BPM for four beats (2 s), then 60 BPM; the first beat is at frame 10 000.
        long change = 10_000 + 2L * Rate;
        var grid = new BeatGrid(120, 10_000, Rate,
            [new TempoSegment(10_000, 120, 0), new TempoSegment(change, 60, 4)]);
        Assert.Equal(2, grid.Segments.Count);

        Assert.Equal(10_000 + Rate, grid.BeatToFrame(2));
        Assert.Equal(change, grid.BeatToFrame(4));
        Assert.Equal(change + Rate, grid.BeatToFrame(5));          // one beat is a second now
        Assert.Equal(change + 3L * Rate, grid.BeatToFrame(7));
        Assert.Equal(10_000 - Rate / 2, grid.BeatToFrame(-1));     // before the first segment: its tempo

        Assert.Equal(4.0, grid.FrameToBeat(change), 9);
        Assert.Equal(4.5, grid.FrameToBeat(change + Rate / 2), 9);
        Assert.Equal(3.0, grid.FrameToBeat(change - Rate / 2), 9);
        Assert.Equal(-1.0, grid.FrameToBeat(10_000 - Rate / 2), 9);
        foreach (double beat in new[] { -2.0, 0, 3.75, 4, 4.5, 20 })
            Assert.Equal(beat, grid.FrameToBeat(grid.BeatToFrame(beat)), 3);
    }

    [Fact]
    public void Grid_RejectsNonsense()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => BeatGrid.Constant(0, 0, Rate));
        Assert.Throws<ArgumentOutOfRangeException>(() => BeatGrid.Constant(double.NaN, 0, Rate));
        Assert.Throws<ArgumentOutOfRangeException>(() => BeatGrid.Constant(120, 0, 0));
        Assert.Throws<ArgumentOutOfRangeException>(() => new BeatGrid(120, 0, Rate, [new TempoSegment(0, -5, 0)]));
    }

    // ---- a stream's grid ----------------------------------------------------------------

    [Fact]
    public void StreamBeatGrid_RoundTrips()
    {
        using var engine = CreateHeadless();
        using var s = engine.OpenMemory(TestAudio.SineWav(seconds: 5), DecodeOnly);
        Assert.Null(s.BeatGrid);   // a WAV file has no grid of its own

        s.BeatGrid = BeatGrid.Constant(128.5, 1234, Rate);
        var g = s.BeatGrid;
        Assert.NotNull(g);
        Assert.Equal(128.5, g!.Bpm);
        Assert.Equal(1234, g.FirstBeat);
        Assert.Equal(Rate, g.SampleRate);
        Assert.Equal(4, g.BeatsPerBar);
        Assert.Equal(0, g.BarOffset);
        Assert.Equal(1f, g.Confidence);
        Assert.Equal(BeatGridFlags.Constant | BeatGridFlags.Edited, g.Flags);   // kept as given
        Assert.Equal(new[] { new TempoSegment(1234, 128.5, 0) }, g.Segments);

        // A tempo map, with a bar every three beats.
        var map = new BeatGrid(90, 500, Rate,
            [new TempoSegment(500, 90, 0), new TempoSegment(100_500, 120, 2.5), new TempoSegment(200_000, 60, 6)],
            beatsPerBar: 3, barOffset: 2, confidence: 0.75f, flags: BeatGridFlags.Exact | BeatGridFlags.Bars);
        s.BeatGrid = map;
        var m = s.BeatGrid!;
        Assert.Equal(map.Segments, m.Segments);
        Assert.Equal(90, m.Bpm);
        Assert.Equal(3, m.BeatsPerBar);
        Assert.Equal(2, m.BarOffset);
        Assert.Equal(0.75f, m.Confidence);
        Assert.Equal(BeatGridFlags.Exact | BeatGridFlags.Bars, m.Flags);
        Assert.Equal(map.BeatToFrame(5), m.BeatToFrame(5));

        s.BeatGrid = null;
        Assert.Null(s.BeatGrid);
    }

    [Fact]
    public void StreamBeatGrid_RejectsGridsTheNativeLibraryRefuses()
    {
        using var engine = CreateHeadless();
        using var s = engine.OpenMemory(TestAudio.SineWav(seconds: 1), DecodeOnly);
        var tooFast = Assert.Throws<FatzAudioException>(() => s.BeatGrid = BeatGrid.Constant(200_000, 0, Rate));
        Assert.Equal(ErrorCode.InvalidArgument, tooFast.ErrorCode);
        var barOffset = Assert.Throws<FatzAudioException>(() => s.BeatGrid = new BeatGrid(120, 0, Rate, barOffset: 4));
        Assert.Equal(ErrorCode.InvalidArgument, barOffset.ErrorCode);
        var backwards = Assert.Throws<FatzAudioException>(() => s.BeatGrid = new BeatGrid(120, 0, Rate,
            [new TempoSegment(1000, 120, 0), new TempoSegment(500, 120, 1)]));
        Assert.Equal(ErrorCode.InvalidArgument, backwards.ErrorCode);
        Assert.Null(s.BeatGrid);
    }

    [Fact]
    public void StreamBeatGrid_ThrowsAfterDispose()
    {
        using var engine = CreateHeadless();
        var s = engine.OpenMemory(TestAudio.SineWav(seconds: 1), DecodeOnly);
        s.Dispose();
        Assert.Throws<ObjectDisposedException>(() => s.BeatGrid);
        Assert.Throws<ObjectDisposedException>(() => s.BeatGrid = null);
        Assert.Throws<ObjectDisposedException>(() => s.Analyze());
    }

    // ---- analysing audio ------------------------------------------------------------------

    [Fact]
    public void Analyze_FindsTheTempoOfKicks()
    {
        string path = TempFile(KickWav(30));
        try
        {
            using var engine = CreateHeadless();
            var t = engine.Analyze(path, AnalysisKinds.Beats | AnalysisKinds.Waveform | AnalysisKinds.Loudness);
            Assert.Equal(AnalysisKinds.Beats | AnalysisKinds.Waveform | AnalysisKinds.Loudness, t.Kinds);
            Assert.False(t.FromCache);
            Assert.Equal(Rate, t.SampleRate);
            Assert.Equal(30L * Rate, t.Length);
            Assert.Null(t.Key);

            var grid = t.Beats;
            Assert.NotNull(grid);
            Assert.Equal(120.0, grid!.Bpm, 0.1);
            Assert.True(grid.Confidence > 0.5f, $"confidence {grid.Confidence}");
            Assert.Equal(Rate, grid.SampleRate);
            Assert.True(grid.Flags.HasFlag(BeatGridFlags.Constant));
            Assert.False(grid.Flags.HasFlag(BeatGridFlags.Exact));
            Assert.Equal(4, grid.BeatsPerBar);
            Assert.Equal(0, grid.BarOffset);
            var seg = Assert.Single(grid.Segments);
            Assert.Equal(grid.FirstBeat, seg.Start);
            Assert.Equal(grid.Bpm, seg.Bpm);
            Assert.Equal(0.0, seg.Beat);
            // The kicks fall on multiples of half a second: the first beat is on one of them.
            double phase = grid.FirstBeatTime.TotalSeconds % 0.5;
            Assert.True(Math.Min(phase, 0.5 - phase) < 0.03, $"first beat at {grid.FirstBeatTime}");

            Assert.NotNull(t.Waveform);
            Assert.InRange(t.Waveform!.Length, 2999, 3001);   // 10 ms per point
            Assert.Equal(TimeSpan.FromMilliseconds(10), WaveformPoint.Interval);
            // A kick starts on every 50th point: loud in the low band, with nothing up high.
            var kick = t.Waveform[50];
            Assert.True(kick.Peak > 0.5f && kick.Peak <= 1f, $"peak {kick.Peak}");
            Assert.True(kick.Low > kick.Mid && kick.Low > kick.High);
            var gap = t.Waveform[45];   // 450 ms: the previous kick has decayed
            Assert.True(gap.Peak < 0.05f, $"peak {gap.Peak}");

            Assert.NotNull(t.Loudness);
            Assert.True(t.Loudness!.IsFinal);
            Assert.InRange(t.Loudness.IntegratedLufs, -60, 0);
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public async Task AnalyzeAsync_AndStreamAnalyze_GiveTheSameTempo()
    {
        byte[] wav = KickWav(30);
        string path = TempFile(wav);
        try
        {
            using var engine = CreateHeadless();
            var fromPath = await engine.AnalyzeAsync(path);
            Assert.Equal(AnalysisKinds.Beats | AnalysisKinds.Key, fromPath.Kinds);
            Assert.Null(fromPath.Waveform);
            Assert.Null(fromPath.Loudness);
            Assert.Equal(120.0, fromPath.Beats!.Bpm, 0.1);

            using var s = engine.OpenMemory(wav, DecodeOnly);
            var fromStream = s.Analyze(AnalysisKinds.Beats);
            Assert.Equal(AnalysisKinds.Beats, fromStream.Kinds);
            Assert.Null(fromStream.Key);
            Assert.Equal(fromPath.Beats.Bpm, fromStream.Beats!.Bpm, 0.01);
            Assert.Equal(fromPath.Beats.FirstBeat, fromStream.Beats.FirstBeat);

            // The point of it: hand the result to a stream.
            using var deck = engine.OpenMemory(wav, DecodeOnly);
            deck.BeatGrid = (await deck.AnalyzeAsync(AnalysisKinds.Beats)).Beats;
            Assert.Equal(fromStream.Beats.Bpm, deck.BeatGrid!.Bpm, 0.01);
            Assert.Equal(fromStream.Beats.FirstBeat, deck.BeatGrid.FirstBeat);
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void Analyze_UsesTheCacheAndNoCacheSkipsIt()
    {
        string dir = Path.Combine(Path.GetTempPath(), $"fa-cache-{Guid.NewGuid():N}");
        string path = TempFile(KickWav(12));
        try
        {
            using var engine = CreateHeadless(dir);
            var first = engine.Analyze(path);
            var second = engine.Analyze(path);
            var uncached = engine.Analyze(path, options: new AnalysisOptions { NoCache = true });
            Assert.False(first.FromCache);
            Assert.True(second.FromCache);
            Assert.False(uncached.FromCache);
            Assert.Equal(first.Beats!.Bpm, second.Beats!.Bpm, 3);
            Assert.Equal(first.Beats.FirstBeat, second.Beats.FirstBeat);
        }
        finally
        {
            File.Delete(path);
            if (Directory.Exists(dir)) Directory.Delete(dir, true);
        }
    }

    [Fact]
    public void Analyze_TempoRangeFoldsTheTempo()
    {
        // 120 BPM kicks with a range of 50..95 BPM read as half time.
        string path = TempFile(KickWav(30));
        try
        {
            using var engine = CreateHeadless();
            var t = engine.Analyze(path, AnalysisKinds.Beats, new AnalysisOptions { MinBpm = 50, MaxBpm = 95 });
            Assert.Equal(60.0, t.Beats!.Bpm, 0.1);
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void Analyze_SilenceHasNoBeatAndNoKey()
    {
        string path = TempFile(FloatWav(new float[10 * Rate]));
        try
        {
            using var engine = CreateHeadless();
            var t = engine.Analyze(path, AnalysisKinds.Beats | AnalysisKinds.Key | AnalysisKinds.Waveform);
            Assert.Null(t.Beats);
            Assert.Null(t.Key);
            Assert.InRange(t.Waveform!.Length, 999, 1001);
            Assert.All(t.Waveform, p => Assert.Equal(0f, p.Peak));
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void Analyze_FindsTheKeyOfAChord()
    {
        // An A minor triad (A, C, E) with a little of each overtone, held for 15 seconds.
        var samples = new float[15 * Rate];
        foreach (double freq in new[] { 220.0, 261.63, 329.63 })
            for (int h = 1; h <= 4; h++)
                for (int i = 0; i < samples.Length; i++)
                    samples[i] += (float)(Math.Sin(2 * Math.PI * freq * h * i / Rate) * 0.1 / h);
        string path = TempFile(FloatWav(samples));
        try
        {
            using var engine = CreateHeadless();
            var k = engine.Analyze(path, AnalysisKinds.Key).Key;
            Assert.NotNull(k);
            Assert.InRange(k!.Tonic, 0, 11);
            Assert.InRange(k.Confidence, 0f, 1f);
            Assert.Equal(k.IsMinor, k.Name.EndsWith('m'));
            Assert.Equal(MusicalKey.FromTonic(k.Tonic, k.IsMinor).Name, k.Name);
            Assert.Equal(MusicalKey.FromTonic(k.Tonic, k.IsMinor).Camelot, k.Camelot);
            // Transposing keeps the confidence and agrees with making the key from its transposed tonic.
            var up = k.Transpose(5);
            Assert.Equal(k.Confidence, up.Confidence);
            Assert.Equal((k.Tonic + 5) % 12, up.Tonic);
            Assert.Equal(MusicalKey.FromTonic(up.Tonic, k.IsMinor).Name, up.Name);
            Assert.Equal(MusicalKey.FromTonic(up.Tonic, k.IsMinor).Camelot, up.Camelot);
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void Analyze_Errors()
    {
        using var engine = CreateHeadless();
        Assert.Equal(ErrorCode.NotFound,
            Assert.Throws<FatzAudioException>(() => engine.Analyze("no-such-file.wav")).ErrorCode);
        Assert.Throws<ArgumentNullException>(() => engine.Analyze(null!));

        string path = TempFile(KickWav(5));
        try
        {
            // The tempo range must span at least 1.9 times its start.
            var bad = new AnalysisOptions { MinBpm = 100, MaxBpm = 120 };
            Assert.Equal(ErrorCode.InvalidArgument,
                Assert.Throws<FatzAudioException>(() => engine.Analyze(path, options: bad)).ErrorCode);
        }
        finally { File.Delete(path); }

        string junk = TempFile(new byte[4096], "bin");
        try
        {
            Assert.Equal(ErrorCode.UnsupportedFormat,
                Assert.Throws<FatzAudioException>(() => engine.Analyze(junk)).ErrorCode);
        }
        finally { File.Delete(junk); }
    }

    [Fact]
    public async Task AnalyzeAsync_FailuresComeThroughTheTask_AndTokensCancel()
    {
        using var engine = CreateHeadless();
        var ex = await Assert.ThrowsAsync<FatzAudioException>(() => engine.AnalyzeAsync("no-such-file.wav"));
        Assert.Equal(ErrorCode.NotFound, ex.ErrorCode);

        using var cts = new CancellationTokenSource();
        cts.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => engine.AnalyzeAsync("whatever.wav", cancellationToken: cts.Token));
    }

    [Fact]
    public async Task AnalyzeAsync_SurvivesDisposingTheOwnerMeanwhile()
    {
        byte[] wav = KickWav(30);
        var engine = CreateHeadless();
        var stream = engine.OpenMemory(wav, DecodeOnly);
        var task = stream.AnalyzeAsync(AnalysisKinds.Beats);
        stream.Dispose();
        engine.Dispose();
        var t = await task;
        Assert.Equal(120.0, t.Beats!.Bpm, 0.1);
    }

    [Fact]
    public void StreamAnalyze_NeedsASeekableSource()
    {
        using var engine = CreateHeadless();
        using var push = engine.CreatePushStream(new AudioFormat(SampleFormat.Float32, Rate, 1));
        var ex = Assert.Throws<FatzAudioException>(() => push.Analyze());
        Assert.Equal(ErrorCode.NotSeekable, ex.ErrorCode);
    }

    // ---- MIDI files: the grid is exact -----------------------------------------------------

    private static void VarLen(List<byte> t, int v)
    {
        var tmp = new Stack<byte>();
        tmp.Push((byte)(v & 0x7F));
        while ((v >>= 7) != 0) tmp.Push((byte)(0x80 | (v & 0x7F)));
        t.AddRange(tmp);
    }

    private static void Event(List<byte> t, int delta, params byte[] e) { VarLen(t, delta); t.AddRange(e); }

    /// <summary>A 480-ticks-per-beat song in 3/4: 120 BPM for four beats (2 s), then 60 BPM for four more (4 s).</summary>
    private static byte[] TempoChangeSong()
    {
        var t = new List<byte>();
        Event(t, 0, 0xFF, 0x51, 3, 0x07, 0xA1, 0x20);          // 120 BPM
        Event(t, 0, 0xFF, 0x58, 4, 3, 2, 24, 8);               // 3/4
        Event(t, 0, 0x90, 69, 100);
        Event(t, 480 * 4, 0x80, 69, 0);
        Event(t, 0, 0xFF, 0x51, 3, 0x0F, 0x42, 0x40);          // 60 BPM
        Event(t, 0, 0x90, 69, 100);
        Event(t, 480 * 4, 0x80, 69, 0);
        Event(t, 0, 0xFF, 0x2F, 0);
        var f = new List<byte>();
        f.AddRange("MThd"u8.ToArray()); f.AddRange(new byte[] { 0, 0, 0, 6, 0, 0, 0, 1, 480 >> 8, 480 & 255 });
        f.AddRange("MTrk"u8.ToArray());
        f.AddRange(new[] { (byte)(t.Count >> 24), (byte)(t.Count >> 16), (byte)(t.Count >> 8), (byte)t.Count });
        f.AddRange(t);
        return f.ToArray();
    }

    private static void AssertTempoChangeGrid(BeatGrid g)
    {
        Assert.Equal(BeatGridFlags.Exact | BeatGridFlags.Bars, g.Flags & (BeatGridFlags.Exact | BeatGridFlags.Bars));
        Assert.False(g.Flags.HasFlag(BeatGridFlags.Constant));
        Assert.Equal(1f, g.Confidence);
        Assert.Equal(3, g.BeatsPerBar);
        Assert.Equal(0, g.BarOffset);
        Assert.Equal(0, g.FirstBeat);
        Assert.Equal(Rate, g.SampleRate);
        Assert.Equal(60, g.Bpm);   // the slow half lasts longer
        Assert.Equal(2, g.Segments.Count);
        Assert.Equal(new TempoSegment(0, 120, 0), g.Segments[0]);
        Assert.Equal(2L * Rate, g.Segments[1].Start);
        Assert.Equal(60, g.Segments[1].Bpm);
        Assert.Equal(4.0, g.Segments[1].Beat, 9);
        Assert.Equal(2L * Rate + Rate, g.BeatToFrame(5));
    }

    [Fact]
    public void Midi_StreamAndAnalysisGiveTheExactGrid()
    {
        using var engine = CreateHeadless();
        using var font = SoundFont.FromMemory(MidiTests.SineSoundFont());
        using var s = engine.OpenMemory(TempoChangeSong(), DecodeOnly with { SoundFont = font });

        // The stream already has the file's grid, without any analysis.
        AssertTempoChangeGrid(s.BeatGrid!);

        var t = s.Analyze(AnalysisKinds.Beats);
        AssertTempoChangeGrid(t.Beats!);
        Assert.Equal(Rate, t.SampleRate);

        // Setting a grid replaces the file's; clearing it brings the file's back.
        s.BeatGrid = BeatGrid.Constant(100, 0, Rate);
        Assert.Equal(100, s.BeatGrid!.Bpm);
        s.BeatGrid = null;
        AssertTempoChangeGrid(s.BeatGrid!);
    }

    [Fact]
    public async Task Midi_AnalyzeByPath_UsesTheEngineSoundFont()
    {
        string path = TempFile(TempoChangeSong(), "mid");
        try
        {
            using var engine = CreateHeadless();
            using (var font = SoundFont.FromMemory(MidiTests.SineSoundFont()))
                engine.DefaultSoundFont = font;
            var t = await engine.AnalyzeAsync(path, AnalysisKinds.Beats);
            AssertTempoChangeGrid(t.Beats!);
        }
        finally { File.Delete(path); }
    }
}
