using System.Net;
using System.Net.Sockets;
using System.Text;
using FatzAudio;
using Xunit;

namespace FatzAudio.Tests;

/// <summary>Web streams against a small HTTP server on 127.0.0.1 inside the test.</summary>
public sealed class WebStreamTests : IDisposable
{
    private static readonly StreamOptions DecodeOnly = new()
    {
        DecodeOnly = true,
        Network = new NetworkOptions { Proxy = "", Timeout = TimeSpan.FromSeconds(3) },
    };

    private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
    private readonly CancellationTokenSource _stop = new();
    private readonly byte[] _wav = TestAudio.SineWav(seconds: 2);
    private readonly byte[] _mp3 = File.ReadAllBytes(FindTestFile("tests/data/net/tone_cbr.mp3"));

    public WebStreamTests()
    {
        _listener.Start();
        _ = Task.Run(AcceptLoop);
    }

    private string Url(string path) => $"http://127.0.0.1:{((IPEndPoint)_listener.LocalEndpoint).Port}{path}";

    private static string FindTestFile(string relative)
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
        {
            string p = Path.Combine(dir.FullName, relative);
            if (File.Exists(p)) return p;
        }
        throw new FileNotFoundException(relative);
    }

    private async Task AcceptLoop()
    {
        while (!_stop.IsCancellationRequested)
        {
            TcpClient c;
            try { c = await _listener.AcceptTcpClientAsync(_stop.Token); }
            catch { return; }
            _ = Task.Run(() => Serve(c));
        }
    }

    private async Task Serve(TcpClient client)
    {
        using var c = client;
        try
        {
            var s = c.GetStream();
            var buf = new byte[4096];
            int got = 0;
            string req;
            while (true)
            {
                int k = await s.ReadAsync(buf.AsMemory(got), _stop.Token);
                if (k <= 0) return;
                got += k;
                req = Encoding.ASCII.GetString(buf, 0, got);
                if (req.Contains("\r\n\r\n")) break;
            }
            string path = req.Split(' ')[1];
            if (path == "/tone.wav")
            {
                await Write(s, $"HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: {_wav.Length}\r\nConnection: close\r\n\r\n");
                await s.WriteAsync(_wav, _stop.Token);
            }
            else if (path == "/radio")
            {
                // An Icecast station: the MP3 looped, the second title from audio byte 32768.
                await Write(s, "ICY 200 OK\r\nContent-Type: audio/mpeg\r\nicy-name: Test FM\r\nicy-br: 128\r\nicy-metaint: 8192\r\n\r\n");
                long audio = 0;
                int at = 0;
                while (!_stop.IsCancellationRequested)
                {
                    int k = (int)Math.Min(8192 - audio % 8192, _mp3.Length - at);
                    await s.WriteAsync(_mp3.AsMemory(at, k), _stop.Token);
                    at = (at + k) % _mp3.Length;
                    audio += k;
                    if (audio % 8192 == 0)
                    {
                        string title = audio < 32768 ? "Artist A - Song 1" : "Artist B - Song 2";
                        byte[] text = Encoding.ASCII.GetBytes($"StreamTitle='{title}';");
                        int blocks = (text.Length + 15) / 16;
                        var meta = new byte[1 + blocks * 16];
                        meta[0] = (byte)blocks;
                        text.CopyTo(meta, 1);
                        await s.WriteAsync(meta, _stop.Token);
                    }
                }
            }
            else
            {
                await Write(s, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            }
        }
        catch
        {
            // the client went away
        }
    }

    private Task Write(NetworkStream s, string text) => s.WriteAsync(Encoding.ASCII.GetBytes(text), _stop.Token).AsTask();

    public void Dispose()
    {
        _stop.Cancel();
        _listener.Stop();
    }

    [Fact]
    public void OpenUrl_DecodesLikeTheFile()
    {
        using var engine = AudioEngine.Create(new AudioEngineOptions { NullOutput = true });
        using var web = engine.OpenUrl(Url("/tone.wav"), DecodeOnly);
        using var local = engine.OpenMemory(_wav, new StreamOptions { DecodeOnly = true });
        Assert.Equal(local.Info.LengthFrames, web.Info.LengthFrames);
        var a = new float[4096];
        var b = new float[4096];
        int n;
        while ((n = local.Read(a)) > 0)
        {
            int m = web.Read(b.AsSpan(0, n));
            Assert.Equal(n, m);
            Assert.True(a.AsSpan(0, n).SequenceEqual(b.AsSpan(0, n)));
        }
        Assert.Equal("audio/wav", web.GetHttpHeader("content-type"));
        Assert.Null(web.GetHttpHeader("icy-name"));
        var status = web.NetworkStatus;
        Assert.NotNull(status);
        Assert.Equal(200, status!.HttpStatus);
        Assert.Equal(_wav.Length, status.ContentLength);
        Assert.False(status.IsLive);
        Assert.Equal(Url("/tone.wav"), status.Url);
    }

    [Fact]
    public void SetAndroidJavaVM_IsForAndroidOnly()
    {
        Assert.Throws<ArgumentException>(() => AudioEngine.SetAndroidJavaVM(0));
        if (!OperatingSystem.IsAndroid())
            Assert.Throws<PlatformNotSupportedException>(() => AudioEngine.SetAndroidJavaVM(1));
    }

    [Fact]
    public void Radio_AnnouncesSongTitles()
    {
        using var engine = AudioEngine.Create(new AudioEngineOptions { NullOutput = true });
        using var radio = engine.OpenUrl(new Uri(Url("/radio")), DecodeOnly);
        var titles = new List<StreamMetadata>();
        radio.MetadataChanged += (_, e) => { lock (titles) titles.Add(e.Metadata); };
        var buf = new float[4096 * 2];
        long frames = 0;
        // Song 2 starts 2 s in: read 1 s and let song 1's event arrive before going on.
        while (frames < 44100) frames += radio.Read(buf) / 2;
        for (int i = 0; i < 200; i++) { lock (titles) if (titles.Count >= 1) break; Thread.Sleep(10); }
        while (frames < 3 * 44100) frames += radio.Read(buf) / 2;

        var md = radio.Metadata;
        Assert.Equal("Test FM", md.Station);
        Assert.Equal(128, md.BitrateKbps);
        Assert.Equal("Artist B - Song 2", md.StreamTitle);
        Assert.Equal("Artist B", md.Artist);
        Assert.Equal("Song 2", md.Title);
        Assert.Equal(2, md.Sequence);
        Assert.True(radio.NetworkStatus!.IsLive);
        for (int i = 0; i < 200; i++) { lock (titles) if (titles.Count >= 2) break; Thread.Sleep(10); }
        lock (titles)
        {
            Assert.Equal(2, titles.Count);
            Assert.Equal("Song 1", titles[0].Title);
            Assert.Equal("Song 2", titles[1].Title);
        }
    }

    [Fact]
    public void OpenUrl_Errors()
    {
        using var engine = AudioEngine.Create(new AudioEngineOptions { NullOutput = true });
        Assert.Throws<ArgumentException>(() => engine.OpenUrl("ftp://example.com/x.mp3"));
        Assert.Equal(ErrorCode.NotFound, Assert.Throws<FatzAudioException>(() => engine.OpenUrl(Url("/missing.mp3"), DecodeOnly)).ErrorCode);
        using var file = engine.OpenMemory(_wav, new StreamOptions { DecodeOnly = true });
        Assert.Null(file.NetworkStatus);
        Assert.Equal(StreamMetadata.Empty, file.Metadata);
        Assert.Null(file.GetHttpHeader("content-type"));
    }
}
