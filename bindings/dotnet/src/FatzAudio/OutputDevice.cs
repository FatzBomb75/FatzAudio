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

    internal bool IsDisposed => _handle.IsClosed;

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

    /// <summary>
    /// Silences everything the device plays without touching <see cref="Volume"/>; glides over 5 ms. An encoder
    /// recording the device records the silence too.
    /// </summary>
    /// <exception cref="ObjectDisposedException">The device was disposed.</exception>
    public bool Muted
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_device_get_mute(Handle, out int m));
            return m != 0;
        }
        set => FatzAudioException.ThrowIfFailed(NativeMethods.fa_device_set_mute(Handle, value ? 1 : 0));
    }

    /// <summary>
    /// Peak and RMS of the device's whole output being heard now (after the limiter), for a master VU meter.
    /// All zeros while the device is stopped. The first call starts recording, so it reads silence.
    /// </summary>
    /// <param name="window">Length of the measurement window; defaults to 50 ms, at most about 1 s.</param>
    /// <returns>Levels for left and right.</returns>
    /// <exception cref="ObjectDisposedException">The device was disposed.</exception>
    public StreamLevels GetLevels(TimeSpan? window = null)
    {
        var peak = new float[2];
        var rms = new float[2];
        GetLevels(peak, rms, window);
        return new StreamLevels(peak, rms);
    }

    /// <summary>Like <see cref="GetLevels(TimeSpan?)"/>, into your own buffers (no allocation per call).</summary>
    /// <param name="peak">Receives the left and right peaks (at least 2 values).</param>
    /// <param name="rms">Receives the left and right RMS values (at least 2 values).</param>
    /// <param name="window">Length of the measurement window; defaults to 50 ms, at most about 1 s.</param>
    /// <exception cref="ArgumentException">A span shorter than 2.</exception>
    /// <exception cref="ObjectDisposedException">The device was disposed.</exception>
    public unsafe void GetLevels(Span<float> peak, Span<float> rms, TimeSpan? window = null)
    {
        var handle = Handle;
        if (peak.Length < 2 || rms.Length < 2) throw new ArgumentException("Room for left and right is needed.", peak.Length < 2 ? nameof(peak) : nameof(rms));
        fixed (float* pp = peak)
        fixed (float* pr = rms)
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_device_get_levels(
                handle, TimeConvert.ToNanoseconds(window ?? StreamLevels.DefaultWindow), pp, pr, 2));
        }
    }

    /// <summary>The magnitude spectrum of the device's whole output being heard now, for a visualizer.</summary>
    /// <param name="size">Transform size: a power of two from 64 to 16384.</param>
    /// <param name="options">Channel and window.</param>
    /// <returns><paramref name="size"/> / 2 magnitudes and the bin width.</returns>
    /// <exception cref="FatzAudioException">An invalid size (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    public unsafe Spectrum GetSpectrum(int size = 2048, SpectrumOptions options = SpectrumOptions.None)
    {
        var bins = new float[Math.Max(1, size / 2)];
        return new Spectrum(bins, GetSpectrumCore((uint)Math.Max(0, size), bins, options));
    }

    /// <summary>Like <see cref="GetSpectrum(int, SpectrumOptions)"/>, into your own buffer (no allocation per call).</summary>
    /// <param name="magnitudes">Half the transform size: 32 to 8192 values, a power of two.</param>
    /// <param name="options">Channel and window.</param>
    /// <returns>The bin width in Hz; 0 while the device is stopped.</returns>
    /// <exception cref="FatzAudioException">An invalid length (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    public unsafe float GetSpectrum(Span<float> magnitudes, SpectrumOptions options = SpectrumOptions.None)
    {
        return GetSpectrumCore((uint)magnitudes.Length * 2, magnitudes, options);
    }

    private unsafe float GetSpectrumCore(uint size, Span<float> bins, SpectrumOptions options)
    {
        float hz;
        fixed (float* p = bins)
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_device_get_spectrum(Handle, size, (uint)options, p, &hz));
        }
        return hz;
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
