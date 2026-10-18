namespace FatzAudio;

/// <summary>
/// A tracker module's format, names and counts (<see cref="AudioStream.TrackerInfo"/>): MOD, S3M, XM, IT, MPTM, MTM,
/// UMX and the other formats libopenmpt reads.
/// </summary>
/// <param name="Type">The format's usual extension: "mod", "xm", "it", "s3m", ...</param>
/// <param name="TypeName">The format's name: "ProTracker MOD", "Impulse Tracker", ...</param>
/// <param name="Tracker">The program that saved it, when known.</param>
/// <param name="Title">The song title, empty when it has none.</param>
/// <param name="Artist">The author, empty when unknown.</param>
/// <param name="Message">The song message, empty when it has none.</param>
/// <param name="Channels">Pattern channels.</param>
/// <param name="Orders">Entries of the order list (the song's sequence of patterns).</param>
/// <param name="Patterns">Patterns.</param>
/// <param name="RestartOrder">Order the song's own loop goes back to.</param>
/// <param name="RestartRow">Row the song's own loop goes back to.</param>
/// <param name="Duration">The song played once.</param>
/// <param name="Instruments">Instrument names (empty for formats without instruments, such as MOD).</param>
/// <param name="Samples">Sample names.</param>
/// <param name="ChannelNames">Channel names (most formats leave them empty).</param>
/// <param name="PatternNames">Pattern names (most formats leave them empty).</param>
/// <param name="Subsongs">Subsong names.</param>
public sealed record TrackerInfo(
    string Type, string TypeName, string Tracker, string Title, string Artist, string Message,
    int Channels, int Orders, int Patterns, int RestartOrder, int RestartRow, TimeSpan Duration,
    IReadOnlyList<string> Instruments, IReadOnlyList<string> Samples, IReadOnlyList<string> ChannelNames,
    IReadOnlyList<string> PatternNames, IReadOnlyList<string> Subsongs);

/// <summary>Resampling of a tracker module's samples (<see cref="AudioStream.TrackerInterpolation"/>).</summary>
public enum TrackerInterpolation
{
    /// <summary>libopenmpt's default (8-tap windowed sinc).</summary>
    Default = 0,
    /// <summary>None (zero-order hold): the crunchy sound of early trackers.</summary>
    None = 1,
    /// <summary>Linear.</summary>
    Linear = 2,
    /// <summary>Cubic.</summary>
    Cubic = 4,
    /// <summary>8-tap windowed sinc.</summary>
    Sinc = 8,
}
