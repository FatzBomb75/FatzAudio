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

    protected override bool ReleaseHandle()
    {
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
