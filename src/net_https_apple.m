/* HTTPS on macOS and iOS: URLSession, so TLS, certificates, proxies and App
 * Transport Security follow the system. The delegate runs on a private queue and
 * hands the body to the download thread through a small buffer; when the buffer
 * is full the delegate waits, which pushes back on the connection. */
#if defined(__APPLE__)
#import <Foundation/Foundation.h>
#import <CFNetwork/CFNetwork.h>
#include "net.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define FA_APPLE_PENDING_MAX (512 * 1024)

/* The delegate of a stream's session. Requests run one at a time: `current` is the
 * task they are for, and callbacks of an earlier (cancelled) task are dropped. */
@interface FAFetchDelegate : NSObject <NSURLSessionDataDelegate>
{
@public
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    NSURLSessionTask* current;
    NSMutableData* pending;
    NSHTTPURLResponse* response;
    NSError* error;
    int done;
    int stop;         /* the reader wants no more data */
}
@end

@implementation FAFetchDelegate
- (instancetype)init
{
    if ((self = [super init])) {
        pthread_mutex_init(&mutex, NULL);
        pthread_cond_init(&cond, NULL);
        pending = [NSMutableData data];
    }
    return self;
}
- (void)dealloc
{
    pthread_cond_destroy(&cond);
    pthread_mutex_destroy(&mutex);
}
- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task
    didReceiveResponse:(NSURLResponse*)resp completionHandler:(void (^)(NSURLSessionResponseDisposition))handler
{
    int mine;
    (void)session;
    pthread_mutex_lock(&mutex);
    mine = task == current;
    if (mine && [resp isKindOfClass:[NSHTTPURLResponse class]]) response = (NSHTTPURLResponse*)resp;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&mutex);
    handler(mine ? NSURLSessionResponseAllow : NSURLSessionResponseCancel);
}
- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveData:(NSData*)data
{
    (void)session;
    pthread_mutex_lock(&mutex);
    while (task == current && !stop && pending.length >= FA_APPLE_PENDING_MAX) pthread_cond_wait(&cond, &mutex);
    if (task == current && !stop) [pending appendData:data];
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&mutex);
}
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)err
{
    (void)session;
    pthread_mutex_lock(&mutex);
    if (task == current) {
        error = err;
        done = 1;
    }
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&mutex);
}
/* Redirects come back to fa_http_fetch, which follows them for every transport
 * (URLSession's own would keep the Authorization header on another server). */
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)resp newRequest:(NSURLRequest*)request
    completionHandler:(void (^)(NSURLRequest*))handler
{
    (void)session; (void)task; (void)resp; (void)request;
    handler(nil);   /* the redirect response itself is the task's response */
}
@end

/* One URLSession per stream: it keeps its connections between tasks. Made for the
 * first request, again when the proxy setting or timeout changes (both are part of
 * the session's configuration). */
struct fa_http_session {
    void* session;            /* NSURLSession*, retained (CFBridgingRetain) */
    void* delegate;           /* FAFetchDelegate*, retained */
    char* proxy;              /* the request's proxy setting (a copy), NULL = the system's */
    uint32_t timeout_ms;
};

fa_http_session* fa_http_session_create(void) { return (fa_http_session*)calloc(1, sizeof(fa_http_session)); }

static void session_close(fa_http_session* s)
{
    if (s->session) {
        NSURLSession* session = (NSURLSession*)CFBridgingRelease(s->session);
        [session invalidateAndCancel];
    }
    if (s->delegate) (void)CFBridgingRelease(s->delegate);
    free(s->proxy);
    memset(s, 0, sizeof(*s));
}

void fa_http_session_destroy(fa_http_session* s)
{
    if (!s) return;
    @autoreleasepool { session_close(s); }
    free(s);
}

/* Opens s's URLSession for req's settings. 0 when out of memory. */
static int session_open(fa_http_session* s, const fa_http_request* req)
{
    NSURLSessionConfiguration* cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    NSOperationQueue* queue;
    FAFetchDelegate* d;
    NSURLSession* session;
    cfg.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    cfg.URLCache = nil;
    /* No cookie store, as on the other transports: the kept session would replay a
     * server's cookies on the stream's later requests. The app's Cookie line still goes. */
    cfg.HTTPCookieStorage = nil;
    cfg.HTTPShouldSetCookies = NO;
    cfg.HTTPCookieAcceptPolicy = NSHTTPCookieAcceptPolicyNever;
    cfg.timeoutIntervalForRequest = req->timeout_ms / 1000.0;
    if (req->proxy && !req->proxy[0]) {
        cfg.connectionProxyDictionary = @{};
    } else if (req->proxy) {
        fa_url p;
        if (fa_proxy_parse(req->proxy, &p) == FA_OK) {
            NSString* host = [NSString stringWithUTF8String:p.host];
            cfg.connectionProxyDictionary = @{ @"HTTPEnable": @1, @"HTTPProxy": host, @"HTTPPort": @(p.port),
                                               @"HTTPSEnable": @1, @"HTTPSProxy": host, @"HTTPSPort": @(p.port) };
        }
    }
    if (req->proxy && !(s->proxy = strdup(req->proxy))) return 0;
    d = [[FAFetchDelegate alloc] init];
    queue = [[NSOperationQueue alloc] init];
    queue.maxConcurrentOperationCount = 1;
    session = [NSURLSession sessionWithConfiguration:cfg delegate:d delegateQueue:queue];
    if (!d || !session) return 0;
    s->delegate = (void*)CFBridgingRetain(d);
    s->session = (void*)CFBridgingRetain(session);
    s->timeout_ms = req->timeout_ms;
    return 1;
}

void fa_http_abort(fa_atomic32* cancel) { (void)cancel; /* the fetch loop polls the flag every 100 ms */ }

int fa_net_system_proxy(const char* scheme, const char* host, char* out, size_t cap)
{
    int found = 0;
    (void)scheme; (void)host;
#if TARGET_OS_OSX || TARGET_OS_IPHONE
    @autoreleasepool {
        CFDictionaryRef dict = CFNetworkCopySystemProxySettings();
        if (dict) {
            NSDictionary* d = (__bridge NSDictionary*)dict;
            NSNumber* enabled = d[@"HTTPEnable"];
            NSString* proxy = d[@"HTTPProxy"];
            NSNumber* port = d[@"HTTPPort"];
            if (enabled.intValue && proxy.length > 0) {
                NSString* hp = port ? [NSString stringWithFormat:@"%@:%d", proxy, port.intValue] : proxy;
                const char* s = hp.UTF8String;
                if (s && strlen(s) < cap) { strcpy(out, s); found = 1; }
            }
            CFRelease(dict);
        }
    }
#endif
    return found;
}

static void timed_wait(FAFetchDelegate* d, int ms)
{
    struct timespec ts;   /* relative: a wall-clock step doesn't stretch the wait */
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    pthread_cond_timedwait_relative_np(&d->cond, &d->mutex, &ts);
}

/* fa_http_each_header's sink (ASCII only). addValue sets a header not set yet,
 * and joins a repeated app header with a comma. */
static int add_request_header(void* user, const char* name, const char* value)
{
    NSMutableURLRequest* request = (__bridge NSMutableURLRequest*)user;
    NSString* n = [NSString stringWithUTF8String:name];
    NSString* v = [NSString stringWithUTF8String:value];
    if (!n || !v) return 1;
    [request addValue:v forHTTPHeaderField:n];
    return 0;
}

fa_result fa_http_fetch_tls(const fa_http_request* req, fa_http_head_proc on_head, fa_http_data_proc on_data, void* user,
                            char** out_redirect)
{
    fa_result r = FA_ERROR_NETWORK;
    fa_http_session one = { 0 };
    fa_http_session* s = req->session ? req->session : &one;
    *out_redirect = NULL;
    @autoreleasepool {
        NSString* urlString = [NSString stringWithUTF8String:req->url];
        NSURL* url = urlString ? [NSURL URLWithString:urlString] : nil;
        NSMutableURLRequest* request;
        NSURLSessionDataTask* task;
        FAFetchDelegate* d;
        int head_done = 0, cancelled = 0, redirect = 0;
        size_t drained = 0;
        if (!url) return FA_ERROR_INVALID_ARG;

        request = [NSMutableURLRequest requestWithURL:url];
        request.HTTPMethod = @"GET";
        {
            /* URLSession ignores credentials in the URL (it answers challenges only
             * through a delegate): the walk sends them. It adds Host and the
             * connection headers itself. */
            fa_url u;
            if (fa_http_each_header(req, fa_url_parse(req->url, &u) == FA_OK ? &u : NULL, add_request_header,
                                    (__bridge void*)request) != FA_OK)
                return FA_ERROR_OUT_OF_MEMORY;
        }

        if (!s->session || s->timeout_ms != req->timeout_ms ||
            !(req->proxy ? s->proxy && strcmp(s->proxy, req->proxy) == 0 : !s->proxy)) {
            session_close(s);
            if (!session_open(s, req)) { session_close(s); return FA_ERROR_OUT_OF_MEMORY; }
        }
        d = (__bridge FAFetchDelegate*)s->delegate;
        task = [(__bridge NSURLSession*)s->session dataTaskWithRequest:request];
        pthread_mutex_lock(&d->mutex);
        d->current = task;
        d->pending = [NSMutableData data];
        d->response = nil;
        d->error = nil;
        d->done = 0;
        d->stop = 0;
        pthread_mutex_unlock(&d->mutex);
        [task resume];

        pthread_mutex_lock(&d->mutex);
        for (;;) {
            if (req->cancel && fa_load32(req->cancel)) { cancelled = 1; break; }
            if (!head_done && (d->response || d->done)) {
                if (!d->response) break;     /* failed before any response */
                head_done = 1;
                {
                    NSHTTPURLResponse* resp = d->response;
                    NSMutableString* lines = [NSMutableString string];
                    fa_http_response fr;
                    char* headers;
                    int stop;
                    for (NSString* key in resp.allHeaderFields)
                        [lines appendFormat:@"%@: %@\r\n", key, resp.allHeaderFields[key]];
                    headers = strdup(lines.UTF8String ? lines.UTF8String : "");
                    if (headers && strlen(headers) > FA_HTTP_HEAD_MAX) { free(headers); r = FA_ERROR_NETWORK; break; }   /* as the other backends */
                    memset(&fr, 0, sizeof(fr));
                    fr.status = (int)resp.statusCode;
                    fr.headers = headers ? headers : (char*)"";
                    fr.final_url = req->url;
                    redirect = fa_http_redirect_target(req->url, fr.status, fr.headers, out_redirect);
                    if (redirect) {
                        free(headers);
                        if (redirect < 0) { r = FA_ERROR_NETWORK; break; }
                        continue;    /* its body is read to the end, so the connection can be kept */
                    }
                    fa_http_fill_lengths(&fr);
                    pthread_mutex_unlock(&d->mutex);
                    stop = on_head ? on_head(user, &fr) : 0;
                    free(headers);
                    pthread_mutex_lock(&d->mutex);
                    if (stop) { r = FA_OK; break; }
                    if (fr.status >= 400) { r = FA_ERROR_NOT_FOUND; break; }
                }
                continue;
            }
            if (head_done && d->pending.length > 0) {
                NSData* chunk = d->pending;
                int stop;
                d->pending = [NSMutableData data];
                pthread_cond_broadcast(&d->cond);
                if (redirect) {
                    drained += chunk.length;
                    if (drained > FA_HTTP_DRAIN_MAX) { r = FA_OK; break; }
                    continue;
                }
                pthread_mutex_unlock(&d->mutex);
                stop = on_data(user, (const uint8_t*)chunk.bytes, chunk.length);
                pthread_mutex_lock(&d->mutex);
                if (stop) { r = FA_OK; break; }
                continue;
            }
            if (d->done) {
                r = d->error && !redirect ? FA_ERROR_NETWORK : FA_OK;
                break;
            }
            timed_wait(d, 100);
        }
        d->stop = 1;
        d->current = nil;
        d->pending = [NSMutableData data];
        pthread_cond_broadcast(&d->cond);
        pthread_mutex_unlock(&d->mutex);
        [task cancel];    /* nothing when it is complete; else its connection closes */
        if (s == &one) session_close(&one);
        if (cancelled) r = FA_ERROR_BUSY;
        if (r != FA_OK) { free(*out_redirect); *out_redirect = NULL; }
    }
    return r;
}

#endif
