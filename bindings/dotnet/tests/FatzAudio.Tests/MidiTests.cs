using System.Text;
using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>MIDI files with a soundfont built in code (decode-only, no audio hardware needed).</summary>
public class MidiTests
{
    private const int Rate = 44100;
    private static readonly StreamOptions DecodeOnly = new() { DecodeOnly = true };

    private static AudioEngine CreateHeadless() => AudioEngine.Create(new AudioEngineOptions { NullOutput = true, MixSampleRate = Rate });

    // ---- a one-instrument SoundFont: a looped 440 Hz sine on bank 0 and bank 128 ----

    private static void Chunk(BinaryWriter w, string id, byte[] body)
    {
        w.Write(Encoding.ASCII.GetBytes(id));
        w.Write(body.Length);
        w.Write(body);
        if ((body.Length & 1) != 0) w.Write((byte)0);
    }

    private static byte[] Build(Action<BinaryWriter> write)
    {
        using var ms = new MemoryStream();
        using (var w = new BinaryWriter(ms)) write(w);
        return ms.ToArray();
    }

    private static void Name(BinaryWriter w, string s, int n)
    {
        var b = new byte[n];
        Encoding.ASCII.GetBytes(s).CopyTo(b, 0);
        w.Write(b);
    }

    internal static byte[] SineSoundFont()
    {
        const int len = 4410; // 100 periods of 440 Hz
        var info = Build(w =>
        {
            w.Write("INFO"u8);
            Chunk(w, "ifil", Build(c => { c.Write((ushort)2); c.Write((ushort)1); }));
            Chunk(w, "INAM", Build(c => Name(c, "Test Sine", 10)));
        });
        var sdta = Build(w =>
        {
            w.Write("sdta"u8);
            Chunk(w, "smpl", Build(c =>
            {
                for (int i = 0; i < len; i++) c.Write((short)Math.Round(Math.Sin(2 * Math.PI * 440 * i / Rate) * 32000));
                for (int i = 0; i < 46; i++) c.Write((short)0);
            }));
        });
        var pdta = Build(w =>
        {
            w.Write("pdta"u8);
            Chunk(w, "phdr", Build(c =>
            {
                void P(string n, ushort bank, ushort bag) { Name(c, n, 20); c.Write((ushort)0); c.Write(bank); c.Write(bag); c.Write(0); c.Write(0); c.Write(0); }
                P("Sine", 0, 0); P("Kit", 128, 1); P("EOP", 0, 2);
            }));
            Chunk(w, "pbag", Build(c => { foreach (var v in new ushort[] { 0, 0, 1, 0, 2, 0 }) c.Write(v); }));
            Chunk(w, "pmod", new byte[10]);
            Chunk(w, "pgen", Build(c => { c.Write((ushort)41); c.Write((ushort)0); c.Write((ushort)41); c.Write((ushort)1); c.Write(0); }));
            Chunk(w, "inst", Build(c => { Name(c, "SineInst", 20); c.Write((ushort)0); Name(c, "KitInst", 20); c.Write((ushort)1); Name(c, "EOI", 20); c.Write((ushort)2); }));
            Chunk(w, "ibag", Build(c => { foreach (var v in new ushort[] { 0, 0, 3, 0, 6, 0 }) c.Write(v); }));
            Chunk(w, "imod", new byte[10]);
            Chunk(w, "igen", Build(c =>
            {
                for (int k = 0; k < 2; k++)
                {
                    c.Write((ushort)54); c.Write((ushort)1);       // sampleModes: loop
                    c.Write((ushort)38); c.Write((short)-3600);    // releaseVolEnv: 125 ms
                    c.Write((ushort)53); c.Write((ushort)0);       // sampleID
                }
                c.Write(0);
            }));
            Chunk(w, "shdr", Build(c =>
            {
                Name(c, "Sine", 20); c.Write(0); c.Write(len); c.Write(0); c.Write(len); c.Write(Rate);
                c.Write((byte)69); c.Write((byte)0); c.Write((ushort)0); c.Write((ushort)1);
                Name(c, "EOS", 46);
            }));
        });
        var riff = Build(w =>
        {
            w.Write("sfbk"u8);
            Chunk(w, "LIST", info);
            Chunk(w, "LIST", sdta);
            Chunk(w, "LIST", pdta);
        });
        return Build(w => Chunk(w, "RIFF", riff));
    }

    // ---- a format 0 song: note 69 for 0.5 s, silence, note 69 from 1.5 s to 2 s; three lyrics ----

    private static void VarLen(List<byte> t, int v)
    {
        var tmp = new Stack<byte>();
        tmp.Push((byte)(v & 0x7F));
        while ((v >>= 7) != 0) tmp.Push((byte)(0x80 | (v & 0x7F)));
        t.AddRange(tmp);
    }

    private static void Meta(List<byte> t, int delta, byte type, string text)
    {
        var b = Encoding.ASCII.GetBytes(text);
        VarLen(t, delta); t.Add(0xFF); t.Add(type); VarLen(t, b.Length); t.AddRange(b);
    }

    private static void Ev(List<byte> t, int delta, params byte[] e) { VarLen(t, delta); t.AddRange(e); }

    internal static byte[] Song()
    {
        var t = new List<byte>();
        Meta(t, 0, 0x03, "Test Song");
        Ev(t, 0, 0x90, 69, 127);
        Meta(t, 0, 0x05, "Hel");
        Meta(t, 240, 0x05, "lo ");
        Ev(t, 240, 0x80, 69, 0);
        Meta(t, 960, 0x05, "\rworld");
        Ev(t, 0, 0x90, 69, 100);
        Ev(t, 480, 0x80, 69, 0);
        Ev(t, 0, 0xFF, 0x2F, 0);
        var f = new List<byte>();
        f.AddRange("MThd"u8.ToArray()); f.AddRange(new byte[] { 0, 0, 0, 6, 0, 0, 0, 1, 480 >> 8, 480 & 255 });
        f.AddRange("MTrk"u8.ToArray());
        f.AddRange(new[] { (byte)(t.Count >> 24), (byte)(t.Count >> 16), (byte)(t.Count >> 8), (byte)t.Count });
        f.AddRange(t);
        return f.ToArray();
    }

    private static float[] ReadAll(AudioStream s, long frames)
    {
        var all = new float[frames * 2];
        int at = 0, n;
        while (at < all.Length && (n = s.Read(all.AsSpan(at, Math.Min(4096, all.Length - at)))) > 0) at += n * 2;
        return all;
    }

    private static double Rms(float[] x, int from, int to)
    {
        double sum = 0;
        for (int i = from * 2; i < to * 2; i++) sum += x[i] * (double)x[i];
        return Math.Sqrt(sum / ((to - from) * 2));
    }

    [Fact]
    public void Song_PlaysWithTheEngineSoundFont()
    {
        using var engine = CreateHeadless();
        using (var font = SoundFont.FromMemory(SineSoundFont()))
        {
            Assert.Equal("Test Sine", font.Info.Name);
            Assert.Equal(2, font.Info.PresetCount);
            engine.DefaultSoundFont = font;
        }
        // The engine keeps its own reference after the SoundFont object was disposed.
        using var s = engine.OpenMemory(Song(), DecodeOnly);
        var info = s.Info;
        Assert.Equal("midi", info.Codec);
        Assert.Equal(Rate, info.Format.SampleRate);
        Assert.Equal(2, info.Format.Channels);
        Assert.Equal(3L * Rate, info.LengthFrames); // last event at 2 s, plus 1 s to ring out
        var all = ReadAll(s, 3L * Rate);
        Assert.True(Rms(all, 1000, 20000) > 0.1);
        Assert.True(Rms(all, 30000, 65000) < 1e-6);

        // Seeking is exact: the second note, read again, is the same audio.
        s.PositionFrames = 66150;
        var again = ReadAll(s, 3L * Rate - 66150);
        Assert.True(all.AsSpan(66150 * 2).SequenceEqual(again));
    }

    [Fact]
    public void Lyrics_AndInfo()
    {
        using var engine = CreateHeadless();
        using var font = SoundFont.FromMemory(SineSoundFont());
        using var s = engine.OpenMemory(Song(), DecodeOnly with { SoundFont = font });
        var mi = s.MidiInfo;
        Assert.NotNull(mi);
        Assert.Equal("Test Song", mi!.Title);
        Assert.Equal(0, mi.Format);
        Assert.Equal(3, mi.LyricCount);
        var lyrics = s.MidiTexts.Where(t => t.Type == MidiTextType.Lyric).ToList();
        Assert.Equal(new[] { "Hel", "lo ", "world" }, lyrics.Select(l => l.Text));
        Assert.Equal(MidiTextFlags.NewLine, lyrics[2].Flags);
        Assert.Equal(66150, lyrics[2].Frame);
        Assert.Equal(TimeSpan.FromSeconds(1.5), lyrics[2].Position);
    }

    [Fact]
    public void LiveControls()
    {
        using var engine = CreateHeadless();
        using var font = SoundFont.FromMemory(SineSoundFont());
        using var s = engine.OpenMemory(Song(), DecodeOnly with { SoundFont = font });
        s.MidiTranspose = -12;
        Assert.Equal(-12, s.MidiTranspose);
        Assert.Throws<ArgumentOutOfRangeException>(() => s.MidiTranspose = 49);
        s.SetMidiChannelMuted(0, true);
        Assert.True(s.IsMidiChannelMuted(0));
        Assert.False(s.IsMidiChannelMuted(1));
        var all = ReadAll(s, Rate);
        Assert.True(Rms(all, 0, Rate) < 1e-6);
        Assert.Throws<ArgumentOutOfRangeException>(() => s.SetMidiChannelMuted(16, true));
    }

    [Fact]
    public void NoSoundFont_Throws()
    {
        using var engine = AudioEngine.Create(new AudioEngineOptions { NullOutput = true, MixSampleRate = Rate, NoSystemSoundFont = true });
        var ex = Assert.Throws<FatzAudioException>(() => engine.OpenMemory(Song(), DecodeOnly));
        Assert.Equal(ErrorCode.NoSoundFont, ex.ErrorCode);
    }

    [Fact]
    public void SystemSoundFont_IsTheFallback()
    {
        // Whatever this machine has: CI images usually have no OS bank.
        using var system = SoundFont.FromSystem();
        using var engine = CreateHeadless();
        if (system is null)
        {
            var ex = Assert.Throws<FatzAudioException>(() => engine.OpenMemory(Song(), DecodeOnly));
            Assert.Equal(ErrorCode.NoSoundFont, ex.ErrorCode);
            return;
        }
        Assert.True(system.Info.PresetCount > 0);
        using var s = engine.OpenMemory(Song(), DecodeOnly);
        Assert.Equal("midi", s.Info.Codec);
    }

    // ---- a DLS bank: one instrument, one region, the same looped 440 Hz sine ----

    private static byte[] List(string type, byte[] body) => Build(w => Chunk(w, "LIST", Build(b => { b.Write(Encoding.ASCII.GetBytes(type)); b.Write(body); })));

    private static byte[] SineDls()
    {
        var wave = List("wave", Build(w =>
        {
            Chunk(w, "fmt ", Build(b => { b.Write((short)1); b.Write((short)1); b.Write(Rate); b.Write(Rate * 2); b.Write((short)2); b.Write((short)16); }));
            // wsmp: unity note 69, a forward loop over the whole wave.
            Chunk(w, "wsmp", Build(b => { b.Write(20); b.Write((short)69); b.Write((short)0); b.Write(0); b.Write(0); b.Write(1); b.Write(16); b.Write(0); b.Write(0); b.Write(4410); }));
            Chunk(w, "data", Build(b => { for (int i = 0; i < 4410; i++) b.Write((short)Math.Round(Math.Sin(2 * Math.PI * 440 * i / Rate) * 32000)); }));
        }));
        var region = List("rgn ", Build(w =>
        {
            Chunk(w, "rgnh", Build(b => { b.Write((short)0); b.Write((short)127); b.Write((short)0); b.Write((short)127); b.Write((short)0); b.Write((short)0); }));
            Chunk(w, "wlnk", Build(b => { b.Write((short)0); b.Write((short)0); b.Write(1); b.Write(0); }));
        }));
        var ins = List("ins ", Build(w =>
        {
            Chunk(w, "insh", Build(b => { b.Write(1); b.Write(0); b.Write(0); }));
            w.Write(List("lrgn", region));
        }));
        var riff = Build(w =>
        {
            w.Write(Encoding.ASCII.GetBytes("DLS "));
            Chunk(w, "colh", BitConverter.GetBytes(1));
            w.Write(List("lins", ins));
            Chunk(w, "ptbl", Build(b => { b.Write(8); b.Write(1); b.Write(0); }));
            w.Write(List("wvpl", wave));
            w.Write(List("INFO", Build(b => Chunk(b, "INAM", Encoding.ASCII.GetBytes("Test DLS\0")))));
        });
        return Build(w => Chunk(w, "RIFF", riff));
    }

    [Fact]
    public void DlsBank_Plays()
    {
        using var engine = CreateHeadless();
        using var font = SoundFont.FromMemory(SineDls());
        Assert.Equal("Test DLS", font.Info.Name);
        Assert.Equal(1, font.Info.PresetCount);
        using var s = engine.OpenMemory(Song(), DecodeOnly with { SoundFont = font });
        var all = ReadAll(s, 3L * Rate);
        Assert.True(Rms(all, 1000, 20000) > 0.1);
        Assert.True(Rms(all, 30000, 65000) < 1e-3);
    }

    [Fact]
    public void NotMidi_HasNoMidiInfo()
    {
        using var engine = CreateHeadless();
        using var s = engine.OpenMemory(TestAudio.SineWav(), DecodeOnly);
        Assert.Null(s.MidiInfo);
        Assert.Empty(s.MidiTexts);
        var ex = Assert.Throws<FatzAudioException>(() => s.MidiTranspose = 2);
        Assert.Equal(ErrorCode.InvalidArgument, ex.ErrorCode);
    }

    [Fact]
    public void BadSoundFont_Throws()
    {
        var ex = Assert.Throws<FatzAudioException>(() => SoundFont.FromMemory(Encoding.ASCII.GetBytes("RIFF....sfbkLIST")));
        Assert.Equal(ErrorCode.UnsupportedFormat, ex.ErrorCode);
        var missing = Assert.Throws<FatzAudioException>(() => SoundFont.FromFile("/nonexistent/none.sf2"));
        Assert.Equal(ErrorCode.NotFound, missing.ErrorCode);
    }
}
