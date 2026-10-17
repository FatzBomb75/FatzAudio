using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace FatzAudio;

/// <summary>Bridges a managed <see cref="Stream"/> to the native custom I/O callbacks.</summary>
internal sealed unsafe class IoState
{
    private readonly Stream _stream;
    private readonly bool _leaveOpen;
    private readonly object _lock = new();

    public IoState(Stream stream, bool leaveOpen)
    {
        _stream = stream;
        _leaveOpen = leaveOpen;
    }

    private static IoState? From(void* user) =>
        user == null ? null : GCHandle.FromIntPtr((nint)user).Target as IoState;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static long Read(void* user, void* buffer, nuint bytes)
    {
        try
        {
            var s = From(user);
            if (s is null) return (long)ErrorCode.Io;
            int count = (int)Math.Min(bytes, (nuint)int.MaxValue);
            lock (s._lock) return s._stream.Read(new Span<byte>(buffer, count));
        }
        catch
        {
            return (long)ErrorCode.Io;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static int Seek(void* user, long offset)
    {
        try
        {
            var s = From(user);
            if (s is null) return (int)ErrorCode.Io;
            lock (s._lock) s._stream.Seek(offset, SeekOrigin.Begin);
            return 0;
        }
        catch
        {
            return (int)ErrorCode.Io;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static long Length(void* user)
    {
        try
        {
            var s = From(user);
            if (s is null) return -1;
            lock (s._lock) return s._stream.Length;
        }
        catch
        {
            return -1;
        }
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static void Close(void* user)
    {
        try
        {
            var s = From(user);
            if (s is not null && !s._leaveOpen) lock (s._lock) s._stream.Dispose();
        }
        catch
        {
            // never let an exception cross the native boundary
        }
    }
}
