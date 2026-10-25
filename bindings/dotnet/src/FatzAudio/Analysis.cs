using System.Collections.ObjectModel;
using System.Runtime.InteropServices;
using FatzAudio.Native;

namespace FatzAudio;

/// <summary>What to find out about a track (<see cref="AudioEngine.Analyze"/>); combine with <c>|</c>. One decode of the audio gives any mix.</summary>
[Flags]
public enum AnalysisKinds
{
    /// <summary>Integrated loudness, range and true peak: the same measurement as <see cref="AudioEngine.ScanLoudness"/>.</summary>
    Loudness = 1,
    /// <summary>A beat grid: tempo, the first beat and a confidence. MIDI files and tracker modules get their exact grid from the file.</summary>
    Beats = 2,
    /// <summary>The musical key: tonic and mode, with the name and the Camelot code.</summary>
    Key = 4,
    /// <summary>Peak and low/mid/high band levels every 10 ms, for drawing the track.</summary>
    Waveform = 8,
}

/// <summary>How a <see cref="BeatGrid"/> came about and what it knows.</summary>
[Flags]
public enum BeatGridFlags
{
    /// <summary>One tempo for the whole song (a single segment).</summary>
    Constant = 1,
    /// <summary>Read from the file (MIDI tempo events, tracker rows), not measured.</summary>
    Exact = 2,
    /// <summary>Set by the app, for example after the user corrected it.</summary>
    Edited = 4,
    /// <summary><see cref="BeatGrid.BeatsPerBar"/> and <see cref="BeatGrid.BarOffset"/> are known (exact grids with one time signature); otherwise they are 4 and 0.</summary>
    Bars = 8,
}

/// <summary>Settings for <see cref="AudioEngine.Analyze"/> and <see cref="AudioStream.Analyze"/>. All properties are optional.</summary>
/// <example>
/// <code>
/// var t = engine.Analyze("song.mp3", AnalysisKinds.Beats | AnalysisKinds.Key, new AnalysisOptions { MinBpm = 80, MaxBpm = 160 });
/// </code>
/// </example>
public sealed record AnalysisOptions
{
    /// <summary>
    /// Preferred tempo range: a measured tempo outside it is doubled or halved into it (default 70). MIDI and tracker
    /// grids are never folded. <see cref="MaxBpm"/> must be at least 1.9 times this.
    /// </summary>
    public float MinBpm { get; init; } = 70f;
    /// <summary>The top of the preferred tempo range (default 180).</summary>
    public float MaxBpm { get; init; } = 180f;
    /// <summary>Neither read nor write the analysis cache (<see cref="AudioEngineOptions.CacheDirectory"/>).</summary>
    public bool NoCache { get; init; }
}

/// <summary>A stretch of a song at one tempo (<see cref="BeatGrid.Segments"/>).</summary>
/// <param name="Start">Frame (of the grid's sample rate) where the segment starts.</param>
/// <param name="Bpm">Tempo in beats per minute.</param>
/// <param name="Beat">Beat number at <paramref name="Start"/>. Beats count from <see cref="BeatGrid.FirstBeat"/> (0) and are not whole where a tempo change falls between beats.</param>
public readonly record struct TempoSegment(long Start, double Bpm, double Beat);

/// <summary>
/// The beats of a song: a main tempo, the frame of the first beat and, for songs with tempo changes, the tempo map.
/// Immutable. Frames are in <see cref="SampleRate"/>, the sample rate of the audio that was analysed.
/// </summary>
/// <remarks>
/// <see cref="AudioStream.BeatGrid"/> sets a grid on a stream (for sync, beat loops and aligned mixes), and reads the one
/// from a MIDI file or tracker module. Make your own, for example after the user corrected the tempo, with <see cref="Constant"/>.
/// </remarks>
/// <example>
/// <code>
/// var t = engine.Analyze("song.mp3");
/// if (t.Beats is { } grid)
/// {
///     deck.Stream.BeatGrid = grid;
///     long nextBar = grid.BeatToFrame(Math.Ceiling(grid.FrameToBeat(deck.Stream.PositionFrames) / 4) * 4);
/// }
/// </code>
/// </example>
public sealed class BeatGrid
{
    private readonly TempoSegment[] _segments;

    /// <summary>Creates a grid.</summary>
    /// <param name="bpm">The main tempo: the one that lasts longest (0.1..100000).</param>
    /// <param name="firstBeat">Frame of the first beat.</param>
    /// <param name="sampleRate">Sample rate of the frames in Hz.</param>
    /// <param name="segments">The tempo map: starts rising, tempos 0.1..100000. Null or empty makes a constant grid (one segment at <paramref name="firstBeat"/>, beat 0).</param>
    /// <param name="beatsPerBar">Beats in a bar (1..64).</param>
    /// <param name="barOffset">Number of the beat (0..<paramref name="beatsPerBar"/> - 1, counting the first beat as 0) that starts a bar.</param>
    /// <param name="confidence">0..1; 1 for a grid you are sure of.</param>
    /// <param name="flags">What the grid knows and where it came from. <see cref="BeatGridFlags.Constant"/> is added when the grid has one segment.</param>
    /// <exception cref="ArgumentOutOfRangeException">The tempo is not a positive number, or the sample rate is not positive.</exception>
    public BeatGrid(double bpm, long firstBeat, int sampleRate, IEnumerable<TempoSegment>? segments = null,
        int beatsPerBar = 4, int barOffset = 0, float confidence = 1f, BeatGridFlags flags = BeatGridFlags.Edited)
    {
        if (!(bpm > 0) || double.IsInfinity(bpm)) throw new ArgumentOutOfRangeException(nameof(bpm), "The tempo must be a positive number.");
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(sampleRate);
        var list = segments?.ToArray() ?? [];
        foreach (var s in list)
            if (!(s.Bpm > 0) || double.IsInfinity(s.Bpm) || double.IsNaN(s.Beat))
                throw new ArgumentOutOfRangeException(nameof(segments), "Every segment needs a positive tempo and a beat number.");
        if (list.Length == 0) list = [new TempoSegment(firstBeat, bpm, 0)];
        Bpm = bpm;
        FirstBeat = firstBeat;
        SampleRate = sampleRate;
        BeatsPerBar = beatsPerBar;
        BarOffset = barOffset;
        Confidence = confidence;
        Flags = list.Length == 1 ? flags | BeatGridFlags.Constant : flags & ~BeatGridFlags.Constant;
        _segments = list;
        Segments = Array.AsReadOnly(list);
    }

    /// <summary>A grid with one tempo for the whole song, flagged <see cref="BeatGridFlags.Constant"/> and <see cref="BeatGridFlags.Edited"/>, confidence 1, four beats to a bar.</summary>
    /// <param name="bpm">The tempo (0.1..100000).</param>
    /// <param name="firstBeat">Frame of the first beat.</param>
    /// <param name="sampleRate">Sample rate of the frames in Hz.</param>
    /// <exception cref="ArgumentOutOfRangeException">The tempo is not a positive number, or the sample rate is not positive.</exception>
    public static BeatGrid Constant(double bpm, long firstBeat, int sampleRate) =>
        new(bpm, firstBeat, sampleRate, flags: BeatGridFlags.Constant | BeatGridFlags.Edited);

    /// <summary>The main tempo in beats per minute: the one that lasts longest.</summary>
    public double Bpm { get; }

    /// <summary>Frame of the first beat heard. The grid goes on backwards from it at the first segment's tempo.</summary>
    public long FirstBeat { get; }

    /// <summary>Sample rate in Hz of the frames of this grid (<see cref="FirstBeat"/> and the segments).</summary>
    public int SampleRate { get; }

    /// <summary>Beats in a bar: 4, or what the file's time signature says.</summary>
    public int BeatsPerBar { get; }

    /// <summary>Number of the beat (0..<see cref="BeatsPerBar"/> - 1, counting the first beat as 0) that starts a bar.</summary>
    public int BarOffset { get; }

    /// <summary>How sure the analysis is, 0..1; 1 for exact grids.</summary>
    public float Confidence { get; }

    /// <summary>What the grid knows and where it came from.</summary>
    public BeatGridFlags Flags { get; }

    /// <summary>The tempo map in time order; one segment for a constant grid.</summary>
    public IReadOnlyList<TempoSegment> Segments { get; }

    /// <summary>Where the first beat falls in the song.</summary>
    public TimeSpan FirstBeatTime => TimeConvert.FromFrames(FirstBeat, SampleRate);

    /// <summary>
    /// The frame of a beat. A beat number counts from the first beat (0) and may be fractional or negative; before the
    /// first segment the grid goes on at the first segment's tempo.
    /// </summary>
    /// <param name="beat">The beat number.</param>
    /// <returns>The frame, in <see cref="SampleRate"/>.</returns>
    public long BeatToFrame(double beat)
    {
        // The last segment that starts at or before this beat; the first one stands in before it.
        int lo = 0, hi = _segments.Length - 1;
        while (lo < hi)
        {
            int mid = (lo + hi + 1) / 2;
            if (_segments[mid].Beat <= beat) lo = mid; else hi = mid - 1;
        }
        var seg = _segments[lo];
        return seg.Start + (long)Math.Round((beat - seg.Beat) * 60.0 * SampleRate / seg.Bpm);
    }

    /// <summary>
    /// The beat number at a frame (the inverse of <see cref="BeatToFrame"/>). Before the first segment the grid goes on at
    /// the first segment's tempo, so the result is negative before the first beat.
    /// </summary>
    /// <param name="frame">The frame, in <see cref="SampleRate"/>.</param>
    /// <returns>The beat number, with a fraction between beats.</returns>
    public double FrameToBeat(long frame)
    {
        int lo = 0, hi = _segments.Length - 1;
        while (lo < hi)
        {
            int mid = (lo + hi + 1) / 2;
            if (_segments[mid].Start <= frame) lo = mid; else hi = mid - 1;
        }
        var seg = _segments[lo];
        return seg.Beat + (frame - seg.Start) * seg.Bpm / (60.0 * SampleRate);
    }

    /// <inheritdoc/>
    public override string ToString() => $"{Bpm:0.##} BPM, first beat at {FirstBeatTime}, {Segments.Count} segment(s)";

    internal static unsafe BeatGrid? FromAnalysis(nint analysis)
    {
        FaBeatGrid g = default;
        g.StructSize = (uint)sizeof(FaBeatGrid);
        int rc = NativeMethods.fa_analysis_get_beat_grid(analysis, &g);
        if (rc == (int)ErrorCode.NotFound) return null;
        FatzAudioException.ThrowIfFailed(rc);
        uint count = 0;
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_analysis_get_tempo_map(analysis, null, 0, &count));
        var native = new FaTempoSegment[count];
        if (count > 0)
            fixed (FaTempoSegment* p = native)
                FatzAudioException.ThrowIfFailed(NativeMethods.fa_analysis_get_tempo_map(analysis, p, count, &count));
        return FromNative(g, native, (int)Math.Min(count, (uint)native.Length));
    }

    internal static unsafe BeatGrid? FromStream(StreamHandle stream)
    {
        FaBeatGrid g = default;
        g.StructSize = (uint)sizeof(FaBeatGrid);
        uint count = 0;
        int rc = NativeMethods.fa_stream_get_beat_grid(stream, &g, null, 0, &count);
        if (rc == (int)ErrorCode.NotFound) return null;
        FatzAudioException.ThrowIfFailed(rc);
        var native = new FaTempoSegment[count];
        if (count > 0)
        {
            // The grid can be replaced between the two calls; the second call's grid and segments belong together.
            fixed (FaTempoSegment* p = native)
            {
                rc = NativeMethods.fa_stream_get_beat_grid(stream, &g, p, count, &count);
                if (rc == (int)ErrorCode.NotFound) return null;
                FatzAudioException.ThrowIfFailed(rc);
            }
        }
        return FromNative(g, native, (int)Math.Min(count, (uint)native.Length));
    }

    private static BeatGrid FromNative(in FaBeatGrid g, FaTempoSegment[] segments, int count)
    {
        var list = new TempoSegment[count];
        for (int i = 0; i < count; i++) list[i] = new TempoSegment(segments[i].Start, segments[i].Bpm, segments[i].Beat);
        return new BeatGrid(g.Bpm, g.FirstBeat, (int)g.SampleRate, list, (int)g.BeatsPerBar, (int)g.BarOffset, g.Confidence, (BeatGridFlags)g.Flags);
    }

    internal unsafe void Apply(StreamHandle stream)
    {
        var g = new FaBeatGrid
        {
            StructSize = (uint)sizeof(FaBeatGrid),
            Bpm = Bpm,
            FirstBeat = FirstBeat,
            SampleRate = (uint)SampleRate,
            BeatsPerBar = (uint)BeatsPerBar,
            BarOffset = (uint)BarOffset,
            Confidence = Confidence,
            Flags = (uint)Flags,
            SegmentCount = (uint)_segments.Length,
        };
        var native = new FaTempoSegment[_segments.Length];
        for (int i = 0; i < native.Length; i++)
            native[i] = new FaTempoSegment { Start = _segments[i].Start, Bpm = _segments[i].Bpm, Beat = _segments[i].Beat };
        fixed (FaTempoSegment* p = native)
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_beat_grid(stream, &g, p, (uint)native.Length));
    }
}

/// <summary>
/// A musical key: tonic and mode, with the usual name ("F#m", "Bb") and the Camelot code used for harmonic mixing ("11A").
/// Immutable. Find one with <see cref="AudioEngine.Analyze"/> or make one with <see cref="FromTonic"/>.
/// </summary>
/// <remarks>
/// Keys found in recordings are wrong more often than tempos; most misses are the relative major or minor, or a fifth
/// away, so show the Camelot code and the confidence, and let users correct it.
/// </remarks>
/// <example>
/// <code>
/// var key = MusicalKey.FromTonic(9, minor: true);   // "Am", Camelot "8A"
/// var up2 = key.Transpose(2);                       // "Bm", Camelot "10A"
/// </code>
/// </example>
public sealed class MusicalKey : IEquatable<MusicalKey>
{
    private MusicalKey(int tonic, bool isMinor, float confidence, string name, string camelot)
    {
        Tonic = tonic;
        IsMinor = isMinor;
        Confidence = confidence;
        Name = name;
        Camelot = camelot;
    }

    /// <summary>The tonic: 0 = C, 1 = C#/Db, ... 11 = B.</summary>
    public int Tonic { get; }

    /// <summary>True for a minor key, false for major.</summary>
    public bool IsMinor { get; }

    /// <summary>How sure the analysis is, 0..1; 1 for keys made with <see cref="FromTonic"/>.</summary>
    public float Confidence { get; }

    /// <summary>The key's name in its usual spelling: "C", "F#m", "Bb", "Ebm".</summary>
    public string Name { get; }

    /// <summary>The Camelot code: "8B" (C major) to "12A".</summary>
    public string Camelot { get; }

    /// <summary>
    /// The key after a pitch shift of some semitones (a karaoke key change): the tonic moved, with the name and Camelot
    /// code to match and the confidence kept.
    /// </summary>
    /// <param name="semitones">Semitones to shift by, up (positive) or down (negative); any whole number.</param>
    /// <returns>The shifted key.</returns>
    public unsafe MusicalKey Transpose(int semitones)
    {
        NativeLoader.EnsureInitialized();
        FaKeyInfo from = ToNative();
        FaKeyInfo to = default;
        to.StructSize = (uint)sizeof(FaKeyInfo);
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_key_transpose(&from, semitones, &to));
        return FromNative(to);
    }

    /// <summary>Makes the key of a tonic and mode, with confidence 1: for keys from the app's library.</summary>
    /// <param name="tonic">0 = C, 1 = C#/Db, ... 11 = B.</param>
    /// <param name="minor">True for minor, false for major.</param>
    /// <returns>The key.</returns>
    /// <exception cref="ArgumentOutOfRangeException">The tonic is not 0..11.</exception>
    public static unsafe MusicalKey FromTonic(int tonic, bool minor)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(tonic);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(tonic, 11);
        NativeLoader.EnsureInitialized();
        FaKeyInfo k = default;
        k.StructSize = (uint)sizeof(FaKeyInfo);
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_key_make(tonic, minor ? 1 : 0, &k));
        return FromNative(k);
    }

    /// <inheritdoc/>
    public bool Equals(MusicalKey? other) => other is not null && Tonic == other.Tonic && IsMinor == other.IsMinor && Confidence.Equals(other.Confidence);

    /// <inheritdoc/>
    public override bool Equals(object? obj) => Equals(obj as MusicalKey);

    /// <inheritdoc/>
    public override int GetHashCode() => HashCode.Combine(Tonic, IsMinor, Confidence);

    /// <summary>The key's name and Camelot code, for example "F#m (11A)".</summary>
    public override string ToString() => $"{Name} ({Camelot})";

    private unsafe FaKeyInfo ToNative() => new()
    {
        StructSize = (uint)sizeof(FaKeyInfo),
        Tonic = Tonic,
        Minor = IsMinor ? 1 : 0,
        Confidence = Confidence,
    };

    private static unsafe MusicalKey FromNative(in FaKeyInfo k)
    {
        fixed (byte* name = k.Name)
        fixed (byte* camelot = k.Camelot)
            return new MusicalKey(k.Tonic, k.Minor != 0, k.Confidence,
                Marshal.PtrToStringUTF8((nint)name) ?? "", Marshal.PtrToStringUTF8((nint)camelot) ?? "");
    }

    internal static unsafe MusicalKey? FromAnalysis(nint analysis)
    {
        FaKeyInfo k = default;
        k.StructSize = (uint)sizeof(FaKeyInfo);
        int rc = NativeMethods.fa_analysis_get_key(analysis, &k);
        if (rc == (int)ErrorCode.NotFound) return null;
        FatzAudioException.ThrowIfFailed(rc);
        return FromNative(k);
    }
}

/// <summary>One 10 ms slice of a track's waveform overview (<see cref="TrackAnalysis.Waveform"/>), all levels linear 0..1.</summary>
/// <param name="Peak">The highest sample of all channels.</param>
/// <param name="Low">RMS level below 250 Hz (a sine at full scale is 1).</param>
/// <param name="Mid">RMS level from 250 Hz to 2.5 kHz.</param>
/// <param name="High">RMS level above 2.5 kHz.</param>
[StructLayout(LayoutKind.Sequential)]   // read straight from the native fa_waveform_point (ReadWaveform)
public readonly record struct WaveformPoint(float Peak, float Low, float Mid, float High)
{
    /// <summary>The length of audio each point covers.</summary>
    public static TimeSpan Interval { get; } = TimeSpan.FromMilliseconds(10);
}

/// <summary>
/// What <see cref="AudioEngine.Analyze"/> and <see cref="AudioStream.Analyze"/> found out about a track. A plain object:
/// everything is copied out of the native result, so there is nothing to dispose.
/// </summary>
/// <example>
/// <code>
/// var t = await engine.AnalyzeAsync("song.mp3", AnalysisKinds.Beats | AnalysisKinds.Key | AnalysisKinds.Waveform);
/// Console.WriteLine($"{t.Beats?.Bpm:0.0} BPM, {t.Key?.Camelot}, {t.Waveform?.Length} waveform points");
/// </code>
/// </example>
public sealed class TrackAnalysis
{
    internal TrackAnalysis(AnalysisKinds kinds, bool fromCache, int sampleRate, long length, BeatGrid? beats, MusicalKey? key,
        WaveformPoint[]? waveform, Loudness? loudness)
    {
        Kinds = kinds;
        FromCache = fromCache;
        SampleRate = sampleRate;
        Length = length;
        Beats = beats;
        Key = key;
        Waveform = waveform;
        Loudness = loudness;
    }

    /// <summary>The kinds this result holds.</summary>
    public AnalysisKinds Kinds { get; }

    /// <summary>True when the result was read from the cache (<see cref="AudioEngineOptions.CacheDirectory"/>) instead of measured now.</summary>
    public bool FromCache { get; }

    /// <summary>Sample rate in Hz of the audio, and of the frames in <see cref="Beats"/>.</summary>
    public int SampleRate { get; }

    /// <summary>Frames of audio analysed.</summary>
    public long Length { get; }

    /// <summary>The beat grid; null when <see cref="AnalysisKinds.Beats"/> was not asked for, or the audio has no steady beat (silence, a few seconds of speech).</summary>
    public BeatGrid? Beats { get; }

    /// <summary>The key; null when <see cref="AnalysisKinds.Key"/> was not asked for, or the audio has none (silence, drums only).</summary>
    public MusicalKey? Key { get; }

    /// <summary>The waveform overview, one point per 10 ms (<see cref="WaveformPoint.Interval"/>); null when <see cref="AnalysisKinds.Waveform"/> was not asked for.</summary>
    public WaveformPoint[]? Waveform { get; }

    /// <summary>The loudness; null when <see cref="AnalysisKinds.Loudness"/> was not asked for.</summary>
    public Loudness? Loudness { get; }

    internal static unsafe FaAnalysisOptions NativeOptions(AnalysisKinds kinds, AnalysisOptions? options)
    {
        FaAnalysisOptions o = default;
        NativeMethods.fa_analysis_options_init(&o);
        o.Kinds = (uint)kinds;
        if (options is not null)
        {
            o.MinBpm = options.MinBpm;
            o.MaxBpm = options.MaxBpm;
            if (options.NoCache) o.Flags |= 1; // FA_ANALYZE_NO_CACHE
        }
        return o;
    }

    /// <summary>Copies everything out of a native analysis and frees it.</summary>
    internal static unsafe TrackAnalysis Take(nint analysis)
    {
        try
        {
            FaAnalysisInfo info = default;
            info.StructSize = (uint)sizeof(FaAnalysisInfo);
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_analysis_get_info(analysis, &info));
            var kinds = (AnalysisKinds)info.Kinds;
            BeatGrid? beats = (kinds & AnalysisKinds.Beats) != 0 ? BeatGrid.FromAnalysis(analysis) : null;
            MusicalKey? key = (kinds & AnalysisKinds.Key) != 0 ? MusicalKey.FromAnalysis(analysis) : null;
            WaveformPoint[]? waveform = (kinds & AnalysisKinds.Waveform) != 0 ? ReadWaveform(analysis) : null;
            Loudness? loudness = null;
            if ((kinds & AnalysisKinds.Loudness) != 0)
            {
                FatzAudioException.ThrowIfFailed(NativeMethods.fa_analysis_get_loudness(analysis, out FaLoudness l));
                loudness = FatzAudio.Loudness.From(l);
            }
            return new TrackAnalysis(kinds, (info.Flags & 1) != 0 /* FA_ANALYSIS_FROM_CACHE */, (int)info.SampleRate, info.Length,
                beats, key, waveform, loudness);
        }
        finally
        {
            NativeMethods.fa_analysis_free(analysis);
        }
    }

    private static unsafe WaveformPoint[] ReadWaveform(nint analysis)
    {
        uint count = 0;
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_analysis_get_waveform(analysis, null, 0, &count));
        var points = new WaveformPoint[count];
        // Read in place: WaveformPoint is FaWaveformPoint's four floats, in the same order.
        if (count > 0)
            fixed (WaveformPoint* p = points)
                FatzAudioException.ThrowIfFailed(NativeMethods.fa_analysis_get_waveform(analysis, (FaWaveformPoint*)p, count, &count));
        if (count < points.Length) Array.Resize(ref points, (int)count);
        return points;
    }

    internal static unsafe TrackAnalysis Run(EngineHandle engine, string path, AnalysisKinds kinds, AnalysisOptions? options)
    {
        var o = NativeOptions(kinds, options);
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_analyze_path(engine, path, &o, out nint analysis));
        return Take(analysis);
    }

    internal static unsafe TrackAnalysis Run(StreamHandle stream, AnalysisKinds kinds, AnalysisOptions? options)
    {
        var o = NativeOptions(kinds, options);
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_analyze_stream(stream, &o, out nint analysis));
        return Take(analysis);
    }

    /// <summary>Runs <paramref name="work"/> on a pool thread while holding <paramref name="handle"/> alive, so disposing its owner meanwhile is safe.</summary>
    internal static Task<TrackAnalysis> RunAsync<T>(T handle, Func<T, TrackAnalysis> work, CancellationToken cancellationToken)
        where T : SafeHandle
    {
        cancellationToken.ThrowIfCancellationRequested();
        Native.HandleRef.AddRef(handle);
        var task = Task.Run(() =>
        {
            try { return work(handle); }
            finally { handle.DangerousRelease(); }
        }, CancellationToken.None);
        return cancellationToken.CanBeCanceled ? task.WaitAsync(cancellationToken) : task;
    }
}
