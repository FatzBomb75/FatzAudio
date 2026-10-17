using System.Runtime.InteropServices;

namespace FatzAudio.Native;

/// <summary>Owns an <c>fa_engine*</c>. Children (devices, streams) add a reference so the engine outlives them.</summary>
internal sealed class EngineHandle : SafeHandle
{
    public EngineHandle() : base(0, true) { }
    public override bool IsInvalid => handle == 0;

    protected override bool ReleaseHandle()
    {
        NativeMethods.fa_engine_destroy(handle);
        return true;
    }
}

/// <summary>Owns an <c>fa_device*</c> and keeps its engine alive.</summary>
internal sealed class DeviceHandle : SafeHandle
{
    private EngineHandle? _engine;

    public DeviceHandle() : base(0, true) { }
    public override bool IsInvalid => handle == 0;

    public void Initialize(nint native, EngineHandle engine)
    {
        _engine = HandleRef.AddRef(engine);
        SetHandle(native);
    }

    protected override bool ReleaseHandle()
    {
        NativeMethods.fa_device_close(handle);
        _engine?.DangerousRelease();
        _engine = null;
        return true;
    }
}

/// <summary>
/// Owns an <c>fa_stream*</c>, the GC handles of its callbacks, and references to the engine and
/// the attached device so that native teardown always happens in a safe order
/// (stream first, then device, then engine).
/// </summary>
internal sealed class StreamHandle : SafeHandle
{
    private EngineHandle? _engine;
    private DeviceHandle? _device;
    private readonly List<GCHandle> _pins = new();

    public StreamHandle() : base(0, true) { }
    public override bool IsInvalid => handle == 0;

    public void Initialize(nint native, EngineHandle engine)
    {
        _engine = HandleRef.AddRef(engine);
        SetHandle(native);
    }

    /// <summary>Keeps a callback state object alive until the native stream is closed.</summary>
    public GCHandle Pin(object state)
    {
        var h = GCHandle.Alloc(state);
        lock (_pins) _pins.Add(h);
        return h;
    }

    /// <summary>Takes ownership of an already allocated GC handle (freed when the stream closes).</summary>
    public void Adopt(GCHandle h)
    {
        lock (_pins) _pins.Add(h);
    }

    public void Unpin(GCHandle h)
    {
        lock (_pins)
        {
            if (_pins.Remove(h)) h.Free();
        }
    }

    public void AttachDevice(DeviceHandle device)
    {
        var previous = _device;
        _device = HandleRef.AddRef(device);
        previous?.DangerousRelease();
    }

    /// <summary>Releases the device reference taken by <see cref="AttachDevice"/>, so a detached
    /// stream no longer keeps its old device open.</summary>
    public void DetachDevice()
    {
        var previous = _device;
        _device = null;
        previous?.DangerousRelease();
    }

    private readonly List<CdgSurface> _surfaces = new();

    /// <summary>Registers a CD+G surface: native surfaces are freed with the stream.</summary>
    public void TrackSurface(CdgSurface surface)
    {
        lock (_surfaces) _surfaces.Add(surface);
    }

    public void UntrackSurface(CdgSurface surface)
    {
        lock (_surfaces) _surfaces.Remove(surface);
    }

    protected override bool ReleaseHandle()
    {
        // Surfaces the app did not dispose are destroyed with the stream.
        lock (_surfaces)
        {
            foreach (var s in _surfaces) s.Orphan();
            _surfaces.Clear();
        }
        NativeMethods.fa_stream_close(handle);
        lock (_pins)
        {
            foreach (var h in _pins) h.Free();
            _pins.Clear();
        }
        _device?.DangerousRelease();
        _device = null;
        _engine?.DangerousRelease();
        _engine = null;
        return true;
    }
}

/// <summary>Owns an <c>fa_bus*</c> and keeps its device (and so its engine) alive.</summary>
internal sealed class BusHandle : SafeHandle
{
    private DeviceHandle? _device;

    public BusHandle() : base(0, true) { }
    public override bool IsInvalid => handle == 0;

    public void Initialize(nint native, DeviceHandle device)
    {
        _device = HandleRef.AddRef(device);
        SetHandle(native);
    }

    protected override bool ReleaseHandle()
    {
        NativeMethods.fa_bus_destroy(handle);
        _device?.DangerousRelease();
        _device = null;
        return true;
    }
}

/// <summary>Owns an <c>fa_disc*</c>. Streams opened from the disc hold their own native reference.</summary>
internal sealed class DiscHandle : SafeHandle
{
    public DiscHandle() : base(0, true) { }
    public override bool IsInvalid => handle == 0;

    public void Initialize(nint native) => SetHandle(native);

    protected override bool ReleaseHandle()
    {
        NativeMethods.fa_disc_close(handle);
        return true;
    }
}

/// <summary>Owns one reference to an <c>fa_soundfont*</c>. Engines and streams using it hold their own native references.</summary>
internal sealed class SoundFontHandle : SafeHandle
{
    public SoundFontHandle() : base(0, true) { }
    public override bool IsInvalid => handle == 0;

    protected override bool ReleaseHandle()
    {
        NativeMethods.fa_soundfont_close(handle);
        return true;
    }
}

internal static class HandleRef
{
    public static T AddRef<T>(T handle) where T : SafeHandle
    {
        bool success = false;
        handle.DangerousAddRef(ref success);
        if (!success) throw new ObjectDisposedException(typeof(T).Name);
        return handle;
    }
}

/// <summary>
/// Owns an <c>fa_encoder*</c> and the GC handle of its output callbacks. Releasing it completes the file
/// (<c>fa_encoder_close</c>); the result is kept in <see cref="CloseResult"/>.
/// </summary>
internal sealed class EncoderHandle : SafeHandle
{
    private GCHandle _output;

    public EncoderHandle() : base(0, true) { }
    public override bool IsInvalid => handle == 0;

    /// <summary>What <c>fa_encoder_close</c> returned, once released.</summary>
    public int CloseResult { get; private set; }

    private volatile bool _released;

    /// <summary>True once <c>fa_encoder_close</c> has run.</summary>
    public bool IsReleased => _released;

    public void Initialize(nint native, GCHandle output)
    {
        _output = output;
        SetHandle(native);
    }

    protected override bool ReleaseHandle()
    {
        CloseResult = NativeMethods.fa_encoder_close(handle);
        if (_output.IsAllocated) _output.Free();
        _released = true;
        return true;
    }
}

