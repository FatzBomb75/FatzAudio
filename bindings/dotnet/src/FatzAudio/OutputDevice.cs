using FatzAudio.Native;

namespace FatzAudio;

/// <summary>
/// An opened playback device. Attach streams with <see cref="AudioStream.PlayOn"/>.
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
    private bool _started;

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

    /// <summary>True after <see cref="Start"/> (or after a stream was started on it) and before <see cref="Stop"/>.</summary>
    public bool IsStarted => _started;

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

    /// <summary>Starts the device so that attached, playing streams become audible. Does nothing if already started.</summary>
    /// <exception cref="ObjectDisposedException">The device was disposed.</exception>
    /// <exception cref="FatzAudioException">The backend failed to start the device (<see cref="ErrorCode.Device"/>).</exception>
    public void Start()
    {
        var h = Handle;
        if (_started) return;
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_device_start(h));
        _started = true;
    }

    /// <summary>Stops the device; attached streams are silenced but keep their state. Does nothing if not started.</summary>
    /// <exception cref="ObjectDisposedException">The device was disposed.</exception>
    /// <exception cref="FatzAudioException">The backend failed to stop the device.</exception>
    public void Stop()
    {
        var h = Handle;
        if (!_started) return;
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_device_stop(h));
        _started = false;
    }

    /// <summary>Stops the device and releases this reference to it.</summary>
    public void Dispose()
    {
        if (_handle.IsClosed) return;
        if (_started)
        {
            try { NativeMethods.fa_device_stop(_handle); } catch (ObjectDisposedException) { }
            _started = false;
        }
        _handle.Dispose();
    }
}
