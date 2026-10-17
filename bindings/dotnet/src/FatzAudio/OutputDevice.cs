using FatzAudio.Native;

namespace FatzAudio;

/// <summary>
/// An opened playback device. Attach streams with <see cref="AudioStream.PlayOn(OutputDevice)"/>.
/// </summary>
/// <remarks>
/// Disposing a device stops it. If streams are still attached the native device object stays alive
/// until those streams are disposed, so teardown order never matters.
/// </remarks>
/// <example>
/// <code>
/// using var output = engine.OpenOutput();
/// output.Volume = 0.8f;
/// Console.WriteLine($"Latency: {output.Latency.TotalMilliseconds} ms");
/// </code>
/// </example>
public sealed class OutputDevice : IDisposable
{
    private readonly DeviceHandle _handle;

    internal OutputDevice(AudioEngine engine, DeviceHandle handle, AudioDeviceInfo? info)
    {
        Engine = engine;
        _handle = handle;
        Info = info;
    }

    /// <summary>The engine that opened this device.</summary>
    public AudioEngine Engine { get; }

    /// <summary>The device description this output was opened with, or null when the system default device was requested.</summary>
    public AudioDeviceInfo? Info { get; }

    internal DeviceHandle Handle
    {
        get
        {
            ObjectDisposedException.ThrowIf(_handle.IsClosed, this);
            return _handle;
        }
    }

    /// <summary>True while the device is running: after <see cref="Start"/> (or after a stream was started on it) and before <see cref="Stop"/>.</summary>
    /// <exception cref="ObjectDisposedException">The device was disposed.</exception>
    public bool IsStarted
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_device_is_started(Handle, out int started));
            return started != 0;
        }
    }

    /// <summary>Master volume of the device as linear gain (1 = unity, values above 1 amplify).</summary>
    /// <exception cref="ArgumentOutOfRangeException">The value is negative.</exception>
    /// <exception cref="ObjectDisposedException">The device was disposed.</exception>
    /// <exception cref="FatzAudioException">The backend rejected the change.</exception>
    public float Volume
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_device_get_volume(Handle, out float v));
            return v;
        }
        set
        {
            if (value < 0 || float.IsNaN(value)) throw new ArgumentOutOfRangeException(nameof(value), "Volume must be >= 0.");
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_device_set_volume(Handle, value));
        }
    }

    /// <summary>Output latency of the device.</summary>
    /// <exception cref="ObjectDisposedException">The device was disposed.</exception>
    /// <exception cref="FatzAudioException">The latency could not be determined.</exception>
    public TimeSpan Latency
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_device_get_latency(Handle, out long ns));
            return TimeConvert.FromNanoseconds(ns);
        }
    }

    /// <summary>
    /// The device's sample clock: how much audio it has mixed since it was opened. It advances only while
    /// the device runs and never jumps. <see cref="AudioStream.PlayAt"/>, scheduled ramps and queues are
    /// placed on this clock to the exact frame. It moves in steps of one device period, so schedule at least
    /// one period ahead (<c>Time + 50 ms</c> is always safe).
    /// </summary>
    /// <exception cref="ObjectDisposedException">The device was disposed.</exception>
    /// <example>
    /// <code>
    /// var start = output.Time + TimeSpan.FromMilliseconds(100);
    /// left.PlayAt(start);
    /// right.PlayAt(start);   // both start on the same frame
    /// </code>
    /// </example>
    public TimeSpan Time
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_device_get_time(Handle, out long ns));
            return TimeConvert.FromNanoseconds(ns);
        }
    }

    /// <summary>
    /// Creates a mix bus on this device: a group of streams with its own volume and pan, such as a DJ deck or
    /// a preview channel. Route streams to it with <see cref="AudioStream.PlayOn(MixBus)"/>.
    /// </summary>
    /// <returns>The bus; dispose it when done (its streams then continue on the device's master mix).</returns>
    /// <exception cref="ObjectDisposedException">The device was disposed.</exception>
    /// <example>
    /// <code>
    /// using var deckA = output.CreateBus();
    /// using var deckB = output.CreateBus();
    /// trackA.PlayOn(deckA);
    /// trackB.PlayOn(deckB);
    /// deckA.Volume = 0.3f;    // crossfader towards B
    /// </code>
    /// </example>
    public MixBus CreateBus()
    {
        var h = Handle;
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_bus_create(h, out nint native));
        var bh = new BusHandle();
        try
        {
            bh.Initialize(native, h);
        }
        catch
        {
            NativeMethods.fa_bus_destroy(native);
            throw;
        }
        return new MixBus(this, bh);
    }

    /// <summary>Starts the device so that attached, playing streams become audible. Does nothing if already started.</summary>
    /// <exception cref="ObjectDisposedException">The device was disposed.</exception>
    /// <exception cref="FatzAudioException">The backend failed to start the device (<see cref="ErrorCode.Device"/>).</exception>
    public void Start() => FatzAudioException.ThrowIfFailed(NativeMethods.fa_device_start(Handle));

    /// <summary>Stops the device; attached streams are silenced but keep their state. Does nothing if not started.</summary>
    /// <exception cref="ObjectDisposedException">The device was disposed.</exception>
    /// <exception cref="FatzAudioException">The backend failed to stop the device.</exception>
    public void Stop() => FatzAudioException.ThrowIfFailed(NativeMethods.fa_device_stop(Handle));

    /// <summary>Stops the device and releases this reference to it.</summary>
    public void Dispose()
    {
        if (_handle.IsClosed) return;
        try { NativeMethods.fa_device_stop(_handle); } catch (ObjectDisposedException) { }
        _handle.Dispose();
    }
}
