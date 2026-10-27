/* HTTPS on Android: the platform's own java.net.HttpURLConnection, called through
 * JNI, so TLS uses the system's Conscrypt, certificate store, network security
 * config and proxy settings. Nothing is bundled or linked: the app's Java VM is
 * found at run time (JNI_GetCreatedJavaVMs, or fa_android_set_java_vm). */
#if defined(__ANDROID__)
#include "net.h"
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the Java VM --------------------------------------------------------------- */

static JavaVM* volatile g_vm;
static pthread_key_t g_env_key;
static pthread_once_t g_env_once = PTHREAD_ONCE_INIT;

FA_API fa_result FA_CALL fa_android_set_java_vm(void* java_vm)
{
    if (!java_vm) return FA_ERROR_INVALID_ARG;
    __atomic_store_n(&g_vm, (JavaVM*)java_vm, __ATOMIC_RELEASE);
    return FA_OK;
}

typedef jint (*get_vms_fn)(JavaVM** vms, jsize cap, jsize* count);

/* The app's VM: the one set by the app, else the one already running in the
 * process. libnativehelper exports JNI_GetCreatedJavaVMs to apps from API 31;
 * older versions keep it in libart.so, which apps may not open. */
static get_vms_fn g_get_vms;
static pthread_once_t g_get_vms_once = PTHREAD_ONCE_INIT;

static void get_vms_init(void)
{
    static const char* libs[] = { "libnativehelper.so", "libart.so" };
    size_t i;
    for (i = 0; i < sizeof(libs) / sizeof(libs[0]) && !g_get_vms; i++) {
        /* Stays loaded: it is part of the runtime and never unloads. */
        void* lib = dlopen(libs[i], RTLD_NOW | RTLD_LOCAL);
        union { void* p; get_vms_fn f; } u;
        if (!lib) continue;
        u.p = dlsym(lib, "JNI_GetCreatedJavaVMs");
        g_get_vms = u.f;
    }
}

static JavaVM* find_vm(void)
{
    JavaVM* vm = __atomic_load_n(&g_vm, __ATOMIC_ACQUIRE);
    jsize n = 0;
    if (vm) return vm;
    pthread_once(&g_get_vms_once, get_vms_init);
    /* Asked every time until there is one: the app may start its VM later. */
    if (!g_get_vms || g_get_vms(&vm, 1, &n) != JNI_OK || n < 1 || !vm) return NULL;
    __atomic_store_n(&g_vm, vm, __ATOMIC_RELEASE);
    return vm;
}

/* Threads this file attached to the VM detach when they end. */
static void detach_thread(void* vm) { (*(JavaVM*)vm)->DetachCurrentThread((JavaVM*)vm); }
static void env_key_init(void) { pthread_key_create(&g_env_key, detach_thread); }

/* The calling thread's JNIEnv, attaching the thread (a download thread) if needed. */
static JNIEnv* get_env(void)
{
    JavaVM* vm = find_vm();
    JNIEnv* env = NULL;
    jint r;
    if (!vm) return NULL;
    r = (*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6);
    if (r == JNI_OK) return env;
    if (r != JNI_EDETACHED) return NULL;
    {
        JavaVMAttachArgs args;
        args.version = JNI_VERSION_1_6;
        args.name = "FatzAudio net";
        args.group = NULL;
        if ((*vm)->AttachCurrentThread(vm, &env, &args) != JNI_OK) return NULL;
    }
    pthread_once(&g_env_once, env_key_init);
    pthread_setspecific(g_env_key, vm);
    return env;
}

/* ---- classes and methods ---------------------------------------------------------- */

static struct {
    fa_atomic32 state;             /* 0 not tried, 1 loading, 2 ready, 3 missing */
    jclass url, proxy, inet, system;
    jmethodID url_init, url_open, url_open_proxy;
    jobject no_proxy, type_http;   /* Proxy.NO_PROXY, Proxy.Type.HTTP */
    jmethodID proxy_init, inet_unresolved;
    jmethodID set_connect_timeout, set_read_timeout, set_use_caches, set_follow, add_property;
    jmethodID response_code, header_key, header_value, input_stream, disconnect;
    jmethodID read, close;
    jmethodID get_property;
} J;

static int clear_exception(JNIEnv* env)
{
    if (!(*env)->ExceptionCheck(env)) return 0;
    (*env)->ExceptionClear(env);
    return 1;
}

static jclass global_class(JNIEnv* env, const char* name)
{
    jclass local = (*env)->FindClass(env, name);
    jclass g;
    if (!local) { clear_exception(env); return NULL; }
    g = (jclass)(*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    return g;
}

static int load_classes(JNIEnv* env)
{
    jclass conn, stream, type;
    jfieldID f;
    jobject o;
    int ok;
    J.url = global_class(env, "java/net/URL");
    J.proxy = global_class(env, "java/net/Proxy");
    J.inet = global_class(env, "java/net/InetSocketAddress");
    J.system = global_class(env, "java/lang/System");
    conn = (*env)->FindClass(env, "java/net/HttpURLConnection");
    stream = (*env)->FindClass(env, "java/io/InputStream");
    type = (*env)->FindClass(env, "java/net/Proxy$Type");
    if (!J.url || !J.proxy || !J.inet || !J.system || !conn || !stream || !type) { clear_exception(env); return 0; }
    /* Each lookup only while none failed: no JNI call may run with an exception pending. */
    ok = 1;
#define M(dst, cls, name, sig) do { if (ok && !(dst = (*env)->GetMethodID(env, cls, name, sig))) ok = 0; } while (0)
#define SM(dst, cls, name, sig) do { if (ok && !(dst = (*env)->GetStaticMethodID(env, cls, name, sig))) ok = 0; } while (0)
    M(J.url_init, J.url, "<init>", "(Ljava/lang/String;)V");
    M(J.url_open, J.url, "openConnection", "()Ljava/net/URLConnection;");
    M(J.url_open_proxy, J.url, "openConnection", "(Ljava/net/Proxy;)Ljava/net/URLConnection;");
    M(J.proxy_init, J.proxy, "<init>", "(Ljava/net/Proxy$Type;Ljava/net/SocketAddress;)V");
    SM(J.inet_unresolved, J.inet, "createUnresolved", "(Ljava/lang/String;I)Ljava/net/InetSocketAddress;");
    M(J.set_connect_timeout, conn, "setConnectTimeout", "(I)V");
    M(J.set_read_timeout, conn, "setReadTimeout", "(I)V");
    M(J.set_use_caches, conn, "setUseCaches", "(Z)V");
    M(J.set_follow, conn, "setInstanceFollowRedirects", "(Z)V");
    M(J.add_property, conn, "addRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V");
    M(J.response_code, conn, "getResponseCode", "()I");
    M(J.header_key, conn, "getHeaderFieldKey", "(I)Ljava/lang/String;");
    M(J.header_value, conn, "getHeaderField", "(I)Ljava/lang/String;");
    M(J.input_stream, conn, "getInputStream", "()Ljava/io/InputStream;");
    M(J.disconnect, conn, "disconnect", "()V");
    M(J.read, stream, "read", "([BII)I");
    M(J.close, stream, "close", "()V");
    SM(J.get_property, J.system, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;");
#undef M
#undef SM
    if (!ok) { clear_exception(env); return 0; }
    f = (*env)->GetStaticFieldID(env, J.proxy, "NO_PROXY", "Ljava/net/Proxy;");
    o = f ? (*env)->GetStaticObjectField(env, J.proxy, f) : NULL;
    J.no_proxy = o ? (*env)->NewGlobalRef(env, o) : NULL;
    f = J.no_proxy ? (*env)->GetStaticFieldID(env, type, "HTTP", "Ljava/net/Proxy$Type;") : NULL;
    o = f ? (*env)->GetStaticObjectField(env, type, f) : NULL;
    J.type_http = o ? (*env)->NewGlobalRef(env, o) : NULL;
    ok = !clear_exception(env) && J.no_proxy && J.type_http;
    (*env)->DeleteLocalRef(env, conn);
    (*env)->DeleteLocalRef(env, stream);
    (*env)->DeleteLocalRef(env, type);
    return ok;
}

/* The JNIEnv of this thread once the classes are loaded, else NULL. */
static JNIEnv* java_ready(void)
{
    JNIEnv* env = get_env();
    if (!env) return NULL;
    for (;;) {
        int32_t st = fa_load32(&J.state);
        if (st == 2) return env;
        if (st == 3) return NULL;
        if (st == 0 && fa_cas32(&J.state, 0, 1)) {
            fa_store32(&J.state, load_classes(env) ? 2 : 3);
            continue;
        }
        fa_sleep_ms(1);
    }
}

/* A Java string of UTF-8 text. Bytes that are not valid UTF-8 are taken as
 * Latin-1 (header values), which NewStringUTF would reject. */
static jstring jstr(JNIEnv* env, const char* s, size_t n)
{
    jchar stack[512];
    jchar* w = n <= 512 ? stack : (jchar*)malloc(n * sizeof(jchar));
    const uint8_t* p = (const uint8_t*)s;
    size_t i = 0, k = 0;
    jstring out;
    if (!w) return NULL;
    while (i < n) {
        uint32_t c = p[i], len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        uint32_t j, cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
        int ok = len > 0 && i + len <= n;
        for (j = 1; ok && j < len; j++) {
            if ((p[i + j] & 0xC0) != 0x80) ok = 0;
            else cp = (cp << 6) | (p[i + j] & 0x3F);
        }
        if (ok && ((len == 2 && cp < 0x80) || (len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp < 0xE000))) ||
                   (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))))
            ok = 0;
        if (!ok) { w[k++] = (jchar)c; i++; continue; }   /* Latin-1 */
        if (cp >= 0x10000) { cp -= 0x10000; w[k++] = (jchar)(0xD800 + (cp >> 10)); w[k++] = (jchar)(0xDC00 + (cp & 0x3FF)); }
        else w[k++] = (jchar)cp;
        i += len;
    }
    out = (*env)->NewString(env, w, (jsize)k);
    if (w != stack) free(w);
    return out;
}

static jstring jstr0(JNIEnv* env, const char* s) { return jstr(env, s, strlen(s)); }

/* Copies a Java string as UTF-8 (modified UTF-8, the same for any header) into
 * out[cap]; "" if it does not fit. */
static void get_jstring(JNIEnv* env, jstring s, char* out, size_t cap)
{
    const char* c = (*env)->GetStringUTFChars(env, s, NULL);
    out[0] = 0;
    if (!c) { clear_exception(env); return; }
    if (strlen(c) < cap) strcpy(out, c);
    (*env)->ReleaseStringUTFChars(env, s, c);
}

/* ---- requests in flight -------------------------------------------------------------- */

/* So fa_http_abort can disconnect the connection a cancel flag belongs to: that
 * closes its socket, and a read or connect blocked in Java returns at once. */
#define FA_JNI_SLOTS 64
static fa_lazy_mutex g_slots_lock;
static struct { fa_atomic32* cancel; jobject conn; } g_slots[FA_JNI_SLOTS];

static int slot_add(JNIEnv* env, fa_atomic32* cancel, jobject conn)
{
    int i, found = -1;
    if (!cancel) return -1;
    fa_lazy_mutex_lock(&g_slots_lock);
    for (i = 0; i < FA_JNI_SLOTS; i++)
        if (!g_slots[i].conn) {
            g_slots[i].conn = (*env)->NewGlobalRef(env, conn);
            g_slots[i].cancel = g_slots[i].conn ? cancel : NULL;
            if (g_slots[i].conn) found = i;
            break;
        }
    fa_lazy_mutex_unlock(&g_slots_lock);
    return found;
}

static void slot_remove(JNIEnv* env, int i)
{
    if (i < 0) return;
    fa_lazy_mutex_lock(&g_slots_lock);
    (*env)->DeleteGlobalRef(env, g_slots[i].conn);
    g_slots[i].conn = NULL;
    g_slots[i].cancel = NULL;
    fa_lazy_mutex_unlock(&g_slots_lock);
}

void fa_http_abort(fa_atomic32* cancel)
{
    JavaVM* vm = __atomic_load_n(&g_vm, __ATOMIC_ACQUIRE);
    JNIEnv* env = NULL;
    jobject conns[4];
    int i, n = 0, attached = 0;
    /* A cleared flag means the transfer this call was meant for is over already. */
    if (!cancel || !fa_load32(cancel) || fa_load32(&J.state) != 2 || !vm) return;
    /* The caller is the app's thread (closing or seeking a stream): attached only
     * for this call. */
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return;
        attached = 1;
    } else if (!env) {
        return;
    }
    /* Own references, so the disconnects run outside the lock even if the
     * transfers end meanwhile (disconnecting a finished connection is harmless). */
    fa_lazy_mutex_lock(&g_slots_lock);
    for (i = 0; i < FA_JNI_SLOTS && n < 4; i++)
        if (g_slots[i].conn && g_slots[i].cancel == cancel && (conns[n] = (*env)->NewGlobalRef(env, g_slots[i].conn))) n++;
    fa_lazy_mutex_unlock(&g_slots_lock);
    for (i = 0; i < n; i++) {
        (*env)->CallVoidMethod(env, conns[i], J.disconnect);
        clear_exception(env);
        (*env)->DeleteGlobalRef(env, conns[i]);
    }
    if (attached) (*vm)->DetachCurrentThread(vm);
}

/* ---- one request -------------------------------------------------------------------- */

static void add_property(JNIEnv* env, jobject conn, const char* name, const char* value, size_t value_len)
{
    jstring k = jstr0(env, name), v = jstr(env, value, value_len);
    if (k && v) (*env)->CallVoidMethod(env, conn, J.add_property, k, v);
    clear_exception(env);
    if (k) (*env)->DeleteLocalRef(env, k);
    if (v) (*env)->DeleteLocalRef(env, v);
}

/* fa_http_each_header's sink: addRequestProperty, which sets a header not set yet. */
typedef struct property_sink {
    JNIEnv* env;
    jobject conn;
} property_sink;

static int add_header(void* user, const char* name, const char* value)
{
    property_sink* p = (property_sink*)user;
    add_property(p->env, p->conn, name, value, strlen(value));
    return 0;
}

/* A proxy from the request's setting: NULL = the system's (ProxySelector). */
static jobject make_proxy(JNIEnv* env, const char* setting)
{
    char host[256];
    const char* p;
    const char* at;
    size_t len;
    long port = 80;
    jstring h;
    jobject addr, proxy;
    if (!setting) return NULL;
    if (!setting[0]) return (*env)->NewLocalRef(env, J.no_proxy);
    /* "http://user:pass@host:port/" -> host, port (credentials are not passed on). */
    p = strstr(setting, "://");
    p = p ? p + 3 : setting;
    at = strchr(p, '@');
    if (at) p = at + 1;
    if (*p == '[') {                                   /* [IPv6]:port */
        const char* end = strchr(p, ']');
        if (!end) return NULL;
        len = (size_t)(end - p - 1);
        if (len >= sizeof(host)) return NULL;
        memcpy(host, p + 1, len);
        p = end + 1;
    } else {
        len = strcspn(p, ":/");
        if (len >= sizeof(host)) return NULL;
        memcpy(host, p, len);
        p += len;
    }
    host[len] = 0;
    if (*p == ':') port = strtol(p + 1, NULL, 10);
    if (!host[0] || port <= 0 || port > 65535) return NULL;
    h = jstr0(env, host);
    addr = h ? (*env)->CallStaticObjectMethod(env, J.inet, J.inet_unresolved, h, (jint)port) : NULL;
    proxy = addr && !clear_exception(env) ? (*env)->NewObject(env, J.proxy, J.proxy_init, J.type_http, addr) : NULL;
    clear_exception(env);
    return proxy;
}

/* Opens `url` and waits for the response. Returns the connection (a local ref) and
 * its status, or NULL with *out_r set. *out_slot: where fa_http_abort finds it
 * (from before it connects), for slot_remove. */
static jobject open_connection(JNIEnv* env, const fa_http_request* req, const char* url, int* out_status, fa_result* out_r,
                               int* out_slot)
{
    jint timeout = req->timeout_ms > 0x7FFFFFFFu ? 0x7FFFFFFF : (jint)req->timeout_ms;
    jstring jurl = jstr0(env, url);
    jobject u = jurl ? (*env)->NewObject(env, J.url, J.url_init, jurl) : NULL;
    jobject proxy, conn;
    fa_url parsed;
    if (!u || clear_exception(env)) { clear_exception(env); *out_r = FA_ERROR_INVALID_ARG; return NULL; }
    proxy = make_proxy(env, req->proxy);
    conn = proxy ? (*env)->CallObjectMethod(env, u, J.url_open_proxy, proxy) : (*env)->CallObjectMethod(env, u, J.url_open);
    if (!conn || clear_exception(env)) { clear_exception(env); *out_r = FA_ERROR_NETWORK; return NULL; }
    *out_slot = slot_add(env, req->cancel, conn);
    (*env)->CallVoidMethod(env, conn, J.set_connect_timeout, timeout);
    (*env)->CallVoidMethod(env, conn, J.set_read_timeout, timeout);
    (*env)->CallVoidMethod(env, conn, J.set_use_caches, JNI_FALSE);
    (*env)->CallVoidMethod(env, conn, J.set_follow, JNI_FALSE);   /* followed here, also to http:// */
    clear_exception(env);
    {
        /* HttpURLConnection ignores credentials in the URL: the walk sends them. */
        property_sink sink;
        sink.env = env;
        sink.conn = conn;
        if (fa_http_each_header(req, fa_url_parse(url, &parsed) == FA_OK ? &parsed : NULL, add_header, &sink) != FA_OK) {
            (*env)->CallVoidMethod(env, conn, J.disconnect);
            clear_exception(env);
            *out_r = FA_ERROR_OUT_OF_MEMORY;
            return NULL;
        }
    }
    *out_status = (*env)->CallIntMethod(env, conn, J.response_code);
    if (clear_exception(env) || *out_status < 100) {
        (*env)->CallVoidMethod(env, conn, J.disconnect);
        clear_exception(env);
        *out_r = FA_ERROR_NETWORK;
        return NULL;
    }
    return conn;
}

/* Appends a Java string to out (malloc'd, NUL-terminated, grown as needed). */
static int append_jstring(JNIEnv* env, jstring s, char** out, size_t* n, size_t* cap, const char* suffix)
{
    const char* c = (*env)->GetStringUTFChars(env, s, NULL);
    size_t len, slen = strlen(suffix);
    if (!c) { clear_exception(env); return 0; }
    len = strlen(c);
    if (*n + len + slen + 1 > *cap) {
        size_t ncap = (*n + len + slen + 1) * 2;
        char* p = (char*)realloc(*out, ncap);
        if (!p) { (*env)->ReleaseStringUTFChars(env, s, c); return 0; }
        *out = p;
        *cap = ncap;
    }
    memcpy(*out + *n, c, len);
    memcpy(*out + *n + len, suffix, slen + 1);
    *n += len + slen;
    (*env)->ReleaseStringUTFChars(env, s, c);
    return 1;
}

/* The response headers as "Name: value\r\n" lines (malloc'd). NULL with *err set
 * when out of memory or longer than FA_HTTP_HEAD_MAX, as the other backends. */
static char* collect_headers(JNIEnv* env, jobject conn, fa_result* err)
{
    size_t cap = 4096, n = 0;
    char* out = (char*)malloc(cap);
    jint i;
    *err = FA_ERROR_OUT_OF_MEMORY;
    if (!out) return NULL;
    out[0] = 0;
    for (i = 0; i < 1024; i++) {
        jstring k = (jstring)(*env)->CallObjectMethod(env, conn, J.header_key, i);
        jstring v = clear_exception(env) ? NULL : (jstring)(*env)->CallObjectMethod(env, conn, J.header_value, i);
        int ok = 1;
        clear_exception(env);
        if (!v) { if (k) (*env)->DeleteLocalRef(env, k); break; }
        if (k) {   /* index 0 is the status line, without a name */
            size_t line = n;
            ok = append_jstring(env, k, &out, &n, &cap, ": ") && append_jstring(env, v, &out, &n, &cap, "\r\n");
            if (!ok) { n = line; out[n] = 0; }   /* never half a line */
            (*env)->DeleteLocalRef(env, k);
        }
        (*env)->DeleteLocalRef(env, v);
        if (!ok) break;
        if (n > FA_HTTP_HEAD_MAX) { free(out); *err = FA_ERROR_NETWORK; return NULL; }
    }
    return out;
}

/* Delivers the response and streams the body. *out_redirect is set (malloc'd,
 * absolute) instead for a redirect. *out_whole: the body was read to its end, so
 * the connection can go back to the pool. */
static fa_result run_response(JNIEnv* env, const fa_http_request* req, const char* url, jobject conn, int status,
                              fa_http_head_proc on_head, fa_http_data_proc on_data, void* user, char** out_redirect,
                              int* out_whole)
{
    fa_result head_err = FA_OK;
    char* headers = collect_headers(env, conn, &head_err);
    fa_http_response resp;
    jobject in;
    jbyteArray buf;
    jbyte* chunk;
    fa_result r = FA_OK;
    if (!headers) return head_err;
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        char loc[FA_NET_URL_MAX];
        if (fa_http_header(headers, "Location", loc, sizeof(loc))) {
            char* target = (char*)malloc(FA_NET_URL_MAX);
            if (target && fa_url_resolve(url, loc, target, FA_NET_URL_MAX) == FA_OK) {
                free(headers);
                *out_redirect = target;
                return FA_OK;
            }
            free(target);
        }
    }
    memset(&resp, 0, sizeof(resp));
    resp.status = status;
    resp.headers = headers;
    resp.final_url = url;
    fa_http_fill_lengths(&resp);
    if (on_head && on_head(user, &resp)) { free(headers); return FA_OK; }
    free(headers);
    if (status >= 400) return FA_ERROR_NOT_FOUND;

    in = (*env)->CallObjectMethod(env, conn, J.input_stream);
    if (!in || clear_exception(env)) { clear_exception(env); return FA_ERROR_NETWORK; }
    buf = (*env)->NewByteArray(env, 65536);
    chunk = (jbyte*)malloc(65536);
    if (!buf || !chunk) { clear_exception(env); r = FA_ERROR_OUT_OF_MEMORY; goto done; }
    for (;;) {
        jint got;
        if (req->cancel && fa_load32(req->cancel)) { r = FA_ERROR_BUSY; break; }
        got = (*env)->CallIntMethod(env, in, J.read, buf, 0, 65536);
        if (clear_exception(env)) { r = FA_ERROR_NETWORK; break; }   /* also a body cut short */
        if (got < 0) { r = FA_OK; *out_whole = 1; break; }
        if (got == 0) continue;
        (*env)->GetByteArrayRegion(env, buf, 0, got, chunk);
        if (on_data(user, (const uint8_t*)chunk, (size_t)got)) { r = FA_OK; break; }
    }
done:
    (*env)->CallVoidMethod(env, in, J.close);
    clear_exception(env);
    free(chunk);
    return r;
}

fa_result fa_http_fetch_tls(const fa_http_request* req, fa_http_head_proc on_head, fa_http_data_proc on_data, void* user)
{
    JNIEnv* env = java_ready();
    fa_http_request cur = *req;
    char* owned = NULL;
    fa_result r = FA_ERROR_NETWORK;
    int hops;
    if (!env) return FA_ERROR_PROVIDER_MISSING;
    for (hops = 0; hops <= FA_NET_MAX_REDIRECTS; hops++) {
        char* next = NULL;
        fa_url u;
        if (fa_url_parse(cur.url, &u) != FA_OK) { r = FA_ERROR_INVALID_ARG; break; }
        if (strcmp(u.scheme, "https") != 0) {
            r = fa_http_fetch_socket(&cur, on_head, on_data, user, &next);
        } else if ((*env)->PushLocalFrame(env, 32) != JNI_OK) {
            clear_exception(env);
            r = FA_ERROR_OUT_OF_MEMORY;
        } else {
            int status = 0, slot = -1, whole = 0;
            jobject conn = NULL;
            if (req->cancel && fa_load32(req->cancel)) r = FA_ERROR_BUSY;
            else conn = open_connection(env, &cur, cur.url, &status, &r, &slot);
            if (conn) {
                r = run_response(env, &cur, cur.url, conn, status, on_head, on_data, user, &next, &whole);
                if (!whole) {   /* a body left unread (a redirect, an error, a stop) closes the socket */
                    (*env)->CallVoidMethod(env, conn, J.disconnect);
                    clear_exception(env);
                }
            }
            slot_remove(env, slot);
            (*env)->PopLocalFrame(env, NULL);
        }
        if (req->cancel && fa_load32(req->cancel)) { free(next); r = FA_ERROR_BUSY; break; }
        if (r != FA_OK || !next) { free(next); break; }
        free(owned);
        owned = next;
        cur.url = owned;
        r = FA_ERROR_NETWORK;    /* too many redirects, unless a later hop answers */
    }
    free(owned);
    return r;
}

static void system_property(JNIEnv* env, const char* name, char* out, size_t cap)
{
    jstring k = jstr0(env, name);
    jstring v = k ? (jstring)(*env)->CallStaticObjectMethod(env, J.system, J.get_property, k) : NULL;
    out[0] = 0;
    if (!clear_exception(env) && v) get_jstring(env, v, out, cap);
}

/* 1 if `host` matches a pattern of http.nonProxyHosts ("a.com|*.lan|192.168.*"). */
static int non_proxy_host(const char* list, const char* host)
{
    size_t hl = strlen(host);
    while (*list) {
        size_t len = strcspn(list, "|");
        const char* star = (const char*)memchr(list, '*', len);
        int match;
        if (!star) match = len == hl && fa_strncasecmp_ascii(list, host, len) == 0;
        else {
            /* One '*': what comes before it is a prefix of the host, what follows a suffix. */
            size_t pre = (size_t)(star - list), post = len - pre - 1;
            match = pre + post <= hl && fa_strncasecmp_ascii(list, host, pre) == 0 &&
                    fa_strncasecmp_ascii(star + 1, host + hl - post, post) == 0;
        }
        if (match && len > 0) return 1;
        list += len;
        if (*list == '|') list++;
    }
    return 0;
}

/* The system proxy for plain http:// (used by the socket client): Android puts
 * the proxy of the active network in the http.proxyHost / http.proxyPort /
 * http.nonProxyHosts properties of every app's VM. */
int fa_net_system_proxy(const char* scheme, const char* host, char* out, size_t cap)
{
    JNIEnv* env;
    char h[256], port[16], skip[1024];
    int found = 0;
    (void)scheme;
    env = java_ready();
    if (!env || (*env)->PushLocalFrame(env, 8) != JNI_OK) return 0;
    system_property(env, "http.proxyHost", h, sizeof(h));
    system_property(env, "http.proxyPort", port, sizeof(port));
    system_property(env, "http.nonProxyHosts", skip, sizeof(skip));
    (*env)->PopLocalFrame(env, NULL);
    if (h[0] && !(host && non_proxy_host(skip, host))) {
        size_t n = (size_t)snprintf(out, cap, "%s:%s", h, port[0] ? port : "80");
        found = n < cap;
    }
    return found;
}

#endif
