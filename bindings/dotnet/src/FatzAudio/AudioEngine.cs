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

    /// <summary>Opens a network URL (http/https; availability depends on the native build).</summary>
    /// <param name="url">The URL.</param>
    /// <param name="options">Optional open options.</param>
    /// <returns>The stream; dispose it when done.</returns>
    /// <exception cref="FatzAudioException">The URL could not be opened (<see cref="ErrorCode.NotImplemented"/> if the native library has no network support yet).</exception>
    /// <example>
    /// <code>
    /// using var radio = engine.OpenUrl("https://example.com/stream.mp3");
    /// </code>
    /// </example>
    public unsafe AudioStream OpenUrl(string url, StreamOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(url);
        var h = Handle;
        using var opts = NativeOptions.Create(options);
        return AudioStream.Wrap(this, NativeMethods.fa_stream_open_url(h, url, opts.Pointer, out nint s), s, null);
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
        return new PushAudioStream(this, sh, null, null, format);
    }

    /// <summary>The engine clock (monotonic presentation time of the mix).</summary>
    /// <exception cref="FatzAudioException">Not supported by the native build yet (<see cref="ErrorCode.NotImplemented"/>).</exception>
    public TimeSpan Time
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_engine_get_time(Handle, out long ns));
            return TimeConvert.FromNanoseconds(ns);
        }
    }

    /// <summary>Configures loudness normalization for all streams.</summary>
    /// <param name="mode">Normalization mode.</param>
    /// <param name="targetLufs">Target loudness in LUFS (for example -14).</param>
    /// <exception cref="FatzAudioException">Not supported by the native build yet (<see cref="ErrorCode.NotImplemented"/>).</exception>
    public void SetNormalization(NormalizeMode mode, double targetLufs = -14.0) =>
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_engine_set_normalization(Handle, (int)mode, targetLufs));

    /// <summary>Measures the loudness of a file without playing it, using all cores.</summary>
    /// <param name="path">Path of the audio file.</param>
    /// <returns>The measured loudness.</returns>
    /// <exception cref="FatzAudioException">The scan failed or is not supported yet (<see cref="ErrorCode.NotImplemented"/>).</exception>
    public Loudness ScanLoudness(string path)
    {
        ArgumentNullException.ThrowIfNull(path);
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_scan_loudness_path(Handle, path, out FaLoudness l));
        return new Loudness(l.IntegratedLufs, l.RangeLu, l.TruePeakDbtp, l.IsFinal != 0);
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
        if (options.MimeHint is { } mime)
        {
            o._mime = (byte*)Marshal.StringToCoTaskMemUTF8(mime);
            o._ptr->MimeHint = o._mime;
        }
        return o;
    }

    public void Dispose()
    {
        if (_mime != null) Marshal.FreeCoTaskMem((nint)_mime);
        if (_ptr != null) NativeMemory.Free(_ptr);
        _mime = null;
        _ptr = null;
    }
}
