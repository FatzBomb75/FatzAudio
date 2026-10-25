using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using FatzAudio.Native;

namespace FatzAudio;

/// <summary>Where the headphone cue of a <see cref="DjMixer"/> goes.</summary>
public sealed class CueOutput
{
    private CueOutput(int mode, int firstChannel, AudioDeviceInfo? headphones = null, bool lineUp = true)
    {
        Mode = mode;
        FirstChannel = firstChannel;
        Headphones = headphones;
        LineUp = lineUp;
    }

    internal int Mode { get; }

    /// <summary>The headphones of <see cref="Device"/>; null = the system default output (or not a second device).</summary>
    public AudioDeviceInfo? Headphones { get; }

    /// <summary>Whether <see cref="Device"/> lines the two outputs up (see <see cref="DjMixer.CueTrim"/>).</summary>
    public bool LineUp { get; }

    /// <summary>The first of the two cue channels, counted from 0 (<see cref="Channels"/> only).</summary>
    public int FirstChannel { get; }

    /// <summary>No headphone cue.</summary>
    public static CueOutput None { get; } = new(0, 0);

    /// <summary>
    /// Channels 3 and 4 of the master device (DJ controllers and many USB interfaces have 4 outputs): the same
    /// clock as the master, so no drift and no delay.
    /// </summary>
    public static CueOutput Channels34 { get; } = new(1, 2);

    /// <summary>
    /// One stereo output with a Y-cable: the master in mono on the left, the cue in mono on the right.
    /// </summary>
    public static CueOutput Split { get; } = new(3, 0);

    /// <summary>
    /// A second output device, such as USB headphones. It runs on its own clock: the cue reaches it through a small
    /// buffer kept steady by a resampler that moves by at most 0.05% (about 1 cent), whatever the drift or the rates.
    /// With <paramref name="lineUp"/> the faster output is delayed so the master in the headphones lands with the
    /// speakers (<see cref="DjMixer.Latency"/> says which one), except that the master is never delayed while a mic is
    /// monitored. A delay on the master is part of every heard position (<see cref="AudioStream.Clock"/>, CD+G and
    /// lyric surfaces). Not on iOS (use <see cref="Split"/>).
    /// </summary>
    /// <param name="headphones">The headphones (a playback device); null = the system default output.</param>
    /// <param name="lineUp">False delays neither output (the lowest latency on both).</param>
    /// <exception cref="ArgumentException"><paramref name="headphones"/> is not a playback device.</exception>
    public static CueOutput Device(AudioDeviceInfo? headphones = null, bool lineUp = true)
    {
        if (headphones is { Kind: not DeviceKind.Playback })
            throw new ArgumentException("Not a playback device.", nameof(headphones));
        return new CueOutput(2, 0, headphones, lineUp);
    }

    /// <summary>Two other channels of the master device, for a sound card with more outputs (channels 5-6 of a 5.1 card: 4).</summary>
    /// <param name="firstChannel">The first of the two cue channels, counted from 0 (2 or more).</param>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="firstChannel"/> is below 2.</exception>
    public static CueOutput Channels(int firstChannel)
    {
        if (firstChannel < 2 || firstChannel > 30) throw new ArgumentOutOfRangeException(nameof(firstChannel));
        return new CueOutput(1, firstChannel);
    }
}

/// <summary>How small the mixer's output period is.</summary>
public enum LatencyMode
{
    /// <summary>The engine's period (<see cref="AudioEngineOptions"/>).</summary>
    Normal = 0,
    /// <summary>
    /// The smallest shared-mode period the system gives: WASAPI's low-latency mode on Windows (at the device's own
    /// sample rate; it needs a driver that offers small periods), about 5 ms elsewhere.
    /// </summary>
    Low = 1,
    /// <summary>
    /// Windows WASAPI exclusive, for live mic monitoring: 3 ms periods for the output and the mic inputs. Other apps
    /// can't use those devices meanwhile, and the Windows volume no longer applies (the mixer's gains and
    /// <see cref="OutputDevice.Volume"/> still do). <see cref="ErrorCode.NotImplemented"/> on other systems;
    /// <see cref="ErrorCode.Device"/> when another app holds the device.
    /// </summary>
    Exclusive = 2,
}

/// <summary>How the crossfader shares the level between its two sides.</summary>
public enum CrossfaderCurve
{
    /// <summary>Steady loudness over the travel: each side at -3 dB in the middle (the default).</summary>
    ConstantPower = 0,
    /// <summary>Each side at -6 dB in the middle: a slight dip.</summary>
    Linear = 1,
}

/// <summary>Which side of the crossfader a deck is on.</summary>
public enum CrossfaderSide
{
    /// <summary>Not on the crossfader: always at full level.</summary>
    Thru = 0,
    /// <summary>Side A (crossfader at 0).</summary>
    A = 1,
    /// <summary>Side B (crossfader at 1).</summary>
    B = 2,
}

/// <summary>Mixer parameters that can be ramped with <see cref="DjMixer.Ramp"/>.</summary>
public enum MixerParameter
{
    /// <summary>The crossfader: 0 = side A only, 0.5 = the middle, 1 = side B only.</summary>
    Crossfader = 0,
    /// <summary>Linear master gain before the limiter, 0..4.</summary>
    MasterGain = 1,
    /// <summary>The headphones' blend: 0 = cue only, 1 = master only.</summary>
    CueMix = 2,
    /// <summary>Linear headphone gain, 0..4.</summary>
    CueGain = 3,
}

/// <summary>Deck parameters that can be ramped with <see cref="DjDeck.Ramp"/>.</summary>
public enum DeckParameter
{
    /// <summary>The channel fader, linear 0..1.</summary>
    Gain = 0,
    /// <summary>The trim in dB, -12..12.</summary>
    TrimDb = 1,
    /// <summary>The low band in dB, <see cref="DeckEq.Kill"/>..6.</summary>
    EqLowDb = 2,
    /// <summary>The mid band in dB, <see cref="DeckEq.Kill"/>..6.</summary>
    EqMidDb = 3,
    /// <summary>The high band in dB, <see cref="DeckEq.Kill"/>..6.</summary>
    EqHighDb = 4,
}

/// <summary>Options for <see cref="AudioEngine.CreateDjMixer"/>.</summary>
public sealed class DjMixerOptions
{
    /// <summary>Number of decks: 2 (more are planned).</summary>
    public int Decks { get; init; } = 2;

    /// <summary>The master output; null = the system default device (followed when it changes).</summary>
    public AudioDeviceInfo? Output { get; init; }

    /// <summary>Where the headphone cue goes; defaults to <see cref="CueOutput.None"/>.</summary>
    public CueOutput Cue { get; init; } = CueOutput.None;

    /// <summary>The output period; defaults to <see cref="LatencyMode.Low"/>.</summary>
    public LatencyMode Latency { get; init; } = LatencyMode.Low;
}

/// <summary>What a <see cref="DjMixer"/>'s outputs and inputs add up to. Times the system does not report are estimated from the device's buffer sizes.</summary>
/// <param name="Master">The master output: the system's buffer plus the limiter's look-ahead (about 2 ms).</param>
/// <param name="Cue">The headphones: the output's buffer (the cue is not limited, so it has no look-ahead); on a
/// second device also the bridge and any alignment delay.</param>
/// <param name="CueAlignment">The delay added to line up a second-device cue (on the output <paramref name="CueAligned"/> names).</param>
/// <param name="MicInput">Per mic slot: the input buffer plus the drift-corrected bridge; null = no mic in the slot.</param>
/// <param name="MicMonitor">Per mic slot: mic to speaker, the input plus the master (with any delay on it); null = no mic in the slot.</param>
/// <param name="CueAligned">Which output <paramref name="CueAlignment"/> delays, or why neither is.</param>
public sealed record MixerLatency(TimeSpan Master, TimeSpan Cue, TimeSpan CueAlignment,
    IReadOnlyList<TimeSpan?> MicInput, IReadOnlyList<TimeSpan?> MicMonitor, CueAlign CueAligned = CueAlign.None);

/// <summary>How a second-device cue (<see cref="CueOutput.Device"/>) is lined up with the master.</summary>
public enum CueAlign
{
    /// <summary>Nothing delayed: no second device (or not open yet), or the outputs already line up (within 1 ms).</summary>
    None = 0,
    /// <summary>The headphones are delayed by <see cref="MixerLatency.CueAlignment"/>.</summary>
    Cue = 1,
    /// <summary>The master is delayed by <see cref="MixerLatency.CueAlignment"/> (part of <see cref="MixerLatency.Master"/> and every heard position).</summary>
    Master = 2,
    /// <summary>The master is the faster output but a mic is monitored, so it is not delayed: the headphones are <see cref="MixerLatency.Cue"/> - <see cref="MixerLatency.Master"/> late.</summary>
    Skipped = 3,
    /// <summary>Lining up is off (<see cref="CueOutput.Device"/> with lineUp false).</summary>
    Off = 4,
}

/// <summary>A deck loop between two source frames.</summary>
/// <param name="In">The loop's first frame.</param>
/// <param name="Out">The frame it turns back at.</param>
public readonly record struct DeckLoop(long In, long Out);

/// <summary>
/// A DJ mixer: decks on one output device, each with a channel fader, a trim, an isolator EQ with kills and a meter,
/// a crossfader between them, a master gain and a limiter that is always on, plus a headphone cue.
/// </summary>
/// <remarks>
/// Decks hold ordinary <see cref="AudioStream"/>s: play, pause, seek, tempo, pitch, markers, CD+G and lyrics work on
/// them as on any stream, and every deck shares the master device's clock (<see cref="Output"/>), so
/// <see cref="AudioStream.PlayAt"/> on two decks starts them on the same frame. Disposing the mixer unloads its decks
/// (their streams stay open) and closes the device.
/// </remarks>
/// <example>
/// <code>
/// using var mixer = engine.CreateDjMixer(new DjMixerOptions { Cue = CueOutput.Channels34 });
/// DjDeck a = mixer.Decks[0], b = mixer.Decks[1];
/// a.Load(engine.OpenFile(@"C:\karaoke\song1.mp3"));
/// a.Stream!.Play();
/// a.Eq.Low = -6;
/// a.Eq.High = DeckEq.Kill;
/// b.Cue = true;                                        // in the headphones
/// mixer.Crossfader.Ramp(1f, TimeSpan.FromSeconds(8));  // to side B over 8 s
/// Span&lt;float&gt; peak = stackalloc float[2], rms = stackalloc float[2];
/// a.GetLevels(peak, rms);                              // the channel meter, each frame without allocating
/// </code>
/// </example>
public sealed partial class DjMixer : IDisposable
{
    private readonly nint _mixer;
    private readonly DjDeck[] _decks;
    private readonly List<Mic> _mics = new();
    private IReadOnlyList<Mic> _micsView = Array.Empty<Mic>();   // what Mics returns: made again only when a mic comes or goes

    internal unsafe DjMixer(AudioEngine engine, nint mixer, OutputDevice output)
    {
        Engine = engine;
        _mixer = mixer;
        Output = output;
        _decks = new DjDeck[2];
        for (int i = 0; i < _decks.Length; i++)
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_get_deck(mixer, (uint)i, out nint d));
            _decks[i] = new DjDeck(this, i, d);
        }
        Crossfader = new DjCrossfader(this);
        AutoMix = new DjAutoMix(this);
        // The state lives until the device closes (fa_mixer_destroy waits for running callbacks).
        var gc = output.Handle.Pin(new EventState(this));
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_set_event_callback(mixer, &OnNativeEvent, (void*)GCHandle.ToIntPtr(gc)));
    }

    /// <summary>The engine the mixer belongs to.</summary>
    public AudioEngine Engine { get; }

    /// <summary>
    /// The master output as an ordinary device: its clock (<see cref="OutputDevice.Time"/>), levels, spectrum and
    /// recording. The mixer owns it: disposing it disposes the mixer.
    /// </summary>
    public OutputDevice Output { get; }

    /// <summary>The decks, in order (deck 1 starts on crossfader side A, deck 2 on side B).</summary>
    public IReadOnlyList<DjDeck> Decks => _decks;

    /// <summary>The crossfader.</summary>
    public DjCrossfader Crossfader { get; }

    /// <summary>Linear master gain before the limiter, 0..4 (1 = unity); changes glide over 5 ms.</summary>
    public float MasterGain { get => Get(MixerParameter.MasterGain); set => Set(MixerParameter.MasterGain, value); }

    /// <summary>The headphones' blend: 0 = the cue only (default), 1 = the master only.</summary>
    public float CueMix { get => Get(MixerParameter.CueMix); set => Set(MixerParameter.CueMix, value); }

    /// <summary>Linear headphone gain, 0..4 (1 = unity).</summary>
    public float CueGain { get => Get(MixerParameter.CueGain); set => Set(MixerParameter.CueGain, value); }

    /// <summary>
    /// Second-device cue: a correction to the master's reported latency when the outputs are lined up, -250..250 ms.
    /// Raise it when the headphones sound early against the speakers, lower it when they sound late. Takes effect at
    /// once (a short gap in the output it delays).
    /// </summary>
    public TimeSpan CueTrim
    {
        get => TimeSpan.FromMilliseconds(Get(ParamCueTrimMs));
        set => Set(ParamCueTrimMs, (float)value.TotalMilliseconds);
    }

    private const MixerParameter ParamCueTrimMs = (MixerParameter)4;   // FA_MIX_CUE_TRIM_MS: not rampable, so not public

    /// <summary>
    /// Second-device cue: opens other headphones, or the same again after <see cref="CueLost"/>. The cue is silent
    /// meanwhile and is lined up anew.
    /// </summary>
    /// <param name="headphones">A playback device; null = the system default output.</param>
    /// <exception cref="ArgumentException"><paramref name="headphones"/> is not a playback device.</exception>
    /// <exception cref="FatzAudioException"><see cref="ErrorCode.InvalidArgument"/> when the mixer was made without
    /// <see cref="CueOutput.Device"/>; the open error (the cue stays silent) when the device won't open.</exception>
    public unsafe void SetCueDevice(AudioDeviceInfo? headphones)
    {
        if (headphones is { Kind: not DeviceKind.Playback })
            throw new ArgumentException("Not a playback device.", nameof(headphones));
        using var lease = Lease();
        byte* id = headphones?.Id is { } devId ? (byte*)Marshal.StringToCoTaskMemUTF8(devId) : null;
        try
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_set_cue_device(_mixer, id));
        }
        finally
        {
            if (id != null) Marshal.FreeCoTaskMem((nint)id);
        }
    }

    /// <summary>The output device's channel count now (it can change when the default device changes).</summary>
    public int OutputChannels
    {
        get
        {
            using var lease = Lease();
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_get_output_channels(_mixer, out uint ch, out _));
            return (int)ch;
        }
    }

    /// <summary>
    /// The first output channel carrying the cue (counted from 0), or null when the cue has no channels now: no cue,
    /// or a device that does not have them (the system default changed to a stereo device).
    /// </summary>
    public int? CueChannel
    {
        get
        {
            using var lease = Lease();
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_get_output_channels(_mixer, out _, out int cue));
            return cue < 0 ? null : cue;
        }
    }

    /// <summary>The mics, in the order added (at most 2).</summary>
    public IReadOnlyList<Mic> Mics
    {
        get
        {
            lock (_mics) return _micsView;
        }
    }

    /// <summary>
    /// Adds a karaoke mic. It fades in once its input runs. Two mics can share a 2-input interface (channels 0 and 1).
    /// The app asks for the microphone permission first (Android manifest and prompt, macOS and iOS usage strings).
    /// </summary>
    /// <param name="options">The input and channel; null = the default input, all channels summed.</param>
    /// <exception cref="ArgumentException"><see cref="MicOptions.Device"/> is not an input device.</exception>
    /// <exception cref="ArgumentOutOfRangeException"><see cref="MicOptions.Channel"/> is negative.</exception>
    /// <exception cref="FatzAudioException"><see cref="ErrorCode.Busy"/> for a third mic;
    /// <see cref="ErrorCode.NotEnoughChannels"/> when the device has no such channel;
    /// <see cref="ErrorCode.PermissionDenied"/> without the microphone permission.</exception>
    public unsafe Mic AddMic(MicOptions? options = null)
    {
        options ??= new MicOptions();
        if (options.Device is { Kind: not DeviceKind.Capture })
            throw new ArgumentException("Not an input device.", nameof(options));
        if (options.Channel < 0) throw new ArgumentOutOfRangeException(nameof(options), "Channel must be 0 or more.");
        FaMicConfig c;
        NativeMethods.fa_mic_config_init(&c);
        c.Channel = options.Channel ?? -1;
        c.Monitor = options.Monitor ? 1 : 0;
        c.Flags = (options.DucksMusic ? 1u : 0u) | (options.KeepOsProcessing ? 2u : 0u);
        byte* id = options.Device?.Id is { } devId ? (byte*)Marshal.StringToCoTaskMemUTF8(devId) : null;
        var handle = new MicHandle();
        using var lease = Lease();
        nint native;
        try
        {
            c.DeviceId = id;
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_add_mic(_mixer, &c, out native));
        }
        finally
        {
            if (id != null) Marshal.FreeCoTaskMem((nint)id);
        }
        handle.Initialize(native, Output.Handle);
        Mic mic;
        try
        {
            mic = new Mic(this, handle);
        }
        catch
        {
            handle.Dispose();
            throw;
        }
        lock (_mics)
        {
            _mics.Add(mic);
            _micsView = Array.AsReadOnly(_mics.ToArray());
        }
        return mic;
    }

    internal void Forget(Mic mic)
    {
        lock (_mics)
        {
            if (_mics.Remove(mic)) _micsView = Array.AsReadOnly(_mics.ToArray());
        }
    }

    /// <summary>
    /// The latency mode the output and the mic inputs run in. Setting it closes and opens the devices again (a moment
    /// of silence); playback carries on where it was. A mic input that won't open in the new mode raises
    /// <see cref="InputLost"/>.
    /// </summary>
    /// <exception cref="FatzAudioException">The mode can't be had (<see cref="LatencyMode.Exclusive"/>): the mixer
    /// stays in its mode; <see cref="ErrorCode.Busy"/> while <see cref="MeasureLatencyAsync"/> runs.</exception>
    public LatencyMode LatencyMode
    {
        get
        {
            using var lease = Lease();
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_get_latency_mode(_mixer, out int mode));
            return (LatencyMode)mode;
        }
        set
        {
            using var lease = Lease();
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_set_latency_mode(_mixer, (int)value));
        }
    }

    /// <summary>
    /// The setup check, for the app's setup screen: plays short test sounds through the speakers, listens on
    /// <paramref name="mic"/>, and says how long the mic takes to reach the speakers, how much mic gain the room takes
    /// before it feeds back, and whether to monitor the mics. Put the mic where the singer will stand. It takes 3-6 s
    /// per mode measured, with the music and the mics silenced while the test sounds play. Afterwards the mixer uses
    /// the measured time for that mic and mode (<see cref="Mic.MonitorLatency"/>, the vocals' recording alignment).
    /// Keep the mic until the task completes.
    /// </summary>
    /// <exception cref="ArgumentException"><paramref name="mic"/> is another mixer's.</exception>
    /// <exception cref="FatzAudioException"><see cref="ErrorCode.Busy"/> while another check runs;
    /// <see cref="ErrorCode.Device"/> when only the mixer's own mode is measured and its output isn't running, or when
    /// the mixer can't go back to its own mode after <see cref="LatencyCheckOptions.AllModes"/> (it stays in the last
    /// mode measured).</exception>
    /// <example>
    /// <code>
    /// var check = await mixer.MeasureLatencyAsync(mic, new LatencyCheckOptions { AllModes = true });
    /// if (check.Advice == CheckAdvice.Monitor)
    /// {
    ///     mixer.LatencyMode = check.AdvisedMode;
    ///     mic.Monitor = true;
    /// }
    /// </code>
    /// </example>
    public Task<LatencyCheck> MeasureLatencyAsync(Mic mic, LatencyCheckOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(mic);
        if (mic.Mixer != this) throw new ArgumentException("The mic is another mixer's.", nameof(mic));
        options ??= new LatencyCheckOptions();
        return Task.Run(() => MeasureLatency(mic, options));
    }

    private unsafe LatencyCheck MeasureLatency(Mic mic, LatencyCheckOptions options)
    {
        FaLatencyCheckConfig c;
        FaLatencyCheck r = default;
        NativeMethods.fa_latency_check_config_init(&c);
        c.Flags = options.AllModes ? 1u : 0u;
        c.MaxLevelDb = options.MaxLevelDb;
        c.MaxMonitor = TimeConvert.ToNanoseconds(options.MaxMonitor);
        r.StructSize = (uint)sizeof(FaLatencyCheck);
        using (Lease()) FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_measure_latency(_mixer, mic.Handle, &c, &r));
        var modes = new LatencyCheckMode[Math.Min(r.Count, 3)];
        for (int i = 0; i < modes.Length; i++)
        {
            var m = i == 0 ? r.Mode0 : i == 1 ? r.Mode1 : r.Mode2;
            modes[i] = new LatencyCheckMode((LatencyMode)m.Mode, m.Result == 0 ? null : (ErrorCode)m.Result,
                TimeConvert.FromNanoseconds(m.MicToSpeaker), TimeConvert.FromNanoseconds(m.Spread),
                TimeConvert.FromNanoseconds(m.Reported), m.MaxGainDb, m.LevelDb);
        }
        return new LatencyCheck(modes, (CheckAdvice)r.Advice, (LatencyMode)r.AdvisedMode);
    }

    /// <summary>What the mixer's outputs and mic inputs add up to now.</summary>
    public unsafe MixerLatency Latency
    {
        get
        {
            FaMixerLatency l = default;
            l.StructSize = (uint)sizeof(FaMixerLatency);
            using (Lease()) FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_get_latency(_mixer, &l));
            var input = new TimeSpan?[2];
            var monitor = new TimeSpan?[2];
            for (int i = 0; i < 2; i++)
            {
                input[i] = l.MicInput[i] < 0 ? null : TimeConvert.FromNanoseconds(l.MicInput[i]);
                monitor[i] = l.MicMonitor[i] < 0 ? null : TimeConvert.FromNanoseconds(l.MicMonitor[i]);
            }
            return new MixerLatency(TimeConvert.FromNanoseconds(l.Master), TimeConvert.FromNanoseconds(l.Cue),
                TimeConvert.FromNanoseconds(l.CueAlignment), input, monitor, (CueAlign)l.CueAlign);
        }
    }

    /// <summary>The mics' shared reverb and echo; set a new record to change them.</summary>
    public unsafe MicEffects MicEffects
    {
        get
        {
            FaMicEffects e;
            NativeMethods.fa_mic_effects_init(&e);
            using (Lease()) FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_get_mic_effects(_mixer, &e));
            return new MicEffects
            {
                ReverbSize = e.ReverbSize,
                ReverbDamping = e.ReverbDamping,
                EchoTime = TimeSpan.FromMilliseconds(e.EchoTimeMs),
                EchoFeedback = e.EchoFeedback,
                EchoTone = e.EchoTone,
            };
        }
        set
        {
            ArgumentNullException.ThrowIfNull(value);
            FaMicEffects e;
            NativeMethods.fa_mic_effects_init(&e);
            e.ReverbSize = value.ReverbSize;
            e.ReverbDamping = value.ReverbDamping;
            e.EchoTimeMs = (float)value.EchoTime.TotalMilliseconds;
            e.EchoFeedback = value.EchoFeedback;
            e.EchoTone = value.EchoTone;
            using (Lease()) FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_set_mic_effects(_mixer, &e));
        }
    }

    /// <summary>
    /// How a host mic (<see cref="MicOptions.DucksMusic"/>) ducks the music; on with the <see cref="FatzAudio.Ducking"/>
    /// defaults from the start. Null turns it off (and reads back as null when off). Singers never duck the music.
    /// </summary>
    public unsafe Ducking? Ducking
    {
        get
        {
            FaDucking d;
            NativeMethods.fa_ducking_init(&d);
            using (Lease()) FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_get_ducking(_mixer, &d));
            if (d.DepthDb == 0f) return null;
            return new Ducking
            {
                DepthDb = d.DepthDb,
                Attack = TimeConvert.FromNanoseconds(d.Attack),
                Hold = TimeConvert.FromNanoseconds(d.Hold),
                Release = TimeConvert.FromNanoseconds(d.Release),
                ThresholdDb = d.ThresholdDb,
            };
        }
        set
        {
            using var lease = Lease();
            if (value is null)
            {
                FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_set_ducking(_mixer, null));
                return;
            }
            FaDucking d;
            NativeMethods.fa_ducking_init(&d);
            d.DepthDb = value.DepthDb;
            d.Attack = TimeConvert.ToNanoseconds(value.Attack);
            d.Hold = TimeConvert.ToNanoseconds(value.Hold);
            d.Release = TimeConvert.ToNanoseconds(value.Release);
            d.ThresholdDb = value.ThresholdDb;
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_set_ducking(_mixer, &d));
        }
    }

    /// <summary>
    /// Starts encoders attached to this mixer's taps (<see cref="AudioEncoder.Record(DjMixer, MixerTap)"/>) together,
    /// on one device frame, so the files line up in any editor. The vocals start later by the mics' input latency and
    /// the master's output latency, so the voice lines up with the music as the singer heard it.
    /// </summary>
    /// <param name="encoders">Encoders attached to this mixer and not started yet.</param>
    /// <param name="startAt">When to start on the <see cref="OutputDevice.Time"/> clock of <see cref="Output"/>; null, or a time less than one mixer block (2048 frames) ahead, starts one block ahead.</param>
    /// <param name="rawTiming">Start the vocals on the same frame instead (the voice as it reached the mixer).</param>
    /// <exception cref="FatzAudioException"><see cref="ErrorCode.InvalidArgument"/> for an encoder not on this mixer;
    /// <see cref="ErrorCode.Busy"/> for one already started.</exception>
    /// <example>
    /// <code>
    /// using var master = AudioEncoder.Create("night.flac", new EncoderOptions { Format = EncodeFormat.Flac });
    /// using var vocals = AudioEncoder.Create("vocals.wav");
    /// master.Record(mixer, MixerTap.Master);
    /// vocals.Record(mixer, MixerTap.Vocals);
    /// mixer.StartRecording([master, vocals]);
    /// </code>
    /// </example>
    public unsafe void StartRecording(IReadOnlyList<AudioEncoder> encoders, TimeSpan? startAt = null, bool rawTiming = false)
    {
        ArgumentNullException.ThrowIfNull(encoders);
        if (startAt < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(startAt));
        var handles = new EncoderHandle[encoders.Count];
        var native = new nint[encoders.Count];
        int added = 0;
        try
        {
            for (; added < handles.Length; added++)
            {
                var h = (encoders[added] ?? throw new ArgumentNullException(nameof(encoders))).Handle;
                bool ok = false;
                h.DangerousAddRef(ref ok);
                handles[added] = h;
                native[added] = h.DangerousGetHandle();
            }
            using var lease = Lease();
            fixed (nint* p = native)
            {
                FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_start_recording(_mixer, p, (uint)native.Length,
                    startAt is { } t ? TimeConvert.ToNanoseconds(t) : 0, rawTiming ? 1u : 0u));
            }
        }
        finally
        {
            for (int i = 0; i < added; i++) handles[i].DangerousRelease();
        }
    }

    /// <summary>
    /// A mic's input device went away (unplugged, or taken by another app). The mic goes silent and the mix carries
    /// on; dispose it and add a new one when the device is back. Raised on the engine's event thread.
    /// </summary>
    public event EventHandler<MicEventArgs>? InputLost;

    /// <summary>
    /// The headphones of a second-device cue went away. The cue goes silent and the master plays on untouched (keeping
    /// any delay); <see cref="SetCueDevice"/> opens headphones again. Raised on the engine's event thread.
    /// </summary>
    public event EventHandler? CueLost;

    private sealed class EventState
    {
        public EventState(DjMixer m) => Mixer = new WeakReference<DjMixer>(m);
        public WeakReference<DjMixer> Mixer { get; }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe void OnNativeEvent(FaMixerEvent* ev, void* user)
    {
        try
        {
            if (ev == null || user == null) return;
            if (GCHandle.FromIntPtr((nint)user).Target is EventState st && st.Mixer.TryGetTarget(out var mixer))
                mixer.Raise(ev->Type, ev->Index, ev->Mic);
        }
        catch
        {
            // never throw across the native boundary
        }
    }

    private void Raise(int type, uint index, nint native)
    {
        const int inputLost = 16, cueLost = 17;
        if (AutoMix.Raise(type, index)) return;
        if (type == cueLost)
        {
            try
            {
                CueLost?.Invoke(this, EventArgs.Empty);
            }
            catch
            {
                // a handler's exception must not stop the event thread
            }
            return;
        }
        if (type != inputLost) return;
        Mic? mic;
        lock (_mics) mic = _mics.Find(m => m.NativeHandle == native);
        if (mic is null) return;
        try
        {
            InputLost?.Invoke(this, new MicEventArgs(mic));
        }
        catch
        {
            // a handler's exception must not stop the event thread
        }
    }

    /// <summary>Ramps a mixer parameter to a target and returns at once.</summary>
    /// <param name="parameter">The parameter.</param>
    /// <param name="target">The target value (clamped to the parameter's range).</param>
    /// <param name="duration">The ramp's length.</param>
    /// <param name="curve">The ramp's shape; defaults to <see cref="Curve.Linear"/>.</param>
    /// <param name="startAt">When to start on the <see cref="OutputDevice.Time"/> clock of <see cref="Output"/>; null starts now.</param>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="duration"/> or <paramref name="startAt"/> is negative.</exception>
    public unsafe void Ramp(MixerParameter parameter, float target, TimeSpan duration, Curve? curve = null, TimeSpan? startAt = null)
    {
        if (duration < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(duration));
        if (startAt < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(startAt));
        var c = curve ?? Curve.Linear;
        var ramp = c.ToNative();
        using (Lease())
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_ramp_param(_mixer, (int)parameter, target,
                TimeConvert.ToNanoseconds(duration), startAt is { } t ? Math.Max(1, TimeConvert.ToNanoseconds(t)) : 0, &ramp));
        }
        GC.KeepAlive(c);
    }

    internal float Get(MixerParameter p)
    {
        using var _ = Lease();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_get_param(_mixer, (int)p, out float v));
        return v;
    }

    internal void Set(MixerParameter p, float v)
    {
        using var _ = Lease();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_set_param(_mixer, (int)p, v));
    }

    internal nint Native => _mixer;

    /// <summary>Keeps the native mixer alive (it lives as long as its device) for one call.</summary>
    internal DeviceLease Lease() => new(Output.Handle);

    internal readonly struct DeviceLease : IDisposable
    {
        private readonly DeviceHandle _h;

        public DeviceLease(DeviceHandle h)
        {
            bool ok = false;
            h.DangerousAddRef(ref ok);
            if (!ok) throw new ObjectDisposedException(nameof(DjMixer));
            _h = h;
        }

        public void Dispose() => _h.DangerousRelease();
    }

    /// <summary>Removes the mics, unloads the decks (their streams stay open and are detached) and closes the device.</summary>
    public void Dispose()
    {
        if (Output.IsDisposed) return;
        foreach (var m in Mics) m.Dispose();
        foreach (var d in _decks)
        {
            try { d.Unload(force: true); }
            catch (ObjectDisposedException) { }
        }
        Output.Dispose();
    }
}

/// <summary>The crossfader of a <see cref="DjMixer"/>.</summary>
public sealed class DjCrossfader
{
    private readonly DjMixer _mixer;

    internal DjCrossfader(DjMixer mixer) => _mixer = mixer;

    /// <summary>0 = side A only, 0.5 = the middle (default), 1 = side B only; changes glide over 5 ms.</summary>
    public float Position { get => _mixer.Get(MixerParameter.Crossfader); set => _mixer.Set(MixerParameter.Crossfader, value); }

    /// <summary>How the level is shared between the sides.</summary>
    public CrossfaderCurve Curve
    {
        get
        {
            using var _ = _mixer.Lease();
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_get_crossfader_curve(_mixer.Native, out int c));
            return (CrossfaderCurve)c;
        }
        set
        {
            using var _ = _mixer.Lease();
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_mixer_set_crossfader_curve(_mixer.Native, (int)value));
        }
    }

    /// <summary>Moves the crossfader to <paramref name="target"/> over <paramref name="duration"/> and returns at once.</summary>
    /// <param name="target">0 = side A .. 1 = side B.</param>
    /// <param name="duration">The move's length.</param>
    /// <param name="curve">The move's shape; defaults to <see cref="FatzAudio.Curve.Linear"/>.</param>
    /// <param name="startAt">When to start on the device clock; null starts now.</param>
    public void Ramp(float target, TimeSpan duration, Curve? curve = null, TimeSpan? startAt = null) =>
        _mixer.Ramp(MixerParameter.Crossfader, target, duration, curve, startAt);
}

/// <summary>The isolator EQ of a <see cref="DjDeck"/>: three bands split at 250 Hz and 2.5 kHz, flat at 0 dB.</summary>
public sealed class DeckEq
{
    private readonly DjDeck _deck;

    internal DeckEq(DjDeck deck) => _deck = deck;

    /// <summary>The setting that silences a band (-60 dB); lower values are taken as it.</summary>
    public const float Kill = -60f;

    /// <summary>The lows (below 250 Hz) in dB, <see cref="Kill"/>..6.</summary>
    public float Low { get => _deck.Get(DeckParameter.EqLowDb); set => _deck.Set(DeckParameter.EqLowDb, value); }

    /// <summary>The mids (250 Hz to 2.5 kHz) in dB, <see cref="Kill"/>..6.</summary>
    public float Mid { get => _deck.Get(DeckParameter.EqMidDb); set => _deck.Set(DeckParameter.EqMidDb, value); }

    /// <summary>The highs (above 2.5 kHz) in dB, <see cref="Kill"/>..6.</summary>
    public float High { get => _deck.Get(DeckParameter.EqHighDb); set => _deck.Set(DeckParameter.EqHighDb, value); }
}

/// <summary>
/// A deck of a <see cref="DjMixer"/>: trim, isolator EQ, cue send, channel fader, meter and crossfader side around an
/// ordinary <see cref="AudioStream"/>. Transport stays on the stream (<see cref="AudioStream.Play"/>,
/// <see cref="AudioStream.Pause"/>, seeking, tempo and pitch).
/// </summary>
public sealed partial class DjDeck
{
    private const int ParamXfaderSide = 5, ParamCue = 6;
    private readonly DjMixer _mixer;
    private readonly nint _deck;
    private AudioStream? _stream;

    internal DjDeck(DjMixer mixer, int index, nint deck)
    {
        _mixer = mixer;
        Index = index;
        _deck = deck;
        Eq = new DeckEq(this);
    }

    /// <summary>The deck's index in <see cref="DjMixer.Decks"/>.</summary>
    public int Index { get; }

    /// <summary>The isolator EQ.</summary>
    public DeckEq Eq { get; }

    /// <summary>
    /// The stream on the deck, or null when empty. A stream leaves the deck when it is disposed, detached or played
    /// on another device or bus.
    /// </summary>
    public AudioStream? Stream
    {
        get
        {
            using var _ = _mixer.Lease();
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_deck_get_stream(_deck, out nint native));
            if (_stream is not null && (native == 0 || _stream.IsDisposed || _stream.NativeHandle != native)) _stream = null;
            return _stream;
        }
    }

    /// <summary>
    /// Puts <paramref name="stream"/> on the deck (from another device or deck too, keeping its position); it is not
    /// started. The deck does not own it: dispose it after unloading. Loading also shortens the stream's decode-ahead
    /// to 60 ms when it is not playing (so tempo and pitch moves are heard quickly), and the cue point goes back to
    /// the stream's <see cref="StreamMarkers.CueIn"/>.
    /// </summary>
    /// <param name="stream">The stream; null unloads.</param>
    /// <param name="force">Replace a stream that is playing and heard (fader up, crossfader not closed on its side).</param>
    /// <exception cref="FatzAudioException"><see cref="ErrorCode.Busy"/> when the current stream is playing and heard and <paramref name="force"/> is false.</exception>
    public void Load(AudioStream? stream, bool force = false)
    {
        var old = Stream;
        if (ReferenceEquals(old, stream) && stream is not null) return;
        using (_mixer.Lease())
        {
            var sh = stream?.Handle;
            bool added = false;
            try
            {
                // A reference for the call: a Dispose on another thread must not free the stream under it.
                sh?.DangerousAddRef(ref added);
                FatzAudioException.ThrowIfFailed(NativeMethods.fa_deck_load(_deck, sh is null ? 0 : sh.DangerousGetHandle(), force ? 1u : 0u));
            }
            finally
            {
                if (added) sh!.DangerousRelease();
            }
        }
        old?.LeftDeck();
        stream?.JoinedDeck(_mixer.Output);
        _stream = stream;
    }

    /// <summary>Takes the stream off the deck (it is detached, stays open, and its deck loop is cleared).</summary>
    /// <param name="force">Unload a stream that is playing and heard.</param>
    public void Unload(bool force = false) => Load(null, force);

    /// <summary>The channel fader, linear 0..1 (default 1); changes glide over 5 ms.</summary>
    public float Gain { get => Get(DeckParameter.Gain); set => Set(DeckParameter.Gain, value); }

    /// <summary>The trim in dB, -12..12 (default 0), on top of loudness normalization.</summary>
    public float TrimDb { get => Get(DeckParameter.TrimDb); set => Set(DeckParameter.TrimDb, value); }

    /// <summary>The crossfader side; switching glides over 5 ms.</summary>
    public CrossfaderSide Side
    {
        get => (CrossfaderSide)(int)GetRaw(ParamXfaderSide);
        set => SetRaw(ParamXfaderSide, (int)value);
    }

    /// <summary>Whether the deck is sent to the headphones (before its fader).</summary>
    public bool Cue { get => GetRaw(ParamCue) != 0f; set => SetRaw(ParamCue, value ? 1f : 0f); }

    /// <summary>Ramps a deck parameter to a target and returns at once.</summary>
    /// <param name="parameter">The parameter.</param>
    /// <param name="target">The target value (clamped to the parameter's range).</param>
    /// <param name="duration">The ramp's length.</param>
    /// <param name="curve">The ramp's shape; defaults to <see cref="Curve.Linear"/>.</param>
    /// <param name="startAt">When to start on the device clock; null starts now.</param>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="duration"/> or <paramref name="startAt"/> is negative.</exception>
    public unsafe void Ramp(DeckParameter parameter, float target, TimeSpan duration, Curve? curve = null, TimeSpan? startAt = null)
    {
        if (duration < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(duration));
        if (startAt < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(startAt));
        var c = curve ?? Curve.Linear;
        var ramp = c.ToNative();
        using (_mixer.Lease())
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_deck_ramp_param(_deck, (int)parameter, target,
                TimeConvert.ToNanoseconds(duration), startAt is { } t ? Math.Max(1, TimeConvert.ToNanoseconds(t)) : 0, &ramp));
        }
        GC.KeepAlive(c);
    }

    /// <summary>
    /// The cue point in source frames (CDJ style; see <see cref="ReturnToCuePoint"/>). Default: the stream's
    /// <see cref="StreamMarkers.CueIn"/>, else 0. Setting null restores the default.
    /// </summary>
    public long? CuePoint
    {
        get
        {
            using var _ = _mixer.Lease();
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_deck_get_cue_point(_deck, out long f));
            return f;
        }
        set
        {
            if (value < 0) throw new ArgumentOutOfRangeException(nameof(value));
            using var _ = _mixer.Lease();
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_deck_set_cue_point(_deck, value ?? -1));
        }
    }

    /// <summary>The CDJ cue button: pauses the deck and goes back to the <see cref="CuePoint"/>.</summary>
    /// <exception cref="FatzAudioException"><see cref="ErrorCode.NotFound"/> when the deck is empty.</exception>
    public void ReturnToCuePoint()
    {
        using var _ = _mixer.Lease();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_deck_cue(_deck));
    }

    /// <summary>
    /// The deck loop, or null when off. Setting one works at any time, also while playing, without a gap: it follows
    /// tempo and pitch, raises <see cref="AudioStream.Looped"/> at each turn, and <see cref="AudioStream.Position"/>
    /// reports the loop as heard. Setting an out point the deck has just passed (up to 0.5 s ago, as a "loop out"
    /// button does) loops back at once, keeping the loop's phase; one further behind jumps to the in point. Setting
    /// null turns it off (playback carries on from where it is); a seek past the out point also leaves the loop.
    /// </summary>
    /// <exception cref="FatzAudioException"><see cref="ErrorCode.NotFound"/> when the deck is empty;
    /// <see cref="ErrorCode.NotSeekable"/> for a stream that cannot loop.</exception>
    public DeckLoop? Loop
    {
        get
        {
            using var _ = _mixer.Lease();
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_deck_get_loop(_deck, out long i, out long o));
            return o > i ? new DeckLoop(i, o) : null;
        }
        set
        {
            if (value is { } v && (v.In < 0 || v.Out <= v.In)) throw new ArgumentOutOfRangeException(nameof(value), "Out must be after In.");
            using var _ = _mixer.Lease();
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_deck_set_loop(_deck, value?.In ?? 0, value?.Out ?? 0));
        }
    }

    /// <summary>
    /// The deck's channel meter: peak and RMS after trim, EQ and fader, before the crossfader, as heard now. The
    /// first call starts measuring, so it reads silence.
    /// </summary>
    /// <param name="window">Length of the measurement window; defaults to 50 ms, at most about 1 s.</param>
    /// <returns>Levels for left and right.</returns>
    public StreamLevels GetLevels(TimeSpan? window = null)
    {
        var peak = new float[2];
        var rms = new float[2];
        GetLevels(peak, rms, window);
        return new StreamLevels(peak, rms);
    }

    /// <summary>Like <see cref="GetLevels(TimeSpan?)"/>, into your own buffers (no allocation per call).</summary>
    /// <param name="peak">Receives the left and right peaks (at least 2 values).</param>
    /// <param name="rms">Receives the left and right RMS values (at least 2 values).</param>
    /// <param name="window">Length of the measurement window; defaults to 50 ms, at most about 1 s.</param>
    /// <exception cref="ArgumentException">A span shorter than 2.</exception>
    public unsafe void GetLevels(Span<float> peak, Span<float> rms, TimeSpan? window = null)
    {
        if (peak.Length < 2 || rms.Length < 2) throw new ArgumentException("Room for left and right is needed.", peak.Length < 2 ? nameof(peak) : nameof(rms));
        using var _ = _mixer.Lease();
        fixed (float* pp = peak)
        fixed (float* pr = rms)
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_deck_get_levels(
                _deck, TimeConvert.ToNanoseconds(window ?? StreamLevels.DefaultWindow), pp, pr, 2));
        }
    }

    internal float Get(DeckParameter p) => GetRaw((int)p);

    internal void Set(DeckParameter p, float v) => SetRaw((int)p, v);

    private float GetRaw(int p)
    {
        using var _ = _mixer.Lease();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_deck_get_param(_deck, p, out float v));
        return v;
    }

    private void SetRaw(int p, float v)
    {
        using var _ = _mixer.Lease();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_deck_set_param(_deck, p, v));
    }
}
