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

    /// <summary>What an <see cref="AudioCallback"/> returns when there is no more audio.</summary>
    public const int EndOfStream = -1;

    /// <summary>
    /// How busy the engine is (BASS_GetCPU), for a diagnostics panel: the output callbacks' load and the decode
    /// workers'. Cheap enough to call from a UI timer.
    /// </summary>
    /// <returns>The current figures.</returns>
    /// <exception cref="ObjectDisposedException">The engine was disposed.</exception>
    /// <example>
    /// <code>
    /// var stats = engine.GetStats();
    /// cpuLabel.Text = $"Mix {stats.AudioLoad:0.0}% (peak {stats.AudioLoadPeak:0.0}%), decoding {stats.WorkerLoad:0.0}%";
    /// </code>
    /// </example>
    public unsafe EngineStats GetStats()
    {
        FaEngineStats st = default;
        st.StructSize = (uint)sizeof(FaEngineStats);
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_engine_get_stats(Handle, &st));
        return new EngineStats(st.AudioLoad, st.AudioLoadPeak, st.WorkerLoad, (int)st.Workers, (int)st.Devices,
            (int)st.AttachedStreams);
    }

    /// <summary>Creates an engine.</summary>
    /// <param name="options">Optional configuration; null uses the defaults.</param>
    /// <returns>A new engine that must be disposed.</returns>
    /// <exception cref="FatzAudioException">The native engine could not be created.</exception>
    /// <exception cref="DllNotFoundException">The native <c>fatzaudio</c> library could not be located.</exception>
    /// <exception cref="InvalidOperationException">The native library is another major.minor version than this binding (see <see cref="BindingVersion"/>).</exception>
    public static unsafe AudioEngine Create(AudioEngineOptions? options = null)
    {
        NativeLoader.EnsureInitialized();

        FaEngineConfig config = default;
        NativeMethods.fa_engine_config_init(&config);
        nint cachePtr = 0, tempPtr = 0;
        try
        {
            if (options is not null)
            {
                config.MixSampleRate = (uint)Math.Max(0, options.MixSampleRate);
                config.PeriodMs = options.Period is { } p ? (uint)Math.Max(0, p.TotalMilliseconds) : 0;
                config.WorkerThreads = (uint)Math.Max(0, options.WorkerThreads);
                if (options.NullOutput) config.Flags |= 1; // FA_ENGINE_NULL_OUTPUT
                if (options.NoSystemSoundFont) config.Flags |= 2; // FA_ENGINE_NO_SYSTEM_SOUNDFONT
                if (options.CacheDirectory is { } dir)
                {
                    cachePtr = Marshal.StringToCoTaskMemUTF8(dir);
                    config.CacheDirectory = (byte*)cachePtr;
                }
                if (options.TempDirectory is { } temp)
                {
                    tempPtr = Marshal.StringToCoTaskMemUTF8(temp);
                    config.TempDirectory = (byte*)tempPtr;
                }
                config.ZipMemoryLimitMb = (uint)Math.Max(0, options.ZipMemoryLimitMb);
            }

            FatzAudioException.ThrowIfFailed(NativeMethods.fa_engine_create(&config, out var native));
            var handle = new EngineHandle();
            Marshal.InitHandle(handle, native);
            return new AudioEngine(handle);
        }
        finally
        {
            if (cachePtr != 0) Marshal.FreeCoTaskMem(cachePtr);
            if (tempPtr != 0) Marshal.FreeCoTaskMem(tempPtr);
        }
    }

    /// <summary>
    /// The version of the native library this binding was built for (major.minor.patch). Every call
    /// into the library throws <see cref="InvalidOperationException"/> when <see cref="NativeVersion"/>
    /// has another major.minor.
    /// </summary>
    public static Version BindingVersion => NativeLoader.BindingVersion;

    /// <summary>Version of the loaded native library.</summary>
    public static Version NativeVersion
    {
        get
        {
            NativeLoader.EnsureLoaded(); // no version check, so a mismatch can be reported
            return NativeLoader.Unpack(NativeMethods.fa_version());
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
                    NativeText.Read(f.Name),
                    NativeText.Read(f.Description),
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
                NativeText.Read(info.Id),
                NativeText.Read(info.Name),
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

    /// <summary>
    /// Creates a DJ mixer: decks on one output device with trim, isolator EQ, faders, meters, a crossfader, a master
    /// gain, a limiter that is always on, and a headphone cue. It opens and starts its own output device
    /// (<see cref="DjMixer.Output"/>).
    /// </summary>
    /// <param name="options">The decks, the output and the cue; null uses the defaults (2 decks, the default device, no cue).</param>
    /// <returns>The mixer; dispose it when done.</returns>
    /// <exception cref="ArgumentException"><see cref="DjMixerOptions.Output"/> is not a playback device.</exception>
    /// <exception cref="FatzAudioException"><see cref="ErrorCode.NotEnoughChannels"/> when the cue asks for channels the device
    /// does not have; <see cref="ErrorCode.NotImplemented"/> for more than 2 decks, or <see cref="LatencyMode.Exclusive"/> off Windows;
    /// <see cref="ErrorCode.Device"/> when the device cannot be opened.</exception>
    /// <example>
    /// <code>
    /// using var mixer = engine.CreateDjMixer(new DjMixerOptions { Cue = CueOutput.Split });
    /// mixer.Decks[0].Load(engine.OpenFile("song1.mp3"));
    /// mixer.Decks[0].Stream!.Play();
    /// </code>
    /// </example>
    public unsafe DjMixer CreateDjMixer(DjMixerOptions? options = null)
    {
        options ??= new DjMixerOptions();
        if (options.Output is { Kind: not DeviceKind.Playback })
            throw new ArgumentException("Not a playback device.", nameof(options));
        var h = Handle;
        FaMixerConfig c;
        NativeMethods.fa_mixer_config_init(&c);
        c.DeckCount = (uint)Math.Max(0, options.Decks);
        c.CueMode = options.Cue.Mode;
        c.CueChannel = (uint)options.Cue.FirstChannel;
        c.Latency = (int)options.Latency;
        c.Flags = options.Cue.Mode == 2 && !options.Cue.LineUp ? 1u : 0u;   // FA_MIXER_CUE_LOWEST_LATENCY
        byte* id = options.Output?.Id is { } devId ? (byte*)Marshal.StringToCoTaskMemUTF8(devId) : null;
        byte* cueId = options.Cue.Headphones?.Id is { } cueDev ? (byte*)Marshal.StringToCoTaskMemUTF8(cueDev) : null;
        nint native;
        try
        {
            c.DeviceId = id;
            c.CueDeviceId = cueId;
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_create(h, &c, out native));
        }
        finally
        {
            if (id != null) Marshal.FreeCoTaskMem((nint)id);
            if (cueId != null) Marshal.FreeCoTaskMem((nint)cueId);
        }
        // The mixer lives as long as its device: closing the device destroys it.
        NativeMethods.fa_mixer_get_device(native, out nint device);
        var dh = new DeviceHandle();
        dh.Initialize(device, h);
        var output = new OutputDevice(this, dh, options.Output);
        try
        {
            return new DjMixer(this, native, output);
        }
        catch
        {
            output.Dispose();
            throw;
        }
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

    /// <summary>
    /// Opens an audio file on a worker thread, so the caller does not wait for it. Cancelling stops a long open (a
    /// compressed song in a ZIP archive being unpacked) and throws <see cref="OperationCanceledException"/>.
    /// </summary>
    /// <param name="path">Path of the file.</param>
    /// <param name="options">Optional open options.</param>
    /// <param name="cancellationToken">Stops the open; no stream is created.</param>
    /// <returns>The stream; dispose it when done.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="path"/> is null.</exception>
    /// <exception cref="OperationCanceledException">The token was cancelled.</exception>
    /// <exception cref="FatzAudioException">See <see cref="OpenFile"/>.</exception>
    public Task<AudioStream> OpenFileAsync(string path, StreamOptions? options = null, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(path);
        return OpenAsync(o => OpenFile(path, o), options, cancellationToken);
    }

    /// <summary>
    /// Opens one audio track of a CD (or disc image). The stream is 44.1 kHz stereo and seekable, and works
    /// like any other: crossfades, queues (gapless track to track), normalization and buses.
    /// </summary>
    /// <param name="disc">The drive, from <see cref="Disc.Open(string?, DiscOptions?)"/>.</param>
    /// <param name="track">The track number, as in <see cref="DiscToc.Tracks"/>.</param>
    /// <param name="options">Optional open options.</param>
    /// <returns>The stream; dispose it when done. If the disc is taken out while it plays, the stream ends.</returns>
    /// <exception cref="FatzAudioException">The disc has no such track (<see cref="ErrorCode.NotFound"/>), it is a data track
    /// (<see cref="ErrorCode.UnsupportedFormat"/>), or there is no disc (<see cref="ErrorCode.NoDisc"/>).</exception>
    /// <example>
    /// <code>
    /// using var disc = Disc.Open();
    /// using var track = engine.OpenDisc(disc, 3);
    /// track.PlayOn(output);
    /// </code>
    /// </example>
    public unsafe AudioStream OpenDisc(Disc disc, int track, StreamOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(disc);
        var h = Handle;
        using var opts = NativeOptions.Create(options);
        return AudioStream.Wrap(this, NativeMethods.fa_stream_open_disc(h, disc.Handle, track, opts.Pointer, out nint s), s, null);
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

    /// <summary>
    /// Loads a short sound into memory to play as many overlapping copies as needed (<see cref="Sound"/>): any
    /// format a stream opens. The whole file is decoded now.
    /// </summary>
    /// <param name="path">Path of the file.</param>
    /// <param name="options">Voice limit, steal rule and open options; null = 8 voices, oldest stolen.</param>
    /// <returns>The sound; dispose it when done.</returns>
    /// <exception cref="FatzAudioException">The file could not be opened or decoded, or is over 1 GB as PCM
    /// (<see cref="ErrorCode.TooLarge"/>).</exception>
    /// <example>
    /// <code>
    /// using var horn = engine.LoadSound("horn.wav", new SoundOptions { MaxVoices = 4 });
    /// horn.Play(output);
    /// </code>
    /// </example>
    public unsafe Sound LoadSound(string path, SoundOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(path);
        var h = Handle;
        var o = Sound.ToNative(options);
        using var opts = NativeOptions.Create(options?.Open);
        o.Open = opts.Pointer;
        return Sound.Wrap(this, NativeMethods.fa_sound_load_path(h, path, &o, out nint s), s);
    }

    /// <summary>Loads a short sound from a file held in memory (decoded now; the bytes are not kept).</summary>
    /// <param name="data">Encoded audio file contents.</param>
    /// <param name="options">Voice limit, steal rule and open options.</param>
    /// <returns>The sound; dispose it when done.</returns>
    /// <exception cref="FatzAudioException">The data could not be decoded.</exception>
    public unsafe Sound LoadSound(ReadOnlyMemory<byte> data, SoundOptions? options = null)
    {
        var h = Handle;
        var o = Sound.ToNative(options);
        using var opts = NativeOptions.Create(options?.Open);
        o.Open = opts.Pointer;
        using var pin = data.Pin();
        return Sound.Wrap(this, NativeMethods.fa_sound_load_memory(h, pin.Pointer, (nuint)data.Length, &o, out nint s), s);
    }

    /// <summary>Makes a sound from your own samples (BASS_SampleCreate + BASS_SampleSetData). They are copied.</summary>
    /// <param name="samples">Interleaved 32-bit float samples, a whole number of frames.</param>
    /// <param name="sampleRate">1000 to 768000 Hz.</param>
    /// <param name="channels">1 to 32.</param>
    /// <param name="options">Voice limit and steal rule (Open is ignored).</param>
    /// <returns>The sound; dispose it when done.</returns>
    /// <exception cref="ArgumentException">The samples are empty or not a whole number of frames.</exception>
    /// <example>
    /// <code>
    /// var beep = new float[4800];
    /// for (int i = 0; i &lt; beep.Length; i++) beep[i] = 0.3f * MathF.Sin(i * 2 * MathF.PI * 1000 / 48000);
    /// using var sound = engine.CreateSound(beep, 48000, 1);
    /// </code>
    /// </example>
    public unsafe Sound CreateSound(ReadOnlySpan<float> samples, int sampleRate, int channels, SoundOptions? options = null)
    {
        ArgumentOutOfRangeException.ThrowIfLessThan(channels, 1);
        if (samples.IsEmpty || samples.Length % channels != 0)
            throw new ArgumentException("The samples must be a whole number of frames.", nameof(samples));
        var h = Handle;
        var o = Sound.ToNative(options);
        var f = new FaAudioFormat { SampleFormat = 0, SampleRate = (uint)Math.Max(sampleRate, 0), Channels = (uint)channels };
        fixed (float* p = samples)
            return Sound.Wrap(this, NativeMethods.fa_sound_load_pcm(h, &f, p, samples.Length / channels, &o, out nint s), s);
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
        return AudioStream.Wrap(this, rc, s, gc);
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
        GCHandle? download = null;
        if (options?.Network?.Downloaded is { } handler)
        {
            // Lives as long as the native stream (adopted by it below), freed here if the open fails.
            var gc = GCHandle.Alloc(new AudioStream.DownloadState(handler, null));
            download = gc;
            opts.Pointer->Net->DownloadProc = &AudioStream.OnDownload;
            opts.Pointer->Net->DownloadUser = (void*)GCHandle.ToIntPtr(gc);
        }
        int rc = NativeMethods.fa_stream_open_url(h, url, opts.Pointer, out nint s);
        if (rc < 0) download?.Free();
        if (rc == (int)ErrorCode.InvalidArgument) throw new ArgumentException("Not an absolute http or https URL.", nameof(url));
        var stream = AudioStream.Wrap(this, rc, s, download);
        stream.InitDownloadHandle(download);
        return stream;
    }

    /// <summary>
    /// Android only: the Java VM that HTTPS web streams run on (HttpURLConnection). You rarely
    /// need this: the library finds the VM itself on Android 12 and later, and the binding hands
    /// over .NET for Android's VM on older versions. Call it when your app hosts the runtime some
    /// other way and <c>https://</c> URLs fail with <see cref="ErrorCode.ProviderMissing"/>.
    /// </summary>
    /// <param name="javaVM">The process's <c>JavaVM*</c>.</param>
    /// <exception cref="ArgumentException"><paramref name="javaVM"/> is zero.</exception>
    /// <exception cref="PlatformNotSupportedException">Not running on Android.</exception>
    public static void SetAndroidJavaVM(nint javaVM)
    {
        if (javaVM == 0) throw new ArgumentException("A JavaVM* is required.", nameof(javaVM));
        if (!OperatingSystem.IsAndroid()) throw new PlatformNotSupportedException("SetAndroidJavaVM is for Android.");
        NativeLoader.EnsureInitialized();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_android_set_java_vm(javaVM));
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

    /// <summary>
    /// Opens a URL (as <see cref="OpenUrl(string, StreamOptions?)"/>) on a worker thread, so the caller does not wait
    /// for the server (BASS_StreamCreateURL on a thread, with BASS_StreamCancel). Cancelling stops the name lookup,
    /// connection or download in progress within about 100 ms and throws <see cref="OperationCanceledException"/>:
    /// the user picked another station before this one answered.
    /// </summary>
    /// <param name="url">The URL.</param>
    /// <param name="options">Optional open options; <see cref="StreamOptions.Network"/> holds the connection settings.</param>
    /// <param name="cancellationToken">Stops the open; no stream is created.</param>
    /// <returns>The stream; dispose it when done.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="url"/> is null.</exception>
    /// <exception cref="OperationCanceledException">The token was cancelled.</exception>
    /// <exception cref="FatzAudioException">See <see cref="OpenUrl(string, StreamOptions?)"/>.</exception>
    /// <example>
    /// <code>
    /// _tuning?.Cancel();
    /// _tuning = new CancellationTokenSource();
    /// try
    /// {
    ///     using var radio = await engine.OpenUrlAsync(station.Url, cancellationToken: _tuning.Token);
    ///     radio.PlayOn(output);
    /// }
    /// catch (OperationCanceledException) { /* another station was picked meanwhile */ }
    /// </code>
    /// </example>
    public Task<AudioStream> OpenUrlAsync(string url, StreamOptions? options = null, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(url);
        return OpenAsync(o => OpenUrl(url, o), options, cancellationToken);
    }

    /// <summary>Runs an open on the thread pool with the token in its options (with the options' own, if any).</summary>
    private static Task<AudioStream> OpenAsync(Func<StreamOptions, AudioStream> open, StreamOptions? options,
        CancellationToken cancellationToken)
    {
        if (cancellationToken.IsCancellationRequested) return Task.FromCanceled<AudioStream>(cancellationToken);
        return Task.Run(() =>
        {
            using var linked = options is { CancellationToken.CanBeCanceled: true }
                ? CancellationTokenSource.CreateLinkedTokenSource(options.CancellationToken, cancellationToken)
                : null;
            var token = linked?.Token ?? cancellationToken;
            // The caller's own token when it is the one that fired, so `when (e.CancellationToken == token)` matches.
            CancellationToken Fired() => cancellationToken.IsCancellationRequested ? cancellationToken : token;
            AudioStream stream;
            try
            {
                stream = open((options ?? new StreamOptions()) with { CancellationToken = token });
            }
            catch (FatzAudioException e) when (e.ErrorCode == ErrorCode.Cancelled)
            {
                throw new OperationCanceledException(e.Message, e, Fired());
            }
            // Cancelled just after the open got through: the caller has moved on, so the stream must not reach it.
            if (token.IsCancellationRequested)
            {
                stream.Dispose();
                throw new OperationCanceledException(Fired());
            }
            return stream;
        }, CancellationToken.None);
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
    /// Opens a stream that asks your callback for audio whenever it wants more (BASS_StreamCreate with a STREAMPROC):
    /// a synthesizer, a tone generator, audio from a library of your own. It plays, decodes, records and takes tempo,
    /// effects and meters like any other stream; it cannot seek or loop.
    /// </summary>
    /// <param name="sampleRate">The audio's sample rate.</param>
    /// <param name="channels">The audio's channel count (interleaved).</param>
    /// <param name="fill">
    /// Writes interleaved float samples and returns the frames written. It runs on a decode worker thread, ahead of
    /// playback by the stream's buffer (or on your thread inside <see cref="AudioStream.Read(Span{float})"/> for
    /// <see cref="StreamOptions.DecodeOnly"/> streams), never twice at once and never after the stream is disposed.
    /// It may take locks but should not block: the stream runs dry meanwhile.
    /// </param>
    /// <param name="options">Optional open options (<see cref="StreamOptions.Loop"/> is not allowed).</param>
    /// <returns>The stream; dispose it when done.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="fill"/> is null.</exception>
    /// <exception cref="FatzAudioException">An invalid rate or channel count, or <see cref="StreamOptions.Loop"/>
    /// (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <example>
    /// <code>
    /// double phase = 0;
    /// using var tone = engine.OpenCallback(48000, 2, buffer =>
    /// {
    ///     for (int i = 0; i &lt; buffer.Length; i += 2, phase += 2 * Math.PI * 440 / 48000)
    ///         buffer[i] = buffer[i + 1] = (float)(0.2 * Math.Sin(phase));
    ///     return buffer.Length / 2;
    /// });
    /// tone.PlayOn(output);
    /// </code>
    /// </example>
    public unsafe AudioStream OpenCallback(int sampleRate, int channels, AudioCallback fill, StreamOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(fill);
        var h = Handle;
        using var opts = NativeOptions.Create(options);
        if (sampleRate <= 0 || channels <= 0) throw new FatzAudioException(ErrorCode.InvalidArgument);
        var nf = AudioFormat.Float32(sampleRate, channels).ToNative();
        // Lives as long as the native stream (adopted by it below), freed here if the open fails.
        var gc = GCHandle.Alloc(new CallbackState(fill, channels));
        int rc = NativeMethods.fa_stream_open_callback(h, &nf, &CallbackState.Fill, (void*)GCHandle.ToIntPtr(gc), opts.Pointer, out nint s);
        if (rc < 0) gc.Free();
        return AudioStream.Wrap(this, rc, s, gc);
    }

    /// <summary>
    /// Opens an input (a microphone, a line input) as an ordinary stream (BASS_RecordStart): record it to a file
    /// (<see cref="StreamOptions.DecodeOnly"/> and <see cref="AudioEncoder.Encode"/>), read its samples, meter
    /// it, or play it to check it. The stream begins with the audio of its first read (or of <see cref="AudioStream.PlayOn(OutputDevice)"/>), not with what came before.
    /// To sing or talk through the speakers use a mixer mic (<see cref="DjMixer.AddMic"/>), which corrects the clock
    /// drift between input and output and keeps latency low.
    /// </summary>
    /// <param name="capture">The input and its format; null = the default input, as it is.</param>
    /// <param name="options">Optional open options (<see cref="StreamOptions.Loop"/> is not allowed).</param>
    /// <returns>The stream; dispose it when done. If the input goes away (unplugged), the stream ends.</returns>
    /// <exception cref="ArgumentException"><see cref="CaptureOptions.Device"/> is not an input.</exception>
    /// <exception cref="FatzAudioException"><see cref="ErrorCode.PermissionDenied"/> without the microphone permission;
    /// <see cref="ErrorCode.Device"/> when the input cannot be opened.</exception>
    /// <example>
    /// <code>
    /// using var mic = engine.OpenCapture(options: new StreamOptions { DecodeOnly = true });
    /// using var file = AudioEncoder.Create("memo.flac", new EncoderOptions { Format = EncodeFormat.Flac });
    /// file.Encode(mic);
    /// // ... later: file.Dispose() completes the recording.
    /// </code>
    /// </example>
    public unsafe AudioStream OpenCapture(CaptureOptions? capture = null, StreamOptions? options = null)
    {
        if (capture?.Device is { Kind: not DeviceKind.Capture })
            throw new ArgumentException("Not an input device.", nameof(capture));
        var h = Handle;
        using var opts = NativeOptions.Create(options);
        FaCaptureOptions c;
        NativeMethods.fa_capture_options_init(&c);
        c.SampleRate = (uint)Math.Max(0, capture?.SampleRate ?? 0);
        c.Channels = (uint)Math.Max(0, capture?.Channels ?? 0);
        c.Flags = capture?.KeepOsProcessing == true ? 1u : 0u;
        byte* id = capture?.Device?.Id is { } devId ? (byte*)Marshal.StringToCoTaskMemUTF8(devId) : null;
        try
        {
            c.DeviceId = id;
            return AudioStream.Wrap(this, NativeMethods.fa_stream_open_capture(h, &c, opts.Pointer, out nint s), s, null);
        }
        finally
        {
            if (id != null) Marshal.FreeCoTaskMem((nint)id);
        }
    }

    /// <summary>
    /// Opens a stream for the compressed packets of one audio track, for apps that demux the file themselves
    /// (typically with FFmpeg, for the video). Push packets straight from <c>av_read_frame</c>; FatzAudio decodes
    /// them on its worker threads (PCM, MP3, AAC, AC-3, E-AC-3, Opus, Vorbis, FLAC, ALAC, and WMA on Windows) and
    /// <see cref="AudioStream.Clock"/> reports their timestamps as they leave the speaker.
    /// </summary>
    /// <param name="parameters">The track: codec name, rate, channels, codec configuration and time base.</param>
    /// <param name="options">Optional open options (<see cref="StreamOptions.Loop"/> is not allowed).</param>
    /// <returns>A packet stream; dispose it when done.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="parameters"/> is null.</exception>
    /// <exception cref="FatzAudioException">FatzAudio does not decode the codec (<see cref="ErrorCode.UnsupportedFormat"/>:
    /// DTS, TrueHD and the like; decode those with your FFmpeg and use <see cref="CreatePushStream"/>), or a parameter is invalid.</exception>
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
    /// Lets the dialogue level that Dolby streams (AC-3, E-AC-3) state ("dialnorm") count as their loudness
    /// (default off). Broadcasters set it to the programme's loudness, so a film needs no scan: when there is no
    /// value from <see cref="AudioStream.SetLoudness"/>, no ReplayGain/R128 tag and no cached scan, the dialnorm is
    /// final at once (<see cref="LoudnessSource.Dialnorm"/>). -31, what encoders write when nobody set it, is not
    /// used. Applies to streams whose analysis starts afterwards; live streams keep the running estimate.
    /// </summary>
    /// <param name="enabled">True to use dialnorm, false to measure every file.</param>
    public void SetDialnorm(bool enabled) =>
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_engine_set_dialnorm(Handle, enabled ? 1 : 0));

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
    /// Analyses a file without playing it: one decode of the audio gives any mix of the beat grid, the key, a waveform
    /// overview and the loudness. MIDI files and tracker modules get their exact beat grid from the file (MIDI files are
    /// rendered with <see cref="DefaultSoundFont"/>, else the OS's instruments, as playback does). Results are cached when
    /// <see cref="AudioEngineOptions.CacheDirectory"/> is set, so a song is analysed once. Blocks until done (about a second
    /// for a 4-minute song on one core): call it from a worker thread, or use <see cref="AnalyzeAsync"/>, and run several
    /// at once for a library.
    /// </summary>
    /// <param name="path">Path of the audio file.</param>
    /// <param name="kinds">What to find out; defaults to the beat grid and the key.</param>
    /// <param name="options">Tempo range and cache use; null uses the defaults.</param>
    /// <returns>The result, a plain object with nothing to dispose.</returns>
    /// <exception cref="FatzAudioException">The file is missing (<see cref="ErrorCode.NotFound"/>), no decoder reads it (<see cref="ErrorCode.UnsupportedFormat"/>), or the options are invalid (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <exception cref="ObjectDisposedException">The engine was disposed.</exception>
    /// <example>
    /// <code>
    /// var t = engine.Analyze("song.mp3");
    /// Console.WriteLine($"{t.Beats?.Bpm:0.0} BPM, key {t.Key?.Name} ({t.Key?.Camelot})");
    /// </code>
    /// </example>
    public TrackAnalysis Analyze(string path, AnalysisKinds kinds = AnalysisKinds.Beats | AnalysisKinds.Key, AnalysisOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(path);
        return TrackAnalysis.Run(Handle, path, kinds, options);
    }

    /// <summary>
    /// <see cref="Analyze"/> on a thread-pool thread. Disposing the engine while it runs is safe: the analysis keeps the
    /// native engine alive until it is done.
    /// </summary>
    /// <param name="path">Path of the audio file.</param>
    /// <param name="kinds">What to find out; defaults to the beat grid and the key.</param>
    /// <param name="options">Tempo range and cache use; null uses the defaults.</param>
    /// <param name="cancellationToken">Stops the wait (the native analysis, which cannot be interrupted, runs to its end).</param>
    /// <returns>A task with the result.</returns>
    /// <exception cref="FatzAudioException">As for <see cref="Analyze"/>, thrown through the task.</exception>
    /// <exception cref="OperationCanceledException">The token was cancelled.</exception>
    /// <exception cref="ObjectDisposedException">The engine was disposed.</exception>
    public Task<TrackAnalysis> AnalyzeAsync(string path, AnalysisKinds kinds = AnalysisKinds.Beats | AnalysisKinds.Key,
        AnalysisOptions? options = null, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(path);
        return TrackAnalysis.RunAsync(Handle, h => TrackAnalysis.Run(h, path, kinds, options), cancellationToken);
    }

    private SoundFont? _defaultSoundFont;

    /// <summary>
    /// The soundfont for MIDI streams opened from now on (streams already open keep theirs); null removes it.
    /// The native engine keeps its own reference, so the <see cref="SoundFont"/> may be disposed after it is set
    /// (the getter then returns the disposed object, which is still in use by the engine).
    /// </summary>
    /// <example>
    /// <code>
    /// using var gm = SoundFont.FromFile("GeneralUser.sf2");
    /// engine.DefaultSoundFont = gm;
    /// using var song = engine.OpenFile("song.mid");
    /// </code>
    /// </example>
    /// <exception cref="ObjectDisposedException">The engine, or the soundfont being set, was disposed.</exception>
    public SoundFont? DefaultSoundFont
    {
        get => _defaultSoundFont;
        set
        {
            var engine = Handle;
            if (value is null)
            {
                FatzAudioException.ThrowIfFailed(NativeMethods.fa_engine_set_soundfont(engine, 0));
            }
            else
            {
                var font = Native.HandleRef.AddRef(value.Handle);
                try { FatzAudioException.ThrowIfFailed(NativeMethods.fa_engine_set_soundfont(engine, font.DangerousGetHandle())); }
                finally { font.DangerousRelease(); }
            }
            _defaultSoundFont = value;
        }
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
    private SoundFontHandle? _font;
    private int* _cancel;
    private CancellationTokenRegistration _cancelRegistration;

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
        if (options.NoCdg) flags |= 1u << 4;
        o._ptr->Flags = flags;
        o._ptr->AudioTrack = options.AudioTrack;
        o._ptr->DynamicRange = (int)options.DynamicRange;
        o._ptr->DolbyDownmix = (int)options.DolbyDownmix;
        o._ptr->TextCodePage = (int)options.TextCodePage;
        if (options.CancellationToken.CanBeCanceled)
        {
            // The native open reads this flag while it unpacks; the token sets it from any thread.
            o._cancel = (int*)NativeMemory.AllocZeroed(sizeof(int));
            o._ptr->Cancel = o._cancel;
            o._cancelRegistration = options.CancellationToken.Register(
                static state => Volatile.Write(ref *(int*)(nint)state!, 1), (nint)o._cancel);
        }
        if (options.SoundFont is { } font)
        {
            // Held until the open call returns; the native stream takes its own reference.
            o._font = Native.HandleRef.AddRef(font.Handle);
            o._ptr->SoundFont = o._font.DangerousGetHandle();
        }
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

    private static byte* Utf8(string? s) => (byte*)NativeText.Alloc(s);

    private static uint Millis(TimeSpan? t) =>
        t is { } v ? (uint)Math.Clamp(Math.Ceiling(v.TotalMilliseconds), 1, uint.MaxValue) : 0;

    public void Dispose()
    {
        // Disposing the registration waits for a callback that is running, so the flag is not written after it is freed.
        _cancelRegistration.Dispose();
        if (_cancel != null) NativeMemory.Free(_cancel);
        _cancel = null;
        if (_mime != null) Marshal.FreeCoTaskMem((nint)_mime);
        if (_userAgent != null) Marshal.FreeCoTaskMem((nint)_userAgent);
        if (_headers != null) Marshal.FreeCoTaskMem((nint)_headers);
        if (_proxy != null) Marshal.FreeCoTaskMem((nint)_proxy);
        if (_net != null) NativeMemory.Free(_net);
        if (_ptr != null) NativeMemory.Free(_ptr);
        _font?.DangerousRelease();
        _font = null;
        _mime = _userAgent = _headers = _proxy = null;
        _net = null;
        _ptr = null;
    }
}
