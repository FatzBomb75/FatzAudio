/* HTTPS on macOS and iOS: URLSession, so TLS, certificates, proxies and App
 * Transport Security follow the system. The delegate runs on a private queue and
 * hands the body to the download thread through a small buffer; when the buffer
 * is full the delegate waits, which pushes back on the connection. */
#if defined(__APPLE__)
#import <Foundation/Foundation.h>
#import <CFNetwork/CFNetwork.h>
#include "net.h"
#include <pthread.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>

#define FA_APPLE_PENDING_MAX (512 * 1024)

@interface FAFetchDelegate : NSObject <NSURLSessionDataDelegate>
{
@public
    pthread_mutex_t mutex;
    pthread_cond_t cond;
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
    (void)session; (void)task;
    pthread_mutex_lock(&mutex);
    if ([resp isKindOfClass:[NSHTTPURLResponse class]]) response = (NSHTTPURLResponse*)resp;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&mutex);
    handler(NSURLSessionResponseAllow);
}
- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveData:(NSData*)data
{
    (void)session; (void)task;
    pthread_mutex_lock(&mutex);
    while (!stop && pending.length >= FA_APPLE_PENDING_MAX) pthread_cond_wait(&cond, &mutex);
    if (!stop) [pending appendData:data];
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&mutex);
}
- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)err
{
    (void)session; (void)task;
    pthread_mutex_lock(&mutex);
    error = err;
    done = 1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&mutex);
}
@end

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
    struct timespec ts;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts.tv_sec = tv.tv_sec + ms / 1000;
    ts.tv_nsec = (long)tv.tv_usec * 1000 + (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(&d->cond, &d->mutex, &ts);
}

fa_result fa_http_fetch_tls(const fa_http_request* req, fa_http_head_proc on_head, fa_http_data_proc on_data, void* user)
{
    fa_result r = FA_ERROR_NETWORK;
    @autoreleasepool {
        NSString* urlString = [NSString stringWithUTF8String:req->url];
        NSURL* url = urlString ? [NSURL URLWithString:urlString] : nil;
        NSURLSessionConfiguration* cfg;
        NSMutableURLRequest* request;
        NSOperationQueue* queue;
        NSURLSession* session;
        NSURLSessionDataTask* task;
        FAFetchDelegate* d;
        int head_done = 0, cancelled = 0;
        if (!url) return FA_ERROR_INVALID_ARG;

        cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        cfg.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
        cfg.URLCache = nil;
        cfg.timeoutIntervalForRequest = req->timeout_ms / 1000.0;
        if (req->proxy && !req->proxy[0]) {
            cfg.connectionProxyDictionary = @{};
        } else if (req->proxy) {
            fa_url p;
            char text[FA_NET_URL_MAX];
            snprintf(text, sizeof(text), "%s%s", strstr(req->proxy, "://") ? "" : "http://", req->proxy);
            if (fa_url_parse(text, &p) == FA_OK) {
                NSString* host = [NSString stringWithUTF8String:p.host];
                cfg.connectionProxyDictionary = @{ @"HTTPEnable": @1, @"HTTPProxy": host, @"HTTPPort": @(p.port),
                                                   @"HTTPSEnable": @1, @"HTTPSProxy": host, @"HTTPSPort": @(p.port) };
            }
        }
        request = [NSMutableURLRequest requestWithURL:url];
        request.HTTPMethod = @"GET";
        [request setValue:[NSString stringWithUTF8String:req->user_agent ? req->user_agent : "FatzAudio"] forHTTPHeaderField:@"User-Agent"];
        /* Raw bytes: no transparent decompression of audio. */
        [request setValue:@"identity" forHTTPHeaderField:@"Accept-Encoding"];
        if (req->icy) [request setValue:@"1" forHTTPHeaderField:@"Icy-MetaData"];
        {
            /* URLSession only answers auth challenges through a delegate: send URL
             * credentials up front, as the socket client does. */
            fa_url u;
            if (fa_url_parse(req->url, &u) == FA_OK && u.userinfo[0]) {
                char auth[512];
                fa_http_basic_auth(u.userinfo, auth, sizeof(auth));
                [request setValue:[NSString stringWithUTF8String:auth] forHTTPHeaderField:@"Authorization"];
            }
        }
        if (req->range_start >= 0)
            [request setValue:[NSString stringWithFormat:@"bytes=%lld-", (long long)req->range_start] forHTTPHeaderField:@"Range"];
        if (req->headers) {
            const char* h = req->headers;
            while (*h) {
                size_t len = strcspn(h, "\r\n");
                const char* colon = memchr(h, ':', len);
                if (colon) {
                    NSString* name = [[NSString alloc] initWithBytes:h length:(NSUInteger)(colon - h) encoding:NSUTF8StringEncoding];
                    const char* v = colon + 1;
                    while (v < h + len && (*v == ' ' || *v == '\t')) v++;
                    NSString* value = [[NSString alloc] initWithBytes:v length:(NSUInteger)(h + len - v) encoding:NSUTF8StringEncoding];
                    if (name && value) [request setValue:value forHTTPHeaderField:name];
                }
                h += len;
                while (*h == '\r' || *h == '\n') h++;
            }
        }

        d = [[FAFetchDelegate alloc] init];
        queue = [[NSOperationQueue alloc] init];
        queue.maxConcurrentOperationCount = 1;
        session = [NSURLSession sessionWithConfiguration:cfg delegate:d delegateQueue:queue];
        task = [session dataTaskWithRequest:request];
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
                    char final_url[FA_NET_URL_MAX];
                    int stop;
                    for (NSString* key in resp.allHeaderFields)
                        [lines appendFormat:@"%@: %@\r\n", key, resp.allHeaderFields[key]];
                    headers = strdup(lines.UTF8String ? lines.UTF8String : "");
                    snprintf(final_url, sizeof(final_url), "%s", resp.URL.absoluteString.UTF8String ? resp.URL.absoluteString.UTF8String : req->url);
                    memset(&fr, 0, sizeof(fr));
                    fr.status = (int)resp.statusCode;
                    fr.headers = headers ? headers : (char*)"";
                    fr.final_url = final_url;
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
                pthread_mutex_unlock(&d->mutex);
                stop = on_data(user, (const uint8_t*)chunk.bytes, chunk.length);
                pthread_mutex_lock(&d->mutex);
                if (stop) { r = FA_OK; break; }
                continue;
            }
            if (d->done) {
                r = d->error ? FA_ERROR_NETWORK : FA_OK;
                break;
            }
            timed_wait(d, 100);
        }
        d->stop = 1;
        pthread_cond_broadcast(&d->cond);
        pthread_mutex_unlock(&d->mutex);
        [task cancel];
        [session invalidateAndCancel];
        if (cancelled) r = FA_ERROR_BUSY;
    }
    return r;
}

#endif
