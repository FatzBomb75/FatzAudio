#!/usr/bin/env python3
"""A local HTTPS server for the tests that need the system's own TLS stack (the
macOS and iOS simulator CI jobs: URLSession checks the certificate as an app's
would). It makes a test certificate authority and a server certificate for
127.0.0.1 and localhost in OUT (once), then serves DIR on 127.0.0.1:PORT until
stopped. Trust OUT/ca.crt first:
  macOS:         sudo security add-trusted-cert -d -r trustRoot -k /Library/Keychains/System.keychain OUT/ca.crt
  iOS simulator: xcrun simctl keychain booted add-root-cert OUT/ca.crt

Usage: serve.py DIR OUT [PORT]   (default port 8443)
Apple accepts a server certificate valid for at most 825 days, with the
serverAuth usage and the names in subjectAltName, so this one is made that way.
"""
import functools, http.server, os, ssl, subprocess, sys


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


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    root, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 8443
    make_certs(out)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(os.path.join(out, "server.crt"), os.path.join(out, "server.key"))
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=root)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
    server.socket = ctx.wrap_socket(server.socket, server_side=True)
    print("serving %s on https://127.0.0.1:%d/" % (root, port), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
