using FatzAudio.Native;

namespace FatzAudio;

/// <summary>Options for <see cref="DjMixer.AddMic"/>.</summary>
public sealed class MicOptions
{
    /// <summary>The input (from <see cref="AudioEngine.InputDevices"/>); null = the system default input.</summary>
    public AudioDeviceInfo? Device { get; init; }

    /// <summary>
    /// The input channel, counted from 0 (two mics on a 2-input interface: 0 and 1); null = all the device's channels
    /// summed (the default).
    /// </summary>
    public int? Channel { get; init; }

    /// <summary>
    /// Heard through the master (the default). False meters it (and, with the recording step, records it) only: for a
    /// mic that already goes through a hardware mixer.
    /// </summary>
    public bool Monitor { get; init; } = true;

    /// <summary>A host mic: its voice ducks the music (<see cref="DjMixer.Ducking"/>). Leave it off for singers.</summary>
    public bool DucksMusic { get; init; }

    /// <summary>
    /// Keep the system's voice processing (automatic gain, noise suppression, echo cancelling), made for calls. Off by
    /// default: it spoils singing and adds latency. Two mics on one device share its input, so the first mic added
    /// decides for both.
    /// </summary>
    public bool KeepOsProcessing { get; init; }
}

/// <summary>
/// The shared reverb and echo of a <see cref="DjMixer"/>'s mics, like the effects unit of a karaoke mixer. Each mic
/// sends to them (<see cref="Mic.ReverbSend"/>, <see cref="Mic.EchoSend"/>); a mic with monitor off sends nothing.
/// Out-of-range values are clamped.
/// </summary>
public sealed record MicEffects
{
    /// <summary>0 (small room) .. 1 (hall), default 0.5.</summary>
    public float ReverbSize { get; init; } = 0.5f;

    /// <summary>0 (bright) .. 1 (dark), default 0.5.</summary>
    public float ReverbDamping { get; init; } = 0.5f;

    /// <summary>The echo's time, 20 ms .. 1 s, default 250 ms; a change glides, bending the repeats for a moment.</summary>
    public TimeSpan EchoTime { get; init; } = TimeSpan.FromMilliseconds(250);

    /// <summary>How much of each repeat comes back, 0..0.9, default 0.35.</summary>
    public float EchoFeedback { get; init; } = 0.35f;

    /// <summary>0 (dark) .. 1 (bright), default 0.6: a low-pass in the feedback, so later repeats get duller.</summary>
    public float EchoTone { get; init; } = 0.6f;
}

/// <summary>
/// How a host's voice ducks the music (<see cref="DjMixer.Ducking"/>): while a mic with
/// <see cref="MicOptions.DucksMusic"/> speaks above the threshold, the decks go down by the depth, and come back after
/// the hold and the release. The music is the decks and any other stream on the mixer's device. Out-of-range values are
/// clamped.
/// </summary>
public sealed record Ducking
{
    /// <summary>How far the music goes down, -40..0 dB, default -12.</summary>
    public float DepthDb { get; init; } = -12f;

    /// <summary>How fast it goes down, 1 ms .. 1 s, default 50 ms.</summary>
    public TimeSpan Attack { get; init; } = TimeSpan.FromMilliseconds(50);

    /// <summary>How long it stays down after the voice, up to 5 s, default 300 ms.</summary>
    public TimeSpan Hold { get; init; } = TimeSpan.FromMilliseconds(300);

    /// <summary>How fast it comes back, 10 ms .. 5 s, default 600 ms.</summary>
    public TimeSpan Release { get; init; } = TimeSpan.FromMilliseconds(600);

    /// <summary>The voice level that ducks, -60..0 dB, default -40.</summary>
    public float ThresholdDb { get; init; } = -40f;
}

/// <summary>What a <see cref="DjMixer"/> recording takes (<see cref="AudioEncoder.Record(DjMixer, MixerTap)"/>).</summary>
public enum MixerTap
{
    /// <summary>What the speakers play, after the limiter.</summary>
    Master = 0,
    /// <summary>Both mics with their reverb and echo, before the master gain; a mic with monitor off is recorded dry.</summary>
    Vocals = 1,
}

/// <summary>A mic's meter reading.</summary>
/// <param name="Peak">The peak, linear (1 = full scale).</param>
/// <param name="Rms">The RMS level, linear.</param>
public readonly record struct MicLevels(float Peak, float Rms);

/// <summary>Arguments of <see cref="DjMixer.InputLost"/>.</summary>
public sealed class MicEventArgs : EventArgs
{
    internal MicEventArgs(Mic mic) => Mic = mic;

    /// <summary>The mic whose input went away.</summary>
    public Mic Mic { get; }
}

/// <summary>
/// A karaoke mic on a <see cref="DjMixer"/>: one channel of an input device (or all of them summed) with a low cut,
/// gain, gate, EQ, meter, mute and pan, mixed into the master before the master gain and the limiter.
/// </summary>
/// <remarks>
/// The input runs on its own clock and reaches the master through a small drift-corrected buffer
/// (<see cref="InputLatency"/>). Dispose the mic to remove it; disposing the mixer removes its mics.
/// </remarks>
public sealed class Mic : IDisposable
{
    private const int ParamGainDb = 0, ParamMute = 1, ParamPan = 2, ParamGateDb = 3, ParamReverb = 4, ParamEcho = 5,
        ParamMonitor = 6, ParamLowCut = 7;
    private readonly DjMixer _mixer;
    private readonly MicHandle _handle;

    internal Mic(DjMixer mixer, MicHandle handle)
    {
        _mixer = mixer;
        _handle = handle;
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_mic_get_index(handle, out uint index));
        Index = (int)index;
    }

    /// <summary>The mic's slot, 0 or 1 (<see cref="MixerLatency.MicInput"/>).</summary>
    public int Index { get; }

    /// <summary>The mixer the mic is on.</summary>
    public DjMixer Mixer => _mixer;

    /// <summary>Gain in dB, -20..40 (default 0); changes glide over 5 ms.</summary>
    public float GainDb { get => Get(ParamGainDb); set => Set(ParamGainDb, value); }

    /// <summary>Silences the mic in the master (the meter still reads it); glides over 5 ms.</summary>
    public bool Muted { get => Get(ParamMute) != 0f; set => Set(ParamMute, value ? 1f : 0f); }

    /// <summary>Balance, -1 (left) .. 1 (right), default 0.</summary>
    public float Pan { get => Get(ParamPan); set => Set(ParamPan, value); }

    /// <summary>
    /// The gate's threshold in dB, -100 (off) .. 0, default -50: it opens at the threshold and closes 6 dB below it
    /// after 100 ms, so breaths and room noise stay out between phrases.
    /// </summary>
    public float GateDb { get => Get(ParamGateDb); set => Set(ParamGateDb, value); }

    /// <summary>Whether the mic is heard through the master (from <see cref="MicOptions.Monitor"/>).</summary>
    public bool Monitor { get => Get(ParamMonitor) != 0f; set => Set(ParamMonitor, value ? 1f : 0f); }

    /// <summary>An 80 Hz high-pass against handling noise and rumble; on by default.</summary>
    public bool LowCut { get => Get(ParamLowCut) != 0f; set => Set(ParamLowCut, value ? 1f : 0f); }

    /// <summary>The send into the shared reverb (<see cref="DjMixer.MicEffects"/>), 0..1, default 0.2; glides over 5 ms.</summary>
    public float ReverbSend { get => Get(ParamReverb); set => Set(ParamReverb, value); }

    /// <summary>The send into the shared echo, 0..1, default 0; glides over 5 ms.</summary>
    public float EchoSend { get => Get(ParamEcho); set => Set(ParamEcho, value); }

    /// <summary>
    /// Sets the mic's EQ: the same bands and limits as <see cref="AudioStream.SetEqualizer(IReadOnlyList{EqualizerBand})"/>.
    /// Off (no bands) by default.
    /// </summary>
    /// <exception cref="ArgumentException">More than 16 bands.</exception>
    public unsafe void SetEqualizer(IReadOnlyList<EqualizerBand> bands)
    {
        ArgumentNullException.ThrowIfNull(bands);
        if (bands.Count > 16) throw new ArgumentException("At most 16 bands.", nameof(bands));
        var native = stackalloc FaEqBand[16];
        for (int i = 0; i < bands.Count; i++) native[i] = new FaEqBand(bands[i]);
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_mic_set_eq(_handle, native, (uint)bands.Count));
    }

    /// <summary>Sets the EQ to a built-in preset; <see cref="EqualizerPreset.LiveVocal"/> is made for mics.</summary>
    public void SetEqualizer(EqualizerPreset preset) => SetEqualizer(Equalizer.GetPreset(preset));

    /// <summary>The EQ's bands as set (after clamping to the ranges).</summary>
    public unsafe EqualizerBand[] GetEqualizer()
    {
        var native = stackalloc FaEqBand[16];
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_mic_get_eq(_handle, native, 16, out uint n));
        var result = new EqualizerBand[n];
        for (int i = 0; i < n; i++) result[i] = native[i].ToBand();
        return result;
    }

    /// <summary>
    /// The mic's meter: after gain, gate and EQ, before mute and pan, as heard now. The first call starts measuring,
    /// so it reads silence.
    /// </summary>
    /// <param name="window">Length of the measurement window; defaults to 50 ms, at most about 1 s.</param>
    public MicLevels GetLevels(TimeSpan? window = null)
    {
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_mic_get_levels(
            _handle, TimeConvert.ToNanoseconds(window ?? StreamLevels.DefaultWindow), out float peak, out float rms));
        return new MicLevels(peak, rms);
    }

    /// <summary>What the input device and the drift-corrected buffer add, from the voice to the mix.</summary>
    public TimeSpan InputLatency => _mixer.Latency.MicInput[Index] ?? TimeSpan.Zero;

    /// <summary>Mic to speaker: <see cref="InputLatency"/> plus the master output's.</summary>
    public TimeSpan MonitorLatency => _mixer.Latency.MicMonitor[Index] ?? TimeSpan.Zero;

    internal nint NativeHandle => _handle.IsClosed ? 0 : _handle.DangerousGetHandle();

    internal MicHandle Handle => _handle;

    internal bool IsDisposed => _handle.IsClosed;

    private float Get(int p)
    {
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_mic_get_param(_handle, p, out float v));
        return v;
    }

    private void Set(int p, float v) => FatzAudioException.ThrowIfFailed(NativeMethods.fa_mic_set_param(_handle, p, v));

    /// <summary>
    /// Removes the mic, and closes its input device when no other mic uses it. Safe from an
    /// <see cref="DjMixer.InputLost"/> handler, also for the event's own mic.
    /// </summary>
    public void Dispose()
    {
        _mixer.Forget(this);
        _handle.Dispose();
    }
}
