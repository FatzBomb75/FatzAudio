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
internal unsafe struct FaEngineConfig
{
    public uint StructSize;
    public uint MixSampleRate;
    public uint PeriodMs;
    public uint WorkerThreads;
    public byte* CacheDirectory;
    public uint Flags;
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
internal unsafe struct FaFfmpegInfo
{
    public int Loaded;
    public uint AvcodecVersion;
    public uint AvutilVersion;
    public fixed byte AvcodecPath[512];
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
}
