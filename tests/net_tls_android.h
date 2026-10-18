/* Android only, for test_net.c: an HTTPS front for the test's HTTP server
 * (net_tls_android.c). */
#ifndef FA_NET_TLS_ANDROID_H
#define FA_NET_TLS_ANDROID_H

/* Starts a Java VM and an HTTPS server on 127.0.0.1 that relays every connection
 * to the plain server on backend_port. 0 on success (*out_port, *out_vm set),
 * -1 when this system cannot run a Java VM (the reason is printed). */
int tls_front_start(const char* cert_dir, int backend_port, int* out_port, void** out_vm);
/* After the plain server stopped (that ends the relays). */
void tls_front_stop(void);

#endif
