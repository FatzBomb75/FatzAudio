# Tutorial: a library scanner

Read the tags of every file in a folder, measure its loudness, and work out its tempo and key, without playing anything and without opening a sound device.

## 1. Tags

```csharp
foreach (var path in Directory.EnumerateFiles(folder, "*.*", SearchOption.AllDirectories))
{
    try
    {
        var tags = Tags.Read(path, TagReadOptions.NoPictures);   // no engine needed; NoPictures skips cover art
        Console.WriteLine($"{tags.Artist} - {tags.Title} ({tags.Album}), {tags.Duration}");
    }
    catch (FatzAudioException ex) { Console.Error.WriteLine($"{path}: {ex.ErrorCode}"); }
}
```

Anything FatzAudio cannot read as audio throws `UnsupportedFormat`; skip it. Details: [tags](../features/tags.md).

## 2. Loudness for normalization

```csharp
using var engine = AudioEngine.Create(new AudioEngineOptions { NullOutput = true });
var scanned = await Task.Run(() => engine.ScanLoudness(path));
Console.WriteLine($"{scanned.IntegratedLufs:F1} LUFS, LRA {scanned.RangeLu:F1} LU, {scanned.TruePeakDbtp:F1} dBTP");
```

Store the numbers in your database. At play time `song.SetLoudness(storedLufs)` skips the scan ([normalization](../features/normalization.md)).

## 3. Tempo, key and waveform

```csharp
var t = await engine.AnalyzeAsync(path, AnalysisKinds.Beats | AnalysisKinds.Key | AnalysisKinds.Waveform);
if (t.Beats is { } grid) Console.WriteLine($"{grid.Bpm:F2} BPM");
if (t.Key is { } key) Console.WriteLine($"{key.Name} ({key.Camelot})");
```

`NullOutput` means the scan needs no sound hardware, so it runs on a server. Analysis has a cache and runs in parallel on the engine's scan threads: [analysis](../features/analysis.md).

## Next

Show the library in your UI and play with the [WPF player](wpf-player.md) pattern.
