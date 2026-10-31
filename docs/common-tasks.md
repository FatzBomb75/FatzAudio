# Common tasks in five minutes

The everyday jobs, in C and C#, each a few lines. Nothing here needs the DJ mixer, effects, encoding or analysis: those are separate classes and options you only meet when you want them. Every snippet assumes the engine and output from [Getting started](getting-started.md); each section links to its full guide.

## Play a file, change volume, seek, know when it ends

```csharp
using var engine = AudioEngine.Create();
using var output = engine.OpenOutput();
using var song = engine.OpenFile("song.mp3");      // MP3, FLAC, Opus, WAV, MIDI, ... detected from content

song.Ended += (_, _) => Console.WriteLine("done"); // raised on a native thread: marshal to your UI thread
song.PlayOn(output);

song.Volume = 0.8f;                                // 0..1, linear
song.Position = TimeSpan.FromSeconds(30);          // seek (blocks briefly)
song.RequestSeek(TimeSpan.FromSeconds(30));        // seek without blocking: use for a seek bar
Console.WriteLine($"{song.Position} / {song.Info.Duration}");
song.Pause();  song.Play();  song.Stop();
```

```c
static void FA_CALL on_event(const fa_event* ev, void* user)
{
    if (ev->type == FA_EVENT_ENDED) *(volatile int*)user = 1;   /* runs on the event thread */
}

volatile int done = 0;
fa_stream_set_event_callback(stream, on_event, (void*)&done);
fa_stream_attach(stream, device, NULL);
fa_device_start(device);
fa_stream_play(stream);

fa_stream_set_param(stream, FA_PARAM_VOLUME, 0.8f);
fa_stream_seek(stream, 30 * (fa_frames)sample_rate);            /* frames, not bytes */
fa_frames pos;  fa_stream_get_position(stream, &pos);
```

More: [playback](features/playback.md), [events](features/events.md).

## Play a URL or web radio

```csharp
using var radio = engine.OpenUrl("https://stream.example.com/live.mp3");   // may block: call off the UI thread
radio.MetadataChanged += (_, e) => Console.WriteLine($"{e.Metadata.Artist} - {e.Metadata.Title}");
radio.PlayOn(output);
```

C: `fa_stream_open_url(engine, url, NULL, &stream)`, then attach and play as above. More: [web streams](features/web-streams.md).

## Read tags and cover art

No engine or device needed:

```csharp
var tags = Tags.Read("song.flac");
Console.WriteLine($"{tags.Artist} - {tags.Title} ({tags.Album}), {tags.Duration}");
if (tags.Cover is { } cover) File.WriteAllBytes("cover.jpg", cover.Data);
```

```c
fa_tags* tags;
if (fa_tags_read_path("song.flac", 0, &tags) == FA_OK) {
    printf("%s - %s\n", fa_tags_get(tags, FA_TAG_ARTIST), fa_tags_get(tags, FA_TAG_TITLE));
    fa_tags_close(tags);
}
```

For a library scan pass `TagReadOptions.NoPictures`. More: [tags](features/tags.md).

## Karaoke: CD+G

An `.mp3` with a `.cdg` of the same name, or a `.zip` holding both, loads its graphics by itself.

```csharp
using var song = engine.OpenFile("Artist - Song.zip");
using var cdg = song.CreateCdgSurface();
song.PlayOn(output);

// Once per frame (WPF: CompositionTarget.Rendering):
var frame = cdg.Update();
if (frame.Changed) { /* copy frame.Dirty rectangle of frame.PixelPointer to your bitmap */ }
```

The full WPF and C versions are in [CD+G](features/cdg.md).

## Karaoke: MIDI and .kar files

Open and play like any file: with no soundfont set, the operating system's General MIDI bank is used (Android and iOS need one loaded). Lyrics arrive as events:

```csharp
using var song = engine.OpenFile("song.kar");
song.MidiTextReached += (_, e) => Console.WriteLine(e.Text.Text);
song.Tempo = 1.1f;                 // faster, same pitch
song.MidiTranspose = -2;           // two semitones down
song.PlayOn(output);
```

To draw the lyrics as a ready-made karaoke page, use `CreateLyricSurface()` ([lyrics](features/lyrics.md)). More: [MIDI](features/midi.md).

## Where to go next

| You want | Read |
|---|---|
| Fades and crossfades | [ramps](features/ramps.md), [crossfade](features/crossfade.md) |
| Equalizer, tempo, pitch | [equalizer](features/equalizer.md), [tempo and pitch](features/tempo-pitch.md) |
| Record or convert to WAV, FLAC, MP3 | [recording and encoding](features/recording-encoding.md) |
| Two decks and a crossfader | [DJ mixer](features/dj-mixer.md) |
| Beats, key, waveform | [analysis](features/analysis.md) |
| Porting a BASS app | [migration guide](migration/bass.md) |
