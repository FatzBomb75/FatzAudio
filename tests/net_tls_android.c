/* Android only, for test_net.c: an HTTPS front for the test's HTTP server, so the
 * URL stream tests also run over HTTPS through HttpURLConnection. The test starts
 * a Java VM of its own (an app has one already), and Java's SSLServerSocket
 * decrypts each connection and relays it to the plain server on 127.0.0.1. The
 * certificate (tests/data/net/tls, a throwaway self-signed one for 127.0.0.1) is
 * made the trusted one of HttpsURLConnection, as an app's network security config
 * would. */
#define _POSIX_C_SOURCE 200809L
#include "net_tls_android.h"
#include "fa_internal.h"
#include <arpa/inet.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <jni.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static JavaVM* g_vm;
static jobject g_server;          /* the SSLServerSocket */
static int g_backend_port;
static fa_thread g_accept;
#define MAX_RELAYS 256
static fa_thread g_relay_threads[MAX_RELAYS * 2];
static int g_relay_count;
static fa_mutex g_lock;

typedef struct relay {
    jobject sock;                 /* the SSLSocket (global ref) */
    int backend;                  /* the plain connection */
    fa_atomic32 refs;
} relay;

static JNIEnv* attach(void)
{
    JNIEnv* env = NULL;
    if ((*g_vm)->GetEnv(g_vm, (void**)&env, JNI_VERSION_1_6) == JNI_OK) return env;
    return (*g_vm)->AttachCurrentThread(g_vm, &env, NULL) == JNI_OK ? env : NULL;
}

static int failed(JNIEnv* env, const char* what)
{
    if (!(*env)->ExceptionCheck(env)) return 0;
    fprintf(stderr, "  java exception in %s:\n", what);
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    return 1;
}

static jobject call_static(JNIEnv* env, const char* cls, const char* name, const char* sig, ...)
{
    jclass c = (*env)->FindClass(env, cls);
    jmethodID m;
    jobject r;
    va_list ap;
    if (!c) return NULL;
    m = (*env)->GetStaticMethodID(env, c, name, sig);
    if (!m) return NULL;
    va_start(ap, sig);
    r = (*env)->CallStaticObjectMethodV(env, c, m, ap);
    va_end(ap);
    return r;
}

static void call_static_void(JNIEnv* env, const char* cls, const char* name, const char* sig, ...)
{
    jclass c = (*env)->FindClass(env, cls);
    jmethodID m;
    va_list ap;
    if (!c) return;
    m = (*env)->GetStaticMethodID(env, c, name, sig);
    if (!m) return;
    va_start(ap, sig);
    (*env)->CallStaticVoidMethodV(env, c, m, ap);
    va_end(ap);
}

static jobject call(JNIEnv* env, jobject o, const char* name, const char* sig, ...)
{
    jclass c = (*env)->GetObjectClass(env, o);
    jmethodID m = (*env)->GetMethodID(env, c, name, sig);
    jobject r;
    va_list ap;
    if (!m) return NULL;
    va_start(ap, sig);
    r = (*env)->CallObjectMethodV(env, o, m, ap);
    va_end(ap);
    return r;
}

static jint call_int(JNIEnv* env, jobject o, const char* name, const char* sig, ...)
{
    jclass c = (*env)->GetObjectClass(env, o);
    jmethodID m = (*env)->GetMethodID(env, c, name, sig);
    jint r;
    va_list ap;
    if (!m) return -1;
    va_start(ap, sig);
    r = (*env)->CallIntMethodV(env, o, m, ap);
    va_end(ap);
    return r;
}

static void call_void(JNIEnv* env, jobject o, const char* name, const char* sig, ...)
{
    jclass c = (*env)->GetObjectClass(env, o);
    jmethodID m = (*env)->GetMethodID(env, c, name, sig);
    va_list ap;
    if (!m) return;
    va_start(ap, sig);
    (*env)->CallVoidMethodV(env, o, m, ap);
    va_end(ap);
}

static jobject new_object(JNIEnv* env, const char* cls, const char* sig, ...)
{
    jclass c = (*env)->FindClass(env, cls);
    jmethodID m;
    jobject r;
    va_list ap;
    if (!c) return NULL;
    m = (*env)->GetMethodID(env, c, "<init>", sig);
    if (!m) return NULL;
    va_start(ap, sig);
    r = (*env)->NewObjectV(env, c, m, ap);
    va_end(ap);
    return r;
}

/* Starts the Java VM through libnativehelper, as app_process does. */
static int start_vm(void)
{
    typedef void* (*create_fn)(void);
    typedef int (*init_fn)(void*, const char*);
    typedef jint (*vm_fn)(JavaVM**, JNIEnv**, void*);
    void* lib = dlopen("libnativehelper.so", RTLD_NOW);
    union { void* p; create_fn f; } c;
    union { void* p; init_fn f; } i;
    union { void* p; vm_fn f; } v;
    JavaVMOption opts[3];
    static char boot[8192];
    const char* bcp = getenv("BOOTCLASSPATH");
    JavaVMInitArgs args;
    JNIEnv* env;
    if (!lib) { printf("  (no libnativehelper.so: %s)\n", dlerror()); return -1; }
    c.p = dlsym(lib, "JniInvocationCreate");
    i.p = dlsym(lib, "JniInvocationInit");
    v.p = dlsym(lib, "JNI_CreateJavaVM");
    if (!c.p || !i.p || !v.p) { printf("  (libnativehelper.so has no JNI invocation API)\n"); return -1; }
    i.f(c.f(), NULL);
    /* The boot classes that init names on a device. No boot image outside a booted
     * device: never compile one, interpret instead. */
    if (!bcp || !bcp[0]) { printf("  (BOOTCLASSPATH is not set)\n"); return -1; }
    snprintf(boot, sizeof(boot), "-Xbootclasspath:%s", bcp);
    opts[0].optionString = boot;
    opts[1].optionString = (char*)"-Xnoimage-dex2oat";
    opts[2].optionString = (char*)"-Xusejit:false";
    args.version = JNI_VERSION_1_6;
    args.nOptions = 3;
    args.options = opts;
    args.ignoreUnrecognized = JNI_TRUE;
    if (v.f(&g_vm, &env, &args) != JNI_OK) { printf("  (the Java VM did not start)\n"); g_vm = NULL; return -1; }
    return 0;
}

static jobject file_stream(JNIEnv* env, const char* path)
{
    jstring p = (*env)->NewStringUTF(env, path);
    return new_object(env, "java/io/FileInputStream", "(Ljava/lang/String;)V", p);
}

/* An SSLContext with the test certificate as key and as the only trusted one. */
static jobject make_context(JNIEnv* env, const char* dir)
{
    char path[1024];
    jstring type, alg, tls;
    jobject ks, kmf, ts, cf, cert, tmf, ctx;
    jcharArray pass = (*env)->NewCharArray(env, 9);
    jchar pw[9] = { 'f', 'a', 't', 'z', 'a', 'u', 'd', 'i', 'o' };
    (*env)->SetCharArrayRegion(env, pass, 0, 9, pw);

    type = (*env)->NewStringUTF(env, "PKCS12");
    ks = call_static(env, "java/security/KeyStore", "getInstance", "(Ljava/lang/String;)Ljava/security/KeyStore;", type);
    if (failed(env, "KeyStore") || !ks) return NULL;
    snprintf(path, sizeof(path), "%s/localhost.p12", dir);
    call_void(env, ks, "load", "(Ljava/io/InputStream;[C)V", file_stream(env, path), pass);
    if (failed(env, "KeyStore.load")) return NULL;
    alg = (jstring)call_static(env, "javax/net/ssl/KeyManagerFactory", "getDefaultAlgorithm", "()Ljava/lang/String;");
    kmf = call_static(env, "javax/net/ssl/KeyManagerFactory", "getInstance", "(Ljava/lang/String;)Ljavax/net/ssl/KeyManagerFactory;", alg);
    if (failed(env, "KeyManagerFactory") || !kmf) return NULL;
    call_void(env, kmf, "init", "(Ljava/security/KeyStore;[C)V", ks, pass);
    if (failed(env, "KeyManagerFactory.init")) return NULL;

    type = (jstring)call_static(env, "java/security/KeyStore", "getDefaultType", "()Ljava/lang/String;");
    ts = call_static(env, "java/security/KeyStore", "getInstance", "(Ljava/lang/String;)Ljava/security/KeyStore;", type);
    if (failed(env, "trust KeyStore") || !ts) return NULL;
    call_void(env, ts, "load", "(Ljava/io/InputStream;[C)V", NULL, NULL);
    cf = call_static(env, "java/security/cert/CertificateFactory", "getInstance",
                     "(Ljava/lang/String;)Ljava/security/cert/CertificateFactory;", (*env)->NewStringUTF(env, "X.509"));
    if (failed(env, "CertificateFactory") || !cf) return NULL;
    snprintf(path, sizeof(path), "%s/localhost.crt", dir);
    cert = call(env, cf, "generateCertificate", "(Ljava/io/InputStream;)Ljava/security/cert/Certificate;", file_stream(env, path));
    if (failed(env, "generateCertificate") || !cert) return NULL;
    call_void(env, ts, "setCertificateEntry", "(Ljava/lang/String;Ljava/security/cert/Certificate;)V",
              (*env)->NewStringUTF(env, "test"), cert);
    alg = (jstring)call_static(env, "javax/net/ssl/TrustManagerFactory", "getDefaultAlgorithm", "()Ljava/lang/String;");
    tmf = call_static(env, "javax/net/ssl/TrustManagerFactory", "getInstance", "(Ljava/lang/String;)Ljavax/net/ssl/TrustManagerFactory;", alg);
    if (failed(env, "TrustManagerFactory") || !tmf) return NULL;
    call_void(env, tmf, "init", "(Ljava/security/KeyStore;)V", ts);
    if (failed(env, "TrustManagerFactory.init")) return NULL;

    tls = (*env)->NewStringUTF(env, "TLS");
    ctx = call_static(env, "javax/net/ssl/SSLContext", "getInstance", "(Ljava/lang/String;)Ljavax/net/ssl/SSLContext;", tls);
    if (failed(env, "SSLContext") || !ctx) return NULL;
    call_void(env, ctx, "init", "([Ljavax/net/ssl/KeyManager;[Ljavax/net/ssl/TrustManager;Ljava/security/SecureRandom;)V",
              call(env, kmf, "getKeyManagers", "()[Ljavax/net/ssl/KeyManager;"),
              call(env, tmf, "getTrustManagers", "()[Ljavax/net/ssl/TrustManager;"), NULL);
    if (failed(env, "SSLContext.init")) return NULL;
    return ctx;
}

static void relay_release(JNIEnv* env, relay* r)
{
    if (fa_fetch_add32(&r->refs, -1) != 1) return;
    call_void(env, r->sock, "close", "()V");
    (*env)->ExceptionClear(env);
    (*env)->DeleteGlobalRef(env, r->sock);
    close(r->backend);
    free(r);
}

/* Client to server: decrypted request bytes to the plain server. */
static void pump_up(void* arg)
{
    relay* r = (relay*)arg;
    JNIEnv* env = attach();
    jobject in = call(env, r->sock, "getInputStream", "()Ljava/io/InputStream;");
    jbyteArray buf = (*env)->NewByteArray(env, 16384);
    jbyte tmp[16384];
    while (in && !(*env)->ExceptionCheck(env)) {
        jint n = call_int(env, in, "read", "([BII)I", buf, 0, 16384);
        if ((*env)->ExceptionCheck(env) || n < 0) break;
        (*env)->GetByteArrayRegion(env, buf, 0, n, tmp);
        if (send(r->backend, tmp, (size_t)n, MSG_NOSIGNAL) != n) break;
    }
    (*env)->ExceptionClear(env);
    shutdown(r->backend, SHUT_WR);
    (*env)->DeleteLocalRef(env, buf);
    if (in) (*env)->DeleteLocalRef(env, in);
    relay_release(env, r);
    (*g_vm)->DetachCurrentThread(g_vm);
}

/* Server to client: the plain response, encrypted. Closes both when either ends. */
static void pump_down(void* arg)
{
    relay* r = (relay*)arg;
    JNIEnv* env = attach();
    jobject out = call(env, r->sock, "getOutputStream", "()Ljava/io/OutputStream;");
    jbyteArray buf = (*env)->NewByteArray(env, 16384);
    char tmp[16384];
    while (out && !(*env)->ExceptionCheck(env)) {
        ssize_t n = recv(r->backend, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        (*env)->SetByteArrayRegion(env, buf, 0, (jsize)n, (const jbyte*)tmp);
        call_void(env, out, "write", "([BII)V", buf, 0, (jint)n);
        if ((*env)->ExceptionCheck(env)) break;
        call_void(env, out, "flush", "()V");
    }
    (*env)->ExceptionClear(env);
    /* Ends the request pump too (its read fails on the closed socket). */
    call_void(env, r->sock, "close", "()V");
    (*env)->ExceptionClear(env);
    shutdown(r->backend, SHUT_RDWR);
    (*env)->DeleteLocalRef(env, buf);
    if (out) (*env)->DeleteLocalRef(env, out);
    relay_release(env, r);
    (*g_vm)->DetachCurrentThread(g_vm);
}

static void accept_main(void* arg)
{
    JNIEnv* env = attach();
    (void)arg;
    for (;;) {
        jobject s = call(env, g_server, "accept", "()Ljava/net/Socket;");
        struct sockaddr_in a;
        relay* r;
        if ((*env)->ExceptionCheck(env) || !s) break;   /* closed by tls_front_stop */
        r = (relay*)calloc(1, sizeof(*r));
        r->sock = (*env)->NewGlobalRef(env, s);
        (*env)->DeleteLocalRef(env, s);
        r->backend = socket(AF_INET, SOCK_STREAM, 0);
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)g_backend_port);
        fa_store32(&r->refs, 2);
        fa_mutex_lock(&g_lock);
        if (connect(r->backend, (struct sockaddr*)&a, sizeof(a)) != 0 || g_relay_count + 2 > MAX_RELAYS * 2 ||
            fa_thread_create(&g_relay_threads[g_relay_count], pump_up, r) != 0) {
            fa_mutex_unlock(&g_lock);
            fa_store32(&r->refs, 1);
            relay_release(env, r);
            continue;
        }
        g_relay_count++;
        if (fa_thread_create(&g_relay_threads[g_relay_count], pump_down, r) == 0) g_relay_count++;
        else relay_release(env, r);
        fa_mutex_unlock(&g_lock);
    }
    (*env)->ExceptionClear(env);
    (*g_vm)->DetachCurrentThread(g_vm);
}

int tls_front_start(const char* cert_dir, int backend_port, int* out_port, void** out_vm)
{
    JNIEnv* env;
    jobject ctx, factory, addr;
    if (!g_vm && start_vm() != 0) return -1;
    env = attach();
    if (!env) return -1;
    g_backend_port = backend_port;
    if ((*env)->PushLocalFrame(env, 64) != JNI_OK) return -1;
    ctx = make_context(env, cert_dir);
    if (!ctx) { (*env)->PopLocalFrame(env, NULL); return -1; }
    call_static_void(env, "javax/net/ssl/HttpsURLConnection", "setDefaultSSLSocketFactory", "(Ljavax/net/ssl/SSLSocketFactory;)V",
                call(env, ctx, "getSocketFactory", "()Ljavax/net/ssl/SSLSocketFactory;"));
    factory = call(env, ctx, "getServerSocketFactory", "()Ljavax/net/ssl/SSLServerSocketFactory;");
    addr = call_static(env, "java/net/InetAddress", "getByName", "(Ljava/lang/String;)Ljava/net/InetAddress;",
                       (*env)->NewStringUTF(env, "127.0.0.1"));
    g_server = factory && addr ? call(env, factory, "createServerSocket", "(IILjava/net/InetAddress;)Ljava/net/ServerSocket;", 0, 64, addr) : NULL;
    if (failed(env, "createServerSocket") || !g_server) { (*env)->PopLocalFrame(env, NULL); g_server = NULL; return -1; }
    g_server = (*env)->NewGlobalRef(env, g_server);
    *out_port = call_int(env, g_server, "getLocalPort", "()I");
    (*env)->PopLocalFrame(env, NULL);
    fa_mutex_init(&g_lock);
    if (fa_thread_create(&g_accept, accept_main, NULL) != 0) return -1;
    *out_vm = g_vm;
    return 0;
}

void tls_front_stop(void)
{
    JNIEnv* env;
    int i;
    if (!g_server) return;
    env = attach();
    call_void(env, g_server, "close", "()V");
    (*env)->ExceptionClear(env);
    fa_thread_join(&g_accept);
    /* The plain server's shutdown ends every relay; here they are only collected. */
    fa_mutex_lock(&g_lock);
    for (i = 0; i < g_relay_count; i++) fa_thread_join(&g_relay_threads[i]);
    g_relay_count = 0;
    fa_mutex_unlock(&g_lock);
    (*env)->DeleteGlobalRef(env, g_server);
    g_server = NULL;
    fa_mutex_destroy(&g_lock);
}
