using FatzAudio.Native;

namespace FatzAudio;

/// <summary>Sample layout of PCM audio (<c>fa_sample_format</c>).</summary>
public enum SampleFormat
{
    /// <summary>Interleaved 32-bit float (the engine's internal format).</summary>
    Float32 = 0,
    /// <summary>Interleaved signed 16-bit.</summary>
    Int16 = 1,
    /// <summary>Interleaved packed signed 24-bit (3 bytes per sample).</summary>
    Int24 = 2,
    /// <summary>Interleaved signed 32-bit.</summary>
    Int32 = 3,
    /// <summary>Interleaved unsigned 8-bit.</summary>
    UInt8 = 4,
    /// <summary>Interleaved 64-bit float.</summary>
    Float64 = 5,
    /// <summary>Planar 32-bit float.</summary>
    Float32Planar = 6,
    /// <summary>Planar signed 16-bit.</summary>
    Int16Planar = 7,
    /// <summary>Planar signed 32-bit.</summary>
    Int32Planar = 8,
    /// <summary>Planar 64-bit float.</summary>
    Float64Planar = 9,
}

/// <summary>Describes a PCM format.</summary>
/// <param name="SampleFormat">Sample layout.</param>
/// <param name="SampleRate">Frames per second.</param>
/// <param name="Channels">Number of channels.</param>
/// <param name="ChannelMask">Speaker layout bit mask; 0 selects the default layout for the channel count.</param>
public sealed record AudioFormat(SampleFormat SampleFormat, int SampleRate, int Channels, ulong ChannelMask = 0)
{
    /// <summary>Creates an interleaved 32-bit float format.</summary>
    /// <param name="sampleRate">Frames per second.</param>
    /// <param name="channels">Number of channels.</param>
    /// <returns>The format.</returns>
    public static AudioFormat Float32(int sampleRate, int channels) => new(SampleFormat.Float32, sampleRate, channels);

    /// <summary>Creates an interleaved 16-bit integer format.</summary>
    /// <param name="sampleRate">Frames per second.</param>
    /// <param name="channels">Number of channels.</param>
    /// <returns>The format.</returns>
    public static AudioFormat Int16(int sampleRate, int channels) => new(SampleFormat.Int16, sampleRate, channels);

    internal FaAudioFormat ToNative() => new()
    {
        SampleFormat = (int)SampleFormat, SampleRate = (uint)SampleRate, Channels = (uint)Channels, ChannelMask = ChannelMask,
    };

    internal static AudioFormat FromNative(in FaAudioFormat f) =>
        new((SampleFormat)f.SampleFormat, (int)f.SampleRate, (int)f.Channels, f.ChannelMask);
}

/// <summary>Direction of an audio device.</summary>
public enum DeviceKind
{
    /// <summary>Playback (output) device.</summary>
    Playback = 1,
    /// <summary>Capture (input) device.</summary>
    Capture = 2,
}

/// <summary>An audio device reported by the engine.</summary>
/// <param name="Id">Stable backend id; pass the whole record to <see cref="AudioEngine.OpenOutput"/>.</param>
/// <param name="Name">Human-readable name.</param>
/// <param name="Kind">Playback or capture.</param>
/// <param name="IsDefault">True for the system default device.</param>
/// <param name="NativeSampleRate">Native sample rate of the device in Hz.</param>
/// <param name="NativeChannels">Native channel count of the device.</param>
public sealed record AudioDeviceInfo(string Id, string Name, DeviceKind Kind, bool IsDefault, int NativeSampleRate, int NativeChannels);

/// <summary>Playback state of an <see cref="AudioStream"/>.</summary>
public enum StreamState
{
    /// <summary>Not playing; position at the start or where it was stopped.</summary>
    Stopped = 0,
    /// <summary>Playing.</summary>
    Playing = 1,
    /// <summary>Paused.</summary>
    Paused = 2,
    /// <summary>Waiting for data (network or push streams).</summary>
    Buffering = 3,
    /// <summary>Playback reached the end of the source.</summary>
    Ended = 4,
}

/// <summary>Parameters that can be set or ramped on a stream.</summary>
public enum AudioParameter
{
    /// <summary>Linear gain (1 = unity).</summary>
    Volume = 0,
    /// <summary>Stereo pan from -1 (left) to 1 (right).</summary>
    Pan = 1,
    /// <summary>Playback rate multiplier; pitch follows.</summary>
    Rate = 2,
    /// <summary>Tempo multiplier; pitch preserved.</summary>
    Tempo = 3,
    /// <summary>Pitch shift in semitones; tempo preserved.</summary>
    Pitch = 4,
}

/// <summary>Loudness normalization mode of the engine.</summary>
public enum NormalizeMode
{
    /// <summary>No normalization.</summary>
    Off = 0,
    /// <summary>Per-stream integrated loudness.</summary>
    Track = 1,
    /// <summary>Use album gain tags when present.</summary>
    Album = 2,
}

/// <summary>Result of a loudness measurement (EBU R128).</summary>
/// <param name="IntegratedLufs">Integrated loudness in LUFS.</param>
/// <param name="RangeLu">Loudness range in LU.</param>
/// <param name="TruePeakDbtp">True peak in dBTP.</param>
/// <param name="IsFinal">False while the value is only an estimate because scanning is still in progress.</param>
public sealed record Loudness(double IntegratedLufs, double RangeLu, double TruePeakDbtp, bool IsFinal);

/// <summary>Peak and RMS levels per channel, linear 0..1.</summary>
/// <param name="Peak">Peak level per channel.</param>
/// <param name="Rms">RMS level per channel.</param>
public sealed record StreamLevels(float[] Peak, float[] Rms);

/// <summary>Static information about an opened stream.</summary>
/// <param name="Format">Decoded output format.</param>
/// <param name="Codec">Codec name such as <c>mp3</c> or <c>flac</c>.</param>
/// <param name="Container">Container name such as <c>mp4</c> or <c>ogg</c>.</param>
/// <param name="Duration">Total duration, or null when unknown (live streams).</param>
/// <param name="LengthFrames">Length in frames, or -1 when unknown.</param>
/// <param name="CanSeek">True when <see cref="AudioStream.Position"/> can be set.</param>
/// <param name="AudioTrackCount">Number of audio tracks in the container.</param>
/// <param name="HasCdg">True when a CD+G graphics side stream is present.</param>
public sealed record StreamInfo(
    AudioFormat Format, string Codec, string Container, TimeSpan? Duration, long LengthFrames,
    bool CanSeek, int AudioTrackCount, bool HasCdg);

/// <summary>Options for opening a stream. All properties are optional.</summary>
/// <example>
/// <code>
/// using var stream = engine.OpenFile("song.flac", new StreamOptions { Loop = true, Prescan = true });
/// </code>
/// </example>
public sealed record StreamOptions
{
    /// <summary>Decode only: the stream is not attached to a device and is consumed with <see cref="AudioStream.Read"/>.</summary>
    public bool DecodeOnly { get; init; }
    /// <summary>Loop back to the start when the end is reached.</summary>
    public bool Loop { get; init; }
    /// <summary>Build an exact seek index and length before the open call returns (slower to open).</summary>
    public bool Prescan { get; init; }
    /// <summary>Opt this stream out of engine loudness normalization.</summary>
    public bool NoNormalize { get; init; }
    /// <summary>Audio track to use in multi-track containers; -1 selects the default.</summary>
    public int AudioTrack { get; init; } = -1;
    /// <summary>Optional MIME type hint; content sniffing still wins.</summary>
    public string? MimeHint { get; init; }
    /// <summary>
    /// How much audio is decoded ahead on worker threads while the stream plays (default 500 ms,
    /// allowed 50 ms to 10 s). Larger values ride out slow disks and networks; smaller values use less memory.
    /// </summary>
    public TimeSpan? BufferLength { get; init; }
}

/// <summary>Configuration for <see cref="AudioEngine.Create"/>. All properties are optional.</summary>
public sealed record AudioEngineOptions
{
    /// <summary>Internal mix sample rate in Hz; 0 uses the device's native rate.</summary>
    public int MixSampleRate { get; init; }
    /// <summary>Device period length; null uses the backend default.</summary>
    public TimeSpan? Period { get; init; }
    /// <summary>Decode worker threads; 0 uses the number of cores (at most 8).</summary>
    public int WorkerThreads { get; init; }
    /// <summary>Directory for the loudness and seek-index cache; null disables the cache.</summary>
    public string? CacheDirectory { get; init; }
    /// <summary>
    /// Output devices play into a silent output that still runs in real time. Use it for automated
    /// tests, build servers without audio hardware, and headless services.
    /// </summary>
    public bool NullOutput { get; init; }
}

/// <summary>A format the built-in decoder registry can open (see <see cref="AudioEngine.SupportedFormats"/>).</summary>
/// <param name="Name">Registry name, for example <c>flac</c>.</param>
/// <param name="Description">Human-readable description.</param>
/// <param name="Extensions">Typical file extensions, without dots.</param>
/// <param name="MimeTypes">MIME types of the format.</param>
public sealed record AudioFormatInfo(string Name, string Description, IReadOnlyList<string> Extensions, IReadOnlyList<string> MimeTypes);

internal static class TimeConvert
{
    public static TimeSpan FromNanoseconds(long ns) => TimeSpan.FromTicks(ns / 100);
    public static long ToNanoseconds(TimeSpan t) => t.Ticks * 100;

    public static TimeSpan FromFrames(long frames, int sampleRate) =>
        sampleRate <= 0 ? TimeSpan.Zero : TimeSpan.FromTicks((long)((double)frames * TimeSpan.TicksPerSecond / sampleRate));

    public static long ToFrames(TimeSpan t, int sampleRate) =>
        (long)Math.Round(t.TotalSeconds * sampleRate);
}
