/* API functions that are exported but not implemented yet. Each returns
 * FA_ERROR_NOT_IMPLEMENTED so the shared library exposes the full API surface. */
#include "fa_internal.h"

#define FA_UNUSED(x) (void)(x)

FA_API fa_result FA_CALL fa_stream_add_position_event(fa_stream* stream, fa_frames frame)
{ FA_UNUSED(stream); FA_UNUSED(frame); return FA_ERROR_NOT_IMPLEMENTED; }

#if !defined(__ANDROID__)
/* Android only (net_https_android.c). */
FA_API fa_result FA_CALL fa_android_set_java_vm(void* java_vm)
{ FA_UNUSED(java_vm); return FA_ERROR_NOT_IMPLEMENTED; }
#endif
