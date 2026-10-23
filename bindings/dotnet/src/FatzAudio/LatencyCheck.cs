namespace FatzAudio;

/// <summary>Options for <see cref="DjMixer.MeasureLatencyAsync"/>.</summary>
public sealed class LatencyCheckOptions
{
    /// <summary>
    /// Measure each latency mode the system has (Windows: normal, low and exclusive), switching the mixer to it and
    /// back after; false (the default) measures only <see cref="DjMixer.LatencyMode"/>.
    /// </summary>
    public bool AllModes { get; init; }

    /// <summary>
    /// The loudest test sound in dBFS, -40..-6 (default -12). The sounds start at -40 and get louder until the mic
    /// hears them clearly (or 10 dB more no longer helps). In exclusive mode the Windows volume no longer applies, so the same level can be much louder.
    /// </summary>
    public float MaxLevelDb { get; init; } = -12f;

    /// <summary>The mic-to-speaker time that still feels live (default 25 ms); slower is advised against.</summary>
    public TimeSpan MaxMonitor { get; init; } = TimeSpan.FromMilliseconds(25);
}

/// <summary>What <see cref="DjMixer.MeasureLatencyAsync"/> advises.</summary>
public enum CheckAdvice
{
    /// <summary>Monitor the mics, in <see cref="LatencyCheck.AdvisedMode"/>.</summary>
    Monitor = 0,
    /// <summary>Monitoring off: every mode measured is over <see cref="LatencyCheckOptions.MaxMonitor"/> (an audible slap-back).</summary>
    Slow,
    /// <summary>Monitoring off: the room feeds back below 0 dB of mic gain. Move the mic away from the speakers, or point it away.</summary>
    Feedback,
    /// <summary>The mic didn't hear the speakers: is it on and unmuted, are the speakers up, is it a headset?</summary>
    NotHeard,
}

/// <summary>One latency mode's measurement.</summary>
/// <param name="Mode">The mode.</param>
/// <param name="Error">Null when measured; <see cref="ErrorCode.NotImplemented"/> (not on this system),
/// <see cref="ErrorCode.Device"/> (it would not open, or the output isn't running) or <see cref="ErrorCode.NotFound"/>
/// (the mic didn't hear the test).</param>
/// <param name="MicToSpeaker">Measured: the input, the bridge, the limiter, the output, and the air from the speaker
/// back to the mic (3 ms per metre).</param>
/// <param name="Spread">How far the test sounds' times differed.</param>
/// <param name="Reported">What the OS's figures add up to, for comparison.</param>
/// <param name="MaxGainDb">The most <see cref="Mic.GainDb"/> that stays 6 dB clear of feedback, with the master gain
/// as it is now and the mic's EQ flat (an EQ boost takes that much off); -20..40.</param>
/// <param name="LevelDb">The test level the mic heard.</param>
public sealed record LatencyCheckMode(LatencyMode Mode, ErrorCode? Error, TimeSpan MicToSpeaker, TimeSpan Spread,
    TimeSpan Reported, float MaxGainDb, float LevelDb);

/// <summary>The result of <see cref="DjMixer.MeasureLatencyAsync"/>.</summary>
/// <param name="Modes">Each mode measured (normal, low, exclusive; or only the mixer's own).</param>
/// <param name="Advice">Whether to monitor the mics.</param>
/// <param name="AdvisedMode">The cheapest mode fast enough (shared before exclusive, which takes the devices from
/// other apps); with <see cref="CheckAdvice.Slow"/>, the fastest.</param>
public sealed record LatencyCheck(IReadOnlyList<LatencyCheckMode> Modes, CheckAdvice Advice, LatencyMode AdvisedMode);
