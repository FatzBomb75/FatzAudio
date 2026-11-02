#!/usr/bin/env python3
"""Serve this folder on http://localhost:8765 and open it (the site's menu and search need a web server).
Run:  python3 serve.py"""
import http.server, webbrowser, functools, pathlib
here = pathlib.Path(__file__).resolve().parent
handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(here))
webbrowser.open("http://localhost:8765/index.html")
http.server.ThreadingHTTPServer(("127.0.0.1", 8765), handler).serve_forever()
