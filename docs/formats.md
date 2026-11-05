# Formats and codecs

What FatzAudio can open, decode and write. The full detail, with how detection works, is in [Opening and decoding streams](features/streams.md); this page is the overview.

## Playing and decoding

| Format | Built in? | Windows | macOS, iOS | Android | Linux | Guide |
|---|---|---|---|---|---|---|
| WAV, RF64, Wave64, AIFF, AIFF-C | Yes | ✅ | ✅ | ✅ | ✅ | [streams](features/streams.md) |
| FLAC (native, Ogg, in MP4 and MKV) | Yes | ✅ | ✅ | ✅ | ✅ | [streams](features/streams.md) |
| MP3 | Yes | ✅ | ✅ | ✅ | ✅ | [streams](features/streams.md) |
| MP2, MP1 (MPEG audio Layers I and II; `.mp2`, `.mpa`, and in Matroska, MP4) | Yes | ✅ | ✅ | ✅ | ✅ | [MP3, MP2 and MP1](features/streams.md#mp3-mp2-and-mp1) |
| Sun/NeXT AU and SND (PCM, float, µ-law, A-law); µ-law and A-law in CAF and MP4; IMA 4:1 ADPCM (`ima4`) in CAF and QuickTime | Yes | ✅ | ✅ | ✅ | ✅ | [streams](features/streams.md#au-g711-and-ima4) |
| AAC-LC, HE-AAC, HE-AAC v2, Main, LTP (ADTS, MP4, M4A, CAF, MKV, HLS) | Yes | ✅ | ✅ | ✅ | ✅ | [AAC](features/streams.md#aac) |
| Opus, Vorbis (Ogg, WebM, MKV) | Yes | ✅ | ✅ | ✅ | ✅ | [streams](features/streams.md) |
| ALAC (MP4, M4A, CAF) | Yes | ✅ | ✅ | ✅ | ✅ | [streams](features/streams.md) |
| Dolby Digital, Dolby Digital Plus (up to 7.1) | Yes | ✅ | ✅ | ✅ | ✅ | [Dolby](features/dolby.md) |
| MIDI, karaoke `.kar`, RMI | Needs a soundfont (yours or the system's) | ✅ | ✅ | ✅ | ✅ | [MIDI](features/midi.md) |
| Tracker modules (MOD, XM, S3M, IT, MPTM and more) | Yes | ✅ | ✅ | ✅ | ✅ | [trackers](features/trackers.md) |
| Audio of AVI (PCM, MP3, MP2, AC-3, AAC; DTS and ADPCM not) and FLV (AAC, MP3, PCM) video files | Yes (WMA in AVI: system decoder) | ✅ | ✅ | ✅ | ✅ | [AVI and FLV](features/streams.md#avi-and-flv) |
| WMA, ASF audio | System decoder (Windows Media Foundation). On Windows N editions without the Media Feature Pack it returns `ProviderMissing`; off Windows, decode WMA in your app (FatzMedia.Video does) | ✅ | ➖ | ➖ | ➖ | [WMA](features/streams.md#wma) |
| xHE-AAC | System decoder | If present | If present | If present | ➖ | [AAC](features/streams.md#aac) |
| MP3+G, CD+G in ZIP, CD+G discs, audio CDs, `.cue` images | Yes | ✅ | ✅ | ✅ | ✅ | [CD+G](features/cdg.md), [discs](features/disc.md) |
| A lone `.cdg`, or a ZIP holding only one (plays as silence for the graphics' length) | Yes | ✅ | ✅ | ✅ | ✅ | [CD+G](features/cdg.md#graphics-with-no-audio) |
| Internet radio, HLS, HTTP and HTTPS streams | Yes | ✅ | ✅ | ✅ | ✅ | [web streams](features/web-streams.md) |
| DTS, TrueHD, other codecs | No | ➖ | ➖ | ➖ | ➖ | Decode with your own FFmpeg and hand over frames ([FFmpeg](features/ffmpeg.md)) |
| Copy-protected media (WMA DRM, FairPlay, encrypted MP4/MKV, SAMPLE-AES) | Never decrypted | `ProtectedContent` | `ProtectedContent` | `ProtectedContent` | `ProtectedContent` | [streams](features/streams.md) |

You can ask the library at run time: `fa_format_count` and `fa_format_get_info` in C, `AudioEngine.SupportedFormats` in .NET. Detection reads the content, not the file extension.

## Writing

| Format | Notes | Guide |
|---|---|---|
| WAV | 16 or 24-bit PCM or 32-bit float; RF64 past 4 GB | [recording and encoding](features/recording-encoding.md) |
| FLAC | Lossless, 16 or 24 bit, up to 8 channels | [recording and encoding](features/recording-encoding.md) |
| MP3 | Through LAME, which your app ships; FatzAudio loads it at run time | [recording and encoding](features/recording-encoding.md#mp3-and-lame) |

Opus and AAC encoders are not planned.

## Tags and metadata

ID3v1 and v2, APEv2, Vorbis comments, MP4, ASF, RIFF, AIFF and Matroska tags, cover art, and file properties: see [tags](features/tags.md).
