using FatzAudio.Native;

namespace FatzAudio;

/// <summary>
/// A group of streams on one <see cref="OutputDevice"/> with its own volume and pan: a DJ deck, a group of
/// jingles, a preview channel. Create one with <see cref="OutputDevice.CreateBus"/> and route streams to it
/// with <see cref="AudioStream.PlayOn(MixBus)"/>.
/// </summary>
/// <remarks>
/// Gains apply in order: stream (volume, pan, fades, normalization), then bus, then the device limiter. Bus
/// changes are smoothed over 5 ms and exact to the frame. Bus ramps run whenever the device runs. Disposing a
/// bus moves its streams to the device's master mix; they keep playing.
/// </remarks>
/// <example>
/// <code>
/// // A DJ crossfader: two decks, faded in opposite directions.
/// using var deckA = output.CreateBus();
/// using var deckB = output.CreateBus();
/// trackA.PlayOn(deckA);
/// trackB.PlayOn(deckB);
/// deckA.Ramp(AudioParameter.Volume, 0f, TimeSpan.FromSeconds(8), Curve.EqualPower);
/// deckB.Volume = 0f;
/// deckB.Ramp(AudioParameter.Volume, 1f, TimeSpan.FromSeconds(8), Curve.EqualPower);
/// </code>
/// </example>
public sealed class MixBus : IDisposable
{
    private readonly BusHandle _handle;

    internal MixBus(OutputDevice device, BusHandle handle)
    {
        Device = device;
        _handle = handle;
    }

    /// <summary>The device this bus mixes into.</summary>
    public OutputDevice Device { get; }

    internal BusHandle Handle
    {
        get
        {
            ObjectDisposedException.ThrowIf(_handle.IsClosed, this);
            return _handle;
        }
    }

    /// <summary>Linear gain of the bus (1 = unity).</summary>
    /// <exception cref="FatzAudioException">The value is negative (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    public float Volume { get => Get(AudioParameter.Volume); set => Set(AudioParameter.Volume, value); }

    /// <summary>Stereo pan of the bus from -1 (left) to 1 (right).</summary>
    public float Pan { get => Get(AudioParameter.Pan); set => Set(AudioParameter.Pan, value); }

    private float Get(AudioParameter p)
    {
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_bus_get_param(Handle, (int)p, out float v));
        return v;
    }

    private void Set(AudioParameter p, float v) =>
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_bus_set_param(Handle, (int)p, v));

    /// <summary>Ramps the bus volume or pan to a target over time and returns immediately.</summary>
    /// <param name="parameter"><see cref="AudioParameter.Volume"/> or <see cref="AudioParameter.Pan"/>.</param>
    /// <param name="target">Target value.</param>
    /// <param name="duration">Ramp length.</param>
    /// <param name="curve">Ramp shape; defaults to <see cref="Curve.Linear"/>.</param>
    /// <param name="startAt">
    /// When to start, on the <see cref="OutputDevice.Time"/> clock; null starts now. The value holds until then.
    /// </param>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="duration"/> or <paramref name="startAt"/> is negative.</exception>
    /// <exception cref="FatzAudioException">The parameter cannot be ramped on a bus (<see cref="ErrorCode.NotImplemented"/> for rate, tempo and pitch).</exception>
    public unsafe void Ramp(AudioParameter parameter, float target, TimeSpan duration, Curve? curve = null, TimeSpan? startAt = null)
    {
        if (duration < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(duration));
        if (startAt < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(startAt));
        var c = curve ?? Curve.Linear;
        var ramp = c.ToNative();
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_bus_ramp_param(Handle, (int)parameter, target,
            TimeConvert.ToNanoseconds(duration), startAt is { } t ? Math.Max(1, TimeConvert.ToNanoseconds(t)) : 0, &ramp));
        GC.KeepAlive(c);
    }

    /// <summary>Releases the bus. Its streams continue on the device's master mix.</summary>
    public void Dispose() => _handle.Dispose();
}
