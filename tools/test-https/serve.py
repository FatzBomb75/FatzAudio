#!/usr/bin/env python3
"""A local HTTPS server for the tests that need the system's own TLS stack (the
Linux, Windows, macOS and iOS simulator CI jobs: URLSession checks the certificate
as an app's would). It makes a test certificate authority and a server certificate
for 127.0.0.1 and localhost in OUT (once), then serves DIR on 127.0.0.1:PORT, and
again on PORT + 1 (another origin, for redirects), until stopped. Trust OUT/ca.crt first:
  macOS:         sudo security add-trusted-cert -d -r trustRoot -k /Library/Keychains/System.keychain OUT/ca.crt
  iOS simulator: xcrun simctl keychain booted add-root-cert OUT/ca.crt

Usage: serve.py DIR OUT [PORT]   (default port 8443)
Connections stay open between requests (HTTP/1.1 keep-alive), files honour a single
byte Range, and every response says which connection carried it (X-Connection: N,
counted from 1 over both ports). Besides the files of DIR:
  /echo-headers            the request's header lines as text (FA_NET_HTTPS_ECHO_URL)
  /echo-headers?close      the same, then the connection is closed without notice
  /echo-headers?cookie     the same, with a Set-Cookie
  /redirect?to=URL         302 to URL
  /connections             how many connections have been opened so far
  /hls/master.m3u8         an HLS stream: a master playlist, a media playlist and
                           three byte-range segments of DIR/net/tone_cbr.mp3
Every request's headers are also logged to stderr.
Apple accepts a server certificate valid for at most 825 days, with the
serverAuth usage and the names in subjectAltName, so this one is made that way.
"""
import functools, http.server, os, re, ssl, subprocess, sys, threading, urllib.parse

_lock = threading.Lock()
_connections = 0


def make_certs(out):
    if os.path.exists(os.path.join(out, "server.crt")):
        return
    os.makedirs(out, exist_ok=True)

    def run(*args):
        subprocess.run(["openssl", *args], check=True, cwd=out, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    with open(os.path.join(out, "ca.ext"), "w") as f:
        f.write("basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n")
    with open(os.path.join(out, "server.ext"), "w") as f:
        f.write("basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n"
                "extendedKeyUsage=serverAuth\nsubjectAltName=IP:127.0.0.1,DNS:localhost\n")
    run("req", "-new", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key", "-subj", "/CN=FatzAudio test CA", "-out", "ca.csr")
    run("x509", "-req", "-in", "ca.csr", "-signkey", "ca.key", "-days", "30", "-sha256", "-extfile", "ca.ext", "-out", "ca.crt")
    run("req", "-new", "-newkey", "rsa:2048", "-nodes", "-keyout", "server.key", "-subj", "/CN=localhost", "-out", "server.csr")
    run("x509", "-req", "-in", "server.csr", "-CA", "ca.crt", "-CAkey", "ca.key", "-CAcreateserial", "-days", "30",
        "-sha256", "-extfile", "server.ext", "-out", "server.crt")


def mp3_frames(data):
    """Offsets of the MPEG-1 layer III frames in data, and the samples per second."""
    kbps = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0]
    rates = [44100, 48000, 32000, 0]
    pos, frames, rate = 0, [], 0
    while pos + 4 <= len(data) and data[pos] == 0xFF and (data[pos + 1] & 0xFE) == 0xFA:
        br, rate = kbps[data[pos + 2] >> 4], rates[(data[pos + 2] >> 2) & 3]
        if not br or not rate:
            break
        frames.append(pos)
        pos += 144 * br * 1000 // rate + ((data[pos + 2] >> 1) & 1)
    frames.append(pos)
    return frames, rate


def hls_media(root):
    """A media playlist of three byte-range segments of net/tone_cbr.mp3 (cut at frames)."""
    with open(os.path.join(root, "net", "tone_cbr.mp3"), "rb") as f:
        frames, rate = mp3_frames(f.read())
    count = len(frames) - 1
    cuts = [0, count // 3, 2 * count // 3, count]
    text = "#EXTM3U\n#EXT-X-VERSION:4\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n"
    for a, b in zip(cuts, cuts[1:]):
        text += "#EXTINF:%.5f,\n#EXT-X-BYTERANGE:%d@%d\n/net/tone_cbr.mp3\n" % (
            (b - a) * 1152 / rate, frames[b] - frames[a], frames[a])
    return text + "#EXT-X-ENDLIST\n"


class Handler(http.server.SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"   # keep-alive: connection reuse is what the tests check
    timeout = 60

    def setup(self):
        global _connections
        super().setup()
        with _lock:
            _connections += 1
            self.connection_id = _connections

    def end_headers(self):
        self.send_header("X-Connection", str(self.connection_id))
        super().end_headers()

    def log_message(self, format, *args):
        pass   # do_GET logs each request with its headers

    def send_text(self, status, text, content_type="text/plain", extra=()):
        body = text.encode("latin-1", "replace")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        sys.stderr.write("%s (connection %d)\n%s\n" % (self.requestline, self.connection_id, "".join(
            "%s: %s\n" % (k, v if len(v) <= 200 else v[:200] + "... (%d bytes)" % len(v)) for k, v in self.headers.items())))
        if url.path == "/echo-headers":
            self.send_text(200, "".join("%s: %s\r\n" % (k, v) for k, v in self.headers.items()),
                           extra=[("Set-Cookie", "fa=1; Path=/")] if url.query == "cookie" else ())
            if url.query == "close":
                self.close_connection = True   # no "Connection: close": the client finds out later
        elif url.path == "/redirect":
            to = urllib.parse.parse_qs(url.query).get("to", ["/"])[0]
            self.send_text(302, "", extra=[("Location", to)])
        elif url.path == "/connections":
            with _lock:
                self.send_text(200, "%d\n" % _connections)
        elif url.path == "/hls/master.m3u8":
            self.send_text(200, "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=130000,CODECS=\"mp4a.40.34\"\nmedia.m3u8\n",
                           "application/vnd.apple.mpegurl")
        elif url.path == "/hls/media.m3u8":
            self.send_text(200, hls_media(self.directory), "application/vnd.apple.mpegurl")
        else:
            self.send_file(url.path)

    def send_file(self, path):
        """A file of DIR, whole or one byte range (SimpleHTTPRequestHandler has no ranges)."""
        local = self.translate_path(path)
        m = re.fullmatch(r"bytes=(\d+)-(\d*)", self.headers.get("Range", ""))
        if not m or not os.path.isfile(local):
            return super().do_GET()
        with open(local, "rb") as f:
            data = f.read()
        start = int(m.group(1))
        end = min(int(m.group(2)) if m.group(2) else len(data) - 1, len(data) - 1)
        if start > end:
            self.send_response(416)
            self.send_header("Content-Range", "bytes */%d" % len(data))
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(local))
        self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, len(data)))
        self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        self.wfile.write(data[start:end + 1])


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    root, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 8443
    make_certs(out)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(os.path.join(out, "server.crt"), os.path.join(out, "server.key"))
    handler = functools.partial(Handler, directory=root)
    servers = []
    for p in (port, port + 1):
        server = http.server.ThreadingHTTPServer(("127.0.0.1", p), handler)
        server.daemon_threads = True
        # The handshake happens in each request's thread, not in accept(): a client that
        # connects and stalls (a port probe) cannot hold up the next one.
        server.socket = ctx.wrap_socket(server.socket, server_side=True, do_handshake_on_connect=False)
        servers.append(server)
    threading.Thread(target=servers[1].serve_forever, daemon=True).start()
    print("serving %s on https://127.0.0.1:%d/ and :%d" % (root, port, port + 1), flush=True)
    servers[0].serve_forever()


if __name__ == "__main__":
    main()
