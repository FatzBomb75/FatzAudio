using System.Buffers;
using System.Runtime.InteropServices;
using FatzAudio.Native;

namespace FatzAudio;

/// <summary>Kind of a <see cref="MidiEvent"/>.</summary>
public enum MidiEventType
{
    /// <summary>A note ends: <see cref="MidiEvent.Param"/> is the key.</summary>
    NoteOff = 0,
    /// <summary>A note starts: <see cref="MidiEvent.Param"/> is the key, <see cref="MidiEvent.Value"/> the velocity 1..127 (0 ends it).</summary>
    NoteOn = 1,
    /// <summary>Poly aftertouch: <see cref="MidiEvent.Param"/> is the key, <see cref="MidiEvent.Value"/> 0..127.</summary>
    KeyPressure = 2,
    /// <summary>A controller: <see cref="MidiEvent.Param"/> is its number 0..127, <see cref="MidiEvent.Value"/> 0..127.</summary>
    Control = 3,
    /// <summary>A program change: <see cref="MidiEvent.Value"/> is the program 0..127 (bank: controllers 0 and 32 first).</summary>
    Program = 4,
    /// <summary>Channel aftertouch: <see cref="MidiEvent.Value"/> 0..127.</summary>
    ChannelPressure = 5,
    /// <summary>Pitch bend: <see cref="MidiEvent.Value"/> 0..16383, 8192 is the centre.</summary>
    PitchBend = 6,
    /// <summary>A system exclusive message: <see cref="MidiEvent.Data"/>, from its F0 to its F7.</summary>
    SysEx = 7,
}

/// <summary>
/// One MIDI event: sent to a MIDI stream (<see cref="AudioStream.SendMidi(in MidiEvent)"/>), given to
/// <see cref="AudioEngine.OpenMidiEvents"/>, or read from a file (<see cref="AudioStream.GetMidiEvents"/>).
/// </summary>
/// <param name="Type">What it is.</param>
/// <param name="Channel">The channel 0..15 (MIDI channel 1..16); ignored for SysEx.</param>
/// <param name="Param">The key or controller number.</param>
/// <param name="Value">The velocity, controller value, program, pressure or bend.</param>
/// <param name="Frame">Where it is heard, in the stream's frames; <see cref="Now"/> = at once.</param>
public readonly record struct MidiEvent(MidiEventType Type, int Channel, int Param, int Value, long Frame = MidiEvent.Now)
{
    /// <summary>The frame of an event to play at once: at the next frames the synth renders.</summary>
    public const long Now = -1;

    /// <summary>Read from a file: the track (MTrk chunk), 0-based. For <see cref="AudioEngine.OpenMidiEvents"/>: the
    /// track whose volume applies (<see cref="AudioStream.SetMidiTrackVolume"/>). Ignored when sent.</summary>
    public int Track { get; init; }

    /// <summary>Read from a file: where it is heard. Ignored when sent.</summary>
    public TimeSpan Position { get; init; }

    /// <summary>SysEx: the message from its F0 to its F7.</summary>
    public ReadOnlyMemory<byte> Data { get; init; }

    /// <summary>A note on (velocity 0 is a note off).</summary>
    public static MidiEvent NoteOn(int channel, int key, int velocity, long frame = Now) => new(MidiEventType.NoteOn, channel, key, velocity, frame);

    /// <summary>A note off.</summary>
    public static MidiEvent NoteOff(int channel, int key, long frame = Now) => new(MidiEventType.NoteOff, channel, key, 0, frame);

    /// <summary>A controller change (7 volume, 10 pan, 64 sustain pedal, 91 reverb, 93 chorus, ...).</summary>
    public static MidiEvent Control(int channel, int controller, int value, long frame = Now) => new(MidiEventType.Control, channel, controller, value, frame);

    /// <summary>A program change.</summary>
    public static MidiEvent Program(int channel, int program, long frame = Now) => new(MidiEventType.Program, channel, 0, program, frame);

    /// <summary>Pitch bend, 0..16383 with 8192 the centre.</summary>
    public static MidiEvent PitchBend(int channel, int value, long frame = Now) => new(MidiEventType.PitchBend, channel, 0, value, frame);

    /// <summary>Channel aftertouch.</summary>
    public static MidiEvent ChannelPressure(int channel, int value, long frame = Now) => new(MidiEventType.ChannelPressure, channel, 0, value, frame);

    /// <summary>Poly aftertouch on one key.</summary>
    public static MidiEvent KeyPressure(int channel, int key, int value, long frame = Now) => new(MidiEventType.KeyPressure, channel, key, value, frame);

    /// <summary>A system exclusive message (GS or XG settings, a reset), from its F0 to its F7.</summary>
    public static MidiEvent SysEx(ReadOnlyMemory<byte> message, long frame = Now) => new(MidiEventType.SysEx, 0, 0, 0, frame) { Data = message };
}

/// <summary>Settings of a real-time MIDI stream (<see cref="AudioEngine.OpenMidi"/>) or of one made from events
/// (<see cref="AudioEngine.OpenMidiEvents"/>).</summary>
public sealed class MidiStreamOptions
{
    /// <summary>The rate the synth renders at, 8000..192000; 0 = as MIDI files (the engine's mix rate, else 44100).
    /// Event frames count at this rate.</summary>
    public int SampleRate { get; init; }

    /// <summary>Real-time streams: how far ahead of the output the synth renders, 10..500 ms; null = 30 ms. Lower is
    /// heard sooner but needs a less busy machine.</summary>
    public TimeSpan? Latency { get; init; }

    internal FaMidiStreamOptions ToNative()
    {
        FaMidiStreamOptions o;
        unsafe { NativeMethods.fa_midi_stream_options_init(&o); }
        o.SampleRate = (uint)Math.Max(SampleRate, 0);
        if (Latency is { } l) o.LatencyMs = (uint)Math.Clamp(Math.Round(l.TotalMilliseconds), 1, 100000);
        return o;
    }
}

/// <summary>Native copies of events for one call: SysEx data pinned.</summary>
internal unsafe ref struct NativeMidiEvents
{
    private FaMidiEvent* _events;
    private MemoryHandle[]? _pins;

    public readonly FaMidiEvent* Pointer => _events;

    public static NativeMidiEvents Create(ReadOnlySpan<MidiEvent> events, bool withTracks)
    {
        var n = new NativeMidiEvents();
        if (events.IsEmpty) return n;
        n._events = (FaMidiEvent*)NativeMemory.AllocZeroed((nuint)events.Length, (nuint)sizeof(FaMidiEvent));
        try
        {
            for (int i = 0; i < events.Length; i++)
            {
                var e = events[i];
                var o = &n._events[i];
                o->Frame = e.Frame;
                o->Type = (int)e.Type;
                o->Channel = e.Channel;
                o->Param = e.Param;
                o->Value = e.Value;
                o->Track = withTracks ? e.Track : 0;
                if (e.Type == MidiEventType.SysEx && !e.Data.IsEmpty)
                {
                    n._pins ??= new MemoryHandle[events.Length];
                    n._pins[i] = e.Data.Pin();
                    o->Data = (byte*)n._pins[i].Pointer;
                    o->Size = (uint)e.Data.Length;
                }
            }
        }
        catch
        {
            n.Dispose();   // the native copy and the pins taken so far
            throw;
        }
        return n;
    }

    public void Dispose()
    {
        if (_pins is not null) foreach (var p in _pins) p.Dispose();
        _pins = null;
        if (_events != null) NativeMemory.Free(_events);
        _events = null;
    }
}

public sealed partial class AudioEngine
{
    /// <summary>
    /// Opens a real-time MIDI stream (BASS_MIDI_StreamCreate): a synthesizer with no file that plays the events you
    /// send it (<see cref="AudioStream.SendMidi(in MidiEvent)"/>) — a keyboard, your own sequencer, a game's music. It
    /// is silent until then, never ends and cannot seek. It plays with <see cref="StreamOptions.SoundFont"/>, else
    /// <see cref="DefaultSoundFont"/>, else the system's instruments. It renders only <see cref="MidiStreamOptions.Latency"/>
    /// (30 ms) ahead of the output, so a note sent <see cref="MidiEvent.Now"/> is heard about that soon.
    /// </summary>
    /// <param name="midi">The rate and latency; null for the defaults.</param>
    /// <param name="options">Optional open options (<see cref="StreamOptions.Loop"/> is not allowed).</param>
    /// <returns>The stream; dispose it when done.</returns>
    /// <exception cref="FatzAudioException"><see cref="ErrorCode.NoSoundFont"/> without instruments;
    /// <see cref="ErrorCode.InvalidArgument"/> for a rate or latency out of range or <see cref="StreamOptions.Loop"/>.</exception>
    /// <example>
    /// <code>
    /// using var synth = engine.OpenMidi();
    /// synth.PlayOn(output);
    /// synth.SendMidi(MidiEvent.Program(0, 0));          // piano
    /// synth.SendMidi(MidiEvent.NoteOn(0, 60, 100));     // middle C, now
    /// // ... on key up:
    /// synth.SendMidi(MidiEvent.NoteOff(0, 60));
    /// </code>
    /// </example>
    public unsafe AudioStream OpenMidi(MidiStreamOptions? midi = null, StreamOptions? options = null)
    {
        var h = Handle;
        using var opts = NativeOptions.Create(options);
        var mo = (midi ?? new MidiStreamOptions()).ToNative();
        return AudioStream.Wrap(this, NativeMethods.fa_stream_open_midi(h, &mo, opts.Pointer, out nint s), s, null);
    }

    /// <summary>
    /// Opens a MIDI stream that plays a list of events as a MIDI file would (BASS_MIDI_StreamCreateEvents): seekable,
    /// its length the last event plus one second. Frames count at <see cref="MidiStreamOptions.SampleRate"/> (set it to
    /// know them; <see cref="MidiEvent.Now"/> is frame 0). The events are copied and sorted by frame (equal frames keep
    /// their order); <see cref="MidiEvent.Track"/> picks the track for <see cref="AudioStream.SetMidiTrackVolume"/>.
    /// </summary>
    /// <param name="events">The events.</param>
    /// <param name="midi">The rate; null for the defaults.</param>
    /// <param name="options">Optional open options.</param>
    /// <returns>The stream; dispose it when done.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="events"/> is null.</exception>
    /// <exception cref="FatzAudioException"><see cref="ErrorCode.InvalidArgument"/> for an event out of range;
    /// <see cref="ErrorCode.NoSoundFont"/> without instruments.</exception>
    public unsafe AudioStream OpenMidiEvents(IReadOnlyList<MidiEvent> events, MidiStreamOptions? midi = null, StreamOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(events);
        var h = Handle;
        var list = events as MidiEvent[] ?? [.. events];
        using var opts = NativeOptions.Create(options);
        using var native = NativeMidiEvents.Create(list, withTracks: true);
        var mo = (midi ?? new MidiStreamOptions()).ToNative();
        return AudioStream.Wrap(this, NativeMethods.fa_stream_open_midi_events(h, native.Pointer, (uint)list.Length, &mo, opts.Pointer, out nint s), s, null);
    }
}

public unsafe partial class AudioStream
{
    /// <summary>
    /// MIDI streams: sends one event (BASS_MIDI_StreamEvent). It is heard at its <see cref="MidiEvent.Frame"/>, to the
    /// sample, or at once for <see cref="MidiEvent.Now"/> and frames already rendered. Works on real-time streams
    /// (<see cref="AudioEngine.OpenMidi"/>) and on MIDI files, on top of what the file plays. Any thread may send; it
    /// never waits for the audio.
    /// </summary>
    /// <returns>True if sent; false if the queue is full (<c>FA_MIDI_QUEUE</c>, 8192 events the synth has not taken
    /// yet, as when the stream is not playing).</returns>
    /// <exception cref="FatzAudioException">Not a MIDI stream, or the event is out of range
    /// (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public bool SendMidi(in MidiEvent midiEvent) => SendMidi(new ReadOnlySpan<MidiEvent>(in midiEvent));

    /// <summary>
    /// MIDI streams: sends events, all or none (BASS_MIDI_StreamEvents); see <see cref="SendMidi(in MidiEvent)"/>.
    /// </summary>
    /// <returns>True if sent; false if the queue has no room for them all (nothing was sent).</returns>
    /// <exception cref="FatzAudioException">Not a MIDI stream, more than 8192 events, or an event out of range
    /// (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public bool SendMidi(ReadOnlySpan<MidiEvent> events)
    {
        using var native = NativeMidiEvents.Create(events, withTracks: false);
        int r = NativeMethods.fa_stream_send_midi_events(Handle, native.Pointer, (uint)events.Length);
        if (r == (int)ErrorCode.Busy) return false;
        FatzAudioException.ThrowIfFailed(r);
        return true;
    }

    /// <summary>
    /// MIDI streams: sends MIDI bytes as a device or a file's track gives them (BASS_MIDI_EVENTS_RAW): status bytes
    /// with running status, SysEx from F0 to F7. Real-time and system common bytes are skipped, and so is an incomplete
    /// message at the end. All play at <paramref name="frame"/>.
    /// </summary>
    /// <returns>True if sent; false if the queue is full (nothing was sent).</returns>
    /// <exception cref="FatzAudioException">Not a MIDI stream (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public bool SendMidiBytes(ReadOnlySpan<byte> bytes, long frame = MidiEvent.Now)
    {
        int r;
        fixed (byte* p = bytes) r = NativeMethods.fa_stream_send_midi(Handle, frame, p, (nuint)bytes.Length);
        if (r == (int)ErrorCode.Busy) return false;
        FatzAudioException.ThrowIfFailed(r);
        return true;
    }

    /// <summary>
    /// MIDI files: the note, controller, program, pressure, pitch bend and SysEx events, in time order
    /// (BASS_MIDI_StreamGetEvents). A note on with velocity 0 comes as a note off. Texts are <see cref="MidiTexts"/>,
    /// tempo and signatures <see cref="MidiMarks"/>. Empty for other streams and for real-time ones.
    /// </summary>
    /// <param name="track">One track (0-based), or -1 for all.</param>
    /// <param name="type">One kind of event, or null for all.</param>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    /// <example>
    /// <code>
    /// var programs = song.GetMidiEvents(type: MidiEventType.Program);
    /// foreach (var p in programs) Console.WriteLine($"{p.Position}: channel {p.Channel + 1} program {p.Value}");
    /// </code>
    /// </example>
    public IReadOnlyList<MidiEvent> GetMidiEvents(int track = -1, MidiEventType? type = null)
    {
        uint count = 0;
        int t = type is { } k ? (int)k : -1;
        if (track < -1) throw new ArgumentOutOfRangeException(nameof(track));
        if (NativeMethods.fa_stream_get_midi_events(Handle, track, t, null, 0, &count) != 0 || count == 0) return [];
        var native = new FaMidiEvent[count];
        fixed (FaMidiEvent* p = native)
            FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_midi_events(Handle, track, t, p, count, &count));
        var list = new List<MidiEvent>(native.Length);
        foreach (var n in native)
        {
            var e = new MidiEvent((MidiEventType)n.Type, n.Channel, n.Param, n.Value, n.Frame)
            {
                Track = n.Track,
                Position = TimeConvert.FromNanoseconds(n.Time),
                Data = n.Data != null && n.Size > 0 ? new ReadOnlySpan<byte>(n.Data, (int)n.Size).ToArray() : ReadOnlyMemory<byte>.Empty,
            };
            list.Add(e);
        }
        return list;
    }

    /// <summary>
    /// MIDI streams: a channel's current value as the synthesizer has it (BASS_MIDI_StreamGetEvent): a controller
    /// (<see cref="MidiEventType.Control"/> with its number), the <see cref="MidiEventType.Program"/>, the
    /// <see cref="MidiEventType.PitchBend"/> or the <see cref="MidiEventType.ChannelPressure"/>. It is where the synth
    /// has rendered to, up to the decode-ahead past what is heard.
    /// </summary>
    /// <exception cref="FatzAudioException">Not a MIDI stream, another type, or a channel or controller out of range
    /// (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public int GetMidiValue(int channel, MidiEventType type, int controller = 0)
    {
        if (channel is < 0 or > 15) throw new ArgumentOutOfRangeException(nameof(channel));
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_midi_value(Handle, (uint)channel, (int)type, controller, out int v));
        return v;
    }

    /// <summary>
    /// MIDI streams: a track's volume (BASS_ATTRIB_MIDI_TRACK_VOL), 0 (silent) to 4, 1 by default. It scales the level
    /// of the track's notes from their next note on (held notes keep theirs), whatever channel they are on.
    /// </summary>
    /// <exception cref="ArgumentOutOfRangeException">The volume is outside 0..4.</exception>
    /// <exception cref="FatzAudioException">Not a MIDI stream, or no such track (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public void SetMidiTrackVolume(int track, float volume)
    {
        if (track < 0) throw new ArgumentOutOfRangeException(nameof(track));
        if (!(volume is >= 0f and <= 4f)) throw new ArgumentOutOfRangeException(nameof(volume));
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_set_midi_track_volume(Handle, (uint)track, volume));
    }

    /// <summary>MIDI streams: a track's volume (<see cref="SetMidiTrackVolume"/>).</summary>
    /// <exception cref="FatzAudioException">Not a MIDI stream, or no such track (<see cref="ErrorCode.InvalidArgument"/>).</exception>
    /// <exception cref="ObjectDisposedException">The stream was disposed.</exception>
    public float GetMidiTrackVolume(int track)
    {
        if (track < 0) throw new ArgumentOutOfRangeException(nameof(track));
        FatzAudioException.ThrowIfFailed(NativeMethods.fa_stream_get_midi_track_volume(Handle, (uint)track, out float v));
        return v;
    }
}
