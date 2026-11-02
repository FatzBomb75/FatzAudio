#!/usr/bin/env python3
"""Check that every relative Markdown link and #anchor in docs/ and the root *.md files resolves.
Usage: python3 tools/check-doc-links.py   (run from the repo root; exits 1 on a broken link)"""
import re, sys, pathlib

root = pathlib.Path(__file__).resolve().parent.parent
files = list((root / "docs").rglob("*.md")) + list(root.glob("*.md")) + [root / "bindings/dotnet/README.md"]
link = re.compile(r"(?<!!)\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")

def slug(h):
    h = re.sub(r"`", "", h.strip().lower())
    h = re.sub(r"[^\w\- ]", "", h, flags=re.UNICODE)
    return h.replace(" ", "-")

def anchors(p, cache={}):
    if p not in cache:
        out, fence = set(), False
        for line in p.read_text(encoding="utf-8").splitlines():
            if line.startswith("```"): fence = not fence
            m = None if fence else re.match(r"#{1,6}\s+(.*)", line)
            if m: out.add(slug(m.group(1)))
        cache[p] = out
    return cache[p]

bad = 0
for f in files:
    fence = False
    for n, line in enumerate(f.read_text(encoding="utf-8").splitlines(), 1):
        if line.startswith("```"): fence = not fence
        if fence: continue
        for t in link.findall(line):
            if re.match(r"[a-z]+:", t): continue
            path, _, frag = t.partition("#")
            dest = (f.parent / path).resolve() if path else f
            if not dest.exists():
                print(f"{f.relative_to(root)}:{n}: missing {t}"); bad += 1
            elif frag and dest.suffix == ".md" and frag not in anchors(dest):
                print(f"{f.relative_to(root)}:{n}: no anchor #{frag} in {dest.relative_to(root)}"); bad += 1
sys.exit(1 if bad else 0)
