#!/usr/bin/env python3
# Build the change list page from the patches on this branch.
#
#   scripts/yacer-changes.py [OUTDIR]     (default: site)
#
# Every active patch (a *.patch, not *.patch.disabled) under
# overlay/**/patches-yacer/<package>/ contributes one entry:
# its subject and its message body. The page is the current set of patches;
# what changed from one build to the next is in each release's notes.

import html
import os
import re
import subprocess
import sys
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "site")
REPO = os.environ.get("GITHUB_REPOSITORY", "allolive/CoreELEC")
COREELEC = "CoreELEC 22 nightly"

GROUPS = {
    "kodi": "Kodi",
    "common_drivers": "Kernel drivers",
    "media_modules-aml": "Kernel media drivers",
    "linux": "Kernel",
    "CoreELEC-settings": "CoreELEC settings",
}
ORDER = list(GROUPS.values())

TRAILER = re.compile(r"^(Signed-off-by|Co-authored-by|Reviewed-by|Acked-by|Tested-by):", re.I)
PAYLOAD = re.compile(r"^(diff |--- a/|---$|Index: )")


def git(*args):
    try:
        return subprocess.run(["git", "-C", ROOT, *args], check=True,
                              capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def parse(path):
    lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
    end = next((i for i, l in enumerate(lines) if PAYLOAD.match(l)), len(lines))
    head = lines[:end]
    subject, body, credits, trailers, author = None, [], [], [], ""
    i = 0
    while i < len(head):
        line = head[i]
        if subject is None and line.startswith("Subject:"):
            subject = re.sub(r"^Subject:\s*(\[PATCH[^\]]*\]\s*)?", "", line)
            while i + 1 < len(head) and head[i + 1].startswith(" "):
                i += 1
                subject += head[i]
        elif subject is None and re.match(r"^(From |From:|Date:)", line):
            if line.startswith("From:"):
                author = re.sub(r"\s*<[^>]*>", "", line[5:]).strip()
        elif TRAILER.match(line):
            trailers.append(line.strip())
            if line.lower().startswith("co-authored-by:"):
                credits.append(re.sub(r"\s*<[^>]*>", "", line.split(":", 1)[1]).strip())
        elif subject is None and line.strip():
            subject = line.strip()          # headerless patch: first line is the subject
        elif subject is not None:
            body.append(line)
        i += 1
    text = "\n".join(body).strip()
    message = "\n\n".join(x for x in (text, "\n".join(trailers)) if x)
    component, _, title = (subject or os.path.basename(path)).partition(": ")
    if not title:
        component, title = "", component
    return {"component": component, "title": title, "credits": credits,
            "message": message, "author": author}


def updated_dates():
    """Last commit date of every file under overlay/, newest first wins."""
    dates = {}
    date = None
    for line in git("-c", "core.quotePath=false", "log", "--format=@%cs", "--name-only",
                    "--", "overlay").splitlines():
        if line.startswith("@"):
            date = line[1:]
        elif line and line not in dates:
            dates[line] = date
    # -z: unstripped, unquoted paths ("XY path\0")
    status = subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "-z", "--", "overlay"],
                            capture_output=True, text=True).stdout
    dirty = {x[3:] for x in status.split("\0") if len(x) > 3}
    return dates, dirty


def collect():
    dates, dirty = updated_dates()
    entries = []
    base = os.path.join(ROOT, "overlay")
    for dirpath, _, files in os.walk(base):
        if os.path.basename(os.path.dirname(dirpath)) != "patches-yacer":
            continue
        pkg = os.path.basename(dirpath)
        for f in files:
            if f.endswith(".patch"):
                entries.append((GROUPS.get(pkg, pkg), os.path.join(dirpath, f)))
    groups = {}
    for group, path in sorted(entries, key=lambda e: os.path.basename(e[1])):
        item = parse(path)
        item["file"] = os.path.relpath(path, ROOT)
        stamp = None if item["file"] in dirty else dates.get(item["file"])
        item["updated"] = (datetime.strptime(stamp, "%Y-%m-%d") if stamp else
                           datetime.fromtimestamp(os.path.getmtime(path), timezone.utc))
        item["num"] = (re.match(r"(\d+)-", os.path.basename(path)) or [None, ""])[1]
        groups.setdefault(group, []).append(item)
    return dict(sorted(groups.items(), key=lambda g: (ORDER.index(g[0]) if g[0] in ORDER else len(ORDER), g[0])))


def render(groups):
    sha = os.environ.get("GITHUB_SHA") or git("rev-parse", "HEAD")
    ref = sha or "yacer"
    total = sum(len(v) for v in groups.values())
    e = html.escape
    out = []
    n = 0
    for group, items in groups.items():
        out.append(f'<section><h2>{e(group)} <span class="count">{len(items)}</span></h2><ul>')
        for it in items:
            n += 1
            url = f"https://github.com/{REPO}/blob/{ref}/{it['file']}"
            title = it["title"][:1].upper() + it["title"][1:]
            desc = it["message"]
            when = f'updated {it["updated"].day} {it["updated"].strftime("%b %Y")}'
            who = " and ".join(x for x in (it["author"], ", ".join(it["credits"])) if x)
            meta = " · ".join(x for x in (who, when) if x)
            button = (f'<button type="button" class="ellipsis" aria-expanded="false" '
                      f'aria-controls="d{n}" aria-label="Show commit message" title="Show commit message">…</button>' if desc else "")
            panel = f'<pre class="desc" id="d{n}" hidden>{e(desc)}</pre>' if desc else ""
            out.append(f'<li><span class="num">{e(it["num"])}</span>'
                       f'<a class="title" href="{e(url)}">{e(title)}</a>{button}'
                       + (f'<div class="meta">{e(meta)}</div>' if meta else "") + f'{panel}</li>')
        out.append("</ul></section>")
    when = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    short = f'<a href="https://github.com/{REPO}/commit/{sha}">{sha[:7]}</a>' if sha else "working tree"
    return f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>YACER changes · {COREELEC}</title>
<style>
:root {{ --bg:#ffffff; --fg:#1f2328; --muted:#59636e; --line:#d1d9e0; --subtle:#f6f8fa; --accent:#0969da; --expander:rgba(129,139,152,.2); --expander-hover:rgba(129,139,152,.35); }}
@media (prefers-color-scheme: dark) {{ :root {{ --bg:#0d1117; --fg:#f0f6fc; --muted:#9198a1; --line:#3d444d; --subtle:#151b23; --accent:#4493f8; --expander:rgba(101,108,118,.2); --expander-hover:rgba(101,108,118,.4); }} }}
body {{ margin:0; background:var(--bg); color:var(--fg); font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI","Noto Sans",Helvetica,Arial,sans-serif,"Apple Color Emoji","Segoe UI Emoji"; padding:0 16px; }}
main {{ max-width:1012px; margin:0 auto; padding:32px 0 64px; }}
h1 {{ font-size:24px; font-weight:600; margin:0 0 8px; }}
.lead {{ color:var(--muted); margin:0 0 24px; }}
h2 {{ font-size:16px; font-weight:600; margin:28px 0 8px; }}
.count {{ display:inline-block; min-width:20px; padding:0 6px; font-size:12px; font-weight:500; line-height:18px; text-align:center; color:var(--fg); background:var(--expander); border-radius:2em; }}
ul {{ list-style:none; margin:0; padding:0; border:1px solid var(--line); border-radius:6px; }}
li {{ padding:8px 16px; border-top:1px solid var(--line); overflow-wrap:anywhere; }}
li:first-child {{ border-top:0; }}
.num {{ display:inline-block; min-width:4ch; margin-right:8px; font:12px/1.5 ui-monospace,SFMono-Regular,"SF Mono",Menlo,Consolas,"Liberation Mono",monospace; color:var(--muted); }}
.title {{ color:var(--fg); font-weight:600; }}
.title:hover {{ color:var(--accent); }}
.ellipsis {{ display:inline-block; height:12px; margin-left:6px; padding:0 5px 5px; font-size:12px; font-weight:600; line-height:6px; color:var(--fg); vertical-align:middle; background:var(--expander); border:0; border-radius:1px; cursor:pointer; }}
.ellipsis:hover, .ellipsis[aria-expanded="true"] {{ background:var(--expander-hover); }}
.ellipsis:focus-visible {{ outline:2px solid var(--accent); outline-offset:1px; }}
.meta {{ margin:2px 0 0 calc(4ch + 8px); font-size:12px; color:var(--muted); }}
.links .meta {{ margin-left:0; }}
.desc {{ margin:8px 0 0; padding:8px 12px; font:12px/1.5 ui-monospace,SFMono-Regular,"SF Mono",Menlo,Consolas,"Liberation Mono",monospace; color:var(--muted); white-space:pre-wrap; overflow-wrap:anywhere; background:var(--subtle); border-radius:6px; }}
a {{ color:var(--accent); text-decoration:none; }} a:hover {{ text-decoration:underline; }}
footer {{ color:var(--muted); font-size:12px; margin-top:40px; }}
</style></head>
<body><main>
<h1>What YACER builds change in {COREELEC}</h1>
<p class="lead">Unofficial {COREELEC} builds: everything is {COREELEC} as they ship it, plus the {total} patches below.
Release notes list what changed from one build to the next; this page lists the patches as they stand now.</p>
<section><h2>Repository</h2><ul class="links">
<li><a class="title" href="https://github.com/{REPO}/tree/yacer">Yacer</a><div class="meta">our patches, overlay and build workflows</div></li>
<li><a class="title" href="https://github.com/{REPO}/tree/coreelec-22">CoreELEC</a><div class="meta">the {COREELEC} tree the builds start from</div></li>
</ul></section>
<section><h2>Releases</h2><ul class="links">
<li><a class="title" href="https://github.com/{REPO}/releases">All builds</a><div class="meta">each release lists what changed since the build before it</div></li>
</ul></section>
{"".join(out)}
<footer>Generated {when} from {short}.</footer>
</main>
<script>
document.addEventListener("click", function (ev) {{
  var b = ev.target.closest(".ellipsis");
  if (!b) return;
  var d = document.getElementById(b.getAttribute("aria-controls"));
  d.hidden = !d.hidden;
  b.setAttribute("aria-expanded", String(!d.hidden));
}});
</script>
</body></html>
"""


def main():
    os.makedirs(OUT, exist_ok=True)
    groups = collect()
    with open(os.path.join(OUT, "index.html"), "w", encoding="utf-8") as fh:
        fh.write(render(groups))
    print(f"{sum(len(v) for v in groups.values())} patches -> {os.path.join(OUT, 'index.html')}")


if __name__ == "__main__":
    main()
