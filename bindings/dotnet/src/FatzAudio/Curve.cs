using FatzAudio.Native;

namespace FatzAudio;

/// <summary>Shape of a parameter ramp or crossfade.</summary>
/// <example>
/// <code>
/// stream.Ramp(AudioParameter.Volume, 0f, TimeSpan.FromSeconds(3), Curve.EqualPower);
/// stream.Ramp(AudioParameter.Pan, 1f, TimeSpan.FromSeconds(1), Curve.Bezier(0.25f, 0.1f, 0.25f, 1f));
/// </code>
/// </example>
public sealed class Curve : IEquatable<Curve>
{
    private readonly int _kind;
    private readonly float _x1, _y1, _x2, _y2;

    private Curve(int kind, float x1 = 0, float y1 = 0, float x2 = 0, float y2 = 0)
    {
        _kind = kind; _x1 = x1; _y1 = y1; _x2 = x2; _y2 = y2;
    }

    /// <summary>Straight line from start to target.</summary>
    public static Curve Linear { get; } = new(0);
    /// <summary>Equal-power curve; keeps perceived loudness constant while crossfading.</summary>
    public static Curve EqualPower { get; } = new(1);
    /// <summary>Logarithmic curve (fast start, slow finish).</summary>
    public static Curve Logarithmic { get; } = new(2);
    /// <summary>Exponential curve (slow start, fast finish).</summary>
    public static Curve Exponential { get; } = new(3);
    /// <summary>Smooth S-shaped curve.</summary>
    public static Curve SCurve { get; } = new(4);

    /// <summary>A CSS <c>cubic-bezier(x1, y1, x2, y2)</c> style curve.</summary>
    /// <param name="x1">X of the first control point (0..1).</param>
    /// <param name="y1">Y of the first control point.</param>
    /// <param name="x2">X of the second control point (0..1).</param>
    /// <param name="y2">Y of the second control point.</param>
    /// <returns>A new bezier curve.</returns>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="x1"/> or <paramref name="x2"/> is outside 0..1.</exception>
    public static Curve Bezier(float x1, float y1, float x2, float y2)
    {
        if (x1 < 0 || x1 > 1) throw new ArgumentOutOfRangeException(nameof(x1), "x1 must be within 0..1.");
        if (x2 < 0 || x2 > 1) throw new ArgumentOutOfRangeException(nameof(x2), "x2 must be within 0..1.");
        return new Curve(5, x1, y1, x2, y2);
    }

    internal FaRamp ToNative() => new()
    {
        Curve = _kind, BezierX1 = _x1, BezierY1 = _y1, BezierX2 = _x2, BezierY2 = _y2,
    };

    /// <inheritdoc/>
    public bool Equals(Curve? other) =>
        other is not null && _kind == other._kind && _x1 == other._x1 && _y1 == other._y1 && _x2 == other._x2 && _y2 == other._y2;

    /// <inheritdoc/>
    public override bool Equals(object? obj) => Equals(obj as Curve);

    /// <inheritdoc/>
    public override int GetHashCode() => HashCode.Combine(_kind, _x1, _y1, _x2, _y2);

    /// <inheritdoc/>
    public override string ToString() => _kind switch
    {
        0 => "Linear", 1 => "EqualPower", 2 => "Logarithmic", 3 => "Exponential", 4 => "SCurve",
        _ => $"Bezier({_x1}, {_y1}, {_x2}, {_y2})",
    };
}
