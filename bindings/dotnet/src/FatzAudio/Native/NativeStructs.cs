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
}

[StructLayout(LayoutKind.Sequential)]
internal struct FaRamp
{
    public int Curve;
    public float BezierX1, BezierY1, BezierX2, BezierY2;
}

[StructLayout(LayoutKind.Sequential)]
internal struct FaLoudness
{
    public double IntegratedLufs;
    public double RangeLu;
    public double TruePeakDbtp;
    public int IsFinal;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct FaEvent
{
    public int Type;
    public void* Stream;
    public long Frame;
    public long Value;
}
