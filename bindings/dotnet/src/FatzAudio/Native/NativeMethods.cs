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
    public static partial int fa_device_set_mute(DeviceHandle device, int muted);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_device_get_mute(DeviceHandle device, out int muted);

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
    public static partial int fa_stream_push_frame(StreamHandle stream, FaAudioFrame* frame);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void fa_codec_params_init(FaCodecParams* parameters);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_open_packets(EngineHandle engine, FaCodecParams* parameters, FaOpenOptions* options, out nint stream);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_push_packet(StreamHandle stream, FaPacket* packet);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_push_end(StreamHandle stream);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_push_flush(StreamHandle stream);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_buffered(StreamHandle stream, out long duration);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void fa_stream_close(nint stream);

    // ---- Stream info -------------------------------------------------------
    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_info(StreamHandle stream, out FaStreamInfo info);

    // ---- Web streams -------------------------------------------------------
    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void fa_net_options_init(FaNetOptions* options);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_android_set_java_vm(nint javaVm);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_metadata(StreamHandle stream, FaMetadata* metadata);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_net_status(StreamHandle stream, FaNetStatus* status);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_http_header(StreamHandle stream, string name, byte* value, nuint valueSize);

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
    public static partial int fa_stream_seek_async(StreamHandle stream, long frame);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_position(StreamHandle stream, out long frame);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_clock(StreamHandle stream, out long pts);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_loop(StreamHandle stream, int on);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_loop(StreamHandle stream, out int on);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_mute(StreamHandle stream, int muted);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_mute(StreamHandle stream, out int muted);

    // ---- CD+G --------------------------------------------------------------
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_load_cdg_path(StreamHandle stream, string? path);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_load_cdg_memory(StreamHandle stream, void* data, nuint size);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_cdg_offset(StreamHandle stream, long offset);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_cdg_offset(StreamHandle stream, out long offset);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_cdg_info(StreamHandle stream, out FaCdgInfo info);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_cdg_events(StreamHandle stream, int enabled);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_dynamic_range(StreamHandle stream, int mode);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_dynamic_range(StreamHandle stream, out int mode);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void fa_cdg_surface_config_init(FaCdgSurfaceConfig* config);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_cdg_surface_create(StreamHandle stream, FaCdgSurfaceConfig* config, out nint surface);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void fa_cdg_surface_destroy(nint surface);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_cdg_surface_update(nint surface, FaCdgFrame* frame);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_cdg_surface_update_at(nint surface, long time, FaCdgFrame* frame);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_read(StreamHandle stream, void* buffer, long frames, out long framesRead);

    // ---- Parameters and envelopes -----------------------------------------
    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_param(StreamHandle stream, int param, float value);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_param(StreamHandle stream, int param, out float value);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_ramp_param(StreamHandle stream, int param, float target, long duration, long deviceTime, FaRamp* ramp);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void fa_tempo_options_init(FaTempoOptions* options);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_tempo_options(StreamHandle stream, FaTempoOptions* options);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_tempo_options(StreamHandle stream, FaTempoOptions* options);

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

    // ---- Tags --------------------------------------------------------------
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_tags_read_path(string path, uint flags, out nint tags);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_tags_read_memory(void* data, nuint size, uint flags, out nint tags);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_tags(StreamHandle stream, uint flags, out nint tags);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void fa_tags_close(nint tags);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial nint fa_tags_get(nint tags, int field);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial uint fa_tags_get_item_count(nint tags);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_tags_get_item(nint tags, uint index, out FaTagItem item);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial uint fa_tags_get_picture_count(nint tags);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_tags_get_picture(nint tags, uint index, out FaTagPicture picture);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_tags_get_info(nint tags, out FaStreamInfo info);

    // ---- MIDI --------------------------------------------------------------
    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_soundfont_open_path(string path, out nint font);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_soundfont_open_memory(void* data, nuint size, out nint font);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_soundfont_open_system(out nint font);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void fa_soundfont_close(nint font);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_soundfont_get_info(SoundFontHandle font, FaSoundFontInfo* info);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_engine_set_soundfont(EngineHandle engine, nint font);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_tracker_info(StreamHandle stream, FaTrackerInfo* info);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_tracker_text(StreamHandle stream, int type, uint index, byte* buffer, nuint capacity, nuint* length);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_tracker_time(StreamHandle stream, uint order, uint row, out double seconds);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_seek_tracker(StreamHandle stream, uint order, uint row);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_tracker_param(StreamHandle stream, int param, int value);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_tracker_param(StreamHandle stream, int param, out int value);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_tracker_channel_mute(StreamHandle stream, uint channel, int muted);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_tracker_channel_mute(StreamHandle stream, uint channel, out int muted);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_midi_info(StreamHandle stream, FaMidiInfo* info);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_midi_text(StreamHandle stream, uint index, FaMidiText* text);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_midi_marks(StreamHandle stream, FaMidiMark* marks, uint capacity, uint* count);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_midi_transpose(StreamHandle stream, int semitones);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_midi_transpose(StreamHandle stream, out int semitones);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_midi_channel_mute(StreamHandle stream, uint channel, int muted);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_midi_channel_mute(StreamHandle stream, uint channel, out int muted);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_midi_effects(StreamHandle stream, int enabled);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_midi_effects(StreamHandle stream, out int enabled);

    // ---- Events ------------------------------------------------------------
    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_event_callback(StreamHandle stream, delegate* unmanaged[Cdecl]<FaEvent*, void*, void> callback, void* user);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_add_position_event(StreamHandle stream, long frame);

    // ---- User DSP ----------------------------------------------------------
    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_add_dsp(StreamHandle stream, delegate* unmanaged[Cdecl]<float*, long, uint, uint, void*, void> callback, void* user, int priority, out uint dspId);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_eq(StreamHandle stream, FaEqBand* bands, uint count);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_eq(StreamHandle stream, FaEqBand* bands, uint capacity, out uint count);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_eq_gain(StreamHandle stream, uint band, float gainDb);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_set_eq_enabled(StreamHandle stream, int enabled);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_eq_enabled(StreamHandle stream, out int enabled);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_eq_get_preset(int preset, FaEqBand* bands, uint* count);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial byte* fa_eq_preset_name(int preset);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_remove_dsp(StreamHandle stream, uint dspId);

    // ---- Analysis ----------------------------------------------------------
    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_levels(StreamHandle stream, long window, float* peak, float* rms, uint channels);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_device_get_levels(DeviceHandle device, long window, float* peak, float* rms, uint channels);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_get_spectrum(StreamHandle stream, uint size, uint flags, float* bins, float* binHz);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_device_get_spectrum(DeviceHandle device, uint size, uint flags, float* bins, float* binHz);

    // ---- Audio CDs ---------------------------------------------------------
    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_disc_drive_count(out uint count);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_disc_drive_get_info(uint index, out FaDiscDriveInfo info);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void fa_disc_options_init(FaDiscOptions* options);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_disc_open(string? driveId, FaDiscOptions* options, out nint disc);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void fa_disc_close(nint disc);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_disc_get_status(DiscHandle disc, out FaDiscStatus status);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_disc_get_toc(DiscHandle disc, FaDiscToc* toc);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_disc_get_text(DiscHandle disc, int track, int field, byte* text, nuint size);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_disc_eject(DiscHandle disc);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_disc_load(DiscHandle disc);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_disc_set_locked(DiscHandle disc, int locked);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_disc_set_speed(DiscHandle disc, float speed);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_disc_get_speed(DiscHandle disc, out float speed);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_stream_open_disc(EngineHandle engine, DiscHandle disc, int track, FaOpenOptions* options, out nint stream);
    // ---- Encoding ----------------------------------------------------------
    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial void fa_encoder_options_init(FaEncoderOptions* options);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_encoder_create_path(string path, FaEncoderOptions* options, out nint encoder);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_encoder_create_output(FaEncoderOutput* output, void* user, FaEncoderOptions* options, out nint encoder);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_encoder_attach_device(EncoderHandle encoder, DeviceHandle device);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_encoder_attach_stream(EncoderHandle encoder, StreamHandle stream);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_encoder_encode_stream(EncoderHandle encoder, StreamHandle stream);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_encoder_write(EncoderHandle encoder, float* interleaved, long frames, uint sampleRate, uint channels);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_encoder_set_paused(EncoderHandle encoder, int paused);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_encoder_get_status(EncoderHandle encoder, out FaEncoderStatus status);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_encoder_wait(EncoderHandle encoder, long timeout);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_encoder_close(nint encoder);

    [LibraryImport(Lib, StringMarshalling = StringMarshalling.Utf8)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_lame_load(string? directory);

    [LibraryImport(Lib)] [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial int fa_lame_get_info(out FaLameInfo info);
}
