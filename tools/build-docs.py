#!/usr/bin/env python3
"""Build the documentation site (HTML, searchable, works offline) with DocFX.

    python3 tools/build-docs.py            # stage the Markdown, build the site into build/docs-site
    python3 tools/build-docs.py --stage    # only stage (build/docs-stage), for inspection
    python3 tools/build-docs.py --zip      # also write build/FatzAudio-docs.zip: upload it to a site, or unzip and run open-docs.ps1 (Windows) or serve.py to browse offline

Needs the .NET 10 SDK and DocFX (`dotnet tool install -g docfx`). The Markdown in docs/ stays the single source:
GitHub renders it as it is. This script stages a copy in which
  * docs/archive and docs/engineering-notes.md are left out,
  * Markdown files outside docs/ that the docs link to (CHANGELOG.md, tools/*/README.md, ...) are copied in
    under _repo/, and the links are pointed at them,
  * links to anything else outside docs/ (source files, folders) become links to the repository on GitHub,
  * the C reference is regenerated, and the .NET API reference comes from the XML comments (DocFX metadata).
"""
import os, re, shutil, subprocess, sys, pathlib

root = pathlib.Path(__file__).resolve().parent.parent
docs = root / "docs"
stage = root / "build/docs-stage"
site = root / "build/docs-site"
GITHUB = "https://github.com/FatzBomb75/FatzAudio/blob/main/"
SKIP = ("archive/", "engineering-notes.md")
link = re.compile(r"(?<!!)(\]\()([^)\s#]*)(#[^)\s]*)?((?:\s+\"[^\"]*\")?\))")

def stage_path(src):
    """Where a repository file lives in the staged tree."""
    try: p = stage / src.relative_to(docs)
    except ValueError: p = stage / "_repo" / src.relative_to(root)
    return p.with_name("index.md") if p.name == "README.md" else p   # a folder's README is its home page

def is_skipped(src):
    try: rel = src.relative_to(docs).as_posix()
    except ValueError: return False
    return rel.startswith(SKIP)

def rewrite(src, text, queue):
    dst = stage_path(src)
    def fix(m):
        target, frag = m.group(2), m.group(3) or ""
        if not target or re.match(r"[a-z]+:", target): return m.group(0)
        dest = (src.parent / target).resolve()
        if not dest.exists() or not str(dest).startswith(str(root)): return m.group(0)
        if dest.suffix == ".md" and not is_skipped(dest):
            queue.append(dest)
            rel = os.path.relpath(stage_path(dest), dst.parent).replace(os.sep, "/")
            return f"{m.group(1)}{rel}{frag}{m.group(4)}"
        return f"{m.group(1)}{GITHUB}{dest.relative_to(root).as_posix()}{m.group(4)}" if dest.is_file() else m.group(0)
    return link.sub(fix, text)

def do_stage():
    if stage.exists(): shutil.rmtree(stage)
    subprocess.run([sys.executable, str(root / "tools/gen-c-reference.py")], check=True)
    queue = [p for p in docs.rglob("*.md") if not is_skipped(p)]
    done = set()
    while queue:
        src = queue.pop()
        if src in done: continue
        done.add(src)
        dst = stage_path(src)
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(rewrite(src, src.read_text(encoding="utf-8"), queue), encoding="utf-8")
    (stage / "toc.yml").write_text((root / "tools/docs-site/toc.yml").read_text().replace("README.md", "index.md"))
    print(f"staged {len(done)} Markdown files in {stage.relative_to(root)}")

if __name__ == "__main__":
    do_stage()
    if "--stage" in sys.argv: sys.exit(0)
    if shutil.which("docfx") is None:
        sys.exit("docfx not found: dotnet tool install -g docfx (and add ~/.dotnet/tools to PATH)")
    subprocess.run(["docfx", "metadata", str(root / "tools/docs-site/docfx.json")], check=True)
    subprocess.run(["docfx", "build", str(root / "tools/docs-site/docfx.json")], check=True)
    for f in ("open-docs.ps1", "serve.py"): shutil.copy(root / "tools/docs-site" / f, site / f)   # offline: pages need a web server
    if "--zip" in sys.argv:
        shutil.make_archive(str(root / "build/FatzAudio-docs"), "zip", site)
        print("wrote build/FatzAudio-docs.zip")
