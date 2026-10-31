using System.Runtime.InteropServices;
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
    /// <summary>Playback reached the end of the source (or its cue-out, or a crossfade faded it out).</summary>
    Ended = 4,
    /// <summary>
    /// Waiting to start: <see cref="AudioStream.PlayAt"/>, <see cref="AudioStream.CrossfadeTo"/> or a queue
    /// (<see cref="AudioStream.QueueNext"/>). <see cref="AudioStream.Started"/> is raised when it starts.
    /// </summary>
    Scheduled = 5,
}

/// <summary>The markers of a stream (see <see cref="StreamMarkers"/>).</summary>
public enum MarkerKind
{
    /// <summary>Where playback starts.</summary>
    CueIn = 0,
    /// <summary>Where playback ends.</summary>
    CueOut = 1,
    /// <summary>Where the fade-in is complete.</summary>
    FadeInEnd = 2,
    /// <summary>Where the fade-out begins.</summary>
    FadeOutStart = 3,
    /// <summary>Where the queued next stream starts.</summary>
    MixPoint = 4,
}

/// <summary>Parameters that can be set or ramped on a stream.</summary>
public enum AudioParameter
{
    /// <summary>Linear gain (1 = unity).</summary>
    Volume = 0,
    /// <summary>Stereo pan from -1 (left) to 1 (right).</summary>
    Pan = 1,
    /// <summary>Playback speed multiplier (0.1..10); pitch follows, like a turntable.</summary>
    Rate = 2,
    /// <summary>Tempo multiplier (0.1..10); pitch preserved.</summary>
    Tempo = 3,
    /// <summary>Pitch shift in semitones (-36..36); tempo preserved.</summary>
    Pitch = 4,
}

/// <summary>How tempo and pitch are processed (see <see cref="TempoOptions"/>).</summary>
public enum StretchQuality
{
    /// <summary>120 ms analysis blocks, the cleanest sound (the default).</summary>
    Best = 0,
    /// <summary>100 ms blocks, about a third less CPU, slightly less clean.</summary>
    Fast = 1,
    /// <summary><see cref="Best"/> with the original audio played around each onset (drum hits, plucks): no pre-echo
    /// before hits, which stay sharp and on time. For drum-heavy material; a note starting under a held chord can get
    /// a faint seam.</summary>
    Transients = 2,
}

/// <summary>How a stream processes <see cref="AudioParameter.Tempo"/>, <see cref="AudioParameter.Pitch"/> and <see cref="AudioParameter.Rate"/>.</summary>
public sealed record TempoOptions
{
    /// <summary>Stretcher quality (default <see cref="StretchQuality.Best"/>).</summary>
    public StretchQuality Quality { get; init; } = StretchQuality.Best;
    /// <summary>A pitch change keeps the character of voices: karaoke key changes without the "chipmunk" sound.</summary>
    public bool KeepFormants { get; init; }
    /// <summary>
    /// How far ahead the stream is decoded while tempo, pitch or rate is in use, so how soon a change is heard
    /// (50 ms up to the engine's buffer; default 150 ms).
    /// </summary>
    public TimeSpan Latency { get; init; } = TimeSpan.FromMilliseconds(150);
}

/// <summary>Loudness normalization mode of the engine (see <see cref="AudioEngine.SetNormalization"/>).</summary>
public enum NormalizeMode
{
    /// <summary>No normalization (the default).</summary>
    Off = 0,
    /// <summary>Every stream plays at the target loudness.</summary>
    Track = 1,
    /// <summary>
    /// Album gain from ReplayGain or R128 tags when present, so an album's quiet and loud songs keep
    /// their balance; streams without album tags are normalized per track.
    /// </summary>
    Album = 2,
}

/// <summary>Where a <see cref="Loudness"/> value came from.</summary>
public enum LoudnessSource
{
    /// <summary>Nothing is known yet.</summary>
    None = 0,
    /// <summary>A running estimate from the audio decoded so far (live streams, or while a scan runs).</summary>
    Estimate = 1,
    /// <summary>ReplayGain 2 or R128 tags in the file.</summary>
    Tags = 2,
    /// <summary>An earlier scan of the same content, from <see cref="AudioEngineOptions.CacheDirectory"/>.</summary>
    Cache = 3,
    /// <summary>A full scan of the file.</summary>
    Scan = 4,
    /// <summary>A value the app supplied with <see cref="AudioStream.SetLoudness"/>.</summary>
    App = 5,
}

/// <summary>When the output limiter runs (see <see cref="AudioEngine.SetLimiter"/>).</summary>
public enum LimiterMode
{
    /// <summary>On while normalization is on (the default).</summary>
    Auto = 0,
    /// <summary>Always on.</summary>
    On = 1,
    /// <summary>Always off.</summary>
    Off = 2,
}

/// <summary>Loudness of a stream or file (ITU-R BS.1770-4 / EBU R128). Unknown values are <see cref="double.NaN"/>.</summary>
/// <param name="IntegratedLufs">Integrated loudness in LUFS; negative infinity for digital silence.</param>
/// <param name="RangeLu">Loudness range in LU (EBU Tech 3342).</param>
/// <param name="TruePeakDbtp">True peak in dBTP (from tags: the sample peak in dBFS).</param>
/// <param name="IsFinal">False while the value is only an estimate (or nothing is known yet).</param>
/// <param name="Source">Where the value came from.</param>
/// <param name="GainDb">The gain normalization applies to the stream with the engine's current settings, in dB (0 when off or opted out).</param>
public sealed record Loudness(double IntegratedLufs, double RangeLu, double TruePeakDbtp, bool IsFinal, LoudnessSource Source, double GainDb)
{
    internal static Loudness From(in Native.FaLoudness l) =>
        new(l.IntegratedLufs, l.RangeLu, l.TruePeakDbtp, l.IsFinal != 0, (LoudnessSource)l.Source, l.GainDb);
}

/// <summary>Peak and RMS levels per channel, linear 0..1.</summary>
/// <param name="Peak">Peak level per channel: left and right, or one value for both.</param>
/// <param name="Rms">RMS level per channel.</param>
public sealed record StreamLevels(float[] Peak, float[] Rms)
{
    internal static readonly TimeSpan DefaultWindow = TimeSpan.FromMilliseconds(50);
}

/// <summary>Options for <see cref="AudioStream.GetSpectrum(int, SpectrumOptions)"/> and <see cref="OutputDevice.GetSpectrum(int, SpectrumOptions)"/>.</summary>
[Flags]
public enum SpectrumOptions
{
    /// <summary>Both channels mixed, with a Hann window.</summary>
    None = 0,
    /// <summary>The left channel only.</summary>
    Left = 1,
    /// <summary>The right channel only.</summary>
    Right = 2,
    /// <summary>No window (sharper peaks for tones on a bin, more leakage for everything else).</summary>
    NoWindow = 4,
}

/// <summary>A magnitude spectrum: bin <c>k</c> is centred on <c>k * BinHz</c>; a full-scale sine reads 1.0 in its bin.</summary>
/// <param name="Magnitudes">Half the transform size of linear magnitudes, DC first.</param>
/// <param name="BinHz">Width of one bin in Hz; 0 while nothing plays.</param>
public sealed record Spectrum(float[] Magnitudes, float BinHz);

/// <summary>Static information about an opened stream.</summary>
/// <param name="Format">Decoded output format.</param>
/// <param name="Codec">Codec name such as <c>mp3</c> or <c>flac</c>.</param>
/// <param name="Container">Container name such as <c>mp4</c> or <c>ogg</c>.</param>
/// <param name="Duration">Total duration, or null when unknown (live streams).</param>
/// <param name="LengthFrames">Length in frames, or -1 when unknown.</param>
/// <param name="CanSeek">True when <see cref="AudioStream.Position"/> can be set.</param>
/// <param name="AudioTrackCount">Number of audio tracks in the container.</param>
/// <param name="HasCdg">True when a CD+G graphics side stream is present.</param>
/// <param name="FileSize">Bytes of the file (of the ZIP for an MP3+G archive), or null when unknown (live web streams).</param>
/// <param name="AudioBytes">
/// Bytes of this audio stream's coded data, without tags, headers and other tracks, or null when unknown. Exact
/// for WAV, AIFF, CD and MP4; close for MP3, AAC, AC-3, FLAC, Ogg and single-track Matroska.
/// </param>
/// <param name="Bitrate">Average bits per second (<see cref="AudioBytes"/> over the duration, else the codec's nominal rate); 0 when unknown.</param>
/// <param name="BitsPerSample">Bit depth of PCM and lossless sources (WAV, AIFF, FLAC, ALAC, CD); 0 for lossy codecs.</param>
public sealed record StreamInfo(
    AudioFormat Format, string Codec, string Container, TimeSpan? Duration, long LengthFrames,
    bool CanSeek, int AudioTrackCount, bool HasCdg, long? FileSize = null, long? AudioBytes = null, int Bitrate = 0, int BitsPerSample = 0)
{
    internal static unsafe StreamInfo FromNative(in Native.FaStreamInfo n)
    {
        TimeSpan? duration = n.Duration >= 0 ? TimeConvert.FromNanoseconds(n.Duration) : null;
        fixed (byte* codec = n.Codec)
        fixed (byte* container = n.Container)
            return new StreamInfo(
                AudioFormat.FromNative(n.Format),
                NativeText.Read(codec),
                NativeText.Read(container),
                duration, n.LengthFrames, n.Seekable != 0, n.AudioTrackCount, n.HasCdg != 0,
                n.FileSize >= 0 ? n.FileSize : null, n.AudioBytes >= 0 ? n.AudioBytes : null,
                (int)Math.Min(n.Bitrate, int.MaxValue), (int)n.BitsPerSample);
    }
}

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
    /// <summary>Opt this stream out of engine loudness normalization (jingles, sound effects, already-mastered mixes).</summary>
    public bool NoNormalize { get; init; }
    /// <summary>
    /// <see cref="AudioEngine.OpenFile"/>: do not look for a CD+G file with the same name next to the audio
    /// (for example <c>song.cdg</c> next to <c>song.mp3</c>). Graphics inside a ZIP archive are skipped too.
    /// </summary>
    public bool NoCdg { get; init; }
    /// <summary>Audio track to use in multi-track containers; -1 selects the default.</summary>
    public int AudioTrack { get; init; } = -1;
    /// <summary>Optional MIME type hint; content sniffing still wins.</summary>
    public string? MimeHint { get; init; }
    /// <summary>
    /// How much audio is decoded ahead on worker threads while the stream plays (default 500 ms,
    /// allowed 50 ms to 10 s). Larger values ride out slow disks and networks; smaller values use less memory.
    /// </summary>
    public TimeSpan? BufferLength { get; init; }
    /// <summary>Connection settings for <see cref="AudioEngine.OpenUrl(string, StreamOptions?)"/>; null uses the defaults. Ignored by the other open calls.</summary>
    public NetworkOptions? Network { get; init; }
    /// <summary>
    /// Dynamic range of Dolby Digital (AC-3) and Dolby Digital Plus (E-AC-3) audio (default
    /// <see cref="FatzAudio.DynamicRange.Full"/>). Other formats ignore it. Can be changed later with
    /// <see cref="AudioStream.DynamicRange"/>.
    /// </summary>
    public DynamicRange DynamicRange { get; init; } = DynamicRange.Full;
    /// <summary>
    /// MIDI files: the instruments to play them with; null uses <see cref="AudioEngine.DefaultSoundFont"/>.
    /// The stream keeps its own reference, so the soundfont may be disposed after the open call. Other formats ignore it.
    /// </summary>
    public SoundFont? SoundFont { get; init; }
    /// <summary>
    /// MIDI files: how their lyrics and other text are encoded. <see cref="TextCodePage.Detect"/> (the default) decides
    /// for the whole file between UTF-8, Shift-JIS and Windows-1252; the others force one. <see cref="MidiCredits.CodePage"/>
    /// says which was used. Other formats ignore it.
    /// </summary>
    public TextCodePage TextCodePage { get; init; } = TextCodePage.Detect;
    /// <summary>
    /// Stops an open that is unpacking a compressed song from a ZIP archive (the one long step of opening a file or
    /// memory block): the call then throws a <see cref="FatzAudioException"/> with <see cref="ErrorCode.Cancelled"/>,
    /// within about a megabyte of work, and no stream is created. A token that is already cancelled stops it at once.
    /// Other opens ignore it.
    /// </summary>
    public CancellationToken CancellationToken { get; init; }
}

/// <summary>Text encodings of MIDI files (<see cref="StreamOptions.TextCodePage"/>).</summary>
public enum TextCodePage
{
    /// <summary>Decide from the file's bytes: UTF-8 if all of it is, Shift-JIS if it reads as Japanese, else Windows-1252.</summary>
    Detect = 0,
    /// <summary>UTF-8 (text that is not valid UTF-8 is read as Windows-1252).</summary>
    Utf8 = 65001,
    /// <summary>Windows-1252 (Western European), for every event.</summary>
    Windows1252 = 1252,
    /// <summary>Shift-JIS (Windows code page 932, Japanese).</summary>
    ShiftJis = 932,
}

/// <summary>
/// Dynamic range of Dolby Digital (AC-3) and Dolby Digital Plus (E-AC-3) audio. These streams carry the
/// broadcaster's compression gains; the decoder can apply them or play the mix as it was mastered.
/// </summary>
public enum DynamicRange
{
    /// <summary>The full mix, no compression (the default).</summary>
    Full = 0,
    /// <summary>The stream's own compression ("line mode"): quiet parts lifted and loud ones tamed, as a home AV receiver does.</summary>
    Normal = 1,
    /// <summary>Heavy compression for listening at low volume ("RF mode"); streams without heavy gains use <see cref="Normal"/>'s.</summary>
    Night = 2,
}

/// <summary>Connection settings for web streams (<see cref="AudioEngine.OpenUrl(string, StreamOptions?)"/>). All properties are optional.</summary>
/// <example>
/// <code>
/// var opts = new StreamOptions
/// {
///     Network = new NetworkOptions
///     {
///         Headers = new Dictionary&lt;string, string&gt; { ["Authorization"] = "Bearer abc" },
///         Prebuffer = TimeSpan.FromSeconds(3),
///     },
/// };
/// using var s = engine.OpenUrl("https://example.com/live.m3u8", opts);
/// </code>
/// </example>
public sealed record NetworkOptions
{
    /// <summary>The User-Agent header; null sends <c>FatzAudio/&lt;version&gt;</c>.</summary>
    public string? UserAgent { get; init; }
    /// <summary>Extra request headers (cookies, authorization, ...), sent with every request including HLS segments;
    /// <c>Authorization</c> and <c>Cookie</c> only to the stream URL's own server (scheme, host and port).</summary>
    public IReadOnlyDictionary<string, string>? Headers { get; init; }
    /// <summary>
    /// Null uses the system's proxy setting (the http_proxy / https_proxy environment variables, and the
    /// Windows and macOS proxy settings); "" connects directly; otherwise "host:port" or
    /// "http://user:password@host:port".
    /// </summary>
    public string? Proxy { get; init; }
    /// <summary>How long connecting may take, and the longest the server may stay silent; null = 10 s.</summary>
    public TimeSpan? Timeout { get; init; }
    /// <summary>Audio buffered before playback starts, and before it resumes after a stall; null = 1 s.</summary>
    public TimeSpan? Prebuffer { get; init; }
    /// <summary>How long to keep reconnecting after the connection drops; null = 30 s.</summary>
    public TimeSpan? ReconnectFor { get; init; }
    /// <summary>Reconnect when the connection drops (default true). False ends the stream instead.</summary>
    public bool Reconnect { get; init; } = true;
    /// <summary>Ask radio servers for song titles (ICY metadata; default true).</summary>
    public bool SongTitles { get; init; } = true;
    /// <summary>HLS: play the best variant up to this many bits per second; 0 = the best one.</summary>
    public int HlsMaxBandwidth { get; init; }
}

/// <summary>What a web stream is playing (<see cref="AudioStream.Metadata"/>). Strings are empty when unknown.</summary>
/// <param name="StreamTitle">The song heard now as the station sends it (ICY StreamTitle), usually "Artist - Title".</param>
/// <param name="Artist"><paramref name="StreamTitle"/> up to its first " - ".</param>
/// <param name="Title">The rest; all of <paramref name="StreamTitle"/> when there is no " - ".</param>
/// <param name="Url">ICY StreamUrl (often album art or the station's page).</param>
/// <param name="Station">The station's name (icy-name).</param>
/// <param name="Genre">icy-genre.</param>
/// <param name="Description">icy-description.</param>
/// <param name="StationUrl">icy-url.</param>
/// <param name="ContentType">Content type of the audio, for example audio/mpeg.</param>
/// <param name="BitrateKbps">icy-br; 0 = unknown.</param>
/// <param name="Sequence">1, 2, ... for each title heard; 0 = none yet.</param>
public sealed record StreamMetadata(
    string StreamTitle, string Artist, string Title, string Url,
    string Station, string Genre, string Description, string StationUrl,
    string ContentType, int BitrateKbps, long Sequence)
{
    /// <summary>No metadata (streams that are not web streams).</summary>
    public static StreamMetadata Empty { get; } = new("", "", "", "", "", "", "", "", "", 0, 0);
}

/// <summary>The connection of a web stream (<see cref="AudioStream.NetworkStatus"/>).</summary>
/// <param name="Connected">True while a connection is open.</param>
/// <param name="HttpStatus">Status code of the last response; 0 before one.</param>
/// <param name="IsLive">No known end (radio, live HLS).</param>
/// <param name="IsHls">An HLS playlist.</param>
/// <param name="Seekable">Seeking works.</param>
/// <param name="Finished">Everything is downloaded, or reconnecting gave up.</param>
/// <param name="ContentLength">Bytes of the file; -1 = unknown or live.</param>
/// <param name="Downloaded">Bytes received over all connections.</param>
/// <param name="BufferedBytes">Downloaded and not decoded yet.</param>
/// <param name="Buffered">Audio ready ahead of playback, estimated from the bitrate.</param>
/// <param name="Reconnects">Connections re-established after a drop.</param>
/// <param name="LastError">Why the last connection failed; <see cref="ErrorCode.Ok"/> if none did.</param>
/// <param name="Url">The URL in use after redirects (HLS: the media playlist).</param>
public sealed record NetworkStatus(
    bool Connected, int HttpStatus, bool IsLive, bool IsHls, bool Seekable, bool Finished,
    long ContentLength, long Downloaded, long BufferedBytes, TimeSpan Buffered,
    int Reconnects, ErrorCode LastError, string Url);

/// <summary>Configuration for <see cref="AudioEngine.Create"/>. All properties are optional.</summary>
public sealed record AudioEngineOptions
{
    /// <summary>Internal mix sample rate in Hz; 0 uses the device's native rate.</summary>
    public int MixSampleRate { get; init; }
    /// <summary>Device period length; null uses the backend default.</summary>
    public TimeSpan? Period { get; init; }
    /// <summary>Decode worker threads; 0 uses the number of cores (at most 8).</summary>
    public int WorkerThreads { get; init; }
    /// <summary>
    /// Directory for the loudness scan cache (one small file per scanned file, keyed by its content, so
    /// renamed or moved files still hit); created if missing. Null disables the cache.
    /// </summary>
    public string? CacheDirectory { get; init; }
    /// <summary>
    /// Where compressed songs in ZIP archives (MP3+G) too big for <see cref="ZipMemoryLimitMb"/> are
    /// unpacked, in a <c>fatzaudio-tmp</c> folder inside it. The files are deleted when the stream
    /// closes, also if the app crashes. Null (the default) never writes temp files: such archives
    /// fail with <see cref="ErrorCode.TooLarge"/>. Uncompressed entries always play straight from the archive.
    /// </summary>
    public string? TempDirectory { get; init; }
    /// <summary>Compressed ZIP entries up to this many MB are unpacked into memory; 0 uses 256.</summary>
    public int ZipMemoryLimitMb { get; init; }
    /// <summary>
    /// Output devices play into a silent output that still runs in real time. Use it for automated
    /// tests, build servers without audio hardware, and headless services.
    /// </summary>
    public bool NullOutput { get; init; }
    /// <summary>
    /// MIDI files opened without a soundfont throw <see cref="ErrorCode.NoSoundFont"/> instead of playing with the
    /// OS's instruments (<see cref="SoundFont.FromSystem"/>).
    /// </summary>
    public bool NoSystemSoundFont { get; init; }
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
