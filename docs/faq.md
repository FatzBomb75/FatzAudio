# Frequently asked questions

**Does FatzAudio replace BASS completely?**
Yes for what a media player uses: BASS, bass_fx, bassmidi, basscd, bassflac, bass_aac, bassopus, bass_ac3, bassalac, basswebm, basswma and BASSenc each have a mapping. 3D sound, EAX and the generic FX API are not provided. See [Migrating from BASS](migration/bass.md).

**How many files do I ship?**
One native library per platform, plus the .NET assembly if you use .NET. No add-on DLLs, no C++ runtime. LAME (for MP3 encoding) and FFmpeg are optional and loaded at run time only if your app ships them.

**Which languages can use it?**
C directly (`include/fatzaudio/fatzaudio.h`), and .NET through the `FatzAudio` package. Anything that can call a C library can use the C API.

**Do I need an init call like `BASS_Init`?**
No. Create an engine, open an output device, open streams. The engine is a handle you hold, so several can exist. See [Concepts](concepts.md).

**Is it thread-safe?**
Every function is thread-safe unless its page says otherwise. Events arrive on an engine thread, never the audio thread, so marshal to your UI thread. See [Concepts](concepts.md#threads) and [events](features/events.md).

**Why does my console program exit with no sound?**
It exited before the song ended. Wait for the `Ended` event ([Getting started](getting-started.md)).

**Can it play MIDI without a soundfont?**
It uses the operating system's bank where there is one (Windows `gm.dls`, macOS, an installed `.sf2` on Linux). Android and iOS apps supply a soundfont. FatzAudio never bundles one ([MIDI](features/midi.md)).

**Can it play WMA, DTS or TrueHD?**
WMA only on Windows. DTS and TrueHD are not supported. Decode them with your own FFmpeg and push the frames ([FFmpeg hand-off](features/ffmpeg.md)). FatzAudio itself never uses FFmpeg.

**Can I record MP3?**
Yes, if your app ships LAME. WAV and FLAC are built in ([recording and encoding](features/recording-encoding.md)).

**Does it support karaoke?**
Yes: CD+G graphics (MP3+G, ZIP, discs) and a MIDI lyric display, drawn for you into a pixel buffer any renderer can show, with timing events ([CD+G](features/cdg.md), [lyrics](features/lyrics.md)).

**Can it be a DJ mixer?**
Two decks with isolator EQ, crossfader, headphone cue, loops, cue points, beat sync, key detection and two karaoke mic inputs ([DJ mixer](features/dj-mixer.md)). More than two decks is a possible future addition.

**What about 3D audio, EAX or hardware voices?**
Not provided; software mixing only.

**Can I change tempo without changing pitch?**
Yes: tempo, pitch and rate are separate parameters ([tempo and pitch](features/tempo-pitch.md)).

**Is the library clean-room?**
Yes. It was written from public format specifications and the documented BASS behaviour; no BASS code was read or copied.

**How do I know what changed between versions?**
[CHANGELOG](../CHANGELOG.md), and [About](about.md#versions). Before 1.0 a minor version bump may break the API.

**Where do I report a bug or see known issues?**
The [engineering notes](engineering-notes.md) list known issues and follow-ups.
