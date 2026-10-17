/* API functions that are exported but not implemented yet. Each returns
 * FA_ERROR_NOT_IMPLEMENTED so the shared library exposes the full API surface. */
#include "fa_internal.h"

#define FA_UNUSED(x) (void)(x)

FA_API fa_result FA_CALL fa_stream_open_url(fa_engine* engine, const char* url, const fa_open_options* options, fa_stream** out_stream)
{ FA_UNUSED(engine); FA_UNUSED(url); FA_UNUSED(options); if (out_stream) *out_stream = NULL; return FA_ERROR_NOT_IMPLEMENTED; }

FA_API fa_result FA_CALL fa_stream_open_push(fa_engine* engine, const fa_audio_format* format, const fa_open_options* options, fa_stream** out_stream)
{ FA_UNUSED(engine); FA_UNUSED(format); FA_UNUSED(options); if (out_stream) *out_stream = NULL; return FA_ERROR_NOT_IMPLEMENTED; }

FA_API fa_result FA_CALL fa_stream_push_pcm(fa_stream* stream, const void* const* planes, fa_frames frames, fa_time pts)
{ FA_UNUSED(stream); FA_UNUSED(planes); FA_UNUSED(frames); FA_UNUSED(pts); return FA_ERROR_NOT_IMPLEMENTED; }

FA_API fa_result FA_CALL fa_stream_play_at(fa_stream* stream, fa_time engine_time)
{ FA_UNUSED(stream); FA_UNUSED(engine_time); return FA_ERROR_NOT_IMPLEMENTED; }

FA_API fa_result FA_CALL fa_engine_get_time(fa_engine* engine, fa_time* out_time)
{ FA_UNUSED(engine); FA_UNUSED(out_time); return FA_ERROR_NOT_IMPLEMENTED; }

FA_API fa_result FA_CALL fa_crossfade(fa_stream* from, fa_stream* to, fa_time duration, const fa_ramp* ramp)
{ FA_UNUSED(from); FA_UNUSED(to); FA_UNUSED(duration); FA_UNUSED(ramp); return FA_ERROR_NOT_IMPLEMENTED; }

FA_API fa_result FA_CALL fa_stream_add_position_event(fa_stream* stream, fa_frames frame)
{ FA_UNUSED(stream); FA_UNUSED(frame); return FA_ERROR_NOT_IMPLEMENTED; }

FA_API fa_result FA_CALL fa_stream_add_dsp(fa_stream* stream, fa_dsp_callback callback, void* user, int32_t priority, uint32_t* out_dsp_id)
{ FA_UNUSED(stream); FA_UNUSED(callback); FA_UNUSED(user); FA_UNUSED(priority); FA_UNUSED(out_dsp_id); return FA_ERROR_NOT_IMPLEMENTED; }

FA_API fa_result FA_CALL fa_stream_remove_dsp(fa_stream* stream, uint32_t dsp_id)
{ FA_UNUSED(stream); FA_UNUSED(dsp_id); return FA_ERROR_NOT_IMPLEMENTED; }

FA_API fa_result FA_CALL fa_stream_get_levels(fa_stream* stream, fa_time window, float* out_peak, float* out_rms, uint32_t channels)
{ FA_UNUSED(stream); FA_UNUSED(window); FA_UNUSED(out_peak); FA_UNUSED(out_rms); FA_UNUSED(channels); return FA_ERROR_NOT_IMPLEMENTED; }
