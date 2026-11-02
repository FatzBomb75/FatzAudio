#!/usr/bin/env python3
"""Generate docs/reference/c-api.md from include/fatzaudio/*.h.
The headers carry plain /* */ comments (no Doxygen), so each section of a header (a `/* ---- Title ---- */`
banner and everything up to the next banner) becomes a heading, its banner text, a list of its functions and its
declarations with their comments in a C code block.
Usage: python3 tools/gen-c-reference.py [--check]   (--check exits 1 if the file is out of date)"""
import re, sys, pathlib

root = pathlib.Path(__file__).resolve().parent.parent
out_path = root / "docs/reference/c-api.md"
banner = re.compile(r"^/\* -{3,} ?(.*?)\s*(-{2,})?\s*(\*/)?\s*$")
func = re.compile(r"\bFA_API\b[^;(]*?\b(fa_\w+)\s*\(")

def sections(text):
    out, cur = [], None
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        m = banner.match(lines[i])
        if m and not lines[i].startswith("/* ---- */"):
            title = m.group(1).strip(" -")
            desc, closed = [], bool(m.group(3))
            i += 1
            while not closed and i < len(lines):  # multi-line banner comment
                l = lines[i]
                closed = "*/" in l
                l = l.replace("*/", "").rstrip()
                if l.strip() and not re.fullmatch(r"\s*\*?\s*-+\s*", l): desc.append(re.sub(r"^\s*\*?\s?", "", l))
                i += 1
            cur = {"title": title or "Declarations", "desc": desc, "body": []}
            out.append(cur)
            continue
        if cur is not None: cur["body"].append(lines[i])
        i += 1
    return out

def render():
    parts = ["# C API reference\n",
             "Generated from `include/fatzaudio/fatzaudio.h` and `fatzaudio_ffmpeg.h` by `tools/gen-c-reference.py`; do not edit by hand. "
             "Every function returns `fa_result` (`FA_OK` is 0, errors are negative; see [error codes](../troubleshooting.md#error-codes)) "
             "unless its comment says otherwise. The comments are the header's own. For how to use a feature, see its [guide](../features/README.md).\n"]
    for name in ("fatzaudio.h", "fatzaudio_ffmpeg.h"):
        text = (root / "include/fatzaudio" / name).read_text(encoding="utf-8")
        parts.append(f"\n## Header `{name}`\n")
        for s in sections(text):
            body = "\n".join(s["body"]).strip("\n")
            if not body.strip(): continue
            funcs = []
            for stmt in re.split(r";\s*\n", re.sub(r"/\*.*?\*/", "", body, flags=re.S)):
                m = func.search(stmt.replace("\n", " "))
                if m: funcs.append(m.group(1))
            parts.append(f"\n### {s['title']}\n")
            if s["desc"]: parts.append("```text\n" + "\n".join(s["desc"]).strip() + "\n```\n")
            if funcs: parts.append("Functions: " + ", ".join(f"`{f}`" for f in funcs) + "\n")
            parts.append("```c\n" + body + "\n```\n")
    return "\n".join(parts)

if __name__ == "__main__":
    new = render()
    if "--check" in sys.argv:
        sys.exit(0 if out_path.exists() and out_path.read_text(encoding="utf-8") == new else 1)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(new, encoding="utf-8")
    print(f"wrote {out_path.relative_to(root)}: {new.count(chr(10))} lines")
