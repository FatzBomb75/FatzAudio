/* API functions this platform doesn't have. Each returns FA_ERROR_NOT_IMPLEMENTED
 * so the shared library exposes the full API surface everywhere. */
#include "fa_internal.h"

#define FA_UNUSED(x) (void)(x)

#if !defined(__ANDROID__)
/* Android only (net_https_android.c). */
FA_API fa_result FA_CALL fa_android_set_java_vm(void* java_vm)
{ FA_UNUSED(java_vm); return FA_ERROR_NOT_IMPLEMENTED; }
#endif
