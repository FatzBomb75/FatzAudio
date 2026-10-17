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

/// <summary>Callback for in-place DSP on interleaved float frames.</summary>
/// <param name="samples">Interleaved samples (frames x channels); modify in place.</param>
/// <param name="channels">Channel count.</param>
public delegate void DspProcessor(Span<float> samples, int channels);

/// <summary>
/// A playable or decodable audio source. Create one with <see cref="AudioEngine.OpenFile"/>,
/// <see cref="AudioEngine.OpenMemory"/>, <see cref="AudioEngine.OpenStream"/>, <see cref="AudioEngine.OpenUrl(string, StreamOptions?)"/>,
/// <see cref="AudioEngine.CreatePushStream"/> or <see cref="AudioEngine.OpenPackets"/>.
/// </summary>
/// <remarks>
/// <para>
/// <b>Events</b> (<see cref="Ended"/>, <see cref="Started"/>, <see cref="MetadataChanged"/>, <see cref="Stalled"/>, <see cref="Resumed"/>,
/// <see cref="DecodeError"/>, <see cref="LoudnessFinal"/>, <see cref="PositionReached"/>, <see cref="MarkerReached"/>,
/// <see cref="DeviceChanged"/>) are raised on a native engine thread, never on your UI thread. Handlers must return
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
        var format = AudioFormat.FromNative(n.Format);
        TimeSpan? duration = n.Duration >= 0 ? TimeConvert.FromNanoseconds(n.Duration) : null;
        return new StreamInfo(
            format,
            Marshal.PtrToStringUTF8((nint)n.Codec) ?? "",
            Marshal.PtrToStringUTF8((nint)n.Container) ?? "",
            duration, n.LengthFrames, n.Seekable != 0, n.AudioTrackCount, n.HasCdg != 0);
    }

    /// <summary>
    /// Information about the decoded stream: the source's own format, codec, container and duration.
    /// The format stays the same when the stream is attached to a device; conversion to the device
    /// format happens inside the engine.
    /// </summary>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public StreamInfo Info => ReadInfo();

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

    // ---- Parameters -----------------------------------------------------------

    private float GetParam(AudioParameter p)
    {
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_param(Handle, (int)p, out float v));
        return v;
    }

    private void SetParam(AudioParameter p, float v) =>
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_param(Handle, (int)p, v));

    /// <summary>Linear gain of this stream (1 = unity).</summary>
    public float Volume { get => GetParam(AudioParameter.Volume); set => SetParam(AudioParameter.Volume, value); }

    /// <summary>Stereo pan from -1 (left) to 1 (right).</summary>
    public float Pan { get => GetParam(AudioParameter.Pan); set => SetParam(AudioParameter.Pan, value); }

    /// <summary>Playback rate multiplier; pitch follows.</summary>
    public float Rate { get => GetParam(AudioParameter.Rate); set => SetParam(AudioParameter.Rate, value); }

    /// <summary>Tempo multiplier with pitch preserved.</summary>
    public float Tempo { get => GetParam(AudioParameter.Tempo); set => SetParam(AudioParameter.Tempo, value); }

    /// <summary>Pitch shift in semitones with tempo preserved.</summary>
    public float Pitch { get => GetParam(AudioParameter.Pitch); set => SetParam(AudioParameter.Pitch, value); }

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

    /// <summary>Measures peak and RMS levels of the audio currently playing.</summary>
    /// <param name="window">Length of the measurement window; defaults to 50 ms.</param>
    /// <returns>Levels per channel.</returns>
    /// <exception cref="FatzAudioException">Not supported by the native build yet (<see cref="ErrorCode.NotImplemented"/>).</exception>
    public StreamLevels GetLevels(TimeSpan? window = null)
    {
        int ch = Math.Max(1, _channels);
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

    /// <summary>Adds a user DSP processor that runs in place on interleaved float frames on the audio thread.</summary>
    /// <param name="processor">The processor. It runs on the real-time audio thread: do not block or allocate.</param>
    /// <param name="priority">Order among processors; lower runs first.</param>
    /// <returns>A handle; dispose it to remove the processor.</returns>
    /// <exception cref="FatzAudioException">Not supported by the native build yet (<see cref="ErrorCode.NotImplemented"/>).</exception>
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
    private static void DspTrampoline(float* frames, long count, uint channels, void* user)
    {
        try
        {
            if (GCHandle.FromIntPtr((nint)user).Target is DspProcessor p)
                p(new Span<float>(frames, checked((int)(count * channels))), (int)channels);
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
    /// <summary>Raised when tags or stream metadata (for example an ICY title) changed. Raised on the engine's event thread (never the audio thread); handlers may call any FatzAudio method but should return quickly, since they delay later events.</summary>
    public event EventHandler? MetadataChanged;
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
                case 3: MetadataChanged?.Invoke(this, EventArgs.Empty); break;
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
