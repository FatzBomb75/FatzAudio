using System.Runtime.InteropServices;
using FatzAudio.Native;

namespace FatzAudio;

/// <summary>
/// Entry point of the library: enumerates devices and opens output devices and audio streams.
/// Usually one engine exists per process. All members are thread-safe.
/// </summary>
/// <remarks>
/// Disposing the engine is always safe: devices and streams created from it keep the native engine
/// alive until they are disposed (or finalized) themselves, so using a stream after its engine was
/// disposed cannot crash.
/// </remarks>
/// <example>
/// <code>
/// using var engine = AudioEngine.Create();
/// using var output = engine.OpenOutput();           // system default device
/// using var song = engine.OpenFile("song.mp3");
/// song.PlayOn(output);
/// </code>
/// </example>
public sealed class AudioEngine : IDisposable
{
    private readonly EngineHandle _handle;

    private AudioEngine(EngineHandle handle) => _handle = handle;

    internal EngineHandle Handle
    {
        get
        {
            ObjectDisposedException.ThrowIf(_handle.IsClosed, this);
            return _handle;
        }
    }

    /// <summary>Creates an engine.</summary>
    /// <param name="options">Optional configuration; null uses the defaults.</param>
    /// <returns>A new engine that must be disposed.</returns>
    /// <exception cref="FatzAudioException">The native engine could not be created.</exception>
    /// <exception cref="DllNotFoundException">The native <c>fatzaudio</c> library could not be located.</exception>
    public static unsafe AudioEngine Create(AudioEngineOptions? options = null)
    {
        NativeLoader.EnsureInitialized();

        FaEngineConfig config = default;
        NativeMethods.fa_engine_config_init(&config);
        nint cachePtr = 0;
        try
        {
            if (options is not null)
            {
                config.MixSampleRate = (uint)Math.Max(0, options.MixSampleRate);
                config.PeriodMs = options.Period is { } p ? (uint)Math.Max(0, p.TotalMilliseconds) : 0;
                config.WorkerThreads = (uint)Math.Max(0, options.WorkerThreads);
                if (options.NullOutput) config.Flags |= 1; // FA_ENGINE_NULL_OUTPUT
                if (options.CacheDirectory is { } dir)
                {
                    cachePtr = Marshal.StringToCoTaskMemUTF8(dir);
                    config.CacheDirectory = (byte*)cachePtr;
                }
            }

            FatzAudioException.ThrowIfFailed(NativeMethods.fa_engine_create(&config, out var native));
            var handle = new EngineHandle();
            Marshal.InitHandle(handle, native);
            return new AudioEngine(handle);
        }
        finally
        {
            if (cachePtr != 0) Marshal.FreeCoTaskMem(cachePtr);
        }
    }

    /// <summary>Version of the loaded native library.</summary>
    public static Version NativeVersion
    {
        get
        {
            NativeLoader.EnsureInitialized();
            uint v = NativeMethods.fa_version();
            return new Version((int)(v >> 16), (int)((v >> 8) & 0xFF), (int)(v & 0xFF));
        }
    }

    /// <summary>
    /// Every format the native library can open. All formats are built in and detected from the
    /// content, so there is nothing to load or register (unlike BASS plugins).
    /// </summary>
    /// <example>
    /// <code>
    /// foreach (var f in AudioEngine.SupportedFormats)
    ///     Console.WriteLine($"{f.Name}: {f.Description} ({string.Join(", ", f.Extensions)})");
    /// </code>
    /// </example>
    public static unsafe IReadOnlyList<AudioFormatInfo> SupportedFormats
    {
        get
        {
            NativeLoader.EnsureInitialized();
            uint count = NativeMethods.fa_format_count();
            var list = new List<AudioFormatInfo>((int)count);
            for (uint i = 0; i < count; i++)
            {
                FatzAudioException.ThrowIfFailed(NativeMethods.fa_format_get_info(i, out FaFormatInfo f));
                list.Add(new AudioFormatInfo(
                    Marshal.PtrToStringUTF8((nint)f.Name) ?? "",
                    Marshal.PtrToStringUTF8((nint)f.Description) ?? "",
                    Split(Marshal.PtrToStringUTF8((nint)f.Extensions)),
                    Split(Marshal.PtrToStringUTF8((nint)f.MimeTypes))));
            }
            return list;

            static string[] Split(string? s) => string.IsNullOrEmpty(s) ? [] : s.Split(';');
        }
    }

    /// <summary>Playback devices currently reported by the backend. A fresh list is built on every access.</summary>
    /// <exception cref="ObjectDisposedException">The engine was disposed.</exception>
    /// <exception cref="FatzAudioException">The backend failed to enumerate devices.</exception>
    public IReadOnlyList<AudioDeviceInfo> OutputDevices => EnumerateDevices(DeviceKind.Playback);

    /// <summary>Capture devices currently reported by the backend. A fresh list is built on every access.</summary>
    /// <exception cref="ObjectDisposedException">The engine was disposed.</exception>
    /// <exception cref="FatzAudioException">The backend failed to enumerate devices.</exception>
    public IReadOnlyList<AudioDeviceInfo> InputDevices => EnumerateDevices(DeviceKind.Capture);

    private unsafe IReadOnlyList<AudioDeviceInfo> EnumerateDevices(DeviceKind kind)
    {
        var h = Handle;
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_device_count(h, (int)kind, out uint count));
        var list = new List<AudioDeviceInfo>((int)count);
        for (uint i = 0; i < count; i++)
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_device_get_info(h, (int)kind, i, out FaDeviceInfo info));
            list.Add(new AudioDeviceInfo(
                Marshal.PtrToStringUTF8((nint)info.Id) ?? "",
                Marshal.PtrToStringUTF8((nint)info.Name) ?? "",
                (DeviceKind)info.Type,
                info.IsDefault != 0,
                (int)info.NativeSampleRate,
                (int)info.NativeChannels));
        }
        return list;
    }

    /// <summary>Opens an output device.</summary>
    /// <param name="device">A device from <see cref="OutputDevices"/>, or null for the system default (which follows default-device changes).</param>
    /// <returns>The opened device; dispose it when done.</returns>
    /// <exception cref="ArgumentException"><paramref name="device"/> is not a playback device.</exception>
    /// <exception cref="ObjectDisposedException">The engine was disposed.</exception>
    /// <exception cref="FatzAudioException">The device could not be opened.</exception>
    public OutputDevice OpenOutput(AudioDeviceInfo? device = null)
    {
        if (device is { Kind: not DeviceKind.Playback })
            throw new ArgumentException("Not a playback device.", nameof(device));
        var h = Handle;
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_device_open(h, device?.Id, out nint native));
        var dh = new DeviceHandle();
        dh.Initialize(native, h);
        return new OutputDevice(this, dh, device);
    }

    /// <summary>Opens an audio file; the format is detected from its content.</summary>
    /// <param name="path">Path of the file.</param>
    /// <param name="options">Optional open options.</param>
    /// <returns>The stream; dispose it when done.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="path"/> is null.</exception>
    /// <exception cref="FatzAudioException">The file could not be opened or decoded (for example <see cref="ErrorCode.NotFound"/> or <see cref="ErrorCode.UnsupportedFormat"/>).</exception>
    /// <example>
    /// <code>
    /// using var s = engine.OpenFile("song.flac");
    /// Console.WriteLine(s.Info.Duration);
    /// </code>
    /// </example>
    public unsafe AudioStream OpenFile(string path, StreamOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(path);
        var h = Handle;
        using var opts = NativeOptions.Create(options);
        return AudioStream.Wrap(this, NativeMethods.fa_stream_open_path(h, path, opts.Pointer, out nint s), s, null);
    }

    /// <summary>Opens audio held in memory. The native library copies the data, so the buffer may be reused afterwards.</summary>
    /// <param name="data">Encoded audio file contents.</param>
    /// <param name="options">Optional open options.</param>
    /// <returns>The stream; dispose it when done.</returns>
    /// <exception cref="FatzAudioException">The data could not be decoded.</exception>
    /// <example>
    /// <code>
    /// using var s = engine.OpenMemory(File.ReadAllBytes("jingle.wav"));
    /// </code>
    /// </example>
    public unsafe AudioStream OpenMemory(ReadOnlyMemory<byte> data, StreamOptions? options = null)
    {
        var h = Handle;
        using var opts = NativeOptions.Create(options);
        using var pin = data.Pin();
        int rc = NativeMethods.fa_stream_open_memory(h, pin.Pointer, (nuint)data.Length, opts.Pointer, out nint s);
        return AudioStream.Wrap(this, rc, s, null);
    }

    /// <summary>Opens a managed <see cref="Stream"/> as audio source via custom I/O callbacks.</summary>
    /// <param name="source">A readable stream. Seeking and length are used when <see cref="Stream.CanSeek"/> is true.</param>
    /// <param name="options">Optional open options.</param>
    /// <param name="leaveOpen">If false (default), <paramref name="source"/> is disposed when the audio stream is disposed.</param>
    /// <returns>The stream; dispose it when done.</returns>
    /// <exception cref="ArgumentException"><paramref name="source"/> is not readable.</exception>
    /// <exception cref="FatzAudioException">The source could not be opened or decoded.</exception>
    /// <remarks>The native engine reads from <paramref name="source"/> on its own threads; reads are serialized.</remarks>
    /// <example>
    /// <code>
    /// using var s = engine.OpenStream(new MemoryStream(bytes));
    /// </code>
    /// </example>
    public unsafe AudioStream OpenStream(Stream source, StreamOptions? options = null, bool leaveOpen = false)
    {
        ArgumentNullException.ThrowIfNull(source);
        if (!source.CanRead) throw new ArgumentException("The stream must be readable.", nameof(source));
        var h = Handle;
        using var opts = NativeOptions.Create(options);

        var state = new IoState(source, leaveOpen);
        var gc = GCHandle.Alloc(state);
        FaIoCallbacks io = default;
        io.Read = &IoState.Read;
        io.Close = &IoState.Close;
        if (source.CanSeek)
        {
            io.Seek = &IoState.Seek;
            io.Length = &IoState.Length;
        }
        int rc = NativeMethods.fa_stream_open_io(h, &io, (void*)GCHandle.ToIntPtr(gc), opts.Pointer, out nint s);
        if (rc < 0)
        {
            gc.Free();
            throw new FatzAudioException((ErrorCode)rc);
        }
        return AudioStream.Wrap(this, rc, s, gc, state);
    }

    /// <summary>
    /// Opens what is behind an http:// or https:// URL: an audio file on a web server (seekable when the
    /// server supports byte ranges), an Icecast/Shoutcast radio station (song titles arrive as
    /// <see cref="AudioStream.MetadataChanged"/> when the song is heard), or an HLS playlist (.m3u8).
    /// The call connects and waits for the first few KB to detect the format; everything after that is
    /// downloaded on a thread of the stream, so playback never waits for the network.
    /// </summary>
    /// <param name="url">The URL.</param>
    /// <param name="options">Optional open options; <see cref="StreamOptions.Network"/> holds the connection settings.</param>
    /// <returns>The stream; dispose it when done.</returns>
    /// <exception cref="ArgumentException">Not an absolute http or https URL (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <exception cref="FatzAudioException">The server could not be reached (<see cref="ErrorCode.Network"/>), answered with an
    /// error (<see cref="ErrorCode.NotFound"/>), sent something that is not audio (<see cref="ErrorCode.UnsupportedFormat"/>),
    /// or HTTPS has no TLS stack on this system (<see cref="ErrorCode.ProviderMissing"/>).</exception>
    /// <example>
    /// <code>
    /// using var radio = engine.OpenUrl("https://example.com/stream.mp3");
    /// radio.MetadataChanged += (_, e) => Console.WriteLine($"Now playing: {e.Metadata.StreamTitle}");
    /// radio.PlayOn(output);
    /// </code>
    /// </example>
    public unsafe AudioStream OpenUrl(string url, StreamOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(url);
        var h = Handle;
        using var opts = NativeOptions.Create(options);
        int rc = NativeMethods.fa_stream_open_url(h, url, opts.Pointer, out nint s);
        if (rc == (int)ErrorCode.InvalidArgument) throw new ArgumentException("Not an absolute http or https URL.", nameof(url));
        return AudioStream.Wrap(this, rc, s, null);
    }

    /// <summary>Opens a network URL.</summary>
    /// <param name="url">The URL.</param>
    /// <param name="options">Optional open options.</param>
    /// <returns>The stream; dispose it when done.</returns>
    /// <exception cref="FatzAudioException">See <see cref="OpenUrl(string, StreamOptions?)"/>.</exception>
    public AudioStream OpenUrl(Uri url, StreamOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(url);
        return OpenUrl(url.AbsoluteUri, options);
    }

    /// <summary>Creates a stream that you feed with PCM data yourself.</summary>
    /// <param name="format">Format of the PCM data that will be pushed.</param>
    /// <param name="options">Optional open options.</param>
    /// <returns>A push stream; dispose it when done.</returns>
    /// <exception cref="FatzAudioException">The native library could not create the stream.</exception>
    /// <example>
    /// <code>
    /// using var push = engine.CreatePushStream(AudioFormat.Float32(48000, 2));
    /// push.PlayOn(output);
    /// push.Push(samples);   // interleaved floats
    /// </code>
    /// </example>
    public unsafe PushAudioStream CreatePushStream(AudioFormat format, StreamOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(format);
        var h = Handle;
        using var opts = NativeOptions.Create(options);
        var nf = format.ToNative();
        int rc = NativeMethods.fa_stream_open_push(h, &nf, opts.Pointer, out nint s);
        FatzAudioException.ThrowIfFailed(rc);
        var sh = new StreamHandle();
        sh.Initialize(s, h);
        return new PushAudioStream(this, sh, format);
    }

    /// <summary>
    /// Opens a stream for the compressed packets of one audio track, for apps that demux the file themselves
    /// (typically with FFmpeg, for the video). Push packets straight from <c>av_read_frame</c>; FatzAudio decodes
    /// them on its worker threads (PCM and MP3 built in, other codecs through the FFmpeg your app loaded) and
    /// <see cref="AudioStream.Clock"/> reports their timestamps as they leave the speaker.
    /// </summary>
    /// <param name="parameters">The track: codec name, rate, channels, codec configuration and time base.</param>
    /// <param name="options">Optional open options (<see cref="StreamOptions.Loop"/> is not allowed).</param>
    /// <returns>A packet stream; dispose it when done.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="parameters"/> is null.</exception>
    /// <exception cref="FatzAudioException">The codec needs FFmpeg and none was found (<see cref="ErrorCode.ProviderMissing"/>),
    /// FFmpeg has no decoder for it (<see cref="ErrorCode.UnsupportedFormat"/>), or a parameter is invalid.</exception>
    /// <example>
    /// <code>
    /// using var audio = engine.OpenPackets(new CodecParameters("ac3", 48000, 6) { TimeBase = new Rational(1, 90000) });
    /// audio.PlayOn(output);
    /// audio.PushPacket(packetBytes, pts);
    /// </code>
    /// </example>
    public unsafe PacketAudioStream OpenPackets(CodecParameters parameters, StreamOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(parameters);
        ArgumentNullException.ThrowIfNull(parameters.Codec);
        var h = Handle;
        using var opts = NativeOptions.Create(options);
        byte* codec = (byte*)Marshal.StringToCoTaskMemUTF8(parameters.Codec);
        try
        {
            fixed (byte* extra = parameters.ExtraData)
            {
                FaCodecParams p;
                NativeMethods.fa_codec_params_init(&p);
                p.Codec = codec;
                p.SampleRate = (uint)Math.Max(0, parameters.SampleRate);
                p.Channels = (uint)Math.Max(0, parameters.Channels);
                p.Extradata = extra;
                p.ExtradataSize = (nuint)(parameters.ExtraData?.Length ?? 0);
                p.BitRate = parameters.BitRate;
                p.BlockAlign = (uint)Math.Max(0, parameters.BlockAlign);
                p.BitsPerCodedSample = (uint)Math.Max(0, parameters.BitsPerCodedSample);
                p.TimeBase = parameters.TimeBase.ToNative();
                int rc = NativeMethods.fa_stream_open_packets(h, &p, opts.Pointer, out nint s);
                FatzAudioException.ThrowIfFailed(rc);
                var sh = new StreamHandle();
                sh.Initialize(s, h);
                return new PacketAudioStream(this, sh, parameters);
            }
        }
        finally
        {
            Marshal.FreeCoTaskMem((nint)codec);
        }
    }

    /// <summary>
    /// Turns loudness normalization on or off for every stream played on any device. Each stream's
    /// loudness comes from <see cref="AudioStream.SetLoudness"/>, then ReplayGain/R128 tags, then the scan
    /// cache, then a background scan that decodes only the audio, in chunks on all cores. Playback never
    /// waits: it starts with the best value known and glides to the exact gain when the scan is done
    /// (<see cref="AudioStream.LoudnessFinal"/>). Gain is target minus loudness, limited to -30..+12 dB.
    /// </summary>
    /// <param name="mode">Normalization mode.</param>
    /// <param name="targetLufs">Target loudness: -16 suits music players, -14 matches streaming services, -23 is EBU broadcast. Allowed -60..0.</param>
    /// <exception cref="FatzAudioException">The target is out of range (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <example>
    /// <code>
    /// engine.SetNormalization(NormalizeMode.Track, -16);
    /// using var song = engine.OpenFile("song.flac");   // analysis starts now
    /// song.PlayOn(output);                              // plays at once, at the right level
    /// </code>
    /// </example>
    public void SetNormalization(NormalizeMode mode, double targetLufs = -16.0) =>
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_engine_set_normalization(Handle, (int)mode, targetLufs));

    /// <summary>
    /// Configures the true-peak limiter on every output device. It catches peaks a normalization boost
    /// or a crossfade would push over the ceiling, and never touches audio below it. Its 1.5 ms
    /// look-ahead is always in the signal path (also while off), so switching never clicks.
    /// </summary>
    /// <param name="mode">When the limiter runs; <see cref="LimiterMode.Auto"/> follows normalization.</param>
    /// <param name="ceilingDbtp">Highest true peak allowed, in dBTP (-1 recommended, allowed -20..0).</param>
    /// <exception cref="FatzAudioException">The ceiling is out of range (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    public void SetLimiter(LimiterMode mode, double ceilingDbtp = -1.0) =>
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_engine_set_limiter(Handle, (int)mode, ceilingDbtp));

    /// <summary>
    /// Measures the loudness of a file without playing it: only the audio is decoded, in chunks on all
    /// cores, and the result is cached when <see cref="AudioEngineOptions.CacheDirectory"/> is set. Tags are
    /// ignored (this measures). Blocks until done; use <c>Task.Run</c> to keep a UI thread free.
    /// </summary>
    /// <param name="path">Path of the audio file.</param>
    /// <returns>The measured loudness (<see cref="LoudnessSource.Scan"/> or <see cref="LoudnessSource.Cache"/>).</returns>
    /// <exception cref="FatzAudioException">The file is missing (<see cref="ErrorCode.NotFound"/>) or not a supported format.</exception>
    public Loudness ScanLoudness(string path)
    {
        ArgumentNullException.ThrowIfNull(path);
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_scan_loudness_path(Handle, path, out FaLoudness l));
        return Loudness.From(l);
    }

    /// <summary>
    /// Releases the engine reference held by this object. The native engine is destroyed once all
    /// devices and streams created from it have been disposed as well.
    /// </summary>
    public void Dispose() => _handle.Dispose();
}

/// <summary>Holds the native <c>fa_open_options</c> (unmanaged memory) for the duration of a call.</summary>
internal unsafe ref struct NativeOptions
{
    private FaOpenOptions* _ptr;
    private byte* _mime;
    private FaNetOptions* _net;
    private byte* _userAgent, _headers, _proxy;

    public readonly FaOpenOptions* Pointer => _ptr;

    public static NativeOptions Create(StreamOptions? options)
    {
        var o = new NativeOptions();
        if (options is null) return o;
        o._ptr = (FaOpenOptions*)NativeMemory.AllocZeroed((nuint)sizeof(FaOpenOptions));
        NativeMethods.fa_open_options_init(o._ptr);
        uint flags = 0;
        if (options.DecodeOnly) flags |= 1u << 0;
        if (options.Loop) flags |= 1u << 1;
        if (options.Prescan) flags |= 1u << 2;
        if (options.NoNormalize) flags |= 1u << 3;
        o._ptr->Flags = flags;
        o._ptr->AudioTrack = options.AudioTrack;
        if (options.BufferLength is { } buffer)
            o._ptr->BufferMs = (uint)Math.Clamp(buffer.TotalMilliseconds, 1, uint.MaxValue);
        if (options.MimeHint is { } mime)
        {
            o._mime = (byte*)Marshal.StringToCoTaskMemUTF8(mime);
            o._ptr->MimeHint = o._mime;
        }
        if (options.Network is { } net)
        {
            o._net = (FaNetOptions*)NativeMemory.AllocZeroed((nuint)sizeof(FaNetOptions));
            NativeMethods.fa_net_options_init(o._net);
            o._userAgent = Utf8(net.UserAgent);
            o._proxy = net.Proxy is null ? null : Utf8(net.Proxy);
            if (net.Headers is { Count: > 0 } headers)
                o._headers = Utf8(string.Join("\r\n", headers.Select(kv => $"{kv.Key}: {kv.Value}")));
            o._net->UserAgent = o._userAgent;
            o._net->Headers = o._headers;
            o._net->Proxy = o._proxy;
            o._net->TimeoutMs = Millis(net.Timeout);
            o._net->PrebufferMs = Millis(net.Prebuffer);
            o._net->ReconnectMs = Millis(net.ReconnectFor);
            uint netFlags = 0;
            if (!net.Reconnect) netFlags |= 1u << 0;
            if (!net.SongTitles) netFlags |= 1u << 1;
            o._net->Flags = netFlags;
            o._net->HlsMaxBandwidth = (uint)Math.Clamp(net.HlsMaxBandwidth, 0, int.MaxValue);
            o._ptr->Net = o._net;
        }
        return o;
    }

    private static byte* Utf8(string? s) => s is null ? null : (byte*)Marshal.StringToCoTaskMemUTF8(s);

    private static uint Millis(TimeSpan? t) =>
        t is { } v ? (uint)Math.Clamp(Math.Ceiling(v.TotalMilliseconds), 1, uint.MaxValue) : 0;

    public void Dispose()
    {
        if (_mime != null) Marshal.FreeCoTaskMem((nint)_mime);
        if (_userAgent != null) Marshal.FreeCoTaskMem((nint)_userAgent);
        if (_headers != null) Marshal.FreeCoTaskMem((nint)_headers);
        if (_proxy != null) Marshal.FreeCoTaskMem((nint)_proxy);
        if (_net != null) NativeMemory.Free(_net);
        if (_ptr != null) NativeMemory.Free(_ptr);
        _mime = _userAgent = _headers = _proxy = null;
        _net = null;
        _ptr = null;
    }
}
