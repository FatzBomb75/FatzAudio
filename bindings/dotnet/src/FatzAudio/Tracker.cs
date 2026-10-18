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
/// <param name="Subsong">The subsong this stream plays: the <c>AudioTrack</c> it was opened with, 0 by default
/// (<see cref="RestartOrder"/>, <see cref="RestartRow"/> and <see cref="Duration"/> are this subsong's).</param>
public sealed record TrackerInfo(
    string Type, string TypeName, string Tracker, string Title, string Artist, string Message,
    int Channels, int Orders, int Patterns, int RestartOrder, int RestartRow, TimeSpan Duration,
    IReadOnlyList<string> Instruments, IReadOnlyList<string> Samples, IReadOnlyList<string> ChannelNames,
    IReadOnlyList<string> PatternNames, IReadOnlyList<string> Subsongs, int Subsong);

/// <summary>A row of a tracker module: an entry of the order list and a row of its pattern, both 0-based.</summary>
/// <param name="Order">Entry of the order list.</param>
/// <param name="Row">Row of that order's pattern.</param>
public readonly record struct TrackerPosition(int Order, int Row);

/// <summary>Which rows raise <see cref="AudioStream.TrackerRowReached"/> (<see cref="AudioStream.TrackerRowEvents"/>).</summary>
public enum TrackerRowEvents
{
    /// <summary>None (the default).</summary>
    Off = 0,
    /// <summary>Every row heard.</summary>
    EveryRow = 1,
    /// <summary>Only the first row of each order.</summary>
    Orders = 2,
}

/// <summary>A tracker module row being heard: see <see cref="AudioStream.TrackerRowReached"/>.</summary>
public sealed class TrackerRowEventArgs : EventArgs
{
    internal TrackerRowEventArgs(TrackerPosition position, TimeSpan time, long frame)
    {
        Position = position;
        Time = time;
        Frame = frame;
    }

    /// <summary>The order and row.</summary>
    public TrackerPosition Position { get; }

    /// <summary>Where the row starts in the song.</summary>
    public TimeSpan Time { get; }

    /// <summary>Where the row starts, in frames of the stream.</summary>
    public long Frame { get; }
}

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
