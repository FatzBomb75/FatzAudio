using FatzAudio;

if (args.Length != 1)
{
    Console.Error.WriteLine("usage: PlayFile <audio file>");
    return 1;
}

try
{
    using var engine = AudioEngine.Create();
    using var output = engine.OpenOutput();           // system default device
    using var song = engine.OpenFile(args[0]);

    var info = song.Info;
    Console.WriteLine($"{info.Codec} / {info.Container}, {info.Format.SampleRate} Hz, {info.Format.Channels} ch, duration {info.Duration}");

    var ended = new ManualResetEventSlim();
    song.Ended += (_, _) => ended.Set();               // raised on a native thread

    song.PlayOn(output);

    // The event is the primary signal; polling State covers builds without event support.
    while (!ended.Wait(250) && song.State != StreamState.Ended)
        Console.Write($"\r{song.Position:mm\\:ss\\.f} / {info.Duration:mm\\:ss\\.f}   ");

    Console.WriteLine("\nDone.");
    return 0;
}
catch (FatzAudioException ex)
{
    Console.Error.WriteLine($"FatzAudio error {ex.ErrorCode}: {ex.Message}");
    return 2;
}
