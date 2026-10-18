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

    internal AudioStream(AudioEngine engine, StreamHandle handle, GCHandle? ioHandle, IoState? io, AudioFormat? knownFormat = null)
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

    internal static AudioStream Wrap(AudioEngine engine, int result, nint native, GCHandle? io, IoState? state = null)
    {
        FatzAudioException.ThrowIfFailed(result);
        var h = new StreamHandle();
        h.Initialize(native, engine.Handle);
        return new AudioStream(engine, h, io, state);
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
        var info = ReadInfo();
        _sampleRate = info.Format.SampleRate;
        _channels = info.Format.Channels;
    }

    private StreamInfo ReadInfo()
    {
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_info(Handle, out FaStreamInfo n));
        return StreamInfo.FromNative(n);
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
                Str(m.StreamTitle), Str(m.Artist), Str(m.Title), Str(m.Url), Str(m.Station), Str(m.Genre),
                Str(m.Description), Str(m.StationUrl), Str(m.ContentType), (int)m.BitrateKbps, m.Seq);
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
                (int)n.Reconnects, (ErrorCode)n.LastError, Str(n.Url));
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

    private static unsafe string Str(byte* p) => Marshal.PtrToStringUTF8((nint)p) ?? "";

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
    public bool HasCdg => Info.HasCdg;

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
                (int)i.CheckpointCount, i.DiscParity < 0 ? null : i.DiscParity != 0);
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
            return new MidiInfo(Str(n.Title), Str(n.Artist), Str(n.Copyright), n.Format, (int)n.Tracks,
                (int)n.TextCount, (int)n.LyricCount, n.Karaoke != 0);
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

    private unsafe MidiText GetMidiText(int index)
    {
        FaMidiText t;
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_midi_text(Handle, (uint)index, &t));
        return new MidiText(index, (MidiTextType)t.Type, Str(t.Text), (MidiTextFlags)t.Flags,
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

    /// <summary>Stereo pan from -1 (left) to 1 (right).</summary>
    public float Pan { get => GetParam(AudioParameter.Pan); set => SetParam(AudioParameter.Pan, value); }

    /// <summary>Playback speed multiplier (0.1..10, clamped); pitch follows, like a turntable.</summary>
    /// <exception cref="FatzAudioException">On a push or packet stream, which takes <see cref="Pitch"/> only (<see cref="ErrorCode.NotImplemented"/>).</exception>
    public float Rate { get => GetParam(AudioParameter.Rate); set => SetParam(AudioParameter.Rate, value); }

    /// <summary>Tempo multiplier (0.1..10, clamped) with pitch preserved.</summary>
    /// <exception cref="FatzAudioException">On a push or packet stream, which takes <see cref="Pitch"/> only (<see cref="ErrorCode.NotImplemented"/>).</exception>
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
        fixed (float* pp = peak)
        fixed (float* pr = rms)
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_levels(
                Handle, TimeConvert.ToNanoseconds(window ?? TimeSpan.FromMilliseconds(50)), pp, pr, (uint)ch));
        }
        return new StreamLevels(peak, rms);
    }

    /// <summary>
    /// The magnitude spectrum of the stream's audio being heard now, for a visualizer (same source and timing
    /// as <see cref="GetLevels"/>). All zeros while the stream is not playing.
    /// </summary>
    /// <param name="size">Transform size: a power of two from 64 to 16384. 2048 at 48 kHz is 23.4 Hz per bin over 43 ms.</param>
    /// <param name="options">Channel and window.</param>
    /// <returns><paramref name="size"/> / 2 magnitudes and the bin width.</returns>
    /// <exception cref="FatzAudioException">An invalid size, or a decode-only stream (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    public Spectrum GetSpectrum(int size = 2048, SpectrumOptions options = SpectrumOptions.None)
    {
        var bins = new float[Math.Max(1, size / 2)];
        float hz;
        fixed (float* p = bins)
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_spectrum(Handle, (uint)Math.Max(0, size), (uint)options, p, &hz));
        }
        return new Spectrum(bins, hz);
    }

    /// <summary>Like <see cref="GetSpectrum(int, SpectrumOptions)"/>, into your own buffer (no allocation per call).</summary>
    /// <param name="magnitudes">Half the transform size: 32 to 8192 values, a power of two.</param>
    /// <param name="options">Channel and window.</param>
    /// <returns>The bin width in Hz; 0 while the stream is not playing.</returns>
    /// <exception cref="FatzAudioException">An invalid length, or a decode-only stream (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    public float GetSpectrum(Span<float> magnitudes, SpectrumOptions options = SpectrumOptions.None)
    {
        float hz;
        fixed (float* p = magnitudes)
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_spectrum(Handle, (uint)magnitudes.Length * 2, (uint)options, p, &hz));
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

    /// <summary>Requests a <see cref="PositionReached"/> event when playback reaches a position.</summary>
    /// <param name="position">The position to watch.</param>
    /// <exception cref="FatzAudioException">Not supported by the native build yet (<see cref="ErrorCode.NotImplemented"/>).</exception>
    public void AddPositionEvent(TimeSpan position) =>
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_add_position_event(Handle, TimeToFrames(position)));

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
    /// <summary>Raised when a position requested with <see cref="AddPositionEvent"/> is reached. Raised on the engine's event thread (never the audio thread); handlers may call any FatzAudio method but should return quickly, since they delay later events.</summary>
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
    /// went to. Raised on the engine's event thread (never the audio thread); handlers may call any FatzAudio method
    /// but should return quickly, since they delay later events.
    /// </summary>
    public event EventHandler<StreamEventArgs>? Looped;
    /// <summary>Raised when the output device changed. Raised on the engine's event thread (never the audio thread); handlers may call any FatzAudio method but should return quickly, since they delay later events.</summary>
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
