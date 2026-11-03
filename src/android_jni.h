/* The app's Java VM on Android (net_https_android.c): HTTPS and text shaping call
 * the platform's Java classes through it. */
#ifndef FA_ANDROID_JNI_H
#define FA_ANDROID_JNI_H

#include <jni.h>

/* The calling thread's JNIEnv, attaching the thread to the VM if needed (it is
 * detached when the thread ends). NULL when the process has no VM (a command-line
 * program) or attaching failed. */
JNIEnv* fa_android_env(void);

#endif
