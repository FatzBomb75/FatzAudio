# Tags, cover art and file properties

FatzAudio reads the title, artist, album and the rest of a file's tags, its embedded pictures, and its properties (duration, bitrate, channels, sample rate, bits per sample, file and audio size) in one call. No engine or device is needed, so a media library can scan thousands of files with it. It replaces `BASS_ChannelGetTags` and the tag readers of the BASS add-ons, and also covers what the Tags add-on was used for: the app gets every field ready to show and formats it however it likes.

| Tag format | Files | Items | Pictures |
|---|---|---|---|
| ID3v2.2, 2.3, 2.4 | MP3, AAC (ADTS), AC-3, E-AC-3, FLAC (leading), WAV and AIFF (`id3 ` chunk) | Every text frame, `TXXX` (keyed by its description), `COMM`, `USLT`, URL frames | `APIC`, `PIC` |
| ID3v1 / 1.1 | End of MP3, AAC, AC-3, FLAC, WAV, AIFF | Title, artist, album, year, comment, track, genre | - |
| APEv2 | End of MP3, AAC, FLAC, WAV, AIFF (also with a header) | Every text item; lists joined with "; " | Binary `Cover Art (Front)` and the other cover items |
| Vorbis comments | FLAC, Ogg Vorbis, Opus, Ogg FLAC | Every comment | FLAC `PICTURE` blocks, `METADATA_BLOCK_PICTURE` |
| MP4 / iTunes | M4A, MP4, M4B, MOV | Every `ilst` atom: `©nam`, `trkn`, `disk`, `gnre`, `tmpo`, freeform `----` atoms (keyed by their name) | `covr` |
| ASF | WMA, WMV | Content Description (Title, Author, Copyright, Description, Rating) and every extended attribute (`WM/AlbumTitle`, ...) of every type | `WM/Picture` |
| RIFF INFO | WAV, RF64, BW64 | `INAM`, `IART`, `IPRD`, `ICMT`, ... | - |
| AIFF text chunks | AIFF, AIFF-C | `NAME`, `AUTH`, `(c) `, `ANNO` | - |
| Matroska tags | MKV, MKA, WebM | Global and album-level `SimpleTag`s; the segment title as a fallback | Image attachments (`cover.*` is the front cover) |
| MIDI | MID, KAR, RMI (from a stream: see below) | Title, artist, copyright | - |

A ZIP (MP3+G) is read through the audio file inside it: the one named like the `.cdg`, else the first audio file. Text in any encoding the format allows (Latin-1, UTF-16 with or without BOM, UTF-8) comes out as UTF-8; a file that claims UTF-8 but isn't is read as Latin-1, so nothing is lost.

## Common fields

`fa_tags_get(tags, field)` (.NET: `tags.Title`, `tags.Artist`, ...) gives a field whichever format it came from. It never returns NULL: a field that isn't there is `""` in C and `null` in .NET.

| Field | .NET | Notes |
|---|---|---|
| `FA_TAG_TITLE` | `Title` | |
| `FA_TAG_ARTIST` | `Artist` | |
| `FA_TAG_ALBUM` | `Album` | |
| `FA_TAG_ALBUM_ARTIST` | `AlbumArtist` | |
| `FA_TAG_COMPOSER` | `Composer` | |
| `FA_TAG_GENRE` | `Genre` | ID3 numbers (`"(17)"`, `"17"`, `"(RX)"`) and the MP4 `gnre` atom are turned into names ("Rock", "Remix") |
| `FA_TAG_DATE` | `Date`, `Year` | As written: `"2024"`, `"2024-05-17"`. `Year` is its first four digits as a number |
| `FA_TAG_TRACK`, `FA_TAG_TRACK_TOTAL` | `Track`, `TrackTotal` | A `"3/12"` in the file is split into 3 and 12. .NET gives numbers |
| `FA_TAG_DISC`, `FA_TAG_DISC_TOTAL` | `Disc`, `DiscTotal` | Same as tracks |
| `FA_TAG_COMMENT` | `Comment` | |
| `FA_TAG_LYRICS` | `Lyrics` | Unsynchronised lyrics (`USLT`, `LYRICS`, `©lyr`, `WM/Lyrics`) |
| `FA_TAG_COPYRIGHT` | `Copyright` | |
| `FA_TAG_ENCODER` | `Encoder` | The software that made the file |
| `FA_TAG_BPM` | `Bpm` | |

When a file has several tag formats, the richest wins for each field: ID3v2 before APEv2 before ID3v1; in FLAC, the Vorbis comments before a leading ID3v2; in WAV and AIFF, ID3v2 before the native chunks. A field the winning format lacks is filled from the next one. A key that appears more than once in one format (two `ARTIST` comments) is joined with "; ".

Everything else, ReplayGain values and MusicBrainz IDs included, is in the item list exactly as the file stores it: `fa_tags_get_item` (.NET: `tags.Items`, or the indexer `tags["REPLAYGAIN_TRACK_GAIN"]`, which ignores case). The key is the format's own name: an ID3v2 frame ID (`TIT2`; a `TXXX` uses its description, a `COMM` with a description is `COMM:desc`), a Vorbis or APE key, an MP4 atom (`©nam` in UTF-8; freeform atoms by their name), an ASF attribute, a RIFF or AIFF chunk ID, or a Matroska tag name (`ALBUM/TITLE` for album-level tags when the file has track-level ones too).

## Properties

Unless `FA_TAGS_NO_INFO` is set, the reader also opens a decoder on the file (headers only, nothing is decoded) and keeps its `fa_stream_info`: `fa_tags_get_info` (.NET: `tags.Info`, plus the shortcuts `Duration`, `Bitrate`, `Channels`, `SampleRate`, `BitsPerSample`, `Codec`, `FileSize`, `AudioSize` and `HasCdg`). The fields are the same as `fa_stream_get_info` gives for a stream of that file; [streams.md](streams.md#stream-info) describes them. `audio_bytes` is the coded audio alone, without tags, cover art, headers or other tracks, which is what a bitrate needs.

`fa_tags_get_info` returns `FA_ERROR_NOT_FOUND` when no decoder recognised the file (an unsupported codec still gives its tags), and for MIDI files read by path, which need a soundfont to open.

## C

```c
#include <fatzaudio/fatzaudio.h>

fa_tags* tags = NULL;
if (fa_tags_read_path("C:/Music/track.flac", 0, &tags) != FA_OK) { /* missing file */ }

printf("%s - %s (%s, track %s)\n",
       fa_tags_get(tags, FA_TAG_ARTIST), fa_tags_get(tags, FA_TAG_TITLE),
       fa_tags_get(tags, FA_TAG_ALBUM), fa_tags_get(tags, FA_TAG_TRACK));

fa_stream_info info;
if (fa_tags_get_info(tags, &info) == FA_OK)
    printf("%s, %u Hz, %u ch, %.1f s, %u kbps, %lld bytes\n", info.codec,
           info.format.sample_rate, info.format.channels, info.duration / 1e9,
           info.bitrate / 1000, (long long)info.file_size);

fa_tag_picture cover;
if (fa_tags_get_cover(tags, &cover) == FA_OK)
    save_image(cover.data, cover.size, cover.mime);   /* still encoded: JPEG, PNG, ... */

/* Every item, as stored. */
for (uint32_t i = 0; i < fa_tags_get_item_count(tags); i++) {
    fa_tag_item item;
    fa_tags_get_item(tags, i, &item);
    printf("%s = %s\n", item.key, item.value);
}

fa_tags_close(tags);    /* every string and picture above is freed here */
```

A library scan that only needs text and a duration:

```c
fa_tags_read_path(path, FA_TAGS_NO_PICTURES, &tags);    /* pictures are skipped unread */
```

A stream that is already open:

```c
fa_tags* tags = NULL;
fa_stream_get_tags(song, 0, &tags);
```

## C#

```csharp
using FatzAudio;

var tags = Tags.Read(@"C:\Music\track.flac");
Console.WriteLine($"{tags.Artist} - {tags.Title} ({tags.Album}, track {tags.Track}/{tags.TrackTotal})");
Console.WriteLine($"{tags.Codec}, {tags.SampleRate} Hz, {tags.Channels} ch, {tags.Duration}, {tags.Bitrate / 1000} kbps");
Console.WriteLine($"{tags.FileSize} bytes, {tags.AudioSize} of audio, {tags.BitsPerSample} bits");

if (tags.Cover is { } cover)
    File.WriteAllBytes("cover" + (cover.MimeType == "image/png" ? ".png" : ".jpg"), cover.Data);

string? gain = tags["REPLAYGAIN_TRACK_GAIN"];
foreach (var item in tags.Items)
    Console.WriteLine($"{item.Format} {item.Key} = {item.Value}");

// Faster scans (a library scan should always pass NoPictures: the default copies every
// embedded picture into a new array, which lands on the large object heap):
var quick = Tags.Read(path, TagReadOptions.NoPictures);
var textOnly = Tags.Read(path, TagReadOptions.NoPictures | TagReadOptions.NoInfo);

// From memory, or from an open stream:
var fromBytes = Tags.Read(File.ReadAllBytes(path));
var fromStream = song.GetTags();
```

`Tags` is a plain managed object: everything is copied out of the library when it is read, so there is nothing to dispose, and it can be kept, cached or passed between threads.

## Options

| C flag | .NET | Effect |
|---|---|---|
| `FA_TAGS_NO_PICTURES` | `TagReadOptions.NoPictures` | Picture data is skipped without being read: no memory for cover art, faster on files with large covers |
| `FA_TAGS_NO_INFO` | `TagReadOptions.NoInfo` | Only the tags are read; no decoder is opened, and `fa_tags_get_info` returns `FA_ERROR_NOT_FOUND` |

## Streams

`fa_stream_get_tags` (.NET: `AudioStream.GetTags()`) reads the tags of the file a stream plays, opened from a path or from memory; the properties are the stream's own `fa_stream_info`. A MIDI stream also gets the title, artist and copyright of `fa_stream_get_midi_info` as `FA_TAG_FORMAT_MIDI` items (karaoke `@T` lines included). Streams of the app's own I/O callbacks, CD tracks and push streams give empty tags. Web streams have no file tags: their station name and song titles come from `fa_stream_get_metadata` and the metadata event ([web-streams.md](web-streams.md)).

## Threads

Reading is synchronous and does file I/O on the calling thread, so a media library should scan on a worker thread. A tag object is immutable after it is read: any thread may read it, and its strings and pictures stay valid until `fa_tags_close`.

## Pitfalls

- An empty result is not an error. A file without tags, or of a format that has none (raw AAC without ID3, a bare WAV), gives `FA_OK` and an empty tag object.
- Fields are text as written. A date may be a year or a full date, a track may have leading zeros, and a genre may be any text. .NET's `Year`, `Track`, `Disc` and `Bpm` give `null` when the text isn't a number.
- MP4 atom keys start with the copyright sign, which is two bytes in UTF-8 (`"\xC2\xA9nam"`). Use `fa_tags_get` for the common fields instead of matching keys.
- ID3v2 frames stored compressed (the rare zlib flag) are skipped. Lyrics3 tags are not read. Matroska tags aimed at one track or chapter are left out; global and album tags are read.
- Bitrate and `audio_bytes` for MP3, AAC, AC-3 and FLAC are measured between the tags, so a VBR MP3 includes its small Xing header frame (a fraction of a percent). MP4, WAV, AIFF and CD are exact.
- The BASS `TAGS_Read` format strings (`%ARTI - %TITL`) have no equivalent: build the text from the fields.
