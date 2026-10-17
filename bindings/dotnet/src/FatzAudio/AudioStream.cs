using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using FatzAudio.Native;

namespace FatzAudio;

/// <summary>Event data for stream events that carry a position (position reached, marker, device change).</summary>
public sealed class StreamEventArgs : EventArgs
{
    internal StreamEventArgs(TimeSpan position, long frame, long value)
    {
        Position = position;
        Frame = frame;
        Value = value;
    }

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
/// <see cref="AudioEngine.OpenMemory"/>, <see cref="AudioEngine.OpenStream"/>, <see cref="AudioEngine.OpenUrl(string, StreamOptions?)"/>
/// or <see cref="AudioEngine.CreatePushStream"/>.
/// </summary>
/// <remarks>
/// <para>
/// <b>Events</b> (<see cref="Ended"/>, <see cref="MetadataChanged"/>, <see cref="Stalled"/>, <see cref="Resumed"/>,
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
        var h = Handle;
        var dh = device.Handle;
        if (!ReferenceEquals(_device, device))
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_attach(h, dh, 0));
            h.AttachDevice(dh);
            _device = device;
        }
        device.Start();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_play(h));
    }

    /// <summary>
    /// Removes the stream from its output device. It keeps its position, so a later
    /// <see cref="PlayOn"/> continues where it left off. Does nothing if the stream is not attached.
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
    }

    /// <summary>Starts or resumes playback. The stream must have been attached with <see cref="PlayOn"/> before.</summary>
    /// <exception cref="InvalidOperationException">The stream is not attached to a device.</exception>
    /// <exception cref="FatzAudioException">The native call failed.</exception>
    public void Play()
    {
        var h = Handle;
        if (_device is null) throw new InvalidOperationException("Call PlayOn(device) first.");
        _device.Start();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_play(h));
    }

    /// <summary>Starts playback at a precise engine time (sample accurate).</summary>
    /// <param name="engineTime">Time on the <see cref="AudioEngine.Time"/> clock.</param>
    /// <exception cref="FatzAudioException">Not supported by the native build yet (<see cref="ErrorCode.NotImplemented"/>).</exception>
    public void PlayAt(TimeSpan engineTime) =>
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_play_at(Handle, TimeConvert.ToNanoseconds(engineTime)));

    /// <summary>Pauses playback, keeping the position.</summary>
    /// <exception cref="FatzAudioException">The native call failed.</exception>
    public void Pause() => FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_pause(Handle));

    /// <summary>Stops playback and rewinds to the start.</summary>
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

    /// <summary>Playback position; setting it seeks.</summary>
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
    /// Presentation time of the audio currently leaving the speaker: the position minus the device
    /// latency. Video and lyric renderers should follow this clock.
    /// </summary>
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
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="duration"/> is negative.</exception>
    /// <exception cref="FatzAudioException">The parameter cannot be ramped yet (<see cref="ErrorCode.NotImplemented"/> for rate, tempo and pitch).</exception>
    /// <example>
    /// <code>
    /// song.Ramp(AudioParameter.Volume, 0f, TimeSpan.FromSeconds(3), Curve.EqualPower); // fade out
    /// </code>
    /// </example>
    public void Ramp(AudioParameter parameter, float target, TimeSpan duration, Curve? curve = null)
    {
        if (duration < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(duration));
        var ramp = (curve ?? Curve.Linear).ToNative();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_ramp_param(
            Handle, (int)parameter, target, TimeConvert.ToNanoseconds(duration), 0, &ramp));
    }

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
    /// <returns>A task that completes when the ramp has finished.</returns>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="duration"/> is negative.</exception>
    /// <exception cref="FatzAudioException">The parameter cannot be ramped yet (<see cref="ErrorCode.NotImplemented"/> for rate, tempo and pitch).</exception>
    /// <example>
    /// <code>
    /// await song.RampAsync(AudioParameter.Volume, 0f, TimeSpan.FromSeconds(2));
    /// song.Stop();
    /// </code>
    /// </example>
    public Task RampAsync(AudioParameter parameter, float target, TimeSpan duration, Curve? curve = null,
        CancellationToken cancellationToken = default)
    {
        if (duration < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(duration));
        long tag = Interlocked.Increment(ref _nextRampTag);
        var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        lock (_ramps) _ramps[tag] = tcs;
        try
        {
            var ramp = (curve ?? Curve.Linear).ToNative();
            ramp.Tag = tag;
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_ramp_param(
                Handle, (int)parameter, target, TimeConvert.ToNanoseconds(duration), 0, &ramp));
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
    private long _nextRampTag;

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

    /// <summary>Fades this stream out and <paramref name="next"/> in simultaneously.</summary>
    /// <param name="next">The stream to fade in; it should be attached to the same device.</param>
    /// <param name="duration">Length of the crossfade.</param>
    /// <param name="curve">Fade shape; defaults to <see cref="Curve.EqualPower"/>.</param>
    /// <exception cref="ArgumentNullException"><paramref name="next"/> is null.</exception>
    /// <exception cref="FatzAudioException">Not supported by the native build yet (<see cref="ErrorCode.NotImplemented"/>).</exception>
    /// <example>
    /// <code>
    /// current.CrossfadeTo(next, TimeSpan.FromSeconds(5));
    /// </code>
    /// </example>
    public void CrossfadeTo(AudioStream next, TimeSpan duration, Curve? curve = null)
    {
        ArgumentNullException.ThrowIfNull(next);
        var ramp = (curve ?? Curve.EqualPower).ToNative();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_crossfade(Handle, next.Handle, TimeConvert.ToNanoseconds(duration), &ramp));
    }

    // ---- Decode / analysis -----------------------------------------------------

    /// <summary>Pulls decoded PCM from a decode-only stream (<see cref="StreamOptions.DecodeOnly"/>).</summary>
    /// <param name="buffer">Receives interleaved 32-bit float samples; its length should be a multiple of the channel count.</param>
    /// <returns>The number of <b>frames</b> written (samples / channels); 0 at the end of the stream.</returns>
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
    /// <summary>Raised when a marker is passed. Raised on the engine's event thread (never the audio thread); handlers may call any FatzAudio method but should return quickly, since they delay later events.</summary>
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
                case 2: MarkerReached?.Invoke(this, new StreamEventArgs(FramesToTime(frame), frame, value)); break;
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

/// <summary>
/// A stream fed by the application with PCM data (<see cref="AudioEngine.CreatePushStream"/>).
/// </summary>
/// <example>
/// <code>
/// using var push = engine.CreatePushStream(AudioFormat.Float32(48000, 2));
/// push.PlayOn(output);
/// push.Push(interleavedSamples);
/// </code>
/// </example>
public sealed unsafe class PushAudioStream : AudioStream
{
    private readonly AudioFormat _format;

    internal PushAudioStream(AudioEngine engine, StreamHandle handle, GCHandle? io, IoState? state, AudioFormat format)
        : base(engine, handle, io, state, format) => _format = format;

    /// <summary>The PCM format this stream accepts.</summary>
    public AudioFormat Format => _format;

    /// <summary>Pushes interleaved 32-bit float frames. The stream must have been created with <see cref="SampleFormat.Float32"/>.</summary>
    /// <param name="samples">Interleaved samples; the length must be a multiple of the channel count.</param>
    /// <param name="presentationTime">Optional presentation timestamp of the first frame.</param>
    /// <exception cref="InvalidOperationException">The stream format is not <see cref="SampleFormat.Float32"/>.</exception>
    /// <exception cref="ArgumentException">The sample count is not a multiple of the channel count.</exception>
    /// <exception cref="FatzAudioException">Not supported by the native build yet (<see cref="ErrorCode.NotImplemented"/>).</exception>
    public void Push(ReadOnlySpan<float> samples, TimeSpan? presentationTime = null)
    {
        if (_format.SampleFormat != SampleFormat.Float32)
            throw new InvalidOperationException("This push stream was not created with SampleFormat.Float32.");
        fixed (float* p = samples) PushCore(p, samples.Length, presentationTime);
    }

    /// <summary>Pushes interleaved signed 16-bit frames. The stream must have been created with <see cref="SampleFormat.Int16"/>.</summary>
    /// <param name="samples">Interleaved samples; the length must be a multiple of the channel count.</param>
    /// <param name="presentationTime">Optional presentation timestamp of the first frame.</param>
    /// <exception cref="InvalidOperationException">The stream format is not <see cref="SampleFormat.Int16"/>.</exception>
    /// <exception cref="ArgumentException">The sample count is not a multiple of the channel count.</exception>
    /// <exception cref="FatzAudioException">Not supported by the native build yet (<see cref="ErrorCode.NotImplemented"/>).</exception>
    public void Push(ReadOnlySpan<short> samples, TimeSpan? presentationTime = null)
    {
        if (_format.SampleFormat != SampleFormat.Int16)
            throw new InvalidOperationException("This push stream was not created with SampleFormat.Int16.");
        fixed (short* p = samples) PushCore(p, samples.Length, presentationTime);
    }

    private void PushCore(void* data, int sampleCount, TimeSpan? pts)
    {
        if (_format.Channels <= 0 || sampleCount % _format.Channels != 0)
            throw new ArgumentException("Sample count must be a multiple of the channel count.");
        void* plane = data;
        long frames = sampleCount / _format.Channels;
        long ns = pts is { } t ? TimeConvert.ToNanoseconds(t) : 0;
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_push_pcm(Handle, &plane, frames, ns));
    }
}
