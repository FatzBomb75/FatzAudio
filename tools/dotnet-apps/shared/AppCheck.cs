using System.Text;
using FatzAudio;

namespace FatzAudio.AppChecks;

/// <summary>What the CI app jobs check inside a real app process; the first line to print says how it went.</summary>
internal static class AppCheck
{
    public const string Ok = "FATZAUDIO-APP-OK";
    public const string Fail = "FATZAUDIO-APP-FAIL";

    /// <summary>Opens and fully decodes <paramref name="file"/>, then the same through <paramref name="url"/> if given.</summary>
    public static bool Run(string? file, string? url, Action<string> log)
    {
        try
        {
            log($"binding {AudioEngine.BindingVersion}, native {AudioEngine.NativeVersion}");
            using var engine = AudioEngine.Create();
            if (!string.IsNullOrEmpty(file)) Decode(engine.OpenFile(file, new StreamOptions { DecodeOnly = true }), "file", log);
            if (!string.IsNullOrEmpty(url)) Decode(engine.OpenUrl(url, new StreamOptions { DecodeOnly = true }), "url", log);
            if (string.IsNullOrEmpty(file) && string.IsNullOrEmpty(url)) throw new InvalidOperationException("no file or url given");
            Lyrics(engine, log);
            log(Ok);
            return true;
        }
        catch (Exception ex)
        {
            log($"{ex.GetType().Name}: {ex.Message}");
            log(Fail);
            return false;
        }
    }

    /// <summary>An Arabic lyric line drawn with the system's shaper (Core Text on iOS, TextRunShaper through the
    /// app's Java VM on Android), which the plain-C tests cannot reach on Android.</summary>
    private static void Lyrics(AudioEngine engine, Action<string> log)
    {
        using var font = SoundFont.FromMemory(SilentSoundFont());
        using var song = engine.OpenMemory(ArabicSong(), new StreamOptions { DecodeOnly = true, SoundFont = font });
        using var lyrics = song.CreateLyricSurface(LyricStyle.FromPreset(LyricPreset.Modern));
        lyrics.PresentationSize = new PixelSize(1280, 720);
        var layout = lyrics.GetLayout();
        log($"lyrics: font {layout.FontUsed}, shaper {layout.TextShaper}");
        string want = OperatingSystem.IsAndroid() ? "TextRunShaper" : OperatingSystem.IsIOS() ? "Core Text" : layout.TextShaper;
        if (layout.TextShaper != want) throw new InvalidOperationException($"lyrics: shaper '{layout.TextShaper}', expected '{want}'");
        var buffer = new byte[layout.Width * 4 * layout.Height];
        lyrics.UpdateInto(buffer, layout.Width * 4, layout.Width, layout.Height, 0, TimeSpan.FromSeconds(1.5));
        int drawn = 0;
        for (int i = 3; i < buffer.Length; i += 4) if (buffer[i] > 128) drawn++;
        log($"lyrics: {drawn} pixels drawn");
        if (drawn == 0) throw new InvalidOperationException("lyrics: nothing drawn");
    }

    /// <summary>Two Arabic syllables ("مرحبا بكم", hello) at 1 and 2 s, a note to 3 s.</summary>
    private static byte[] ArabicSong()
    {
        var t = new List<byte>();
        void VarLen(int v)
        {
            var tmp = new Stack<byte>();
            tmp.Push((byte)(v & 0x7F));
            while ((v >>= 7) != 0) tmp.Push((byte)(0x80 | (v & 0x7F)));
            t.AddRange(tmp);
        }
        void Lyric(int delta, string text)
        {
            var b = Encoding.UTF8.GetBytes(text);
            VarLen(delta); t.Add(0xFF); t.Add(0x05); VarLen(b.Length); t.AddRange(b);
        }
        VarLen(0); t.AddRange(new byte[] { 0x90, 69, 1 });
        Lyric(960, "\u0645\u0631\u062D\u0628\u0627 ");
        Lyric(960, "\u0628\u0643\u0645");
        VarLen(960); t.AddRange(new byte[] { 0x80, 69, 0 });
        VarLen(0); t.AddRange(new byte[] { 0xFF, 0x2F, 0 });
        var f = new List<byte>();
        f.AddRange("MThd"u8.ToArray()); f.AddRange(new byte[] { 0, 0, 0, 6, 0, 0, 0, 1, 480 >> 8, 480 & 255 });
        f.AddRange("MTrk"u8.ToArray());
        f.AddRange(new[] { (byte)(t.Count >> 24), (byte)(t.Count >> 16), (byte)(t.Count >> 8), (byte)t.Count });
        f.AddRange(t);
        return f.ToArray();
    }

    /// <summary>A SoundFont 2 with one preset of one silent looped sample: a MIDI file needs one to open.</summary>
    private static byte[] SilentSoundFont()
    {
        static byte[] Build(Action<BinaryWriter> body)
        {
            using var ms = new MemoryStream();
            using (var w = new BinaryWriter(ms)) body(w);
            return ms.ToArray();
        }
        static void Chunk(BinaryWriter w, string id, byte[] data) { w.Write(Encoding.ASCII.GetBytes(id)); w.Write(data.Length); w.Write(data); }
        static void Name(BinaryWriter w, string n, int len) { var b = new byte[len]; Encoding.ASCII.GetBytes(n).CopyTo(b, 0); w.Write(b); }
        const int len = 64;
        var info = Build(w => { w.Write("INFO"u8); Chunk(w, "ifil", Build(c => { c.Write((ushort)2); c.Write((ushort)1); })); });
        var sdta = Build(w => { w.Write("sdta"u8); Chunk(w, "smpl", new byte[(len + 46) * 2]); });
        var pdta = Build(w =>
        {
            w.Write("pdta"u8);
            Chunk(w, "phdr", Build(c =>
            {
                Name(c, "Silence", 20); c.Write((ushort)0); c.Write((ushort)0); c.Write((ushort)0); c.Write(0); c.Write(0); c.Write(0);
                Name(c, "EOP", 20); c.Write((ushort)0); c.Write((ushort)0); c.Write((ushort)1); c.Write(0); c.Write(0); c.Write(0);
            }));
            Chunk(w, "pbag", Build(c => { foreach (var v in new ushort[] { 0, 0, 1, 0 }) c.Write(v); }));
            Chunk(w, "pmod", new byte[10]);
            Chunk(w, "pgen", Build(c => { c.Write((ushort)41); c.Write((ushort)0); c.Write(0); }));
            Chunk(w, "inst", Build(c => { Name(c, "SilenceInst", 20); c.Write((ushort)0); Name(c, "EOI", 20); c.Write((ushort)1); }));
            Chunk(w, "ibag", Build(c => { foreach (var v in new ushort[] { 0, 0, 2, 0 }) c.Write(v); }));
            Chunk(w, "imod", new byte[10]);
            Chunk(w, "igen", Build(c => { c.Write((ushort)54); c.Write((ushort)1); c.Write((ushort)53); c.Write((ushort)0); c.Write(0); }));
            Chunk(w, "shdr", Build(c =>
            {
                Name(c, "Silence", 20); c.Write(0); c.Write(len); c.Write(0); c.Write(len); c.Write(44100);
                c.Write((byte)69); c.Write((byte)0); c.Write((ushort)0); c.Write((ushort)1);
                Name(c, "EOS", 46);
            }));
        });
        var riff = Build(w => { w.Write("sfbk"u8); Chunk(w, "LIST", info); Chunk(w, "LIST", sdta); Chunk(w, "LIST", pdta); });
        return Build(w => Chunk(w, "RIFF", riff));
    }

    private static void Decode(AudioStream stream, string what, Action<string> log)
    {
        using (stream)
        {
            var info = stream.Info;
            var buf = new float[4096 * Math.Max(1, info.Format.Channels)];
            long frames = 0;
            int n;
            while ((n = stream.Read(buf)) > 0) frames += n;
            log($"{what}: {info.Codec}, {info.Format.SampleRate} Hz, {info.Format.Channels} ch, {frames} frames decoded");
            if (frames == 0) throw new InvalidOperationException($"{what}: no audio decoded");
        }
    }
}
