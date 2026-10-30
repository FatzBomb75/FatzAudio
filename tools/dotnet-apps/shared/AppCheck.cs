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
