using FatzAudio.Native;

namespace FatzAudio;

/// <summary>
/// Cue, fade and mix markers of a stream, as positions within the stream. Null means "not set".
/// </summary>
/// <remarks>
/// <list type="bullet">
/// <item><see cref="CueIn"/>: where playback starts. Play after stop or end, <see cref="AudioStream.Stop"/>,
/// queues and crossfades all go here. With <see cref="StreamOptions.Loop"/> the stream loops from
/// <see cref="CueOut"/> back to <see cref="CueIn"/>.</item>
/// <item><see cref="CueOut"/>: where playback ends, exactly (<see cref="AudioStream.Ended"/>).</item>
/// <item><see cref="FadeInEnd"/>: the stream fades in from <see cref="CueIn"/> (or the start) to here.</item>
/// <item><see cref="FadeOutStart"/>: the stream fades out from here to <see cref="CueOut"/> (or the end).</item>
/// <item><see cref="MixPoint"/>: where the stream queued with <see cref="AudioStream.QueueNext"/> starts.</item>
/// </list>
/// Marker fades follow the position, so seeking into a fade plays it at the right level. They multiply with
/// volume, crossfades, normalization and bus gains. <see cref="AudioStream.MarkerReached"/> is raised when
/// playback passes a marker.
/// </remarks>
/// <example>
/// <code>
/// song.Markers.CueIn = TimeSpan.FromSeconds(1.2);                      // skip the silence
/// song.Markers.FadeOutStart = song.Info.Duration - TimeSpan.FromSeconds(6);
/// song.Markers.MixPoint = song.Info.Duration - TimeSpan.FromSeconds(4);
/// song.Markers.FadeOutCurve = Curve.SCurve;
/// </code>
/// </example>
public sealed class StreamMarkers
{
    private readonly AudioStream _stream;
    private Curve _fadeIn = Curve.EqualPower, _fadeOut = Curve.EqualPower;

    internal StreamMarkers(AudioStream stream) => _stream = stream;

    /// <summary>Where playback starts. Setting it on a stopped stream moves the stream there.</summary>
    public TimeSpan? CueIn { get => Get(MarkerKind.CueIn); set => Set(MarkerKind.CueIn, value); }

    /// <summary>Where playback ends. Changing it while playing takes effect at once.</summary>
    public TimeSpan? CueOut { get => Get(MarkerKind.CueOut); set => Set(MarkerKind.CueOut, value); }

    /// <summary>Where the fade-in (from <see cref="CueIn"/> or the start) is complete.</summary>
    public TimeSpan? FadeInEnd { get => Get(MarkerKind.FadeInEnd); set => Set(MarkerKind.FadeInEnd, value); }

    /// <summary>Where the fade-out (to <see cref="CueOut"/> or the end) begins.</summary>
    public TimeSpan? FadeOutStart { get => Get(MarkerKind.FadeOutStart); set => Set(MarkerKind.FadeOutStart, value); }

    /// <summary>Where the stream queued with <see cref="AudioStream.QueueNext"/> starts.</summary>
    public TimeSpan? MixPoint { get => Get(MarkerKind.MixPoint); set => Set(MarkerKind.MixPoint, value); }

    /// <summary>Gets a marker by kind.</summary>
    /// <param name="kind">The marker.</param>
    /// <returns>Its position, or null when it is not set.</returns>
    public TimeSpan? Get(MarkerKind kind)
    {
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_marker(_stream.Handle, (int)kind, out long frame));
        return frame < 0 ? null : _stream.FramesToTime(frame);
    }

    /// <summary>Sets or clears (null) a marker by kind.</summary>
    /// <param name="kind">The marker.</param>
    /// <param name="position">Its position, or null to clear it.</param>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="position"/> is negative.</exception>
    public void Set(MarkerKind kind, TimeSpan? position)
    {
        if (position < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(position));
        long frame = position is { } t ? _stream.TimeToFrames(t) : -1;
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_marker(_stream.Handle, (int)kind, frame));
    }

    /// <summary>Shape of the <see cref="FadeInEnd"/> fade-in. Defaults to <see cref="Curve.EqualPower"/>.</summary>
    /// <exception cref="ArgumentNullException">The value is null.</exception>
    public Curve FadeInCurve { get => _fadeIn; set => _fadeIn = SetCurve(0, value); }

    /// <summary>
    /// Shape of the <see cref="FadeOutStart"/> fade-out, described as a fade-in and played backwards.
    /// Defaults to <see cref="Curve.EqualPower"/>, so overlapping marker fades keep constant loudness.
    /// </summary>
    /// <exception cref="ArgumentNullException">The value is null.</exception>
    public Curve FadeOutCurve { get => _fadeOut; set => _fadeOut = SetCurve(1, value); }

    private unsafe Curve SetCurve(int fade, Curve curve)
    {
        ArgumentNullException.ThrowIfNull(curve);
        var r = curve.ToNative();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_fade_curve(_stream.Handle, fade, &r));
        GC.KeepAlive(curve);
        return curve;
    }
}
