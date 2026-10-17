using System.Runtime.InteropServices;
using FatzAudio.Native;

namespace FatzAudio;

/// <summary>Error codes returned by the native FatzAudio library (<c>FA_ERROR_*</c>).</summary>
public enum ErrorCode
{
    /// <summary>Success (never carried by an exception).</summary>
    Ok = 0,
    /// <summary>Unspecified failure.</summary>
    Generic = -1,
    /// <summary>An argument was invalid (null, out of range, wrong state for the call).</summary>
    InvalidArgument = -2,
    /// <summary>The native library ran out of memory.</summary>
    OutOfMemory = -3,
    /// <summary>A file, device or other resource was not found.</summary>
    NotFound = -4,
    /// <summary>The audio data is in a format that cannot be decoded.</summary>
    UnsupportedFormat = -5,
    /// <summary>The audio device reported an error.</summary>
    Device = -6,
    /// <summary>An I/O error occurred while reading the source.</summary>
    Io = -7,
    /// <summary>A network error occurred.</summary>
    Network = -8,
    /// <summary>The operation needs a seekable source.</summary>
    NotSeekable = -9,
    /// <summary>The object is busy (for example a stream that is already attached to a device).</summary>
    Busy = -10,
    /// <summary>The end of the stream was reached.</summary>
    EndOfStream = -11,
    /// <summary>The native library does not implement this function yet.</summary>
    NotImplemented = -12,
    /// <summary>An optional provider (for example FFmpeg or LAME) could not be found at runtime.</summary>
    ProviderMissing = -13,
    /// <summary>The CD drive has no disc, its tray is open, or the disc was changed under an open stream.</summary>
    NoDisc = -14,
}

/// <summary>
/// Thrown when a native FatzAudio call fails. The error is always taken from the call's own return
/// value; there is no "last error" state to poll.
/// </summary>
/// <example>
/// <code>
/// try { stream.Ramp(AudioParameter.Volume, 0f, TimeSpan.FromSeconds(2)); }
/// catch (FatzAudioException ex) when (ex.ErrorCode == ErrorCode.NotImplemented) { /* not available yet */ }
/// </code>
/// </example>
public class FatzAudioException : Exception
{
    /// <summary>Creates an exception for a native error code.</summary>
    /// <param name="errorCode">The native error code.</param>
    /// <param name="message">Optional message; defaults to the text from <c>fa_result_string</c>.</param>
    public FatzAudioException(ErrorCode errorCode, string? message = null)
        : base(message ?? DescribeNative(errorCode))
    {
        ErrorCode = errorCode;
        HResult = (int)errorCode;
    }

    /// <summary>The native error code that caused this exception.</summary>
    public ErrorCode ErrorCode { get; }

    private static string DescribeNative(ErrorCode code)
    {
        try
        {
            NativeLoader.EnsureInitialized();
            var p = NativeMethods.fa_result_string((int)code);
            var s = p == 0 ? null : Marshal.PtrToStringUTF8(p);
            if (!string.IsNullOrEmpty(s)) return $"{s} ({code})";
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)
        {
            // fall through to the managed description
        }
        return code.ToString();
    }

    /// <summary>Throws <see cref="FatzAudioException"/> if <paramref name="result"/> is negative.</summary>
    internal static void ThrowIfFailed(int result)
    {
        if (result < 0) throw new FatzAudioException((ErrorCode)result);
    }
}
