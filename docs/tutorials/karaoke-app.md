# Tutorial: a karaoke app

Karaoke songs come as an MP3 plus a `.cdg` file, a ZIP holding both, a MIDI `.kar` file, or a CD+G disc. FatzAudio plays the audio and draws the words into a pixel buffer; your app puts that buffer on screen. Start from the [WPF player](wpf-player.md).

## 1. CD+G songs

```csharp
using var song = engine.OpenFile("Artist - Song.zip");   // or an .mp3 with a .cdg beside it
using var cdg = song.CreateCdgSurface();
song.PlayOn(output);

CompositionTarget.Rendering += (_, _) =>                  // WPF, once per frame
{
    var frame = cdg.Update();
    if (!frame.Changed) return;
    var d = frame.Dirty;
    bitmap.WritePixels(new Int32Rect(d.X, d.Y, d.Width, d.Height),
        frame.PixelPointer + d.Y * frame.Stride + d.X * 4,
        frame.Stride * frame.Height, frame.Stride);
};
```

Only the changed rectangle is copied. The surface follows the stream's clock, so pausing and seeking carry the graphics with them. Options (size, transparent background, smoothing) and the other renderers (Direct3D, SkiaSharp, OpenGL) are in [CD+G](../features/cdg.md). Zip songs that are large need a [temp directory](../features/cdg.md#zip-archives).

## 2. MIDI and `.kar` songs

```csharp
using var song = engine.OpenFile("song.kar");   // no soundfont set: the system's General MIDI bank is used
song.MidiTextReached += (_, e) => Console.WriteLine(e.Text.Text);
song.PlayOn(output);
```

To draw the lyrics as a finished karaoke page (lines, pages, wipes, countdown) use `song.CreateLyricSurface()` and update it the same way as the CD+G surface ([lyrics](../features/lyrics.md)). If the song fails with `NoSoundFont` (Android, iOS, some Linux systems), load one: [MIDI](../features/midi.md).

## 3. Change the key and the speed

```csharp
song.Tempo = 1.1f;           // 10% faster, same pitch
song.Pitch = -2;             // two semitones down, same speed
```

For MIDI files `MidiTranspose` changes the key in the notes themselves ([tempo and pitch](../features/tempo-pitch.md)).

## 4. Next song, silence, singers

- Queue the next song for a gapless start: [crossfade and queues](../features/crossfade.md).
- Skip dead air at the start and end of a recording: [silence detection](../features/analysis.md).
- Mic mixing, monitoring and echo for singers: [DJ mixer microphones](../features/dj-mixer.md), used in the [DJ app tutorial](dj-app.md).
- Disc-based songs: [discs](../features/disc.md).
