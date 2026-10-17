using System.Runtime.InteropServices;
using FatzAudio.Native;

namespace FatzAudio;

/// <summary>Shape of a parameter ramp, marker fade or crossfade.</summary>
/// <remarks>
/// A shape maps progress (0 at the start, 1 at the end) to 0..1; a ramp's value is
/// <c>start + (target - start) * shape</c>. Fade-outs and the outgoing side of a crossfade play the shape
/// backwards, so an in/out pair is symmetric.
/// </remarks>
/// <example>
/// <code>
/// stream.Ramp(AudioParameter.Volume, 0f, TimeSpan.FromSeconds(3), Curve.EqualPower);
/// stream.Ramp(AudioParameter.Pan, 1f, TimeSpan.FromSeconds(1), Curve.Bezier(0.25f, 0.1f, 0.25f, 1f));
/// var swell = Curve.Points(0f, 0.1f, 0.2f, 0.6f, 1f);   // your own shape
/// </code>
/// </example>
public sealed class Curve : IEquatable<Curve>
{
    /// <summary>Most points a <see cref="Points"/> curve may have.</summary>
    public const int MaxPoints = 64;

    private readonly int _kind;
    private readonly float _x1, _y1, _x2, _y2;
    private readonly float[]? _points; // pinned: the native call reads it in place

    private Curve(int kind, float x1 = 0, float y1 = 0, float x2 = 0, float y2 = 0, float[]? points = null)
    {
        _kind = kind; _x1 = x1; _y1 = y1; _x2 = x2; _y2 = y2; _points = points;
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

    /// <summary>
    /// Your own shape: its values at evenly spaced times from the start to the end, joined by straight lines.
    /// Normally the first value is 0 and the last 1.
    /// </summary>
    /// <param name="values">2 to <see cref="MaxPoints"/> values.</param>
    /// <returns>A new point curve.</returns>
    /// <exception cref="ArgumentException">Fewer than 2 or more than <see cref="MaxPoints"/> values, or a value that is not a finite number.</exception>
    /// <example>
    /// <code>
    /// // Hold back for the first third, then come in fast.
    /// song.Ramp(AudioParameter.Volume, 1f, TimeSpan.FromSeconds(3), Curve.Points(0f, 0.05f, 0.1f, 1f));
    /// </code>
    /// </example>
    public static Curve Points(params float[] values)
    {
        ArgumentNullException.ThrowIfNull(values);
        if (values.Length < 2 || values.Length > MaxPoints)
            throw new ArgumentException($"A point curve needs 2 to {MaxPoints} values.", nameof(values));
        var copy = GC.AllocateArray<float>(values.Length, pinned: true);
        for (int i = 0; i < values.Length; i++)
        {
            if (!float.IsFinite(values[i])) throw new ArgumentException("Curve values must be finite numbers.", nameof(values));
            copy[i] = values[i];
        }
        return new Curve(6, points: copy);
    }

    /// <summary>The values of a <see cref="Points"/> curve; empty for the built-in shapes.</summary>
    public IReadOnlyList<float> PointValues => _points ?? [];

    // The pinned point array stays valid while this Curve is reachable; callers keep it alive
    // across the native call with GC.KeepAlive.
    internal unsafe FaRamp ToNative() => new()
    {
        Curve = _kind, BezierX1 = _x1, BezierY1 = _y1, BezierX2 = _x2, BezierY2 = _y2,
        Points = _points is null ? null : (float*)Marshal.UnsafeAddrOfPinnedArrayElement(_points, 0),
        PointCount = (uint)(_points?.Length ?? 0),
    };

    /// <inheritdoc/>
    public bool Equals(Curve? other) =>
        other is not null && _kind == other._kind && _x1 == other._x1 && _y1 == other._y1 && _x2 == other._x2 && _y2 == other._y2
        && PointValues.SequenceEqual(other.PointValues);

    /// <inheritdoc/>
    public override bool Equals(object? obj) => Equals(obj as Curve);

    /// <inheritdoc/>
    public override int GetHashCode() => HashCode.Combine(_kind, _x1, _y1, _x2, _y2, _points?.Length ?? 0);

    /// <inheritdoc/>
    public override string ToString() => _kind switch
    {
        0 => "Linear", 1 => "EqualPower", 2 => "Logarithmic", 3 => "Exponential", 4 => "SCurve",
        5 => $"Bezier({_x1}, {_y1}, {_x2}, {_y2})",
        _ => $"Points({string.Join(", ", PointValues)})",
    };
}
