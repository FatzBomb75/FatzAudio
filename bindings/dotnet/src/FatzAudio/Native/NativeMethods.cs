using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace FatzAudio.Native;

/// <summary>Source-generated P/Invoke declarations for every function of <c>fatzaudio.h</c>.</summary>
internal static unsafe partial class NativeMethods
{
    private const string Lib = "fatzaudio";

    // ---- Library -----------------------------------------------------------
    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial uint fa_version();

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial nint fa_result_string(int result);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void fa_engine_config_init(FaEngineConfig* config);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_engine_create(FaEngineConfig* config, out nint engine);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void fa_engine_destroy(nint engine);

    // ---- Supported formats -------------------------------------------------
    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial uint fa_format_count();

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_format_get_info(uint index, out FaFormatInfo info);

    // ---- Devices -----------------------------------------------------------
    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_device_count(EngineHandle engine, int type, out uint count);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_device_get_info(EngineHandle engine, int type, uint index, out FaDeviceInfo info);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_device_open(EngineHandle engine, string? deviceId, out nint device);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void fa_device_close(nint device);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_device_start(DeviceHandle device);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_device_stop(DeviceHandle device);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_device_is_started(DeviceHandle device, out int started);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_device_set_volume(DeviceHandle device, float volume);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_device_get_volume(DeviceHandle device, out float volume);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_device_get_latency(DeviceHandle device, out long latency);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_device_get_time(DeviceHandle device, out long time);

    // ---- Opening streams ---------------------------------------------------
    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void fa_open_options_init(FaOpenOptions* options);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_open_path(EngineHandle engine, string path, FaOpenOptions* options, out nint stream);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_open_memory(EngineHandle engine, void* data, nuint size, FaOpenOptions* options, out nint stream);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_open_io(EngineHandle engine, FaIoCallbacks* io, void* user, FaOpenOptions* options, out nint stream);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_open_url(EngineHandle engine, string url, FaOpenOptions* options, out nint stream);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_open_push(EngineHandle engine, FaAudioFormat* format, FaOpenOptions* options, out nint stream);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_push_pcm(StreamHandle stream, void** planes, long frames, long pts);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void fa_stream_close(nint stream);

    // ---- Stream info -------------------------------------------------------
    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_info(StreamHandle stream, out FaStreamInfo info);

    // ---- Transport and position -------------------------------------------
    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_attach(StreamHandle stream, DeviceHandle device, nint bus);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_detach(StreamHandle stream);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_play(StreamHandle stream);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_play_at(StreamHandle stream, long deviceTime);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_pause(StreamHandle stream);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_stop(StreamHandle stream);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_state(StreamHandle stream, out int state);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_seek(StreamHandle stream, long frame);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_position(StreamHandle stream, out long frame);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_clock(StreamHandle stream, out long pts);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_read(StreamHandle stream, void* buffer, long frames, out long framesRead);

    // ---- Parameters and envelopes -----------------------------------------
    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_param(StreamHandle stream, int param, float value);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_param(StreamHandle stream, int param, out float value);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_ramp_param(StreamHandle stream, int param, float target, long duration, long deviceTime, FaRamp* ramp);

    // ---- Markers and crossfade --------------------------------------------
    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_marker(StreamHandle stream, int marker, long frame);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_marker(StreamHandle stream, int marker, out long frame);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_fade_curve(StreamHandle stream, int fade, FaRamp* shape);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_crossfade(StreamHandle from, StreamHandle to, long duration, FaRamp* ramp);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_next(StreamHandle stream, nint next, long crossfade, FaRamp* shape);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_next(StreamHandle stream, out nint next);

    // ---- Mix buses and the channel matrix -----------------------------------
    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_bus_create(DeviceHandle device, out nint bus);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void fa_bus_destroy(nint bus);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_bus_set_param(BusHandle bus, int param, float value);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_bus_get_param(BusHandle bus, int param, out float value);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_bus_ramp_param(BusHandle bus, int param, float target, long duration, long deviceTime, FaRamp* ramp);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_matrix(StreamHandle stream, float* weights, uint inChannels, uint outChannels);

    // ---- Normalization -----------------------------------------------------
    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_engine_set_normalization(EngineHandle engine, int mode, double targetLufs);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_engine_set_limiter(EngineHandle engine, int mode, double ceilingDbtp);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_loudness(StreamHandle stream, out FaLoudness loudness);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_loudness(StreamHandle stream, in FaLoudness loudness);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_scan_loudness_path(EngineHandle engine, string path, out FaLoudness loudness);

    // ---- Events ------------------------------------------------------------
    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_event_callback(StreamHandle stream, delegate* unmanaged[Cdecl]<FaEvent*, void*, void> callback, void* user);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_add_position_event(StreamHandle stream, long frame);

    // ---- User DSP ----------------------------------------------------------
    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_add_dsp(StreamHandle stream, delegate* unmanaged[Cdecl]<float*, long, uint, void*, void> callback, void* user, int priority, out uint dspId);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_remove_dsp(StreamHandle stream, uint dspId);

    // ---- Analysis ----------------------------------------------------------
    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_levels(StreamHandle stream, long window, float* peak, float* rms, uint channels);
}
