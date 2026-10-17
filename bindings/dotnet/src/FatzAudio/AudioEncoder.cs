using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using FatzAudio.Native;

namespace FatzAudio;

/// <summary>Output formats of <see cref="AudioEncoder"/>.</summary>
public enum EncodeFormat
{
    /// <summary>WAV: 16 or 24-bit PCM, or 32-bit float. Built in; RF64 past 4 GB.</summary>
    Wav = 1,
    /// <summary>FLAC: lossless, 16 or 24 bit, with an MD5 of the audio. Built in.</summary>
    Flac = 2,
    /// <summary>MP3 through LAME, loaded at runtime (see <see cref="Lame"/>). Never bundled.</summary>
    Mp3 = 3,
}

/// <summary>Where an encoder is in its life.</summary>
public enum EncoderState
{
    /// <summary>No source yet.</summary>
    Idle = 0,
    /// <summary>Encoding.</summary>
    Running = 1,
    /// <summary><see cref="AudioEncoder.Encode"/> reached the end of the stream.</summary>
    Finished = 2,
    /// <summary>Stopped by an error (<see cref="EncoderStatus.Error"/>).</summary>
    Failed = 3,
}

/// <summary>Settings of an <see cref="AudioEncoder"/>. Every value has a sensible default.</summary>
/// <example>
/// <code>
/// var mp3 = new EncoderOptions { Format = EncodeFormat.Mp3, Quality = 2, Title = "Live set", Artist = "DJ Fatz" };
/// var flac = new EncoderOptions { Format = EncodeFormat.Flac, BitsPerSample = 24 };
/// </code>
/// </example>
public sealed record EncoderOptions
{
    /// <summary>The output format (default WAV).</summary>
    public EncodeFormat Format { get; init; } = EncodeFormat.Wav;

    /// <summary>Output sample rate; 0 (default) keeps the input's. MP3 only has 8 to 48 kHz rates: 0 then picks the
    /// input's when allowed, else 44.1 or 48 kHz.</summary>
    public int SampleRate { get; init; }

    /// <summary>Output channels; 0 (default) keeps the input's. Any count is mixed to any other; MP3 has at most 2.</summary>
    public int Channels { get; init; }

    /// <summary>WAV: 16, 24 or 32 (float); FLAC: 16 or 24. 0 (default) = 16. MP3 ignores it.</summary>
    public int BitsPerSample { get; init; }

    /// <summary>MP3 only: a constant bitrate in kbps (8 to 320). 0 (default) = variable bitrate at <see cref="Quality"/>.</summary>
    public int BitrateKbps { get; init; }

    /// <summary>-1 (default) = the format's default. MP3 VBR: 0 (best, about 245 kbps) to 9 (smallest), default 2
    /// (about 190 kbps). FLAC: 0 (fastest) to 8 (smallest), default 5.</summary>
    public int Quality { get; init; } = -1;

    /// <summary>How much input is buffered ahead of the encoder thread; zero (default) = 2 seconds. Recording a
    /// device or a stream drops what does not fit (<see cref="EncoderStatus.DroppedFrames"/>).</summary>
    public TimeSpan QueueLength { get; init; }

    /// <summary>Title tag, or null.</summary>
    public string? Title { get; init; }
    /// <summary>Artist tag, or null.</summary>
    public string? Artist { get; init; }
    /// <summary>Album tag, or null.</summary>
    public string? Album { get; init; }
    /// <summary>Genre tag, or null.</summary>
    public string? Genre { get; init; }
    /// <summary>Date tag ("2026" or "2026-10-17"), or null.</summary>
    public string? Date { get; init; }
    /// <summary>Comment tag, or null.</summary>
    public string? Comment { get; init; }
    /// <summary>Track number tag; 0 = none.</summary>
    public int TrackNumber { get; init; }
}

/// <summary>A snapshot of an encoder.</summary>
/// <param name="State">Where the encoder is.</param>
/// <param name="Error">The first error, <see cref="ErrorCode.Ok"/> if none.</param>
/// <param name="IsPaused">True while paused.</param>
/// <param name="Frames">Output frames encoded so far.</param>
/// <param name="Duration">The encoded frames as time.</param>
/// <param name="Bytes">Bytes written to the file or output stream.</param>
/// <param name="DroppedFrames">Input lost because the queue was full (the encoder fell behind real time).</param>
/// <param name="Input">The input format, fixed by the source; null while idle.</param>
/// <param name="Output">The output format; null while idle.</param>
public sealed record EncoderStatus(EncoderState State, ErrorCode Error, bool IsPaused, long Frames, TimeSpan Duration,
                                   long Bytes, long DroppedFrames, AudioFormat? Input, AudioFormat? Output);

/// <summary>
/// Writes audio to a WAV, FLAC or MP3 file (or any writable <see cref="Stream"/>) on its own thread. Give it one
/// source: <see cref="Record(OutputDevice)"/> (everything a device plays), <see cref="Record(AudioStream)"/> (one
/// playing stream as heard), <see cref="Encode"/> (a decode-only stream as fast as it decodes: file conversion), or
/// <see cref="Write"/> (your own samples). Several encoders may record the same device or stream.
/// </summary>
/// <remarks>
/// The input is resampled and its channels mixed to the output format when they differ; 16-bit output is
/// dithered, except samples that already are 16-bit values, so a 16-bit source stays lossless.
/// <see cref="Close"/> (or <see cref="Dispose"/>) completes the file: lengths, MD5, the MP3's LAME header.
/// Disposing the device or stream being recorded just ends the input.
/// </remarks>
/// <example>
/// <code>
/// // Record the output to MP3 while it plays
/// using var rec = AudioEncoder.Create("mix.mp3", new EncoderOptions { Format = EncodeFormat.Mp3 });
/// rec.Record(output);
/// // ... later
/// rec.Close();   // throws if anything went wrong (disk full, ...)
///
/// // Convert a file to FLAC
/// AudioEncoder.ConvertFile(engine, "song.wav", "song.flac", new EncoderOptions { Format = EncodeFormat.Flac });
/// </code>
/// </example>
public sealed unsafe class AudioEncoder : IDisposable
{
    private readonly EncoderHandle _handle;
    private int _closed;

    private AudioEncoder(EncoderHandle handle) => _handle = handle;

    internal EncoderHandle Handle
    {
        get
        {
            ObjectDisposedException.ThrowIf(_handle.IsClosed, this);
            return _handle;
        }
    }

    /// <summary>Creates an encoder writing to a file (created or replaced now).</summary>
    /// <param name="path">The output file.</param>
    /// <param name="options">The format and its settings; null = 16-bit WAV at the input's format.</param>
    /// <returns>The encoder; close or dispose it to complete the file.</returns>
    /// <exception cref="FatzAudioException">The file cannot be created (<see cref="ErrorCode.Io"/>), an option is invalid
    /// (<see cref="ErrorCode.InvalidArgument"/>), or MP3 was asked for and LAME was not found
    /// (<see cref="ErrorCode.ProviderMissing"/>).</exception>
    public static AudioEncoder Create(string path, EncoderOptions? options = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        NativeLoader.EnsureInitialized();
        var native = new NativeEncoderOptions(options);
        try
        {
            FaEncoderOptions o = native.Value;
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_encoder_create_path(path, &o, out nint e));
            var h = new EncoderHandle();
            h.Initialize(e, default);
            return new AudioEncoder(h);
        }
        finally
        {
            native.Free();
        }
    }

    /// <summary>
    /// Creates an encoder writing to a stream (a network upload, a pipe, memory). When the stream can seek, the
    /// header is completed at the end; otherwise it keeps "length unknown" values (WAV and FLAC stay playable,
    /// MP3 has no LAME header).
    /// </summary>
    /// <param name="output">A writable stream. It is written from the encoder's thread.</param>
    /// <param name="options">The format and its settings; null = 16-bit WAV at the input's format.</param>
    /// <param name="leaveOpen">True to keep <paramref name="output"/> open after the encoder is closed.</param>
    /// <returns>The encoder.</returns>
    /// <exception cref="ArgumentException">The stream is not writable.</exception>
    /// <exception cref="FatzAudioException">An option is invalid, or LAME is missing for MP3.</exception>
    public static AudioEncoder Create(Stream output, EncoderOptions? options = null, bool leaveOpen = false)
    {
        ArgumentNullException.ThrowIfNull(output);
        if (!output.CanWrite) throw new ArgumentException("The stream must be writable.", nameof(output));
        NativeLoader.EnsureInitialized();
        var native = new NativeEncoderOptions(options);
        var gc = GCHandle.Alloc(new OutputState(output, leaveOpen));
        try
        {
            FaEncoderOptions o = native.Value;
            FaEncoderOutput cb = default;
            cb.Write = &OutputState.Write;
            cb.Close = &OutputState.Close;
            if (output.CanSeek) cb.Seek = &OutputState.Seek;
            int rc = NativeMethods.fa_encoder_create_output(&cb, (void*)GCHandle.ToIntPtr(gc), &o, out nint e);
            FatzAudioException.ThrowIfFailed(rc);
            var h = new EncoderHandle();
            h.Initialize(e, gc);
            return new AudioEncoder(h);
        }
        catch
        {
            gc.Free();
            throw;
        }
        finally
        {
            native.Free();
        }
    }

    /// <summary>
    /// Converts a file: decodes <paramref name="inputPath"/> and encodes it to <paramref name="outputPath"/>, as fast
    /// as the machine allows, on the encoder's thread. Blocks until done.
    /// </summary>
    /// <param name="engine">The engine that opens the input.</param>
    /// <param name="inputPath">Any file FatzAudio can open.</param>
    /// <param name="outputPath">The file to write.</param>
    /// <param name="options">The output format and its settings.</param>
    /// <exception cref="FatzAudioException">The input cannot be opened or decoded, or the output cannot be written.</exception>
    public static void ConvertFile(AudioEngine engine, string inputPath, string outputPath, EncoderOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(engine);
        using var input = engine.OpenFile(inputPath, new StreamOptions { DecodeOnly = true });
        using var enc = Create(outputPath, options);
        enc.Encode(input);
        enc.Wait(Timeout.InfiniteTimeSpan);
        enc.Close();
    }

    /// <summary>Records everything <paramref name="device"/> plays (its master mix, after the limiter), in stereo at
    /// the device's rate.</summary>
    /// <param name="device">The output device.</param>
    /// <exception cref="FatzAudioException">The encoder already has a source (<see cref="ErrorCode.Busy"/>).</exception>
    public void Record(OutputDevice device)
    {
        ArgumentNullException.ThrowIfNull(device);
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_encoder_attach_device(Handle, device.Handle));
    }

    /// <summary>
    /// Records one playing stream as it is heard: volume, pan, fades, normalization and its bus applied, in stereo at
    /// its device's rate. Only what the stream plays is recorded (no silence before it starts or after it ends).
    /// </summary>
    /// <param name="stream">A stream attached to a device (<see cref="AudioStream.PlayOn(OutputDevice)"/>).</param>
    /// <exception cref="FatzAudioException">The stream is not attached (<see cref="ErrorCode.InvalidArgument"/>), or the
    /// encoder already has a source (<see cref="ErrorCode.Busy"/>).</exception>
    public void Record(AudioStream stream)
    {
        ArgumentNullException.ThrowIfNull(stream);
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_encoder_attach_stream(Handle, stream.Handle));
    }

    /// <summary>
    /// Encodes a stream that is not playing, from its position to its end, as fast as it decodes, at its own rate and
    /// channels. Leave the stream alone until <see cref="Wait"/> returns true or the encoder is closed.
    /// </summary>
    /// <param name="stream">A decode-only stream (<see cref="StreamOptions.DecodeOnly"/>) or any stream not attached.</param>
    /// <exception cref="FatzAudioException">The stream is attached or already being encoded, or the encoder already has a
    /// source (<see cref="ErrorCode.Busy"/>).</exception>
    public void Encode(AudioStream stream)
    {
        ArgumentNullException.ThrowIfNull(stream);
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_encoder_encode_stream(Handle, stream.Handle));
    }

    /// <summary>
    /// Encodes your own interleaved float samples. The first call fixes the input format; later calls must use the
    /// same. Blocks while the queue is full, so nothing is ever dropped. Calls must not overlap.
    /// </summary>
    /// <param name="interleaved">Whole frames of interleaved samples.</param>
    /// <param name="sampleRate">Their sample rate.</param>
    /// <param name="channels">Their channel count.</param>
    /// <exception cref="ArgumentException">The span does not hold whole frames.</exception>
    /// <exception cref="FatzAudioException">Another format than the first call's (<see cref="ErrorCode.InvalidArgument"/>),
    /// another source (<see cref="ErrorCode.Busy"/>), or the error that stopped the encoder.</exception>
    public void Write(ReadOnlySpan<float> interleaved, int sampleRate, int channels)
    {
        if (channels <= 0) throw new ArgumentOutOfRangeException(nameof(channels));
        if (sampleRate <= 0) throw new ArgumentOutOfRangeException(nameof(sampleRate));
        if (interleaved.Length % channels != 0) throw new ArgumentException("The span must hold whole frames.", nameof(interleaved));
        fixed (float* p = interleaved)
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_encoder_write(Handle, p, interleaved.Length / channels,
                                                                             (uint)sampleRate, (uint)channels));
        }
    }

    /// <summary>While paused, audio from a device or stream is skipped (not queued), so the recording continues
    /// seamlessly when resumed.</summary>
    public bool IsPaused
    {
        get => Status.IsPaused;
        set => FatzAudioException.ThrowIfFailed(NativeMethods.fa_encoder_set_paused(Handle, value ? 1 : 0));
    }

    /// <summary>The encoder's state, progress and formats.</summary>
    public EncoderStatus Status
    {
        get
        {
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_encoder_get_status(Handle, out FaEncoderStatus s));
            var state = (EncoderState)s.State;
            return new EncoderStatus(state, (ErrorCode)s.Error, s.Paused != 0, s.Frames, TimeConvert.FromNanoseconds(s.Duration),
                                     s.Bytes, s.DroppedFrames,
                                     state == EncoderState.Idle ? null : ToFormat(s.Input),
                                     state == EncoderState.Idle ? null : ToFormat(s.Output));
        }
    }

    private static AudioFormat ToFormat(FaAudioFormat f) =>
        new((SampleFormat)f.SampleFormat, (int)f.SampleRate, (int)f.Channels, f.ChannelMask);

    /// <summary>Waits until the encoder is <see cref="EncoderState.Finished"/> or <see cref="EncoderState.Failed"/>.</summary>
    /// <param name="timeout">How long to wait; <see cref="Timeout.InfiniteTimeSpan"/> for no limit.</param>
    /// <returns>True when it finished or failed, false on timeout.</returns>
    /// <exception cref="ObjectDisposedException">The encoder was closed while waiting.</exception>
    public bool Wait(TimeSpan timeout)
    {
        bool infinite = timeout == Timeout.InfiniteTimeSpan;
        if (!infinite && timeout < TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(timeout), "Use Timeout.InfiniteTimeSpan or a positive time.");
        // Waits in slices so that Close from another thread is not held up by this call.
        long deadline = infinite ? long.MaxValue : Environment.TickCount64 + (long)Math.Ceiling(timeout.TotalMilliseconds);
        while (true)
        {
            long left = deadline - Environment.TickCount64;
            long ms = Math.Clamp(left, 0, WaitSliceMs);
            int rc = NativeMethods.fa_encoder_wait(Handle, ms * 1_000_000);
            if (rc != (int)ErrorCode.Busy)
            {
                FatzAudioException.ThrowIfFailed(rc);
                return true;
            }
            if (left <= WaitSliceMs) return false;
        }
    }

    private const long WaitSliceMs = 50;

    /// <summary>
    /// Stops the source, encodes what is still queued, completes the file and closes it. Calling it again does
    /// nothing.
    /// </summary>
    /// <exception cref="FatzAudioException">The first error the encoder hit (for example <see cref="ErrorCode.Io"/> when
    /// the disk is full); the file may be incomplete.</exception>
    public void Close()
    {
        if (Interlocked.Exchange(ref _closed, 1) != 0) return;
        _handle.Dispose();
        // A call in progress on another thread (Write, Wait) delays the native close until it returns.
        SpinWait.SpinUntil(() => _handle.IsReleased);
        FatzAudioException.ThrowIfFailed(_handle.CloseResult);
    }

    /// <summary>Completes and closes the file, like <see cref="Close"/>, without throwing.</summary>
    public void Dispose()
    {
        if (Interlocked.Exchange(ref _closed, 1) != 0) return;
        _handle.Dispose();
    }

    /// <summary>The options as native memory (the strings are UTF-8 copies, freed by <see cref="Free"/>).</summary>
    private struct NativeEncoderOptions
    {
        public FaEncoderOptions Value;
        private nint _title, _artist, _album, _genre, _date, _comment;

        public NativeEncoderOptions(EncoderOptions? o)
        {
            o ??= new EncoderOptions();
            if (o.SampleRate < 0) throw new ArgumentOutOfRangeException(nameof(o), "SampleRate must be >= 0.");
            if (o.Channels < 0) throw new ArgumentOutOfRangeException(nameof(o), "Channels must be >= 0.");
            if (o.BitsPerSample < 0) throw new ArgumentOutOfRangeException(nameof(o), "BitsPerSample must be >= 0.");
            if (o.BitrateKbps < 0) throw new ArgumentOutOfRangeException(nameof(o), "BitrateKbps must be >= 0.");
            if (o.TrackNumber < 0) throw new ArgumentOutOfRangeException(nameof(o), "TrackNumber must be >= 0.");
            if (o.QueueLength < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(o), "QueueLength must be >= 0.");
            FaEncoderOptions v;
            NativeMethods.fa_encoder_options_init(&v);
            v.Format = (int)o.Format;
            v.SampleRate = (uint)o.SampleRate;
            v.Channels = (uint)o.Channels;
            v.BitsPerSample = (uint)o.BitsPerSample;
            v.BitrateKbps = (uint)o.BitrateKbps;
            v.Quality = o.Quality;
            v.QueueMs = (uint)Math.Min(o.QueueLength.TotalMilliseconds, uint.MaxValue);
            v.TrackNumber = (uint)o.TrackNumber;
            _title = Utf8(o.Title); _artist = Utf8(o.Artist); _album = Utf8(o.Album);
            _genre = Utf8(o.Genre); _date = Utf8(o.Date); _comment = Utf8(o.Comment);
            v.Title = (byte*)_title; v.Artist = (byte*)_artist; v.Album = (byte*)_album;
            v.Genre = (byte*)_genre; v.Date = (byte*)_date; v.Comment = (byte*)_comment;
            Value = v;
        }

        private static nint Utf8(string? s) => s is null ? 0 : Marshal.StringToCoTaskMemUTF8(s);

        public void Free()
        {
            Marshal.FreeCoTaskMem(_title); Marshal.FreeCoTaskMem(_artist); Marshal.FreeCoTaskMem(_album);
            Marshal.FreeCoTaskMem(_genre); Marshal.FreeCoTaskMem(_date); Marshal.FreeCoTaskMem(_comment);
        }
    }

    /// <summary>Bridges a managed <see cref="Stream"/> to the native output callbacks.</summary>
    private sealed class OutputState(Stream stream, bool leaveOpen)
    {
        private static OutputState? From(void* user) =>
            user == null ? null : GCHandle.FromIntPtr((nint)user).Target as OutputState;

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static int Write(void* user, void* data, nuint size)
        {
            try
            {
                var s = From(user);
                if (s is null) return (int)ErrorCode.Io;
                var src = new ReadOnlySpan<byte>(data, checked((int)size));
                s.Out.Write(src);
                return 0;
            }
            catch
            {
                return (int)ErrorCode.Io;
            }
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static int Seek(void* user, long offset)
        {
            try
            {
                var s = From(user);
                if (s is null) return (int)ErrorCode.Io;
                s.Out.Seek(s.Start + offset, SeekOrigin.Begin);
                return 0;
            }
            catch
            {
                return (int)ErrorCode.Io;
            }
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static void Close(void* user)
        {
            try
            {
                var s = From(user);
                if (s is null) return;
                s.Out.Flush();
                if (!s.LeaveOpen) s.Out.Dispose();
            }
            catch
            {
                // never let an exception cross the native boundary
            }
        }

        public Stream Out { get; } = stream;
        public bool LeaveOpen { get; } = leaveOpen;
        /// <summary>Where the encoder's first byte goes: offsets are relative to it.</summary>
        public long Start { get; } = stream.CanSeek ? stream.Position : 0;
    }
}

/// <summary>What <see cref="Lame.Info"/> reports.</summary>
/// <param name="IsLoaded">True when LAME is in use.</param>
/// <param name="Version">LAME's version (for example "3.100"), or null.</param>
/// <param name="Path">Where it was found (a file name if the full path is unknown), or null.</param>
public sealed record LameInfo(bool IsLoaded, string? Version, string? Path);

/// <summary>
/// LAME at runtime, for <see cref="EncodeFormat.Mp3"/>. FatzAudio never links or bundles LAME (it is LGPL): it uses
/// the libmp3lame your app ships (libmp3lame.dll, libmp3lame.0.dylib, libmp3lame.so.0), the one already loaded in
/// the process or found by the system, or the one in the folder given to <see cref="Load"/>. LAME 3.98 and later.
/// </summary>
public static unsafe class Lame
{
    /// <summary>
    /// Loads LAME now. Without a folder it uses the copy already loaded or the one the system finds; the first MP3
    /// encoder does this by itself, so call it only to use a specific folder or to check up front.
    /// </summary>
    /// <param name="directory">Folder with libmp3lame, or null.</param>
    /// <returns>True when LAME is (already) loaded; false when none was found.</returns>
    /// <example>
    /// <code>
    /// if (!Lame.Load(AppContext.BaseDirectory))
    ///     Console.WriteLine("MP3 recording needs libmp3lame next to the app");
    /// </code>
    /// </example>
    public static bool Load(string? directory = null)
    {
        NativeLoader.EnsureInitialized();
        int rc = NativeMethods.fa_lame_load(directory);
        if (rc == (int)ErrorCode.ProviderMissing) return false;
        FatzAudioException.ThrowIfFailed(rc);
        return true;
    }

    /// <summary>Which LAME is in use, if any.</summary>
    public static LameInfo Info
    {
        get
        {
            NativeLoader.EnsureInitialized();
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_lame_get_info(out FaLameInfo n));
            if (n.Loaded == 0) return new LameInfo(false, null, null);
            return new LameInfo(true, Marshal.PtrToStringUTF8((nint)n.Version), Marshal.PtrToStringUTF8((nint)n.Path));
        }
    }
}
