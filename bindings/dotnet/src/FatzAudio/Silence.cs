using FatzAudio.Native;

namespace FatzAudio;

/// <summary>How silence detection measures the level.</summary>
public enum SilenceMode
{
    /// <summary>
    /// The RMS level over windows of <see cref="SilenceOptions.Window"/> (the default, for trimming). Audible windows in a row
    /// that last less than <see cref="SilenceOptions.MinSound"/> count as silence, so a lone click, a crackle or a count-in tick
    /// is not audio, while a soft fade-in counts from the first window that reaches the threshold. Times fall on window edges.
    /// </summary>
    Rms = 0,

    /// <summary>The sample peak, frame by frame: exact to the frame, and a single click counts.</summary>
    Peak = 1,
}

/// <summary>
/// Settings for silence detection (<see cref="AudioStream.DetectSilence"/>, <see cref="AnalysisKinds.Silence"/>,
/// <see cref="SilenceDetector"/>). Audio is audible when its level (<see cref="Mode"/>) in any channel is at or above
/// <see cref="ThresholdDb"/>; a silence region is a stretch that is not, at least <see cref="MinLength"/> long.
/// </summary>
public sealed record SilenceOptions
{
    /// <summary>How the level is measured (default <see cref="SilenceMode.Rms"/>).</summary>
    public SilenceMode Mode { get; init; } = SilenceMode.Rms;

    /// <summary>The threshold in dBFS (-120..0, default -50). For <see cref="SilenceMode.Rms"/> a full-scale sine reads -3 dB.</summary>
    public float ThresholdDb { get; init; } = -50f;

    /// <summary>The shortest silence that counts as a region (1 ms..1 hour, default 1 s).</summary>
    public TimeSpan MinLength { get; init; } = TimeSpan.FromSeconds(1);

    /// <summary>The RMS window (10 ms..1 s, default 50 ms); not used by <see cref="SilenceMode.Peak"/>.</summary>
    public TimeSpan Window { get; init; } = TimeSpan.FromMilliseconds(50);

    /// <summary>
    /// The shortest sound that counts as audio (0..10 s, default 200 ms; zero counts every audible window); not used by
    /// <see cref="SilenceMode.Peak"/>.
    /// </summary>
    public TimeSpan MinSound { get; init; } = TimeSpan.FromMilliseconds(200);

    internal static uint ToMilliseconds(TimeSpan t)
    {
        double ms = Math.Round(t.TotalMilliseconds);
        return ms >= 0 && ms <= uint.MaxValue ? (uint)ms : 0; // out of range: the library rejects 0 (a length or window)
    }

    internal static unsafe FaSilenceOptions ToNative(SilenceOptions? options)
    {
        FaSilenceOptions o = default;
        NativeMethods.fa_silence_options_init(&o);
        if (options is not null)
        {
            o.ThresholdDb = options.ThresholdDb;
            o.MinLengthMs = ToMilliseconds(options.MinLength);
            o.Mode = (uint)options.Mode;
            o.WindowMs = ToMilliseconds(options.Window);
            double sound = Math.Round(options.MinSound.TotalMilliseconds);
            o.MinSoundMs = sound >= 0 && sound <= uint.MaxValue ? (uint)sound : uint.MaxValue; // 0 is valid here: out of range is rejected as too long
        }
        return o;
    }
}

/// <summary>A stretch of silence, in frames of <see cref="SilenceInfo.SampleRate"/>.</summary>
/// <param name="Start">First silent frame.</param>
/// <param name="End">The frame after the last silent one.</param>
public readonly record struct SilenceRegion(long Start, long End)
{
    /// <summary>Frames in the region.</summary>
    public long Length => End - Start;
}

/// <summary>
/// Where a song is silent: the first and last audible frame and the silence regions. Frames are the stream's source
/// frames (the unit of markers), so the audible part plays with the cue-in and cue-out markers set to
/// <see cref="FirstAudibleTime"/> and <see cref="LastAudibleTime"/>.
/// </summary>
/// <example>
/// <code>
/// using var song = engine.OpenFile("song.mp3");
/// var s = song.DetectSilence();                 // a few ms: the start, and the end back to the last sound
/// song.Markers.CueIn = s.FirstAudibleTime;      // null (nothing audible) leaves the marker unset
/// song.Markers.CueOut = s.LastAudibleTime;
/// song.PlayOn(output);
/// </code>
/// </example>
public sealed class SilenceInfo
{
    internal unsafe delegate int Getter(FaSilence* silence, FaSilenceRegion* regions, uint capacity, uint* count);

    private SilenceInfo(FaSilence s, SilenceRegion[] regions)
    {
        IsComplete = (s.Flags & 1) != 0;   // FA_SILENCE_COMPLETE
        FromCache = (s.Flags & 2) != 0;    // FA_SILENCE_FROM_CACHE
        SampleRate = (int)s.SampleRate;
        FirstAudible = s.FirstAudible >= 0 ? s.FirstAudible : null;
        LastAudible = s.LastAudible >= 0 ? s.LastAudible : null;
        Length = s.Length;
        Regions = Array.AsReadOnly(regions);
        Mode = (SilenceMode)s.Mode;
        ThresholdDb = s.ThresholdDb;
        MinLength = TimeSpan.FromMilliseconds(s.MinLengthMs);
        Window = TimeSpan.FromMilliseconds(s.WindowMs);
        MinSound = TimeSpan.FromMilliseconds(s.MinSoundMs);
    }

    /// <summary>The mode that produced this result (a cached result has the one it was measured with).</summary>
    public SilenceMode Mode { get; }

    /// <summary>The threshold in dBFS that produced this result.</summary>
    public float ThresholdDb { get; }

    /// <summary>The shortest silence counted as a region.</summary>
    public TimeSpan MinLength { get; }

    /// <summary>The RMS window; zero for <see cref="SilenceMode.Peak"/>.</summary>
    public TimeSpan Window { get; }

    /// <summary>The shortest sound counted as audio; zero for <see cref="SilenceMode.Peak"/>.</summary>
    public TimeSpan MinSound { get; }

    /// <summary>The first audible frame (for the cue-in marker); null when nothing is audible.</summary>
    public long? FirstAudible { get; }

    /// <summary>The frame after the last audible one (for the cue-out marker); null when nothing is audible.</summary>
    public long? LastAudible { get; }

    /// <summary><see cref="FirstAudible"/> as time from the start; null when nothing is audible.</summary>
    public TimeSpan? FirstAudibleTime => FirstAudible is long f ? ToTime(f) : null;

    /// <summary><see cref="LastAudible"/> as time from the start; null when nothing is audible.</summary>
    public TimeSpan? LastAudibleTime => LastAudible is long l ? ToTime(l) : null;

    /// <summary>Sample rate in Hz of the frames.</summary>
    public int SampleRate { get; }

    /// <summary>Frames measured: the stream's length (a <see cref="SilenceDetector"/>: the frames fed so far).</summary>
    public long Length { get; }

    /// <summary>
    /// True when <see cref="Regions"/> covers the whole stream. False when only the leading and trailing silence is known
    /// (<see cref="AudioStream.DetectSilence"/> without a cached analysis, on a long file), and always for a <see cref="SilenceDetector"/>.
    /// </summary>
    public bool IsComplete { get; }

    /// <summary>True when the result came from the analysis cache instead of being measured now.</summary>
    public bool FromCache { get; }

    /// <summary>The silence regions, in order.</summary>
    public IReadOnlyList<SilenceRegion> Regions { get; }

    /// <summary>A frame count of this result as time.</summary>
    /// <param name="frames">Frames of <see cref="SampleRate"/>.</param>
    /// <returns>The time.</returns>
    public TimeSpan ToTime(long frames) =>
        SampleRate > 0 ? TimeSpan.FromTicks((long)Math.Round(frames * (double)TimeSpan.TicksPerSecond / SampleRate)) : TimeSpan.Zero;

    internal static unsafe SilenceInfo Read(Getter get)
    {
        // One call in the usual case: fa_stream_get_silence decodes on every call, and a second call could answer
        // from a cache written in between, so the info and the regions always come from the same call.
        var s = NativeSized.New<FaSilence>();
        uint count = 0;
        var native = new FaSilenceRegion[16];
        fixed (FaSilenceRegion* p = native)
            FatzAudioException.ThrowIfFailed(get(&s, p, (uint)native.Length, &count));
        if (count > native.Length)
        {
            native = new FaSilenceRegion[count];
            s = NativeSized.New<FaSilence>();
            fixed (FaSilenceRegion* p = native)
                FatzAudioException.ThrowIfFailed(get(&s, p, count, &count));
        }
        var regions = new SilenceRegion[Math.Min(count, (uint)native.Length)];
        for (int i = 0; i < regions.Length; i++) regions[i] = new SilenceRegion(native[i].Start, native[i].End);
        return new SilenceInfo(s, regions);
    }
}

/// <summary>
/// The silence detector for audio FatzAudio cannot read twice: PCM your app pushes (<see cref="PushAudioStream"/>, video audio
/// decoded with FFmpeg) or reads from a decode-only stream. Feed it the same frames; the rule and the results are those of
/// <see cref="AudioStream.DetectSilence"/>, with frames counted from the first one fed. Not thread safe.
/// </summary>
/// <example>
/// <code>
/// using var detector = new SilenceDetector(push.Format);
/// // wherever you push:
/// push.Push(samples);
/// detector.Feed(samples);
/// var s = detector.GetResult();   // what the audio so far shows
/// </code>
/// </example>
public sealed unsafe class SilenceDetector : IDisposable
{
    private readonly SilenceDetectorHandle _handle;

    /// <summary>Creates a detector for PCM in <paramref name="format"/>.</summary>
    /// <param name="format">The format of the frames you will feed: any sample format, 1..32 channels.</param>
    /// <param name="options">Mode, threshold and lengths; null uses the defaults.</param>
    /// <exception cref="ArgumentNullException"><paramref name="format"/> is null.</exception>
    /// <exception cref="FatzAudioException">The format or the options are invalid (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    public SilenceDetector(AudioFormat format, SilenceOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(format);
        Format = format;
        var f = format.ToNative();
        var o = SilenceOptions.ToNative(options);
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_silence_detector_create(&f, &o, out nint native));
        _handle = new SilenceDetectorHandle();
        _handle.Initialize(native);
    }

    /// <summary>The format the detector reads.</summary>
    public AudioFormat Format { get; }

    /// <summary>Feeds interleaved 32-bit float frames (<see cref="SampleFormat.Float32"/>).</summary>
    /// <param name="samples">Interleaved samples; the length must be a multiple of the channel count.</param>
    /// <exception cref="InvalidOperationException">The detector's format is not <see cref="SampleFormat.Float32"/>.</exception>
    /// <exception cref="ArgumentException">The sample count is not a multiple of the channel count.</exception>
    /// <exception cref="ObjectDisposedException">The detector was disposed.</exception>
    public void Feed(ReadOnlySpan<float> samples)
    {
        if (Format.SampleFormat != SampleFormat.Float32)
            throw new InvalidOperationException("This detector was not created with SampleFormat.Float32.");
        fixed (float* p = samples) FeedCore(p, samples.Length);
    }

    /// <summary>Feeds interleaved signed 16-bit frames (<see cref="SampleFormat.Int16"/>).</summary>
    /// <param name="samples">Interleaved samples; the length must be a multiple of the channel count.</param>
    /// <exception cref="InvalidOperationException">The detector's format is not <see cref="SampleFormat.Int16"/>.</exception>
    /// <exception cref="ArgumentException">The sample count is not a multiple of the channel count.</exception>
    /// <exception cref="ObjectDisposedException">The detector was disposed.</exception>
    public void Feed(ReadOnlySpan<short> samples)
    {
        if (Format.SampleFormat != SampleFormat.Int16)
            throw new InvalidOperationException("This detector was not created with SampleFormat.Int16.");
        fixed (short* p = samples) FeedCore(p, samples.Length);
    }

    /// <summary>
    /// Feeds frames in the detector's format from plane pointers, as <see cref="PushAudioStream.PushFrame"/> takes them
    /// (<c>AVFrame.extended_data</c>: one pointer per channel when planar, one for interleaved data).
    /// </summary>
    /// <param name="data">Pointer to the array of plane pointers.</param>
    /// <param name="frames">Samples per channel.</param>
    /// <exception cref="ArgumentException"><paramref name="data"/> is zero or <paramref name="frames"/> is negative.</exception>
    /// <exception cref="ObjectDisposedException">The detector was disposed.</exception>
    public void FeedFrame(nint data, int frames)
    {
        if (data == 0) throw new ArgumentException("The plane pointer array is null.", nameof(data));
        if (frames < 0) throw new ArgumentException("Frame count must not be negative.", nameof(frames));
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_silence_detector_feed(_handle, (void**)data, frames));
    }

    private void FeedCore(void* data, int sampleCount)
    {
        if (sampleCount % Format.Channels != 0)
            throw new ArgumentException("Sample count must be a multiple of the channel count.");
        void* plane = data;
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_silence_detector_feed(_handle, &plane, sampleCount / Format.Channels));
    }

    /// <summary>
    /// What the frames fed so far show. A silent run still going at the end is a region once it is long enough, and grows
    /// while more silence is fed; an RMS window still being filled counts with the frames it has.
    /// </summary>
    /// <returns>The result (never <see cref="SilenceInfo.IsComplete"/>: only your app knows when its audio is over).</returns>
    /// <exception cref="ObjectDisposedException">The detector was disposed.</exception>
    public SilenceInfo GetResult() =>
        SilenceInfo.Read((FaSilence* s, FaSilenceRegion* r, uint cap, uint* n) => NativeMethods.fa_silence_detector_get(_handle, s, r, cap, n));

    /// <summary>Frees the detector.</summary>
    public void Dispose() => _handle.Dispose();
}
