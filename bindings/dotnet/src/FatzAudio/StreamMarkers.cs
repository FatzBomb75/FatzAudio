using FatzAudio.Native;

namespace FatzAudio;

/// <summary>
/// Cue, fade and mix markers of a stream, expressed as positions within the stream.
/// A value of null means "marker not set".
/// </summary>
/// <example>
/// <code>
/// stream.Markers.CueIn = TimeSpan.FromSeconds(1.2);
/// stream.Markers.MixPoint = stream.Info.Duration - TimeSpan.FromSeconds(8);
/// </code>
/// </example>
public sealed class StreamMarkers
{
    private readonly AudioStream _stream;

    internal StreamMarkers(AudioStream stream) => _stream = stream;

    /// <summary>Where audible content starts (leading silence is skipped).</summary>
    public TimeSpan? CueIn { get => Get(0); set => Set(0, value); }

    /// <summary>Where audible content ends (trailing silence is skipped).</summary>
    public TimeSpan? CueOut { get => Get(1); set => Set(1, value); }

    /// <summary>Where the fade-in is complete.</summary>
    public TimeSpan? FadeInEnd { get => Get(2); set => Set(2, value); }

    /// <summary>Where the fade-out begins.</summary>
    public TimeSpan? FadeOutStart { get => Get(3); set => Set(3, value); }

    /// <summary>Where the next queued stream starts.</summary>
    public TimeSpan? MixPoint { get => Get(4); set => Set(4, value); }

    private TimeSpan? Get(int marker)
    {
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_marker(_stream.Handle, marker, out long frame));
        return frame < 0 ? null : _stream.FramesToTime(frame);
    }

    private void Set(int marker, TimeSpan? value)
    {
        long frame = value is { } t ? _stream.TimeToFrames(t) : -1;
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_marker(_stream.Handle, marker, frame));
    }
}
