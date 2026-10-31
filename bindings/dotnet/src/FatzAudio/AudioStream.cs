using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using FatzAudio.Native;

namespace FatzAudio;

/// <summary>Event data for stream events that carry a position (position reached, marker, device change).</summary>
public sealed class StreamEventArgs : EventArgs
{
    internal StreamEventArgs(TimeSpan position, long frame, long value, MarkerKind? marker = null)
    {
        Position = position;
        Frame = frame;
        Value = value;
        Marker = marker;
    }

    /// <summary>For <see cref="AudioStream.MarkerReached"/>: which marker was passed; otherwise null.</summary>
    public MarkerKind? Marker { get; }

    /// <summary>Stream position of the event.</summary>
    public TimeSpan Position { get; }
    /// <summary>Stream position of the event in frames.</summary>
    public long Frame { get; }
    /// <summary>Event specific value (for example the marker id); 0 when unused.</summary>
    public long Value { get; }
}

/// <summary>Event data for <see cref="AudioStream.MetadataChanged"/>.</summary>
public sealed class MetadataChangedEventArgs : EventArgs
{
    internal MetadataChangedEventArgs(StreamMetadata metadata, TimeSpan position, long frame)
    {
        Metadata = metadata;
        Position = position;
        Frame = frame;
    }

    /// <summary>The metadata now in effect (the new title).</summary>
    public StreamMetadata Metadata { get; }
    /// <summary>Stream position where the new title starts.</summary>
    public TimeSpan Position { get; }
    /// <summary>Stream position where the new title starts, in frames.</summary>
    public long Frame { get; }
}

/// <summary>Callback for in-place DSP on interleaved float frames (<see cref="AudioStream.AddDsp"/>).</summary>
/// <param name="samples">Interleaved samples (frames x channels); modify in place.</param>
/// <param name="channels">Channel count: 2 on a device; the stream's own for decode-only streams.</param>
/// <param name="sampleRate">Sample rate: the device's on a device; the stream's own for decode-only streams.</param>
public delegate void DspProcessor(Span<float> samples, int channels, int sampleRate);

/// <summary>
/// A playable or decodable audio source. Create one with <see cref="AudioEngine.OpenFile"/>,
/// <see cref="AudioEngine.OpenMemory"/>, <see cref="AudioEngine.OpenStream"/>, <see cref="AudioEngine.OpenUrl(string, StreamOptions?)"/>,
/// <see cref="AudioEngine.CreatePushStream"/> or <see cref="AudioEngine.OpenPackets"/>.
/// </summary>
/// <remarks>
/// <para>
/// <b>Events</b> (<see cref="Ended"/>, <see cref="Started"/>, <see cref="MetadataChanged"/>, <see cref="Stalled"/>, <see cref="Resumed"/>,
/// <see cref="DecodeError"/>, <see cref="LoudnessFinal"/>, <see cref="PositionReached"/>, <see cref="MarkerReached"/>,
/// <see cref="DeviceChanged"/>, <see cref="MidiTextReached"/>) are raised on a native engine thread, never on your UI thread. Handlers must return
/// quickly and must not block; marshal to your own context if needed. Exceptions thrown by handlers are swallowed.
/// </para>
/// <para>
/// Members may be called from any thread. Using a stream after it was disposed throws <see cref="ObjectDisposedException"/>.
/// Disposing a stream is safe even after the engine or device it belongs to has been disposed.
/// </para>
/// </remarks>
/// <example>
/// <code>
/// using var engine = AudioEngine.Create();
/// using var output = engine.OpenOutput();
/// using var song = engine.OpenFile("song.mp3");
/// var done = new TaskCompletionSource();
/// song.Ended += (_, _) => done.TrySetResult();
/// song.PlayOn(output);
/// await done.Task;
/// </code>
/// </example>
public unsafe class AudioStream : IDisposable
{
    private readonly StreamHandle _handle;
    private int _sampleRate;
    private int _channels;
    private OutputDevice? _device;

    internal AudioStream(AudioEngine engine, StreamHandle handle, GCHandle? ioHandle, AudioFormat? knownFormat = null)
    {
        Engine = engine;
        _handle = handle;
        Markers = new StreamMarkers(this);
        if (ioHandle is { } gc) handle.Adopt(gc);

        try
        {
            if (knownFormat is not null)
            {
                _sampleRate = knownFormat.SampleRate;
                _channels = knownFormat.Channels;
            }
            else
            {
                RefreshFormat();
            }
            RegisterEventCallback();
        }
        catch
        {
            handle.Dispose();
            throw;
        }
    }

    internal static AudioStream Wrap(AudioEngine engine, int result, nint native, GCHandle? io)
    {
        FatzAudioException.ThrowIfFailed(result);
        var h = new StreamHandle();
        h.Initialize(native, engine.Handle);
        return new AudioStream(engine, h, io);
    }

    /// <summary>The engine this stream belongs to.</summary>
    public AudioEngine Engine { get; }

    internal StreamHandle Handle
    {
        get
        {
            ObjectDisposedException.ThrowIf(_handle.IsClosed, this);
            return _handle;
        }
    }

    // ---- Info ---------------------------------------------------------------

    private void RefreshFormat()
    {
        var n = ReadNativeInfo();
        _sampleRate = (int)n.Format.SampleRate;
        _channels = (int)n.Format.Channels;
    }

    private FaStreamInfo ReadNativeInfo()
    {
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_info(Handle, out FaStreamInfo n));
        return n;
    }

    // The last info built, with the raw values it came from. Info compares the fresh native values against
    // these (cheap, no allocation) and only builds a new StreamInfo when something changed, so a stale
    // duration, length or bitrate (web and growing streams) is never returned.
    private sealed class InfoSnapshot
    {
        public InfoSnapshot(FaStreamInfo raw, StreamInfo info) { Raw = raw; Info = info; }
        public FaStreamInfo Raw;
        public readonly StreamInfo Info;
    }

    private InfoSnapshot? _infoSnapshot;

    private StreamInfo ReadInfo()
    {
        var n = ReadNativeInfo();
        var snap = _infoSnapshot;
        if (snap is not null && SameInfo(ref n, ref snap.Raw)) return snap.Info;
        var info = StreamInfo.FromNative(n);
        _infoSnapshot = new InfoSnapshot(n, info);
        return info;
    }

    private static unsafe bool SameInfo(ref FaStreamInfo a, ref FaStreamInfo b)
    {
        if (a.Format.SampleFormat != b.Format.SampleFormat || a.Format.SampleRate != b.Format.SampleRate ||
            a.Format.Channels != b.Format.Channels || a.Format.ChannelMask != b.Format.ChannelMask) return false;
        if (a.LengthFrames != b.LengthFrames || a.Seekable != b.Seekable || a.AudioTrackCount != b.AudioTrackCount ||
            a.HasCdg != b.HasCdg || a.Duration != b.Duration || a.FileSize != b.FileSize || a.AudioBytes != b.AudioBytes ||
            a.Bitrate != b.Bitrate || a.BitsPerSample != b.BitsPerSample) return false;
        fixed (byte* ac = a.Codec, bc = b.Codec, ak = a.Container, bk = b.Container)
            return CStringEquals(ac, bc) && CStringEquals(ak, bk);
    }

    private static unsafe bool CStringEquals(byte* a, byte* b)
    {
        for (int i = 0; i < 32; i++)
        {
            if (a[i] != b[i]) return false;
            if (a[i] == 0) return true;
        }
        return true;
    }

    /// <summary>
    /// Information about the decoded stream: the source's own format, codec, container and duration.
    /// The format stays the same when the stream is attached to a device; conversion to the device
    /// format happens inside the engine.
    /// </summary>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public StreamInfo Info => ReadInfo();

    // ---- Web streams ----------------------------------------------------------

    /// <summary>
    /// What a web stream is playing right now: the station and the song heard now. A title that is
    /// downloaded but still in the buffer is not returned until playback reaches it.
    /// <see cref="StreamMetadata.Empty"/> for streams that are not web streams.
    /// </summary>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public unsafe StreamMetadata Metadata
    {
        get
        {
            FaMetadata m;
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_metadata(Handle, &m));
            return new StreamMetadata(
                NativeText.Read(m.StreamTitle), NativeText.Read(m.Artist), NativeText.Read(m.Title), NativeText.Read(m.Url), NativeText.Read(m.Station), NativeText.Read(m.Genre),
                NativeText.Read(m.Description), NativeText.Read(m.StationUrl), NativeText.Read(m.ContentType), (int)m.BitrateKbps, m.Seq);
        }
    }

    /// <summary>The connection of a web stream (buffering, reconnects, the URL after redirects); null for other streams.</summary>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public unsafe NetworkStatus? NetworkStatus
    {
        get
        {
            FaNetStatus n;
            int rc = NativeMethods.fa_stream_get_net_status(Handle, &n);
            if (rc == (int)ErrorCode.InvalidArgument) return null;
            FatzAudioException.ThrowIfFailed(rc);
            return new NetworkStatus(
                n.Connected != 0, n.HttpStatus, n.IsLive != 0, n.IsHls != 0, n.Seekable != 0, n.Finished != 0,
                n.ContentLength, n.Downloaded, n.BufferedBytes, TimeConvert.FromNanoseconds(n.Buffered),
                (int)n.Reconnects, (ErrorCode)n.LastError, NativeText.Read(n.Url));
        }
    }

    /// <summary>A header of the web server's last response, for example "Content-Type" or "icy-name" (any case).</summary>
    /// <param name="name">The header name.</param>
    /// <returns>The value, or null when the response had no such header or this is not a web stream.</returns>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public unsafe string? GetHttpHeader(string name)
    {
        ArgumentNullException.ThrowIfNull(name);
        byte* buf = stackalloc byte[4096];
        int rc = NativeMethods.fa_stream_get_http_header(Handle, name, buf, 4096);
        if (rc == (int)ErrorCode.NotFound) return null;
        FatzAudioException.ThrowIfFailed(rc);
        return Marshal.PtrToStringUTF8((nint)buf);
    }

    private readonly object _downloadLock = new();
    private GCHandle? _download;

    // The handle of NetworkOptions.Downloaded (already adopted by the stream handle), so a later
    // SetDownloadHandler frees it like any replaced handler.
    internal void InitDownloadHandle(GCHandle? gc)
    {
        if (gc is { } g && g.Target is DownloadState st) st.Owner = this;
        _download = gc;
    }

    // What the native tap's user pointer leads to: the handler, and the stream it is for.
    internal sealed class DownloadState(DownloadHandler handler, AudioStream? owner)
    {
        public DownloadHandler Handler { get; } = handler;
        public AudioStream? Owner { get; set; } = owner;
    }

    /// <summary>
    /// Starts handing this web stream's raw bytes to <paramref name="handler"/> from the next downloaded byte on, to
    /// save a station while it plays; null stops it. Once this returns, the previous handler is not called again.
    /// To catch the stream from its first byte, set <see cref="NetworkOptions.Downloaded"/> when opening instead.
    /// After a seek that restarts the download elsewhere, the bytes continue from there.
    /// </summary>
    /// <param name="handler">The handler, or null.</param>
    /// <exception cref="InvalidOperationException">This is not a web stream.</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    /// <example>
    /// <code>
    /// var file = File.Create("recording.mp3");
    /// radio.SetDownloadHandler(data => file.Write(data));
    /// // ...
    /// radio.SetDownloadHandler(null);
    /// file.Dispose();
    /// </code>
    /// </example>
    public void SetDownloadHandler(DownloadHandler? handler)
    {
        var h = Handle;
        // From inside this stream's handler the lock is skipped: another thread holding it waits in the native call
        // for the handler to return, and makes its change after this one, in the same order as the native side.
        if (ReferenceEquals(t_inDownloadOf, this))
        {
            SwapDownloadHandler(h, handler);
            return;
        }
        lock (_downloadLock) SwapDownloadHandler(h, handler);
    }

    private unsafe void SwapDownloadHandler(StreamHandle h, DownloadHandler? handler)
    {
        GCHandle? gc = handler is null ? null : _handle.Pin(new DownloadState(handler, this));
        int rc = NativeMethods.fa_stream_set_download_callback(h, gc is null ? null : &OnDownload,
            gc is { } g ? (void*)GCHandle.ToIntPtr(g) : null);
        if (rc < 0)
        {
            if (gc is { } failed) _handle.Unpin(failed);
            if (rc == (int)ErrorCode.InvalidArgument) throw new InvalidOperationException("Not a web stream.");
            throw new FatzAudioException((ErrorCode)rc);
        }
        // The native side no longer calls the previous handler, so its handle can go.
        if (_download is { } previous) _handle.Unpin(previous);
        _download = gc;
    }

    [ThreadStatic] private static AudioStream? t_inDownloadOf;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    internal static unsafe void OnDownload(void* user, void* data, nuint size)
    {
        try
        {
            if (user != null && GCHandle.FromIntPtr((nint)user).Target is DownloadState st)
            {
                var outer = t_inDownloadOf;
                t_inDownloadOf = st.Owner;
                try { st.Handler(new ReadOnlySpan<byte>(data, checked((int)size))); }
                finally { t_inDownloadOf = outer; }
            }
        }
        catch
        {
            // never throw across the native boundary
        }
    }

    internal TimeSpan FramesToTime(long frames) => TimeConvert.FromFrames(frames, _sampleRate);
    internal long TimeToFrames(TimeSpan time) => TimeConvert.ToFrames(time, _sampleRate);

    // ---- Transport ------------------------------------------------------------

    /// <summary>The output device this stream is attached to, or null.</summary>
    public OutputDevice? Device => _device;

    /// <summary>
    /// Attaches the stream to <paramref name="device"/>, starts the device if necessary and starts playback.
    /// If the stream is already on another device it moves there and continues from the same position.
    /// </summary>
    /// <param name="device">The output device.</param>
    /// <exception cref="ArgumentNullException"><paramref name="device"/> is null.</exception>
    /// <exception cref="FatzAudioException">The stream is decode-only (<see cref="ErrorCode.InvalidArgument"/>) or the device failed.</exception>
    /// <example>
    /// <code>
    /// song.PlayOn(output);
    /// song.PlayOn(headphones); // moves to the other device, same position
    /// </code>
    /// </example>
    public void PlayOn(OutputDevice device)
    {
        ArgumentNullException.ThrowIfNull(device);
        AttachTo(device, null);
        device.Start();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_play(Handle));
    }

    /// <summary>
    /// Plays the stream through a mix bus: like <see cref="PlayOn(OutputDevice)"/> on the bus's device, with
    /// the bus's volume and pan applied. Moving a playing stream between buses of one device is seamless.
    /// </summary>
    /// <param name="bus">The bus.</param>
    /// <exception cref="ArgumentNullException"><paramref name="bus"/> is null.</exception>
    /// <exception cref="FatzAudioException">The stream is decode-only (<see cref="ErrorCode.InvalidArgument"/>) or the device failed.</exception>
    /// <example>
    /// <code>
    /// using var deck = output.CreateBus();
    /// song.PlayOn(deck);
    /// deck.Volume = 0.5f;
    /// </code>
    /// </example>
    public void PlayOn(MixBus bus)
    {
        ArgumentNullException.ThrowIfNull(bus);
        AttachTo(bus.Device, bus);
        bus.Device.Start();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_play(Handle));
    }

    /// <summary>
    /// The bus this stream plays through, or null for the device's master mix (also after the bus was disposed).
    /// </summary>
    public MixBus? Bus => _bus is { IsDisposed: false } b ? b : null;

    private MixBus? _bus;

    private void AttachTo(OutputDevice device, MixBus? bus)
    {
        var h = Handle;
        var dh = device.Handle;
        var bh = bus?.Handle;   // throws for a disposed bus
        if (ReferenceEquals(_device, device) && ReferenceEquals(Bus, bus)) return;
        bool added = false;
        try
        {
            bh?.DangerousAddRef(ref added);
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_attach(h, dh, bh?.DangerousGetHandle() ?? 0));
        }
        finally
        {
            if (added) bh!.DangerousRelease();
        }
        if (!ReferenceEquals(_device, device)) h.AttachDevice(dh);
        _device = device;
        _bus = bus;
    }

    internal bool IsDisposed => _handle.IsClosed;

    internal nint NativeHandle => _handle.DangerousGetHandle();

    // A DJ deck attached the stream to the mixer's device (fa_deck_load): mirror it here.
    internal void JoinedDeck(OutputDevice device)
    {
        var h = Handle;
        if (!ReferenceEquals(_device, device)) h.AttachDevice(device.Handle);
        _device = device;
        _bus = null;
    }

    // The deck let it go (unloaded: detached natively).
    internal void LeftDeck()
    {
        if (_handle.IsClosed) return;
        _handle.DetachDevice();
        _device = null;
        _bus = null;
    }

    // The native side attached `other` to this stream's device (crossfade, queue): mirror it here.
    private void AdoptDevice(AudioStream other)
    {
        if (_device is null || ReferenceEquals(other._device, _device)) return;
        other.Handle.AttachDevice(_device.Handle);
        other._device = _device;
        other._bus = Bus;
    }

    /// <summary>
    /// Removes the stream from its output device. It keeps its position, so a later
    /// <see cref="PlayOn(OutputDevice)"/> continues where it left off. Does nothing if the stream is not attached.
    /// </summary>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    /// <example>
    /// <code>
    /// song.Detach();
    /// Console.WriteLine(song.Position); // unchanged
    /// </code>
    /// </example>
    public void Detach()
    {
        var h = Handle;
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_detach(h));
        h.DetachDevice();
        _device = null;
        _bus = null;
    }

    /// <summary>
    /// Starts or resumes playback. A stopped or ended stream starts at <see cref="StreamMarkers.CueIn"/> (or the
    /// start); a paused one continues where it was. The stream must have been attached with <see cref="PlayOn(OutputDevice)"/> before.
    /// </summary>
    /// <exception cref="InvalidOperationException">The stream is not attached to a device.</exception>
    /// <exception cref="FatzAudioException">The native call failed.</exception>
    public void Play()
    {
        var h = Handle;
        if (_device is null) throw new InvalidOperationException("Call PlayOn(device) first.");
        _device.Start();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_play(h));
    }

    /// <summary>
    /// Like <see cref="Play"/>, but the first frame is heard exactly at <paramref name="deviceTime"/> on the
    /// <see cref="OutputDevice.Time"/> clock of the stream's device. Until then <see cref="State"/> is
    /// <see cref="StreamState.Scheduled"/>; <see cref="Started"/> is raised when it starts. A time already past
    /// starts it at once.
    /// </summary>
    /// <param name="deviceTime">Start time on the device clock.</param>
    /// <exception cref="InvalidOperationException">The stream is not attached to a device.</exception>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="deviceTime"/> is negative.</exception>
    /// <example>
    /// <code>
    /// drums.PlayOn(output); drums.Pause();
    /// bass.PlayOn(output);  bass.Pause();
    /// var at = output.Time + TimeSpan.FromMilliseconds(100);
    /// drums.PlayAt(at);
    /// bass.PlayAt(at);      // sample-locked
    /// </code>
    /// </example>
    public void PlayAt(TimeSpan deviceTime)
    {
        if (deviceTime < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(deviceTime));
        var h = Handle;
        if (_device is null) throw new InvalidOperationException("Call PlayOn(device) first.");
        _device.Start();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_play_at(h, TimeConvert.ToNanoseconds(deviceTime)));
    }

    /// <summary>Pauses playback, keeping the position. Also cancels a scheduled or queued start.</summary>
    /// <exception cref="FatzAudioException">The native call failed.</exception>
    public void Pause() => FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_pause(Handle));

    /// <summary>Stops playback and rewinds to <see cref="StreamMarkers.CueIn"/> (or the start). Also cancels a scheduled or queued start.</summary>
    /// <exception cref="FatzAudioException">The native call failed.</exception>
    public void Stop() => FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_stop(Handle));

    /// <summary>Current playback state.</summary>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public StreamState State
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_state(Handle, out int s));
            return (StreamState)s;
        }
    }

    /// <summary>Playback position; setting it seeks (a stopped stream becomes paused at the new position).</summary>
    /// <exception cref="ArgumentOutOfRangeException">The value is negative.</exception>
    /// <exception cref="FatzAudioException">Seeking failed (for example <see cref="ErrorCode.NotSeekable"/>).</exception>
    public TimeSpan Position
    {
        get => FramesToTime(PositionFrames);
        set
        {
            if (value < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(value));
            PositionFrames = TimeToFrames(value);
        }
    }

    /// <summary>Playback position in frames (sample exact); setting it seeks.</summary>
    /// <exception cref="ArgumentOutOfRangeException">The value is negative.</exception>
    /// <exception cref="FatzAudioException">Seeking failed (for example <see cref="ErrorCode.NotSeekable"/>).</exception>
    public long PositionFrames
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_position(Handle, out long f));
            return f;
        }
        set
        {
            if (value < 0) throw new ArgumentOutOfRangeException(nameof(value));
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_seek(Handle, value));
        }
    }

    /// <summary>
    /// Seeks without waiting: an engine worker drops the buffer and refills from <paramref name="position"/>,
    /// while the stream is silent. <see cref="Position"/> and <see cref="Clock"/> report the target at once. A newer
    /// call replaces a target not reached yet, so call it on every move of a seek bar. Use it from a UI thread and
    /// for web streams, whose seek waits for the network. Setting <see cref="Position"/>, playing from stopped
    /// or detaching carries out or replaces it; if the seek fails, playback goes on where it was and
    /// <see cref="SeekFailed"/> is raised. A stream that is not attached seeks at once, like setting
    /// <see cref="Position"/>.
    /// </summary>
    /// <example>
    /// <code>
    /// seekBar.ValueChanged += (_, e) => song.RequestSeek(TimeSpan.FromSeconds(e.NewValue));
    /// </code>
    /// </example>
    /// <exception cref="ArgumentOutOfRangeException">The position is negative.</exception>
    /// <exception cref="FatzAudioException">The stream cannot seek (<see cref="ErrorCode.NotSeekable"/>) or the
    /// position is past the end (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    public void RequestSeek(TimeSpan position)
    {
        if (position < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(position));
        RequestSeekFrames(TimeToFrames(position));
    }

    /// <summary>Like <see cref="RequestSeek(TimeSpan)"/>, in frames (sample exact).</summary>
    /// <exception cref="ArgumentOutOfRangeException">The frame is negative.</exception>
    /// <exception cref="FatzAudioException">The stream cannot seek or the frame is past the end.</exception>
    public void RequestSeekFrames(long frame)
    {
        if (frame < 0) throw new ArgumentOutOfRangeException(nameof(frame));
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_seek_async(Handle, frame));
    }

    /// <summary>
    /// The presentation clock: the timestamp of the audio leaving the speaker right now. Follow it with your video,
    /// CD+G or lyric renderer (show the frame whose timestamp is closest) and the picture stays in sync by itself.
    /// </summary>
    /// <remarks>
    /// It includes the device latency, moves smoothly between audio callbacks (so it can be read every video frame),
    /// stops while the stream is paused, stalled or ended, and jumps to the target on a seek. File streams report the
    /// position in the file; <see cref="FeedAudioStream"/>s report the timestamps you pushed, converted to time.
    /// Streams that are not playing report the time of their next frame.
    /// Cheap and lock-free for streams on a device and file streams, so a render loop can read it every vsync
    /// (240 Hz and more); it never waits for a seek, a decode worker or the audio thread.
    /// </remarks>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public TimeSpan Clock
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_clock(Handle, out long ns));
            return TimeConvert.FromNanoseconds(ns);
        }
    }

    // ---- CD+G -------------------------------------------------------------------

    /// <summary>
    /// True when the stream has CD+G karaoke graphics: a matching <c>.cdg</c> file found next to the audio
    /// file, a <c>.cdg</c> inside a ZIP archive, or graphics loaded with <see cref="LoadCdg(string)"/>.
    /// </summary>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public bool HasCdg => ReadNativeInfo().HasCdg != 0;   // no StreamInfo built: cheap to poll per frame

    /// <summary>Attaches CD+G graphics from a <c>.cdg</c> file to this stream, replacing any it had.</summary>
    /// <param name="path">The <c>.cdg</c> file; it is read in full.</param>
    /// <exception cref="FatzAudioException"><see cref="ErrorCode.NotFound"/> if the file is missing,
    /// <see cref="ErrorCode.UnsupportedFormat"/> if it holds no whole CD+G pack.</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public void LoadCdg(string path)
    {
        ArgumentNullException.ThrowIfNull(path);
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_load_cdg_path(Handle, path));
    }

    /// <summary>Attaches CD+G graphics held in memory (the bytes of a <c>.cdg</c> file); they are copied.</summary>
    /// <param name="cdg">The contents of a <c>.cdg</c> file.</param>
    /// <exception cref="FatzAudioException"><see cref="ErrorCode.UnsupportedFormat"/> if it holds no whole CD+G pack.</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public void LoadCdg(ReadOnlySpan<byte> cdg)
    {
        if (cdg.IsEmpty) throw new FatzAudioException(ErrorCode.UnsupportedFormat);
        fixed (byte* p = cdg)
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_load_cdg_memory(Handle, p, (nuint)cdg.Length));
    }

    /// <summary>
    /// Where the stream's CD+G graphics come from, how far apart their seek checkpoints are, and for disc tracks
    /// whether the subcode layout was chosen by the packs' parity.
    /// </summary>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public CdgInfo CdgInfo
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_cdg_info(Handle, out var i));
            return new CdgInfo((CdgSource)i.Source, (long)i.PackCount, TimeConvert.FromNanoseconds(i.CheckpointInterval),
                (int)i.CheckpointCount, i.DiscParity < 0 ? null : i.DiscParity != 0, (long)i.DiscCorrected, (long)i.DiscDropped);
        }
    }

    /// <summary>Removes the stream's CD+G graphics; surfaces show a blank picture afterwards.</summary>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public void RemoveCdg() => FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_load_cdg_path(Handle, null));

    /// <summary>
    /// Shifts the CD+G graphics against the audio, for files that are out of sync: positive shows them later,
    /// negative earlier. Default zero.
    /// </summary>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public TimeSpan CdgOffset
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_cdg_offset(Handle, out long ns));
            return TimeConvert.FromNanoseconds(ns);
        }
        set => FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_cdg_offset(Handle, TimeConvert.ToNanoseconds(value)));
    }

    /// <summary>
    /// Dynamic range of Dolby Digital (AC-3) and Dolby Digital Plus (E-AC-3) audio: the full mix, or the
    /// stream's own compression for normal or night listening. Applies to audio decoded from now on (what is
    /// already buffered plays out first; seek to hear it at once). Other formats ignore it.
    /// </summary>
    /// <exception cref="ArgumentException">An unknown <see cref="FatzAudio.DynamicRange"/>.</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public DynamicRange DynamicRange
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_dynamic_range(Handle, out int mode));
            return (DynamicRange)mode;
        }
        set
        {
            int rc = NativeMethods.fa_stream_set_dynamic_range(Handle, (int)value);
            if (rc == (int)ErrorCode.InvalidArgument)
                throw new ArgumentException("Unknown dynamic range.", nameof(value));
            FatzAudioException.ThrowIfFailed(rc);
        }
    }

    /// <summary>
    /// How Dolby Digital (AC-3) and Dolby Digital Plus (E-AC-3) audio with more than two channels is mixed down:
    /// with the stream's own levels (<see cref="FatzAudio.DolbyDownmix.Auto"/>, LoRo, LtRt) or the engine's
    /// downmix. Between Auto, LoRo and LtRt the change applies to audio decoded from now on, like
    /// <see cref="DynamicRange"/>. To or from Engine a playing stream rebuilds its chain and carries on where it
    /// plays, as <see cref="SetMatrix"/> does. Other formats ignore it.
    /// </summary>
    /// <exception cref="ArgumentException">An unknown <see cref="FatzAudio.DolbyDownmix"/>.</exception>
    /// <exception cref="FatzAudioException">To or from Engine on a playing live stream (<see cref="ErrorCode.NotSeekable"/>): change it while detached.</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public DolbyDownmix DolbyDownmix
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_dolby_downmix(Handle, out int mode));
            return (DolbyDownmix)mode;
        }
        set
        {
            int rc = NativeMethods.fa_stream_set_dolby_downmix(Handle, (int)value);
            if (rc == (int)ErrorCode.InvalidArgument)
                throw new ArgumentException("Unknown Dolby downmix.", nameof(value));
            FatzAudioException.ThrowIfFailed(rc);
        }
    }

    // ---- MIDI -----------------------------------------------------------------

    /// <summary>
    /// The tags, cover art and properties of the file this stream plays (see <see cref="Tags"/>). Streams of app
    /// callbacks, web streams and CD tracks give empty tags (web stream titles come from <see cref="Metadata"/>);
    /// MIDI streams give the title, artist and copyright of <see cref="MidiInfo"/>.
    /// </summary>
    /// <param name="options">What to skip.</param>
    /// <returns>The tags.</returns>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public Tags GetTags(TagReadOptions options = TagReadOptions.None) => Tags.FromStream(Handle, options);

    /// <summary>
    /// Format, names and counts of a tracker module (MOD, S3M, XM, IT, ...); null for streams that are not modules.
    /// </summary>
    /// <remarks>Read once and kept: a module's names do not change while the stream is open.</remarks>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public TrackerInfo? TrackerInfo
    {
        get
        {
            if (!_trackerInfoRead)
            {
                _trackerInfo = ReadTrackerInfo();
                _trackerInfoRead = true;
            }
            return _trackerInfo;
        }
    }

    private TrackerInfo? _trackerInfo;
    private bool _trackerInfoRead;

    private unsafe TrackerInfo? ReadTrackerInfo()
    {
        {
            FaTrackerInfo n = default;
            n.StructSize = (uint)sizeof(FaTrackerInfo);
            int rc = NativeMethods.fa_stream_get_tracker_info(Handle, &n);
            if (rc == (int)ErrorCode.InvalidArgument) return null;
            FatzAudioException.ThrowIfFailed(rc);
            return new TrackerInfo(NativeText.Read(n.Type), NativeText.Read(n.TypeName), NativeText.Read(n.Tracker), NativeText.Read(n.Title), NativeText.Read(n.Artist),
                TrackerText(0, 0), (int)n.Channels, (int)n.Orders, (int)n.Patterns, n.RestartOrder, n.RestartRow,
                TimeSpan.FromSeconds(n.Duration), TrackerTexts(1, n.Instruments), TrackerTexts(2, n.Samples),
                TrackerTexts(3, n.Channels), TrackerTexts(4, n.Patterns), TrackerTexts(5, n.Subsongs)) { Subsong = n.Subsong };
        }
    }

    private unsafe string TrackerText(int type, uint index)
    {
        nuint len = 0;
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_tracker_text(Handle, type, index, null, 0, &len));
        if (len == 0) return "";
        var bytes = new byte[(int)len + 1];
        fixed (byte* p = bytes)
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_tracker_text(Handle, type, index, p, (nuint)bytes.Length, &len));
        return System.Text.Encoding.UTF8.GetString(bytes, 0, (int)len);
    }

    private string[] TrackerTexts(int type, uint count)
    {
        var list = new string[count];
        for (uint i = 0; i < count; i++) list[i] = TrackerText(type, i);
        return list;
    }

    /// <summary>
    /// Where a tracker module first plays a row: the time from the start, or null when the song never plays it (a skipped
    /// order, or a row past the end of that order's pattern).
    /// </summary>
    /// <param name="order">Entry of the order list, 0-based.</param>
    /// <param name="row">Row of that order's pattern, 0-based.</param>
    /// <exception cref="ArgumentOutOfRangeException">A negative order or row.</exception>
    /// <exception cref="FatzAudioException">The stream is not a module (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public TimeSpan? GetTrackerTime(int order, int row)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(order);
        ArgumentOutOfRangeException.ThrowIfNegative(row);
        int rc = NativeMethods.fa_stream_get_tracker_time(Handle, (uint)order, (uint)row, out double s);
        if (rc == (int)ErrorCode.NotFound) return null;
        FatzAudioException.ThrowIfFailed(rc);
        return TimeSpan.FromSeconds(s);
    }

    /// <summary>Seeks a tracker module to where it first plays a row (see <see cref="GetTrackerTime"/>).</summary>
    /// <param name="order">Entry of the order list, 0-based.</param>
    /// <param name="row">Row of that order's pattern, 0-based.</param>
    /// <exception cref="ArgumentOutOfRangeException">A negative order or row.</exception>
    /// <exception cref="FatzAudioException">The song never plays that row (<see cref="ErrorCode.NotFound"/>), or the stream is not a module.</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public void SeekTracker(int order, int row)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(order);
        ArgumentOutOfRangeException.ThrowIfNegative(row);
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_seek_tracker(Handle, (uint)order, (uint)row));
    }

    private int GetTrackerParam(int param)
    {
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_tracker_param(Handle, param, out int v));
        return v;
    }

    private void SetTrackerParam(int param, int value) =>
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_tracker_param(Handle, param, value));

    /// <summary>Tracker modules: how samples are resampled. Live, heard after the decode-ahead buffer.</summary>
    /// <exception cref="FatzAudioException">The stream is not a module, or the value is not one of the enum's.</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public TrackerInterpolation TrackerInterpolation
    {
        get => (TrackerInterpolation)GetTrackerParam(0);
        set => SetTrackerParam(0, (int)value);
    }

    /// <summary>Tracker modules: stereo separation in percent, 0 (mono) to 200; 100 by default. Live.</summary>
    /// <exception cref="ArgumentOutOfRangeException">The value is outside 0..200.</exception>
    /// <exception cref="FatzAudioException">The stream is not a module.</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public int TrackerStereoSeparation
    {
        get => GetTrackerParam(1);
        set
        {
            ArgumentOutOfRangeException.ThrowIfNegative(value);
            ArgumentOutOfRangeException.ThrowIfGreaterThan(value, 200);
            SetTrackerParam(1, value);
        }
    }

    /// <summary>
    /// Tracker modules: volume ramping, -1 (default), 0 off (may click), 1..10 softer ramps (<c>BASS_MUSIC_RAMP</c>). Live.
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException">The value is outside -1..10.</exception>
    /// <exception cref="FatzAudioException">The stream is not a module.</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public int TrackerVolumeRamping
    {
        get => GetTrackerParam(2);
        set
        {
            ArgumentOutOfRangeException.ThrowIfLessThan(value, -1);
            ArgumentOutOfRangeException.ThrowIfGreaterThan(value, 10);
            SetTrackerParam(2, value);
        }
    }

    /// <summary>
    /// Tracker modules: Amiga MOD files sound as on an Amiga's Paula chip (true; about 19 times the CPU on those
    /// files) or with the ordinary resampler (false, the default). Live.
    /// </summary>
    /// <exception cref="FatzAudioException">The stream is not a module.</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public bool TrackerAmigaResampler
    {
        get => GetTrackerParam(3) != 0;
        set => SetTrackerParam(3, value ? 1 : 0);
    }

    /// <summary>
    /// Which rows of a tracker module raise <see cref="TrackerRowReached"/> as they are heard; off by default.
    /// </summary>
    /// <exception cref="FatzAudioException">The stream is not a module (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public TrackerRowEvents TrackerRowEvents
    {
        get => (TrackerRowEvents)GetTrackerParam(4);
        set => SetTrackerParam(4, (int)value);
    }

    /// <summary>
    /// The order and row of a tracker module heard right now (at <see cref="Clock"/>), to within about 2 ms of the
    /// row's start; null while that audio is not decoded yet (a stream never played or read, or just after a seek).
    /// Cheap and lock-free, like <see cref="Clock"/>: fine to read every video frame.
    /// </summary>
    /// <exception cref="FatzAudioException">The stream is not a module (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public TrackerPosition? TrackerPosition
    {
        get
        {
            int rc = NativeMethods.fa_stream_get_tracker_position(Handle, out uint order, out uint row);
            if (rc == (int)ErrorCode.NotFound) return null;
            FatzAudioException.ThrowIfFailed(rc);
            return new TrackerPosition((int)order, (int)row);
        }
    }

    /// <summary>Mutes or unmutes a tracker module's channel (0-based, the first 64), live.</summary>
    /// <param name="channel">Channel 0..Channels-1.</param>
    /// <param name="muted">True to mute.</param>
    /// <exception cref="FatzAudioException">The stream is not a module or the channel is out of range (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public void SetTrackerChannelMuted(int channel, bool muted)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(channel);
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_tracker_channel_mute(Handle, (uint)channel, muted ? 1 : 0));
    }

    /// <summary>Whether a tracker module's channel is muted by <see cref="SetTrackerChannelMuted"/>.</summary>
    /// <param name="channel">Channel 0..Channels-1.</param>
    /// <returns>True when muted.</returns>
    /// <exception cref="FatzAudioException">The stream is not a module or the channel is out of range.</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public bool IsTrackerChannelMuted(int channel)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(channel);
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_tracker_channel_mute(Handle, (uint)channel, out int m));
        return m != 0;
    }

    /// <summary>
    /// Title, format and text counts of a MIDI stream (Standard MIDI File, .kar or .rmi); null for streams that are not MIDI.
    /// </summary>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public unsafe MidiInfo? MidiInfo
    {
        get
        {
            FaMidiInfo n;
            int rc = NativeMethods.fa_stream_get_midi_info(Handle, &n);
            if (rc == (int)ErrorCode.InvalidArgument) return null;
            FatzAudioException.ThrowIfFailed(rc);
            return new MidiInfo(NativeText.Read(n.Title), NativeText.Read(n.Artist), NativeText.Read(n.Copyright), n.Format, (int)n.Tracks,
                (int)n.TextCount, (int)n.LyricCount, n.Karaoke != 0);
        }
    }

    /// <summary>
    /// Song credits of a MIDI stream (title, writer, lyricist, year, key, tempo and more, from the .kar header lines and
    /// the file's own events); null for streams that are not MIDI.
    /// </summary>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public unsafe MidiCredits? MidiCredits
    {
        get
        {
            FaMidiCredits n = default;
            n.StructSize = (uint)sizeof(FaMidiCredits);
            int rc = NativeMethods.fa_stream_get_midi_credits(Handle, &n);
            if (rc == (int)ErrorCode.InvalidArgument) return null;
            FatzAudioException.ThrowIfFailed(rc);
            string other = NativeText.Read(n.Other);
            return new MidiCredits((TextCodePage)n.CodePage, NativeText.Read(n.Title), NativeText.Read(n.Artist), NativeText.Read(n.Writer), NativeText.Read(n.Lyricist),
                NativeText.Read(n.Year), NativeText.Read(n.Copyright), NativeText.Read(n.Key), NativeText.Read(n.Tempo), NativeText.Read(n.TimeSignature),
                other.Length == 0 ? [] : other.Split('\n'));
        }
    }

    /// <summary>
    /// The lyrics and other text events of a MIDI stream in time order (empty for other streams). While the stream
    /// plays, <see cref="MidiTextReached"/> is raised as each one is heard; a karaoke screen can also follow
    /// <see cref="Clock"/> through this list.
    /// </summary>
    /// <example>
    /// <code>
    /// var line = new StringBuilder();
    /// foreach (var t in song.MidiTexts.Where(t =&gt; t.Type == MidiTextType.Lyric))
    /// {
    ///     if (t.Flags.HasFlag(MidiTextFlags.NewLine)) { Console.WriteLine(line); line.Clear(); }
    ///     line.Append(t.Text);
    /// }
    /// </code>
    /// </example>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public IReadOnlyList<MidiText> MidiTexts
    {
        get
        {
            var info = MidiInfo;
            if (info is null) return [];
            var list = new List<MidiText>(info.TextCount);
            for (int i = 0; i < info.TextCount; i++) list.Add(GetMidiText(i));
            return list;
        }
    }

    /// <summary>
    /// The tempo changes and time and key signatures of a MIDI stream in time order (empty for other streams).
    /// </summary>
    /// <example>
    /// <code>
    /// var tempo = song.MidiMarks.FirstOrDefault(m =&gt; m.Type == MidiMarkType.Tempo);
    /// double bpm = tempo is null ? 120 : 60_000_000.0 / tempo.Value;
    /// </code>
    /// </example>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public unsafe IReadOnlyList<MidiMark> MidiMarks
    {
        get
        {
            uint count = 0;
            if (NativeMethods.fa_stream_get_midi_marks(Handle, null, 0, &count) != 0 || count == 0) return [];
            var native = new FaMidiMark[count];
            fixed (FaMidiMark* p = native)
                FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_midi_marks(Handle, p, count, &count));
            var list = new List<MidiMark>(native.Length);
            foreach (var m in native)
            {
                var t = m;
                list.Add(new MidiMark((MidiMarkType)t.Type, t.Value, t.Value2, NativeText.Read(t.Text), TimeConvert.FromNanoseconds(t.Time), t.Frame, t.Track));
            }
            return list;
        }
    }

    private unsafe MidiText GetMidiText(int index)
    {
        FaMidiText t;
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_midi_text(Handle, (uint)index, &t));
        return new MidiText(index, (MidiTextType)t.Type, NativeText.Read(t.Text), (MidiTextFlags)t.Flags,
            TimeConvert.FromNanoseconds(t.Time), t.Frame, t.Track);
    }

    /// <summary>
    /// MIDI streams: semitones (-48..48) added to every note but the drums, from the next note on. The soundfont
    /// plays the transposed notes themselves, so a key change sounds as clean as the original (unlike
    /// <see cref="Pitch"/>, which shifts the rendered audio). Heard after the decode-ahead buffer (150 ms by default
    /// for MIDI streams).
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException">The value is outside -48..48.</exception>
    /// <exception cref="FatzAudioException">The stream is not MIDI (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public int MidiTranspose
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_midi_transpose(Handle, out int v));
            return v;
        }
        set
        {
            if (value is < -48 or > 48) throw new ArgumentOutOfRangeException(nameof(value));
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_midi_transpose(Handle, value));
        }
    }

    /// <summary>
    /// MIDI streams: reverb and chorus, on by default. How much of each channel goes to them follows the file's
    /// CC91 (reverb, 40 until the file sets it) and CC93 (chorus, 0) and the soundfont's effect sends. False plays the
    /// dry synthesizer, like <c>BASS_MIDI_NOFX</c>; the tails stop at once. Heard after the decode-ahead buffer.
    /// </summary>
    /// <exception cref="FatzAudioException">The stream is not MIDI (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public bool MidiEffects
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_midi_effects(Handle, out int v));
            return v != 0;
        }
        set => FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_midi_effects(Handle, value ? 1 : 0));
    }

    /// <summary>
    /// Mutes or unmutes a MIDI channel (0..15, MIDI channel 1..16): it goes silent at once and stays silent until
    /// unmuted, for example to turn a karaoke file's guide melody off. Master volume is <see cref="Volume"/> and the
    /// tempo is <see cref="Tempo"/>, as for every stream.
    /// </summary>
    /// <param name="channel">Channel 0..15.</param>
    /// <param name="muted">True to mute.</param>
    /// <exception cref="ArgumentOutOfRangeException">The channel is outside 0..15.</exception>
    /// <exception cref="FatzAudioException">The stream is not MIDI (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public void SetMidiChannelMuted(int channel, bool muted)
    {
        if (channel is < 0 or > 15) throw new ArgumentOutOfRangeException(nameof(channel));
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_midi_channel_mute(Handle, (uint)channel, muted ? 1 : 0));
    }

    /// <summary>Whether a MIDI channel (0..15) is muted by <see cref="SetMidiChannelMuted"/>.</summary>
    /// <param name="channel">Channel 0..15.</param>
    /// <returns>True when muted.</returns>
    /// <exception cref="ArgumentOutOfRangeException">The channel is outside 0..15.</exception>
    /// <exception cref="FatzAudioException">The stream is not MIDI (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public bool IsMidiChannelMuted(int channel)
    {
        if (channel is < 0 or > 15) throw new ArgumentOutOfRangeException(nameof(channel));
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_midi_channel_mute(Handle, (uint)channel, out int m));
        return m != 0;
    }

    /// <summary>
    /// Creates a surface that draws this stream's CD+G graphics, in sync with the audio, into a pixel buffer you
    /// copy to your renderer (a WPF <c>WriteableBitmap</c>, a Direct3D or OpenGL texture, a SkiaSharp bitmap).
    /// Call <see cref="CdgSurface.Update"/> from your render loop. Several surfaces may show one stream.
    /// </summary>
    /// <param name="options">Pixel format, size and drawing options; null = BGRA, 300x216.</param>
    /// <exception cref="ArgumentException">An option is out of range (size above 8192, unknown pixel format).</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public CdgSurface CreateCdgSurface(CdgSurfaceOptions? options = null) => new CdgSurface(this, options ?? new CdgSurfaceOptions());

    /// <summary>
    /// Creates a surface that draws this MIDI or .kar stream's lyrics as a karaoke screen, in sync with the audio
    /// (<see cref="LyricSurface"/>). A song without lyrics draws nothing.
    /// </summary>
    /// <param name="style">The look; null = <see cref="LyricPreset.Classic"/> with the system's UI font.</param>
    /// <exception cref="ArgumentException">Not a MIDI stream, or a style value out of its range.</exception>
    /// <exception cref="FatzAudioException"><see cref="ErrorCode.NotFound"/> when no font could be opened (a missing
    /// <see cref="LyricStyle.FontPath"/>, or no system font); <see cref="ErrorCode.UnsupportedFormat"/> for a file that
    /// is not a font.</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public LyricSurface CreateLyricSurface(LyricStyle? style = null) => new LyricSurface(this, style ?? new LyricStyle());

    // ---- Parameters -----------------------------------------------------------

    private float GetParam(AudioParameter p)
    {
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_param(Handle, (int)p, out float v));
        return v;
    }

    private void SetParam(AudioParameter p, float v) =>
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_param(Handle, (int)p, v));

    // ---- Equalizer ----------------------------------------------------------------

    /// <summary>
    /// Sets the stream's equalizer: up to 16 peak or shelf bands, applied in the mixer before volume, fades and
    /// normalization, so a change is heard at once and glides without clicks. An empty list turns it off. Flat bands
    /// cost nothing. Not applied to decode-only streams.
    /// </summary>
    /// <example><code>
    /// song.SetEqualizer(Equalizer.GetPreset(EqualizerPreset.Rock));
    /// song.SetEqualizerGain(0, 6f);   // the 31 Hz slider
    /// </code></example>
    /// <exception cref="ArgumentException">More than 16 bands.</exception>
    public unsafe void SetEqualizer(IReadOnlyList<EqualizerBand> bands)
    {
        ArgumentNullException.ThrowIfNull(bands);
        if (bands.Count > 16) throw new ArgumentException("At most 16 bands.", nameof(bands));
        var native = stackalloc FaEqBand[16];
        for (int i = 0; i < bands.Count; i++) native[i] = new FaEqBand(bands[i]);
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_eq(Handle, native, (uint)bands.Count));
    }

    /// <summary>Sets the equalizer to a built-in preset (ten octave bands).</summary>
    public void SetEqualizer(EqualizerPreset preset) => SetEqualizer(Equalizer.GetPreset(preset));

    /// <summary>The equalizer's bands as set (after clamping to the ranges; the frequency is also capped at just under half the device rate while playing).</summary>
    public unsafe EqualizerBand[] GetEqualizer()
    {
        var native = stackalloc FaEqBand[16];
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_eq(Handle, native, 16, out uint n));
        var result = new EqualizerBand[n];
        for (int i = 0; i < n; i++) result[i] = native[i].ToBand();
        return result;
    }

    /// <summary>Changes one band's gain, for a slider; glides while playing.</summary>
    /// <exception cref="FatzAudioException">No such band (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    public void SetEqualizerGain(int band, float gainDb)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(band);
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_eq_gain(Handle, (uint)band, gainDb));
    }

    /// <summary>Bypass without losing the bands: false glides to flat, true back to the bands. True by default.</summary>
    public bool EqualizerEnabled
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_eq_enabled(Handle, out int on));
            return on != 0;
        }
        set => FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_eq_enabled(Handle, value ? 1 : 0));
    }

    /// <summary>Linear gain of this stream (1 = unity).</summary>
    public float Volume { get => GetParam(AudioParameter.Volume); set => SetParam(AudioParameter.Volume, value); }

    /// <summary>
    /// Silences the stream without touching <see cref="Volume"/>: unmuting brings the volume back, and fades and ramps
    /// keep running while muted. Glides over 5 ms, so it never clicks. Position, clock and events carry on as usual.
    /// </summary>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public bool Muted
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_mute(Handle, out int m));
            return m != 0;
        }
        set => FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_mute(Handle, value ? 1 : 0));
    }

    /// <summary>
    /// Loops from the cue-out marker (or the end) back to the cue-in marker (or the start), without a gap. Can be
    /// changed at any time, also while playing; <see cref="StreamOptions.Loop"/> sets it at open. Turning it off
    /// lets the stream play on to its end. <see cref="Looped"/> is raised for each jump.
    /// </summary>
    /// <exception cref="FatzAudioException">Turning it on for a stream that cannot seek (push, packet and live web
    /// streams): <see cref="ErrorCode.NotSeekable"/>.</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public bool Loop
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_loop(Handle, out int on));
            return on != 0;
        }
        set => FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_loop(Handle, value ? 1 : 0));
    }

    /// <summary>
    /// Plays backwards (bass_fx's <c>BASS_FX_ReverseCreate</c> and <c>BASS_ATTRIB_REVERSE_DIR</c>). Can be changed at any
    /// time, also while playing: playing or paused, it turns at once (the last 200 ms heard play back the other way while
    /// the decoder catches up, so there is no gap); stopped or ended, it starts over at the position. Reversed, the position and
    /// the clock count down and the stream ends at the cue-in marker (or the start); playing from stopped or ended
    /// starts at the cue-out marker (or the end), and a loop wraps from its start to its end. Tempo, pitch, rate and
    /// fades work as forward. Events tied to a frame (markers, <see cref="Looped"/>, positions, MIDI texts, tracker rows)
    /// are raised as the position counts down past it, last first. On a DJ deck, sync, deck loops and beat jumps work
    /// mirrored: the deck is pulled onto the leader's beat with its beats counting down, and loops turn at their in
    /// point; with <see cref="DjDeck.Slip"/> on, turning back returns to where the deck would be. The blocks the source
    /// is decoded in are cross-faded where they meet.
    /// </summary>
    /// <exception cref="FatzAudioException">Turning it on for a stream that cannot seek (push and packet streams):
    /// <see cref="ErrorCode.NotSeekable"/>; for a web stream: <see cref="ErrorCode.NotImplemented"/>.</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public bool Reverse
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_reverse(Handle, out int on));
            return on != 0;
        }
        set => FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_reverse(Handle, value ? 1 : 0));
    }

    /// <summary>Stereo pan from -1 (left) to 1 (right).</summary>
    public float Pan { get => GetParam(AudioParameter.Pan); set => SetParam(AudioParameter.Pan, value); }

    /// <summary>
    /// Playback speed multiplier (0.1..10, clamped); pitch follows, like a turntable. Push and packet streams take it
    /// too: their <see cref="Clock"/> keeps reporting your timestamps and runs at this speed.
    /// </summary>
    public float Rate { get => GetParam(AudioParameter.Rate); set => SetParam(AudioParameter.Rate, value); }

    /// <summary>
    /// Tempo multiplier (0.1..10, clamped) with pitch preserved. Push and packet streams take it too: their
    /// <see cref="Clock"/> keeps reporting your timestamps and runs at this tempo.
    /// </summary>
    public float Tempo { get => GetParam(AudioParameter.Tempo); set => SetParam(AudioParameter.Tempo, value); }

    /// <summary>Pitch shift in semitones (-36..36, clamped) with tempo preserved.</summary>
    public float Pitch { get => GetParam(AudioParameter.Pitch); set => SetParam(AudioParameter.Pitch, value); }

    /// <summary>
    /// How tempo, pitch and rate are processed. Setting it takes effect at once; a new quality restarts the
    /// processing from the position being heard (on a stream that can't seek, after the next stop, seek or
    /// attach). Null restores the defaults.
    /// </summary>
    /// <remarks>The latency is clamped to 50 ms .. the engine's buffer.</remarks>
    /// <exception cref="ArgumentException">An unknown <see cref="StretchQuality"/>.</exception>
    [System.Diagnostics.CodeAnalysis.AllowNull]
    public TempoOptions TempoOptions
    {
        get
        {
            FaTempoOptions o;
            NativeMethods.fa_tempo_options_init(&o);
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_tempo_options(Handle, &o));
            return new TempoOptions
            {
                Quality = (StretchQuality)o.Quality,
                KeepFormants = o.KeepFormants != 0,
                Latency = TimeSpan.FromMilliseconds(o.LatencyMs),
            };
        }
        set
        {
            if (value is null)
            {
                FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_tempo_options(Handle, null));
                return;
            }
            FaTempoOptions o;
            NativeMethods.fa_tempo_options_init(&o);
            o.Quality = (int)value.Quality;
            o.KeepFormants = value.KeepFormants ? 1 : 0;
            o.LatencyMs = (uint)Math.Clamp(Math.Round(value.Latency.TotalMilliseconds), 1, 60000);
            int rc = NativeMethods.fa_stream_set_tempo_options(Handle, &o);
            if (rc == (int)ErrorCode.InvalidArgument)
                throw new ArgumentException("Unknown stretch quality.", nameof(value));
            FatzAudioException.ThrowIfFailed(rc);
        }
    }

    /// <summary>Gets any parameter by name.</summary>
    /// <param name="parameter">The parameter.</param>
    /// <returns>Its current value.</returns>
    public float GetParameter(AudioParameter parameter) => GetParam(parameter);

    /// <summary>Sets any parameter immediately.</summary>
    /// <param name="parameter">The parameter.</param>
    /// <param name="value">The new value.</param>
    public void SetParameter(AudioParameter parameter, float value) => SetParam(parameter, value);

    /// <summary>
    /// Ramps a parameter from its current value to a target over time and returns immediately.
    /// Ramps advance with playback (they wait while the stream is paused or not playing yet), so
    /// "ramp volume to 1, then play" is a fade-in. Use <see cref="RampAsync"/> to await the end.
    /// </summary>
    /// <param name="parameter">The parameter to ramp.</param>
    /// <param name="target">Target value.</param>
    /// <param name="duration">Ramp length.</param>
    /// <param name="curve">Ramp shape; defaults to <see cref="Curve.Linear"/>.</param>
    /// <param name="startAt">
    /// When to start, on the <see cref="OutputDevice.Time"/> clock of the stream's device; null starts now.
    /// The value holds until then. If the stream is not playing at that moment, the ramp starts when it plays.
    /// </param>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="duration"/> or <paramref name="startAt"/> is negative.</exception>
    /// <exception cref="FatzAudioException">The parameter cannot be ramped yet (<see cref="ErrorCode.NotImplemented"/> for rate, tempo and pitch), or <paramref name="startAt"/> was given for a stream that is not attached (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <example>
    /// <code>
    /// song.Ramp(AudioParameter.Volume, 0f, TimeSpan.FromSeconds(3), Curve.EqualPower);   // fade out now
    /// song.Ramp(AudioParameter.Volume, 0.2f, TimeSpan.FromSeconds(1), Curve.SCurve,
    ///           startAt: output.Time + TimeSpan.FromSeconds(10));                         // duck in 10 s
    /// </code>
    /// </example>
    public void Ramp(AudioParameter parameter, float target, TimeSpan duration, Curve? curve = null, TimeSpan? startAt = null)
    {
        if (duration < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(duration));
        if (startAt < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(startAt));
        var c = curve ?? Curve.Linear;
        var ramp = c.ToNative();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_ramp_param(
            Handle, (int)parameter, target, TimeConvert.ToNanoseconds(duration), StartTime(startAt), &ramp));
        GC.KeepAlive(c);
    }

    // 0 means "now" natively; a real start time of exactly 0 is in the past anyway.
    private static long StartTime(TimeSpan? at) => at is { } t ? Math.Max(1, TimeConvert.ToNanoseconds(t)) : 0;

    /// <summary>
    /// Starts a ramp and completes when the engine reports it finished: when the parameter
    /// reaches <paramref name="target"/>, or when a newer ramp or a direct set of the same parameter
    /// replaces it. Ramps advance with playback, so on a paused stream the task completes after resuming.
    /// </summary>
    /// <param name="parameter">The parameter to ramp.</param>
    /// <param name="target">Target value.</param>
    /// <param name="duration">Ramp length.</param>
    /// <param name="curve">Ramp shape; defaults to <see cref="Curve.Linear"/>.</param>
    /// <param name="cancellationToken">Cancels the wait (the native ramp continues).</param>
    /// <param name="startAt">When to start, on the <see cref="OutputDevice.Time"/> clock; null starts now.</param>
    /// <returns>A task that completes when the ramp has finished.</returns>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="duration"/> or <paramref name="startAt"/> is negative.</exception>
    /// <exception cref="FatzAudioException">The parameter cannot be ramped yet (<see cref="ErrorCode.NotImplemented"/> for rate, tempo and pitch).</exception>
    /// <example>
    /// <code>
    /// await song.RampAsync(AudioParameter.Volume, 0f, TimeSpan.FromSeconds(2));
    /// song.Stop();
    /// </code>
    /// </example>
    public Task RampAsync(AudioParameter parameter, float target, TimeSpan duration, Curve? curve = null,
        CancellationToken cancellationToken = default, TimeSpan? startAt = null)
    {
        if (duration < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(duration));
        if (startAt < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(startAt));
        long tag = Interlocked.Increment(ref s_nextRampTag);
        var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        lock (_ramps) _ramps[tag] = tcs;
        try
        {
            var c = curve ?? Curve.Linear;
            var ramp = c.ToNative();
            ramp.Tag = tag;
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_ramp_param(
                Handle, (int)parameter, target, TimeConvert.ToNanoseconds(duration), StartTime(startAt), &ramp));
            GC.KeepAlive(c);
        }
        catch
        {
            lock (_ramps) _ramps.Remove(tag);
            throw;
        }
        if (!cancellationToken.CanBeCanceled) return tcs.Task;
        return tcs.Task.WaitAsync(cancellationToken);
    }

    private readonly Dictionary<long, TaskCompletionSource> _ramps = new();
    private static long s_nextRampTag; // unique across streams: a crossfade raises its tag on both

    private void CompleteRamp(long tag)
    {
        TaskCompletionSource? tcs;
        lock (_ramps)
        {
            if (!_ramps.Remove(tag, out tcs)) return;
        }
        tcs.TrySetResult();
    }

    /// <summary>Raised when a ramp finished (reached its target or was replaced). Raised on the engine's event thread.</summary>
    public event EventHandler<StreamEventArgs>? RampCompleted;

    /// <summary>Marker positions (cue in/out, fades, mix point).</summary>
    public StreamMarkers Markers { get; }

    /// <summary>
    /// Crossfades now: <paramref name="next"/> starts (from its cue-in if stopped or ended, else where it is)
    /// and fades in while this stream fades out, both from the same frame. This stream's gain is the curve
    /// played backwards, so the pair is symmetric. When the fade-out is done this stream ends
    /// (<see cref="Ended"/>; <see cref="Play"/> starts it again from its cue-in). The fades are crossfade gains
    /// of their own: <see cref="Volume"/> is left as the user set it.
    /// </summary>
    /// <param name="next">The stream to fade in. It joins this stream's device and bus if it is not attached yet.</param>
    /// <param name="duration">Length of the crossfade.</param>
    /// <param name="curve">Fade shape; defaults to <see cref="Curve.EqualPower"/> (constant loudness).</param>
    /// <returns>A task that completes when the crossfade is done.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="next"/> is null.</exception>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="duration"/> is negative.</exception>
    /// <exception cref="FatzAudioException">This stream is not attached, or <paramref name="next"/> plays on another device (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <example>
    /// <code>
    /// await current.CrossfadeTo(next, TimeSpan.FromSeconds(5));
    /// current.Dispose();
    /// </code>
    /// </example>
    public Task CrossfadeTo(AudioStream next, TimeSpan duration, Curve? curve = null)
    {
        ArgumentNullException.ThrowIfNull(next);
        if (duration < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(duration));
        var h = Handle;
        var nh = next.Handle;
        if (_device is null) throw new InvalidOperationException("Call PlayOn(device) first.");
        long tag = Interlocked.Increment(ref s_nextRampTag);
        var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        lock (_ramps) _ramps[tag] = tcs;
        try
        {
            var c = curve ?? Curve.EqualPower;
            var ramp = c.ToNative();
            ramp.Tag = tag;
            _device.Start();
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_crossfade(h, nh, TimeConvert.ToNanoseconds(duration), &ramp));
            GC.KeepAlive(c);
        }
        catch
        {
            lock (_ramps) _ramps.Remove(tag);
            throw;
        }
        AdoptDevice(next);
        return tcs.Task;
    }

    /// <summary>
    /// Queues <paramref name="next"/> to start automatically, to the exact frame, when this stream reaches its
    /// mix point: <see cref="StreamMarkers.MixPoint"/> if set; else <paramref name="crossfade"/> before its end;
    /// else right after its last frame (gapless). With a crossfade, this stream fades out over it (and ends) unless
    /// it has a <see cref="StreamMarkers.FadeOutStart"/>, and <paramref name="next"/> fades in over it unless it has a
    /// <see cref="StreamMarkers.FadeInEnd"/>; markers win.
    /// </summary>
    /// <param name="next">
    /// The stream to start. It joins this stream's device and bus if needed, reports
    /// <see cref="StreamState.Scheduled"/> while it waits, and raises <see cref="Started"/> when it starts. It may
    /// have a next of its own, so a playlist can be queued as a chain.
    /// </param>
    /// <param name="crossfade">Crossfade length; zero (the default) plays gapless.</param>
    /// <param name="curve">Crossfade shape; defaults to <see cref="Curve.EqualPower"/>.</param>
    /// <exception cref="ArgumentNullException"><paramref name="next"/> is null.</exception>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="crossfade"/> is negative.</exception>
    /// <exception cref="FatzAudioException"><paramref name="next"/> is already playing (<see cref="ErrorCode.Busy"/>), on another device or this stream is not attached (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <example>
    /// <code>
    /// song1.PlayOn(output);
    /// song1.QueueNext(song2, TimeSpan.FromSeconds(6));    // 6 s crossfade
    /// song2.Started += (_, _) => song2.QueueNext(song3);  // then gapless into song3
    /// </code>
    /// </example>
    public void QueueNext(AudioStream next, TimeSpan crossfade = default, Curve? curve = null)
    {
        ArgumentNullException.ThrowIfNull(next);
        if (crossfade < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(crossfade));
        var h = Handle;
        var nh = next.Handle;
        var c = curve ?? Curve.EqualPower;
        var ramp = c.ToNative();
        bool added = false;
        try
        {
            nh.DangerousAddRef(ref added);
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_next(h, nh.DangerousGetHandle(), TimeConvert.ToNanoseconds(crossfade), &ramp));
        }
        finally
        {
            if (added) nh.DangerousRelease();
        }
        GC.KeepAlive(c);
        AdoptDevice(next);
        _queued = next;
    }

    /// <summary>Removes the stream queued with <see cref="QueueNext"/>; it goes back to stopped.</summary>
    public void ClearNext()
    {
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_next(Handle, 0, 0, null));
        _queued = null;
    }

    /// <summary>The stream queued with <see cref="QueueNext"/> that has not started yet, or null.</summary>
    public AudioStream? Next
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_next(Handle, out nint n));
            var q = _queued;
            return n != 0 && q is not null && !q._handle.IsClosed && q._handle.DangerousGetHandle() == n ? q : null;
        }
    }

    private AudioStream? _queued;

    /// <summary>
    /// Routes the source channels to the device's channels with your own gains: <c>weights[input, output]</c>
    /// is the gain from source channel <c>input</c> to device channel <c>output</c>. Use it for custom downmixes,
    /// swapping or muting channels, or sending one channel of a karaoke track to both speakers.
    /// </summary>
    /// <param name="weights">A [source channels, 2] array of gains.</param>
    /// <exception cref="ArgumentNullException"><paramref name="weights"/> is null.</exception>
    /// <exception cref="FatzAudioException">The array does not match the source's channel count and the device's 2 channels (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <exception cref="FatzAudioException">The stream is a playing forward-only source, which cannot re-buffer (<see cref="ErrorCode.NotSeekable"/>); set the matrix before <c>PlayOn</c>.</exception>
    /// <example>
    /// <code>
    /// // Karaoke: the guide vocal is on the right channel; play only the left (music) on both speakers.
    /// song.SetMatrix(new float[,] { { 1f, 1f }, { 0f, 0f } });
    /// </code>
    /// </example>
    public void SetMatrix(float[,] weights)
    {
        ArgumentNullException.ThrowIfNull(weights);
        fixed (float* p = weights)
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_matrix(
                Handle, p, (uint)weights.GetLength(0), (uint)weights.GetLength(1)));
        }
    }

    /// <summary>Restores the default channel mapping (undoes <see cref="SetMatrix"/>).</summary>
    public void ResetMatrix() => FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_matrix(Handle, null, 0, 0));

    // ---- Decode / analysis -----------------------------------------------------

    /// <summary>Pulls decoded PCM from a decode-only stream (<see cref="StreamOptions.DecodeOnly"/>).</summary>
    /// <param name="buffer">Receives interleaved 32-bit float samples; its length should be a multiple of the channel count.</param>
    /// <returns>The number of <b>frames</b> written (samples / channels); 0 at the end of the stream. For a
    /// <see cref="FeedAudioStream"/>, 0 before <see cref="FeedAudioStream.End"/> means nothing is queued yet: push more.</returns>
    /// <exception cref="FatzAudioException">The stream is not decode-only or decoding failed.</exception>
    /// <example>
    /// <code>
    /// var buf = new float[4096 * s.Info.Format.Channels];
    /// int frames;
    /// while ((frames = s.Read(buf)) > 0) { /* use buf */ }
    /// </code>
    /// </example>
    public int Read(Span<float> buffer)
    {
        int channels = Math.Max(1, _channels);
        long frames = buffer.Length / channels;
        if (frames == 0) return 0;
        fixed (float* p = buffer)
        {
            int rc = NativeMethods.fa_stream_read(Handle, p, frames, out long got);
            if (rc == (int)ErrorCode.EndOfStream) return 0; // native signals the end with FA_ERROR_END_OF_STREAM
            FatzAudioException.ThrowIfFailed(rc);
            return (int)got;
        }
    }

    /// <summary>
    /// Peak and RMS of the stream's audio being heard now, after its equalizer, before its volume, pan, fades and normalization:
    /// for a VU meter, call it from a timer. All zeros while the stream is not playing. The first call starts
    /// recording, so it reads silence.
    /// </summary>
    /// <param name="window">Length of the measurement window; defaults to 50 ms, at most about 1 s.</param>
    /// <returns>Levels for left and right (one value for a mono stream).</returns>
    /// <exception cref="FatzAudioException">A decode-only stream (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    public StreamLevels GetLevels(TimeSpan? window = null)
    {
        int ch = _channels == 1 ? 1 : 2;
        var peak = new float[ch];
        var rms = new float[ch];
        GetLevels(peak, rms, window);
        return new StreamLevels(peak, rms);
    }

    /// <summary>Like <see cref="GetLevels(TimeSpan?)"/>, into your own buffers (no allocation per call).</summary>
    /// <param name="peak">Receives the peaks: one value for a mono stream, else left and right (room for 2 always fits).</param>
    /// <param name="rms">Receives the RMS values, as many as the peaks.</param>
    /// <param name="window">Length of the measurement window; defaults to 50 ms, at most about 1 s.</param>
    /// <returns>The number of channels written: 1 for a mono stream, else 2.</returns>
    /// <exception cref="ArgumentException">A span shorter than the stream's channels (1 or 2).</exception>
    /// <exception cref="FatzAudioException">A decode-only stream (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public int GetLevels(Span<float> peak, Span<float> rms, TimeSpan? window = null)
    {
        var handle = Handle;
        int ch = _channels == 1 ? 1 : 2;
        if (peak.Length < ch || rms.Length < ch) throw new ArgumentException($"Room for {ch} channel(s) is needed.", peak.Length < ch ? nameof(peak) : nameof(rms));
        fixed (float* pp = peak)
        fixed (float* pr = rms)
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_levels(
                handle, TimeConvert.ToNanoseconds(window ?? StreamLevels.DefaultWindow), pp, pr, (uint)ch));
        }
        return ch;
    }

    /// <summary>
    /// The magnitude spectrum of the stream's audio being heard now, for a visualizer (same source and timing
    /// as <see cref="GetLevels(TimeSpan?)"/>). All zeros while the stream is not playing.
    /// </summary>
    /// <param name="size">Transform size: a power of two from 64 to 16384. 2048 at 48 kHz is 23.4 Hz per bin over 43 ms.</param>
    /// <param name="options">Channel and window.</param>
    /// <returns><paramref name="size"/> / 2 magnitudes and the bin width.</returns>
    /// <exception cref="FatzAudioException">An invalid size, or a decode-only stream (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    public Spectrum GetSpectrum(int size = 2048, SpectrumOptions options = SpectrumOptions.None)
    {
        var bins = new float[Math.Max(1, size / 2)];
        return new Spectrum(bins, GetSpectrumCore((uint)Math.Max(0, size), bins, options));
    }

    /// <summary>Like <see cref="GetSpectrum(int, SpectrumOptions)"/>, into your own buffer (no allocation per call).</summary>
    /// <param name="magnitudes">Half the transform size: 32 to 8192 values, a power of two.</param>
    /// <param name="options">Channel and window.</param>
    /// <returns>The bin width in Hz; 0 while the stream is not playing.</returns>
    /// <exception cref="FatzAudioException">An invalid length, or a decode-only stream (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    public float GetSpectrum(Span<float> magnitudes, SpectrumOptions options = SpectrumOptions.None)
    {
        return GetSpectrumCore((uint)magnitudes.Length * 2, magnitudes, options);
    }

    private unsafe float GetSpectrumCore(uint size, Span<float> bins, SpectrumOptions options)
    {
        float hz;
        fixed (float* p = bins)
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_spectrum(Handle, size, (uint)options, p, &hz));
        }
        return hz;
    }

    /// <summary>
    /// The best loudness known for this stream right now and the gain normalization gives it. Starts the
    /// analysis if it has not run yet (also with normalization off), and never waits for it: until
    /// <see cref="LoudnessFinal"/> is raised the value may be an estimate or unknown.
    /// </summary>
    /// <returns>The loudness.</returns>
    public Loudness GetLoudness()
    {
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_loudness(Handle, out FaLoudness l));
        return Loudness.From(l);
    }

    /// <summary>
    /// Supplies a loudness your app measured or stored earlier (for example from its library database).
    /// It wins over tags, the cache and scans, and cancels a scan in progress.
    /// </summary>
    /// <param name="integratedLufs">Integrated loudness in LUFS.</param>
    /// <param name="rangeLu">Loudness range in LU, if known.</param>
    /// <param name="truePeakDbtp">True peak in dBTP, if known.</param>
    /// <exception cref="ArgumentException"><paramref name="integratedLufs"/> is NaN.</exception>
    public void SetLoudness(double integratedLufs, double rangeLu = double.NaN, double truePeakDbtp = double.NaN)
    {
        if (double.IsNaN(integratedLufs)) throw new ArgumentException("Loudness must be a number.", nameof(integratedLufs));
        var l = new FaLoudness { IntegratedLufs = integratedLufs, RangeLu = rangeLu, TruePeakDbtp = truePeakDbtp, IsFinal = 1 };
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_loudness(Handle, in l));
    }

    /// <summary>
    /// Analyses this stream's file or memory without touching playback (it reads with its own decoder): the beat grid, the
    /// key, a waveform overview and the loudness, in one pass. MIDI files use the stream's soundfont. Results are cached when
    /// <see cref="AudioEngineOptions.CacheDirectory"/> is set. Blocks until done: call it from a worker thread, or use
    /// <see cref="AnalyzeAsync"/>.
    /// </summary>
    /// <param name="kinds">What to find out; defaults to the beat grid and the key.</param>
    /// <param name="options">Tempo range and cache use; null uses the defaults.</param>
    /// <returns>The result, a plain object with nothing to dispose.</returns>
    /// <exception cref="FatzAudioException">The stream cannot be read twice (web, push and app I/O streams: <see cref="ErrorCode.NotSeekable"/>), no decoder reads it, or the options are invalid.</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public TrackAnalysis Analyze(AnalysisKinds kinds = AnalysisKinds.Beats | AnalysisKinds.Key, AnalysisOptions? options = null) =>
        TrackAnalysis.Run(Handle, kinds, options);

    /// <summary>
    /// <see cref="Analyze"/> on a thread-pool thread. Disposing the stream while it runs is safe: the analysis keeps the
    /// native stream alive until it is done.
    /// </summary>
    /// <param name="kinds">What to find out; defaults to the beat grid and the key.</param>
    /// <param name="options">Tempo range and cache use; null uses the defaults.</param>
    /// <param name="cancellationToken">Stops the wait (the native analysis, which cannot be interrupted, runs to its end).</param>
    /// <returns>A task with the result.</returns>
    /// <exception cref="FatzAudioException">As for <see cref="Analyze"/>, thrown through the task.</exception>
    /// <exception cref="OperationCanceledException">The token was cancelled.</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public Task<TrackAnalysis> AnalyzeAsync(AnalysisKinds kinds = AnalysisKinds.Beats | AnalysisKinds.Key,
        AnalysisOptions? options = null, CancellationToken cancellationToken = default) =>
        TrackAnalysis.RunAsync(Handle, h => TrackAnalysis.Run(h, kinds, options), cancellationToken);

    /// <summary>
    /// The stream's beat grid, for decks (sync, beat loops, aligned mixes). Reading returns the grid set last, else, for MIDI
    /// files and tracker modules, the exact one from the file (for a module the first read renders it once without sound,
    /// about 0.25 s for a 4-minute song), else null. Setting one from an analysis (<see cref="TrackAnalysis.Beats"/>), from the
    /// user's correction or from your library replaces it; setting null removes it (a MIDI or tracker stream then reads its
    /// file's grid again).
    /// </summary>
    /// <exception cref="FatzAudioException">Setting: a tempo outside 0.1..100000, segments that do not start in rising order, a bar offset past the bar, or a confidence outside 0..1 (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    /// <example>
    /// <code>
    /// var t = engine.Analyze("song.mp3");
    /// deck.Stream.BeatGrid = t.Beats;
    /// </code>
    /// </example>
    public BeatGrid? BeatGrid
    {
        get => FatzAudio.BeatGrid.FromStream(Handle);
        set
        {
            var h = Handle;
            if (value is null)
                FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_beat_grid(h, null, null, 0));
            else
                value.Apply(h);
        }
    }

    /// <summary>
    /// Raises <see cref="PositionReached"/> once, the first time playback passes <paramref name="position"/>; the
    /// request is then gone. Add it again from the handler to hear about the next pass (a loop's next turn, or after
    /// seeking back). Frame-exact like markers: it follows tempo, rate and loops, and is raised as that audio is
    /// mixed, about one device latency before it is heard. Like markers it is raised playing in
    /// <see cref="Reverse"/> too. A seek over the position doesn't raise it: it stays armed. Streams playing on a
    /// device only. Up to 64 positions are armed at once; adding one that is already armed does nothing.
    /// </summary>
    /// <param name="position">The position to watch.</param>
    /// <exception cref="FatzAudioException">The position is negative or past the stream's length
    /// (<see cref="ErrorCode.InvalidArgument"/>), or 64 are armed already (<see cref="ErrorCode.Busy"/>).</exception>
    public void AddPositionEvent(TimeSpan position)
    {
        long frame = TimeToFrames(position);
        // -1 would be FA_FRAMES_UNSET (remove all); any negative position is an invalid argument.
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_add_position_event(Handle, frame < 0 ? -2 : frame));
    }

    /// <summary>Removes every position armed with <see cref="AddPositionEvent"/> that has not been reached.</summary>
    public void ClearPositionEvents() =>
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_add_position_event(Handle, -1));

    /// <summary>
    /// Adds a user DSP processor that changes the audio in place, after rate, tempo and pitch and before the
    /// equalizer, volume, fades and normalization. It runs where the stream is decoded, never on the audio thread, so
    /// a change is heard after the decode-ahead buffer: normally an engine worker, but the first 100 ms after a seek or
    /// <see cref="PlayOn"/> are decoded on the calling thread, and decode-only streams decode inside <see cref="Read"/>.
    /// Safe to add and remove while playing; once the handle is disposed it is never called again. It runs with the
    /// stream's locks held: don't use this stream (not even <see cref="Position"/>) from inside the processor, and
    /// don't wait there for a thread that might.
    /// </summary>
    /// <param name="processor">The processor.</param>
    /// <param name="priority">Order among processors: higher runs first; equal ones in the order added.</param>
    /// <returns>A handle; dispose it to remove the processor.</returns>
    public IDisposable AddDsp(DspProcessor processor, int priority = 0)
    {
        ArgumentNullException.ThrowIfNull(processor);
        var h = Handle;
        var gc = h.Pin(processor);
        int rc = NativeMethods.fa_stream_add_dsp(h, &DspTrampoline, (void*)GCHandle.ToIntPtr(gc), priority, out uint id);
        if (rc < 0)
        {
            h.Unpin(gc);
            throw new FatzAudioException((ErrorCode)rc);
        }
        return new DspRegistration(this, id, gc);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void DspTrampoline(float* frames, long count, uint channels, uint sampleRate, void* user)
    {
        try
        {
            if (GCHandle.FromIntPtr((nint)user).Target is DspProcessor p)
                p(new Span<float>(frames, checked((int)(count * channels))), (int)channels, (int)sampleRate);
        }
        catch
        {
            // never throw across the native boundary
        }
    }

    private sealed class DspRegistration : IDisposable
    {
        private AudioStream? _owner;
        private readonly uint _id;
        private readonly GCHandle _gc;

        public DspRegistration(AudioStream owner, uint id, GCHandle gc) { _owner = owner; _id = id; _gc = gc; }

        public void Dispose()
        {
            var o = Interlocked.Exchange(ref _owner, null);
            if (o is null || o._handle.IsClosed) return;
            NativeMethods.fa_stream_remove_dsp(o._handle, _id);
            o._handle.Unpin(_gc);
        }
    }

    // ---- Events ---------------------------------------------------------------

    /// <summary>Raised when playback reaches the end of the source. Raised on the engine's event thread (never the audio thread); handlers may call any FatzAudio method but should return quickly, since they delay later events.</summary>
    public event EventHandler? Ended;
    /// <summary>Raised when a web radio station's song title changes, at the moment the new song is heard (not when it was downloaded). When titles change faster than events are delivered (decode-only streams read faster than real time), only the latest title is raised. Raised on the engine's event thread (never the audio thread); handlers may call any FatzAudio method but should return quickly, since they delay later events.</summary>
    public event EventHandler<MetadataChangedEventArgs>? MetadataChanged;
    /// <summary>Raised when playback stalls waiting for data. Raised on the engine's event thread (never the audio thread); handlers may call any FatzAudio method but should return quickly, since they delay later events.</summary>
    public event EventHandler? Stalled;
    /// <summary>Raised when playback resumes after a stall. Raised on the engine's event thread (never the audio thread); handlers may call any FatzAudio method but should return quickly, since they delay later events.</summary>
    public event EventHandler? Resumed;
    /// <summary>Raised when a recoverable decode error occurred (data skipped, playback continues). Raised on the engine's event thread (never the audio thread); handlers may call any FatzAudio method but should return quickly, since they delay later events.</summary>
    public event EventHandler? DecodeError;
    /// <summary>Raised when the stream's final loudness is known (from tags, the cache or a scan); <see cref="GetLoudness"/> then returns it. Raised on the engine's event thread (never the audio thread); handlers may call any FatzAudio method but should return quickly, since they delay later events.</summary>
    public event EventHandler? LoudnessFinal;
    /// <summary>Raised when a position requested with <see cref="AddPositionEvent"/> is reached (<see cref="StreamEventArgs.Frame"/> is that position, in frames). Raised on the engine's event thread (never the audio thread); handlers may call any FatzAudio method but should return quickly, since they delay later events.</summary>
    public event EventHandler<StreamEventArgs>? PositionReached;
    /// <summary>Raised when a scheduled, crossfaded or queued start happened (<see cref="PlayAt"/>, <see cref="CrossfadeTo"/>, <see cref="QueueNext"/>). Raised on the engine's event thread (never the audio thread); handlers may call any FatzAudio method but should return quickly, since they delay later events.</summary>
    public event EventHandler? Started;
    /// <summary>Raised when a marker is passed (<see cref="StreamEventArgs.Marker"/> says which). Raised on the engine's event thread (never the audio thread); handlers may call any FatzAudio method but should return quickly, since they delay later events.</summary>
    public event EventHandler<StreamEventArgs>? MarkerReached;
    /// <summary>
    /// Raised when a MIDI lyric or other text event is heard (streams playing on a device; decode-only streams list
    /// them with <see cref="MidiTexts"/>). Best effort, like markers: under heavy load an event may be dropped, so a
    /// karaoke screen that must never miss a syllable follows <see cref="Clock"/> through <see cref="MidiTexts"/>.
    /// Raised on the engine's event thread (never the audio thread); handlers may call any FatzAudio method but should
    /// return quickly, since they delay later events.
    /// </summary>
    public event EventHandler<MidiTextEventArgs>? MidiTextReached;
    /// <summary>
    /// Raised each time a looping stream (<see cref="Loop"/>) jumps back. Like <see cref="MarkerReached"/> it is raised
    /// as the audio is mixed, about one device latency before the jump is heard; follow <see cref="Clock"/> for
    /// frame-exact timing.
    /// <see cref="StreamEventArgs.Frame"/> is the loop end it left and <see cref="StreamEventArgs.Value"/> the frame it
    /// went to (in <see cref="Reverse"/>, the loop start it left and the loop end it went to). Raised on the engine's event thread (never the audio thread); handlers may call any FatzAudio method
    /// but should return quickly, since they delay later events.
    /// </summary>
    public event EventHandler<StreamEventArgs>? Looped;
    /// <summary>
    /// Raised when the CD+G picture heard now changes: lyrics being coloured in, a screen clear, a colour change, a
    /// seek, new graphics. Call <see cref="CdgSurface.Update"/> on your surfaces then, instead of every frame. The
    /// first handler turns the native events on, and the first event after that covers the whole screen; removing the
    /// last handler turns them off. Raised on the engine's event thread (never the audio thread), at most once per
    /// few milliseconds; handlers may call any FatzAudio method but should return quickly.
    /// </summary>
    public event EventHandler<CdgChangedEventArgs>? CdgChanged
    {
        add
        {
            lock (_cdgEventLock)
            {
                if (_cdgChanged == null && value != null)
                    FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_cdg_events(Handle, 1));
                _cdgChanged += value;
            }
        }
        remove
        {
            lock (_cdgEventLock)
            {
                _cdgChanged -= value;
                if (_cdgChanged != null || _handle.IsClosed) return;
                try { NativeMethods.fa_stream_set_cdg_events(_handle, 0); }
                catch (ObjectDisposedException) { /* closed meanwhile: its events stopped with it */ }
            }
        }
    }
    private EventHandler<CdgChangedEventArgs>? _cdgChanged;
    private readonly object _cdgEventLock = new();

    /// <summary>
    /// Raised when a <see cref="RequestSeek(TimeSpan)"/> could not be carried out (for example a web server refusing
    /// the range); playback goes on where it was. <see cref="StreamEventArgs.Position"/> is the target and
    /// <see cref="StreamEventArgs.Value"/> the <see cref="ErrorCode"/>. Raised on the engine's event thread (never the
    /// audio thread); handlers may call any FatzAudio method but should return quickly, since they delay later events.
    /// </summary>
    public event EventHandler<StreamEventArgs>? SeekFailed;

    /// <summary>
    /// Raised as a tracker module's rows are heard, when <see cref="TrackerRowEvents"/> asks for them (every row, or
    /// the first row of each order). Raised on the engine's event thread, shortly after the row is mixed; best effort,
    /// like markers, so a display that must follow every row reads <see cref="TrackerPosition"/> instead.
    /// </summary>
    public event EventHandler<TrackerRowEventArgs>? TrackerRowReached;

    /// <summary>
    /// Raised when the stream's output changed: <see cref="StreamEventArgs.Value"/> 0 when the system moved it to the
    /// new default device (a headset plugged in), 1 when the device went away (unplugged) and the output was reopened
    /// on the default. The stream stays attached and plays on. Raised on the engine's event thread (never the audio
    /// thread); handlers may call any FatzAudio method but should return quickly, since they delay later events.
    /// </summary>
    public event EventHandler<StreamEventArgs>? DeviceChanged;

    /// <summary>
    /// False when the native library does not deliver events yet (the callback registration returned
    /// <see cref="ErrorCode.NotImplemented"/>). Poll <see cref="State"/> instead in that case.
    /// </summary>
    public bool EventsSupported { get; private set; }

    private sealed class EventState
    {
        public EventState(AudioStream s) => Stream = new WeakReference<AudioStream>(s);
        public WeakReference<AudioStream> Stream { get; }
    }

    private void RegisterEventCallback()
    {
        var gc = _handle.Pin(new EventState(this));
        int rc = NativeMethods.fa_stream_set_event_callback(_handle, &OnNativeEvent, (void*)GCHandle.ToIntPtr(gc));
        if (rc == (int)ErrorCode.NotImplemented)
        {
            _handle.Unpin(gc);
            return;
        }
        if (rc < 0)
        {
            _handle.Unpin(gc);
            throw new FatzAudioException((ErrorCode)rc);
        }
        EventsSupported = true;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnNativeEvent(FaEvent* ev, void* user)
    {
        try
        {
            if (ev == null || user == null) return;
            if (GCHandle.FromIntPtr((nint)user).Target is EventState st && st.Stream.TryGetTarget(out var stream))
                stream.Raise(ev->Type, ev->Frame, ev->Value);
        }
        catch
        {
            // never throw across the native boundary
        }
    }

    private void Raise(int type, long frame, long value)
    {
        try
        {
            switch (type)
            {
                case 0: Ended?.Invoke(this, EventArgs.Empty); break;
                case 1: PositionReached?.Invoke(this, new StreamEventArgs(FramesToTime(frame), frame, value)); break;
                case 2: MarkerReached?.Invoke(this, new StreamEventArgs(FramesToTime(frame), frame, value, (MarkerKind)value)); break;
                case 3:
                    if (MetadataChanged is { } onMeta)
                    {
                        // A newer title was heard before this event was delivered (a fast
                        // decode-only reader): its own event follows, so skip this stale one.
                        var md = Metadata;
                        if (md.Sequence <= value) onMeta(this, new MetadataChangedEventArgs(md, FramesToTime(frame), frame));
                    }
                    break;
                case 4: Stalled?.Invoke(this, EventArgs.Empty); break;
                case 5: Resumed?.Invoke(this, EventArgs.Empty); break;
                case 6: DecodeError?.Invoke(this, EventArgs.Empty); break;
                case 7: DeviceChanged?.Invoke(this, new StreamEventArgs(FramesToTime(frame), frame, value)); break;
                case 8: LoudnessFinal?.Invoke(this, EventArgs.Empty); break;
                case 9:
                    CompleteRamp(value);
                    RampCompleted?.Invoke(this, new StreamEventArgs(FramesToTime(frame), frame, value));
                    break;
                case 10: Started?.Invoke(this, EventArgs.Empty); break;
                case 11:
                    if (MidiTextReached is { } onText) onText(this, new MidiTextEventArgs(GetMidiText((int)value)));
                    break;
                case 12: Looped?.Invoke(this, new StreamEventArgs(FramesToTime(frame), frame, value)); break;
                case 13:
                    _cdgChanged?.Invoke(this, new CdgChangedEventArgs(
                        new CdgRect((int)(value & 0xFFF), (int)((value >> 12) & 0xFFF), (int)((value >> 24) & 0xFFF), (int)((value >> 36) & 0xFFF)),
                        ((value >> 48) & 1) != 0, frame));
                    break;
                case 14: SeekFailed?.Invoke(this, new StreamEventArgs(FramesToTime(frame), frame, value)); break;
                case 15:
                    TrackerRowReached?.Invoke(this, new TrackerRowEventArgs(
                        new TrackerPosition((int)((value >> 16) & 0xFFFF), (int)(value & 0xFFFF)), FramesToTime(frame), frame));
                    break;
            }
        }
        catch
        {
            // handler exceptions must not reach the native engine thread
        }
    }

    // ---- Lifetime -------------------------------------------------------------

    /// <summary>Stops and releases the stream. Safe to call more than once and after the engine was disposed.</summary>
    public void Dispose()
    {
        Dispose(true);
        GC.SuppressFinalize(this);
    }

    /// <summary>Releases the native stream.</summary>
    /// <param name="disposing">True when called from <see cref="Dispose()"/>.</param>
    protected virtual void Dispose(bool disposing)
    {
        if (!disposing) return;
        _handle.Dispose();
        List<TaskCompletionSource> pending;
        lock (_ramps)
        {
            pending = [.. _ramps.Values];
            _ramps.Clear();
        }
        foreach (var t in pending) t.TrySetCanceled();
    }
}
