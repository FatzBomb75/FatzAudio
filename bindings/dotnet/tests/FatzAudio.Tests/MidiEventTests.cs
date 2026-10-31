using System.Runtime.InteropServices;
using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>Real-time MIDI streams, sent events, event lists, a file's events, values and track volume
/// (decode-only, the sine soundfont of <see cref="MidiTests"/>).</summary>
public class MidiEventTests
{
    private const int Rate = 44100;

    private static AudioEngine CreateHeadless() => AudioEngine.Create(new AudioEngineOptions { NullOutput = true, MixSampleRate = Rate });

    private static float[] ReadAll(AudioStream s, long frames, int piece = 4096)
    {
        var all = new float[frames * 2];
        int at = 0, n;
        while (at < all.Length && (n = s.Read(all.AsSpan(at, Math.Min(piece * 2, all.Length - at)))) > 0) at += n * 2;
        return all;
    }

    private static int FirstSound(float[] x, int from = 0)
    {
        for (int i = from; i < x.Length / 2; i++) if (Math.Abs(x[i * 2]) > 1e-6f) return i;
        return -1;
    }

    private static double Rms(float[] x, int from, int to)
    {
        double sum = 0;
        for (int i = from * 2; i < to * 2; i++) sum += x[i] * (double)x[i];
        return Math.Sqrt(sum / ((to - from) * 2));
    }

    private static AudioStream OpenSynth(AudioEngine engine, SoundFont font)
    {
        var s = engine.OpenMidi(new MidiStreamOptions { SampleRate = Rate }, new StreamOptions { DecodeOnly = true, SoundFont = font });
        s.MidiEffects = false;
        return s;
    }

    [Fact]
    public void NativeStructs_MatchTheCLayout()
    {
        Assert.Equal(48, Marshal.SizeOf<Native.FaMidiEvent>());
        Assert.Equal(40, (int)Marshal.OffsetOf<Native.FaMidiEvent>(nameof(Native.FaMidiEvent.Data)));
        Assert.Equal(12, Marshal.SizeOf<Native.FaMidiStreamOptions>());
    }

    [Fact]
    public void RealTime_NotesStartAtTheirFrames()
    {
        using var engine = CreateHeadless();
        using var font = SoundFont.FromMemory(MidiTests.SineSoundFont());
        using var s = OpenSynth(engine, font);
        Assert.Equal(-1, s.Info.LengthFrames);
        Assert.True(s.SendMidi([MidiEvent.NoteOn(0, 69, 127, 1000), MidiEvent.NoteOff(0, 69, 3000)]));
        var x = ReadAll(s, 20000, 333);
        Assert.Equal(1001, FirstSound(x));   // the sine starts at phase 0
        Assert.True(Rms(x, 15000, 20000) < 1e-6);
        // Now: at the next frames rendered.
        Assert.True(s.SendMidi(MidiEvent.NoteOn(0, 69, 127)));
        var y = ReadAll(s, 1000);
        Assert.InRange(FirstSound(y), 0, 66);
        Assert.Throws<FatzAudioException>(() => s.SendMidi(MidiEvent.NoteOn(16, 60, 100)));
        Assert.Throws<FatzAudioException>(() => s.PositionFrames = 1000);
    }

    [Fact]
    public void Bytes_PlayAsTheSameEvents()
    {
        using var engine = CreateHeadless();
        using var font = SoundFont.FromMemory(MidiTests.SineSoundFont());
        using var a = OpenSynth(engine, font);
        using var b = OpenSynth(engine, font);
        byte[] gm = [0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7];
        Assert.True(a.SendMidiBytes([.. gm, 0xB2, 7, 80, 10, 30, 0x92, 69, 127], 500));
        Assert.True(b.SendMidi([MidiEvent.SysEx(gm, 500), MidiEvent.Control(2, 7, 80, 500), MidiEvent.Control(2, 10, 30, 500), MidiEvent.NoteOn(2, 69, 127, 500)]));
        var x = ReadAll(a, 10000);
        var y = ReadAll(b, 10000);
        Assert.Equal(501, FirstSound(x));
        Assert.Equal(x, y);
        Assert.Equal(80, a.GetMidiValue(2, MidiEventType.Control, 7));
        Assert.Equal(30, a.GetMidiValue(2, MidiEventType.Control, 10));
        Assert.Equal(8192, a.GetMidiValue(2, MidiEventType.PitchBend));
        Assert.True(a.SendMidi([MidiEvent.Program(2, 9), MidiEvent.PitchBend(2, 100)]));
        ReadAll(a, 128);
        Assert.Equal(9, a.GetMidiValue(2, MidiEventType.Program));
        Assert.Equal(100, a.GetMidiValue(2, MidiEventType.PitchBend));
    }

    [Fact]
    public void Queue_FullReturnsFalse()
    {
        using var engine = CreateHeadless();
        using var font = SoundFont.FromMemory(MidiTests.SineSoundFont());
        using var s = OpenSynth(engine, font);
        var many = Enumerable.Range(0, 8192).Select(i => MidiEvent.Control(0, 1, i & 127, 1_000_000)).ToArray();
        Assert.True(s.SendMidi(many));
        Assert.False(s.SendMidi(MidiEvent.Control(0, 1, 0)));
        ReadAll(s, 64);
        Assert.True(s.SendMidi(MidiEvent.Control(0, 1, 0)));
    }

    [Fact]
    public void FileEvents_ListAndReplayAsAList()
    {
        using var engine = CreateHeadless();
        using var font = SoundFont.FromMemory(MidiTests.SineSoundFont());
        using var file = engine.OpenMemory(MidiTests.Song(), new StreamOptions { DecodeOnly = true, SoundFont = font });
        file.MidiEffects = false;
        var events = file.GetMidiEvents();
        Assert.Equal(4, events.Count);
        Assert.Equal(MidiEventType.NoteOn, events[0].Type);
        Assert.Equal(69, events[0].Param);
        Assert.Equal(127, events[0].Value);
        Assert.Equal(MidiEventType.NoteOff, events[1].Type);
        Assert.Equal(TimeSpan.FromSeconds(0.5), events[1].Position);
        Assert.Equal(Rate / 2, events[1].Frame);
        Assert.Equal(2, file.GetMidiEvents(type: MidiEventType.NoteOff).Count);
        Assert.Empty(file.GetMidiEvents(track: 1));

        using var list = engine.OpenMidiEvents(events, new MidiStreamOptions { SampleRate = Rate }, new StreamOptions { DecodeOnly = true, SoundFont = font });
        list.MidiEffects = false;
        Assert.Equal(events[3].Frame + Rate, list.Info.LengthFrames);
        Assert.Equal(ReadAll(file, 2 * Rate), ReadAll(list, 2 * Rate));
    }

    [Fact]
    public void TrackVolume_ScalesItsNotes()
    {
        using var engine = CreateHeadless();
        using var font = SoundFont.FromMemory(MidiTests.SineSoundFont());
        var opts = new StreamOptions { DecodeOnly = true, SoundFont = font };
        using var a = engine.OpenMemory(MidiTests.Song(), opts);
        using var b = engine.OpenMemory(MidiTests.Song(), opts);
        a.MidiEffects = b.MidiEffects = false;
        Assert.Equal(1f, b.GetMidiTrackVolume(0));
        b.SetMidiTrackVolume(0, 0.5f);
        Assert.Equal(0.5f, b.GetMidiTrackVolume(0));
        Assert.Throws<ArgumentOutOfRangeException>(() => b.SetMidiTrackVolume(0, 5f));
        Assert.Throws<FatzAudioException>(() => b.SetMidiTrackVolume(1, 0.5f));
        var x = ReadAll(a, Rate / 2);
        var y = ReadAll(b, Rate / 2);
        Assert.Equal(-6.02, 20 * Math.Log10(Rms(y, 1000, 20000) / Rms(x, 1000, 20000)), 2);
    }
}
