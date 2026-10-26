using System.Runtime.InteropServices;

namespace FatzAudio.Native;

/// <summary>UTF-8 strings to and from the native library.</summary>
internal static unsafe class NativeText
{
    /// <summary>The string at <paramref name="p"/>; empty for null.</summary>
    public static string Read(nint p) => Marshal.PtrToStringUTF8(p) ?? "";

    /// <inheritdoc cref="Read(nint)"/>
    public static string Read(byte* p) => Read((nint)p);

    /// <summary>The string at <paramref name="p"/>; null for null.</summary>
    public static string? ReadOrNull(nint p) => p == 0 ? null : Marshal.PtrToStringUTF8(p);

    /// <summary>A CoTaskMem UTF-8 copy (free with Marshal.FreeCoTaskMem); 0 for null.</summary>
    public static nint Alloc(string? s) => s is null ? 0 : Marshal.StringToCoTaskMemUTF8(s);
}
