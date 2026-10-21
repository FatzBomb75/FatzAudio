using System.Runtime.InteropServices;

namespace FatzAudio.Native;

[StructLayout(LayoutKind.Sequential)]
internal struct FaAudioFormat
{
    public int SampleFormat;
    public uint SampleRate;
    public uint Channels;
    public ulong ChannelMask;
}

[StructLayout(LayoutKind.Sequential)]
internal struct FaEqBand
{
    public int Type;
    public float Frequency;
    public float Q;
    public float GainDb;

    public FaEqBand(EqualizerBand b) { Type = (int)b.Type; Frequency = b.Frequency; Q = b.Q; GainDb = b.GainDb; }
    public EqualizerBand ToBand() => new((EqualizerBandType)Type, Frequency, Q, GainDb);
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaEngineConfig
{
    public uint StructSize;
    public uint MixSampleRate;
    public uint PeriodMs;
    public uint WorkerThreads;
    public byte* CacheDirectory;
    public uint Flags;
    public byte* TempDirectory;
    public uint ZipMemoryLimitMb;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaDeviceInfo
{
    public fixed byte Id[256];
    public fixed byte Name[256];
    public int Type;
    public int IsDefault;
    public uint NativeSampleRate;
    public uint NativeChannels;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaIoCallbacks
{
    public delegate* unmanaged[Cdecl]<void*, void*, nuint, long> Read;
    public delegate* unmanaged[Cdecl]<void*, long, int> Seek;
    public delegate* unmanaged[Cdecl]<void*, long> Length;
    public delegate* unmanaged[Cdecl]<void*, void> Close;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaOpenOptions
{
    public uint StructSize;
    public uint Flags;
    public int AudioTrack;
    public byte* MimeHint;
    public uint BufferMs;
    public FaNetOptions* Net;
    public int DynamicRange;
    public nint SoundFont;
    public int TextCodePage;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaNetOptions
{
    public uint StructSize;
    public byte* UserAgent;
    public byte* Headers;
    public byte* Proxy;
    public uint TimeoutMs;
    public uint PrebufferMs;
    public uint ReconnectMs;
    public uint Flags;
    public uint HlsMaxBandwidth;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaMetadata
{
    public fixed byte StreamTitle[1024];
    public fixed byte Artist[256];
    public fixed byte Title[768];
    public fixed byte Url[512];
    public fixed byte Station[256];
    public fixed byte Genre[128];
    public fixed byte Description[256];
    public fixed byte StationUrl[512];
    public fixed byte ContentType[128];
    public uint BitrateKbps;
    public long Seq;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaNetStatus
{
    public int Connected;
    public int HttpStatus;
    public int IsLive;
    public int IsHls;
    public int Seekable;
    public int Finished;
    public long ContentLength;
    public long Downloaded;
    public long BufferedBytes;
    public long Buffered;
    public uint Reconnects;
    public int LastError;
    public fixed byte Url[2048];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaStreamInfo
{
    public FaAudioFormat Format;
    public fixed byte Codec[32];
    public fixed byte Container[32];
    public long LengthFrames;
    public int Seekable;
    public int AudioTrackCount;
    public int HasCdg;
    public long Duration;
    public long FileSize;
    public long AudioBytes;
    public uint Bitrate;
    public uint BitsPerSample;
}

[StructLayout(LayoutKind.Sequential)]
internal struct FaTagItem
{
    public nint Key;
    public nint Value;
    public int Format;
}

[StructLayout(LayoutKind.Sequential)]
internal struct FaTagPicture
{
    public int Type;
    public nint Mime;
    public nint Description;
    public nint Data;
    public nuint Size;
    public int Format;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaFormatInfo
{
    public fixed byte Name[32];
    public fixed byte Description[64];
    public fixed byte Extensions[64];
    public fixed byte MimeTypes[128];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaRamp
{
    public int Curve;
    public float BezierX1, BezierY1, BezierX2, BezierY2;
    public long Tag;
    public float* Points;
    public uint PointCount;
}

[StructLayout(LayoutKind.Sequential)]
internal struct FaLoudness
{
    public double IntegratedLufs;
    public double RangeLu;
    public double TruePeakDbtp;
    public int IsFinal;
    public int Source;
    public double GainDb;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaEvent
{
    public int Type;
    public void* Stream;
    public long Frame;
    public long Value;
}

[StructLayout(LayoutKind.Sequential)]
internal struct FaRational
{
    public int Num;
    public int Den;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaCodecParams
{
    public uint StructSize;
    public byte* Codec;
    public uint SampleRate;
    public uint Channels;
    public void* Extradata;
    public nuint ExtradataSize;
    public long BitRate;
    public uint BlockAlign;
    public uint BitsPerCodedSample;
    public FaRational TimeBase;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaPacket
{
    public void* Data;
    public nuint Size;
    public long Pts;
    public long Duration;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaAudioFrame
{
    public FaAudioFormat Format;
    public void** Data;
    public long Frames;
    public long Pts;
    public FaRational TimeBase;
}

[StructLayout(LayoutKind.Sequential)]
internal struct FaTempoOptions
{
    public uint StructSize;
    public int Quality;
    public int KeepFormants;
    public uint LatencyMs;
}

[StructLayout(LayoutKind.Sequential)]
internal struct FaCdgSurfaceConfig
{
    public uint StructSize;
    public int PixelFormat;
    public uint Width, Height;
    public uint Flags;
    public uint BackgroundColor;
    public uint BackgroundOpacity;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaCdgFrame
{
    public void* Pixels;
    public uint Width, Height;
    public int Stride;
    public int PixelFormat;
    public int Changed;
    public int PaletteChanged;
    public uint DirtyX, DirtyY, DirtyWidth, DirtyHeight;
    public fixed byte Palette[64];
    public long Time;
    public int BackgroundIndex;
    public int BorderIndex;
    public fixed byte Transparency[16];
    public fixed byte Background[4];
    public ulong Revision;
    public ulong PaletteRevision;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaSurfaceLayout
{
    public uint StructSize;
    public uint Width, Height;
    public int BitmapX, BitmapY;
    public uint ContentX, ContentY, ContentWidth, ContentHeight;
    public float Scale;
    public uint CropX, CropY, CropWidth, CropHeight;
    public int SafetyZone;
    public int SafetyZoneResolved;
    public fixed byte FontUsed[128];
    public uint Margin, Padding;
    public ulong LayoutRevision;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaSurfaceFrame
{
    public uint StructSize;
    public ulong Revision;
    public ulong BaseRevision;
    public ulong LayoutRevision;
    public uint DirtyCount;
    public fixed int Dirty[32];          // fa_rect[8]: x, y, width, height
    public int BoundsX, BoundsY, BoundsWidth, BoundsHeight;
    public long Time;
    public long DrawNs;
    public long RenderNs;
    public int Behind;
    public int BackgroundIndex;
    public int BorderIndex;
    public fixed byte Background[4];
    public fixed byte Palette[64];
    public fixed byte Transparency[16];
    public ulong PaletteRevision;
}

[StructLayout(LayoutKind.Sequential)]
internal struct FaCdgPresentation
{
    public uint StructSize;
    public int Scale;
    public int SafetyZone;
    public float AutoIgnoreSeconds;
    public int SuperCentered;
    public uint Padding;
    public int Smoothness;
    public int EdgeBlend;
    public uint Flags;
}

[StructLayout(LayoutKind.Sequential)]
internal struct FaFxOutline
{
    public int On;
    public uint Color;
    public uint SungColor;
    public float Opacity;
    public float Size;
    public float Fade;
}

[StructLayout(LayoutKind.Sequential)]
internal struct FaFxShadow
{
    public int On;
    public uint Color;
    public uint SungColor;
    public float Opacity;
    public float Size;
    public float Distance;
    public float AngleDegrees;
    public float Fade;
}

[StructLayout(LayoutKind.Sequential)]
internal struct FaFxBevel
{
    public int On;
    public uint Highlight;
    public uint Shade;
    public float Strength;
    public float Size;
    public float LightAngleDegrees;
    public float Fade;
}

[StructLayout(LayoutKind.Sequential)]
internal struct FaSurfaceEffects
{
    public uint StructSize;
    public FaFxShadow Shadow;
    public FaFxOutline Outline;
    public FaFxBevel Bevel;
}

/// <summary>fa_lyric_style.</summary>
[StructLayout(LayoutKind.Sequential)]
internal struct FaLyricStyle
{
    public uint StructSize;
    public nint FontFamily;
    public nint FontPath;
    public nint FontData;
    public nuint FontDataSize;
    public uint FontFace;
    public int FontExplicit;
    public float FontSize;
    public int FontWeight;
    public uint TextColor, SungColor;
    public float VerticalPosition;
    public float BoxWidth;
    public float LinePitch;
    public int LinesAbove, LinesBelow;
    public float AboveOpacity0, AboveOpacity1;   // above_opacity[2]
    public float NextLineOpacity;
    public float AfterNextFactor;
    public float NearScale, FarScale;
    public int Wipe;
    public int WipeCapMs;
    public float WipeShare;
    public int InstantCutMs;
    public int LeadMs;
    public int ParagraphGapMs;
    public int SentenceGapMs;
    public int BreakLength;
    public int ScrollMs;
    public int PageMs;
    public int PageHoldMs;
    public int PagePreviewMs;
    public int PageOverlap;
    public float PageInScale;
    public float PageOutScale;
    public int EndHoldMs, EndFadeMs;
    public int Countdown;
    public int CountdownCount;
    public int CountdownMs;
    public int CountdownGapMs;
    public uint CountdownFrom, CountdownTo;
    public float CountdownSize;
    public int Panel;
    public uint PanelColor;
    public float PanelPadding, PanelRadius;
    public int SubtitleMode;
    public FaSurfaceEffects Effects;
}

[StructLayout(LayoutKind.Sequential)]
internal struct FaCdgInfo
{
    public int Source;
    public ulong PackCount;
    public long CheckpointInterval;
    public uint CheckpointCount;
    public int DiscParity;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaDiscDriveInfo
{
    public fixed byte Id[256];
    public fixed byte Name[128];
    public fixed byte Vendor[16];
    public fixed byte Product[32];
    public fixed byte Revision[8];
}

[StructLayout(LayoutKind.Sequential)]
internal struct FaDiscOptions
{
    public uint StructSize;
    public int ReadOffset;
    public uint Retries;
    public uint Flags;
}

[StructLayout(LayoutKind.Sequential)]
internal struct FaDiscStatus
{
    public int State;
    public int Locked;
    public uint ChangeCount;
}

[StructLayout(LayoutKind.Sequential)]
internal struct FaDiscTrack
{
    public int Number;
    public int IsAudio;
    public int Preemphasis;
    public int CopyPermitted;
    public int FourChannel;
    public long StartSector;
    public long Sectors;
    public long Frames;
    public long Duration;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaDiscToc
{
    public const int MaxTracks = 99;
    public int FirstTrack;
    public int LastTrack;
    public int TrackCount;
    public int AudioTrackCount;
    public long LeadoutSector;
    public long Duration;
    public uint FreedbId;
    public fixed byte FreedbQuery[1024];
    public fixed byte MusicBrainzId[32];
    public int HasCdText;
    // fa_disc_track tracks[99]; 56 bytes each, 8-byte aligned at offset 1096.
    public fixed byte Tracks[MaxTracks * 56];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaEncoderOptions
{
    public uint StructSize;
    public int Format;
    public uint SampleRate;
    public uint Channels;
    public uint BitsPerSample;
    public uint BitrateKbps;
    public int Quality;
    public uint QueueMs;
    public byte* Title;
    public byte* Artist;
    public byte* Album;
    public byte* Genre;
    public byte* Date;
    public byte* Comment;
    public uint TrackNumber;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaEncoderOutput
{
    public delegate* unmanaged[Cdecl]<void*, void*, nuint, int> Write;
    public delegate* unmanaged[Cdecl]<void*, long, int> Seek;
    public delegate* unmanaged[Cdecl]<void*, void> Close;
}

[StructLayout(LayoutKind.Sequential)]
internal struct FaEncoderStatus
{
    public int State;
    public int Error;
    public int Paused;
    public long Frames;
    public long Duration;
    public long Bytes;
    public long DroppedFrames;
    public FaAudioFormat Input;
    public FaAudioFormat Output;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaLameInfo
{
    public int Loaded;
    public fixed byte Version[32];
    public fixed byte Path[512];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaSoundFontInfo
{
    public fixed byte Name[256];
    public uint PresetCount;
    public ulong SampleMemory;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaMidiText
{
    public long Frame;
    public long Time;
    public int Type;
    public uint Flags;
    public int Track;
    public fixed byte Text[256];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaMidiMark
{
    public long Frame;
    public long Time;
    public int Type;
    public int Track;
    public int Value;
    public int Value2;
    public fixed byte Text[32];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaTrackerInfo
{
    public uint StructSize;
    public fixed byte Type[16];
    public fixed byte TypeName[64];
    public fixed byte Tracker[64];
    public fixed byte Title[128];
    public fixed byte Artist[128];
    public uint Channels, Orders, Patterns, Instruments, Samples, Subsongs;
    public int RestartOrder, RestartRow;
    public double Duration;
    public int Subsong;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaMidiCredits
{
    public uint StructSize;
    public int CodePage;
    public fixed byte Title[256];
    public fixed byte Artist[256];
    public fixed byte Writer[256];
    public fixed byte Lyricist[256];
    public fixed byte Year[32];
    public fixed byte Copyright[256];
    public fixed byte Key[32];
    public fixed byte Tempo[32];
    public fixed byte TimeSignature[32];
    public fixed byte Other[1024];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaMidiInfo
{
    public fixed byte Title[256];
    public fixed byte Artist[256];
    public fixed byte Copyright[256];
    public int Format;
    public uint Tracks;
    public uint TextCount;
    public uint LyricCount;
    public int Karaoke;
}
