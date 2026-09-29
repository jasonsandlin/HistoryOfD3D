"""Builds the interactive HTML edition of "The History of Direct3D".

Inputs  : docs/*.md (the course text), every sample folder (main.cpp / main.c),
          screenshots/ (from tools/capture-screenshots.ps1), tools/book-src/ (CSS/JS).
Output  : book/  - a fully self-contained static site (no CDN, works from file://).

    pip install markdown pygments pillow
    python tools/build_book.py
"""
from __future__ import annotations

import html
import json
import re
import shutil
from dataclasses import dataclass, field
from pathlib import Path

import markdown
from PIL import Image
from pygments import highlight
from pygments.formatters import HtmlFormatter
from pygments.lexers import get_lexer_by_name

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"
SHOTS = ROOT / "screenshots"
BOOK_SRC = ROOT / "tools" / "book-src"
OUT = ROOT / "book"
AUDIO = ROOT / "audio"                      # narration source of truth (scripts, timings, chapters)
AUDIO_BOOK_NAME = "TheHistoryOfDirect3D.m4b"  # audio/ (built by tools/narrate_book.py); a download, not streamed
CHAPTER_AUDIO = AUDIO / "chapters"           # NN_<page>.m4a - one per page, what the player streams
REPO_URL = "https://github.com/jasonsandlin/HistoryOfD3D"
GITHUB_BASE = REPO_URL + "/blob/main/"
TREE_BASE = REPO_URL + "/tree/main/"
RELEASE_M4B = f"{REPO_URL}/releases/latest/download/{AUDIO_BOOK_NAME}"
SITE_URL = "https://jasonsandlin.github.io/HistoryOfD3D/"
# --web: build for GitHub Pages - no local "Run sample" launcher, links point at the
# public repo, and the m4b download comes from the latest GitHub Release.
WEB = False

# ----------------------------------------------------------------------------
# Static metadata
# ----------------------------------------------------------------------------
ERAS = [
    dict(id="D3D7", year=1999, name="Direct3D 7", identity="The GPU is an appliance you configure.",
         idea="Hardware transform & lighting; fixed-function render states.", part="part-01-fixed-function-era",
         hw="GeForce 256 brings hardware T&L to consumer PCs."),
    dict(id="D3D8", year=2000, name="Direct3D 8", identity="Consolidation, and the first programmable shaders.",
         idea="DirectDraw folded in; vertex/index buffers; shader assembly (SM 1.x).", part="part-01-fixed-function-era",
         hw="Programmable vertex units arrive; the original Xbox (2001) is built on a D3D8-class GPU."),
    dict(id="D3D9", year=2002, name="Direct3D 9", identity="The programmable pipeline matures.",
         idea="HLSL; vertex & pixel shaders you write.", part="part-02-programmable-shading",
         hw="SM 2.0/3.0 hardware; the Xbox 360 era of D3D9-style development."),
    dict(id="D3D10", year=2006, name="Direct3D 10", identity="Tear it down and rebuild it clean.",
         idea="No fixed function; DXGI; constant buffers; geometry shader.", part="part-03-great-redesign",
         hw="Unified shader cores replace separate vertex/pixel units."),
    dict(id="D3D11", year=2009, name="Direct3D 11", identity="The GPU becomes a general compute device.",
         idea="Multithreading; tessellation; DirectCompute; feature levels.", part="part-04-maturity-generality",
         hw="GPGPU goes mainstream; Xbox One ships with a D3D11.x-class API."),
    dict(id="D3D12", year=2015, name="Direct3D 12", identity="You manage the GPU yourself.",
         idea="Command lists/queues, PSOs, root signatures, explicit memory & sync.", part="part-05-going-explicit-d3d12",
         hw="Low-overhead console-style access on PC; Xbox Series X|S and DirectX 12 Ultimate."),
]
ERA_BY_ID = {e["id"]: e for e in ERAS}

TIERS = {
    "Tier1": "Fundamentals, resources & pipeline basics",
    "Tier2": "Memory, submission & extra geometry stages",
    "Tier3": "GPU work & multi-pass",
    "Tier4": "Advanced rendering & the modern pipeline",
    "Tier5": "Recording & querying at scale",
    "Tier6": "Classic pipeline mastery",
    "Tier7": "Presentation & devices",
    "Tier8": "Big resources & staying correct",
    "Tier9": "The frontier (Agility SDK)",
}

# (file stem, short label, era/tier tag shown on cards)
PARTS = [
    ("part-00-orientation", "Part 0", "Before 1999"),
    ("part-01-fixed-function-era", "Part I", "D3D7 · D3D8"),
    ("part-02-programmable-shading", "Part II", "D3D9"),
    ("part-03-great-redesign", "Part III", "D3D10"),
    ("part-04-maturity-generality", "Part IV", "D3D11"),
    ("part-05-going-explicit-d3d12", "Part V", "D3D12 · Tier 1"),
    ("part-06-feeding-the-gpu", "Part VI", "Tier 2 · Tier 3"),
    ("part-07-image-quality-modern-pipeline", "Part VII", "Tier 4 · SM 6"),
    ("part-08-recording-querying-at-scale", "Part VIII", "Tier 5"),
    ("part-09-classic-pipeline-mastery", "Part IX", "Tier 6"),
    ("part-10-presentation-devices-resources", "Part X", "Tier 7 · Tier 8"),
    ("part-11-the-frontier", "Part XI", "Tier 9 · Agility SDK"),
    ("part-12-synthesis", "Part XII", "The whole arc"),
    ("appendices", "Appendices", "Reference"),
]

BASE_DESCRIPTIONS = {
    "D3D7.Basic": "The 1999 baseline: DirectDraw 7 surfaces + a D3D7 device, steered entirely by render states and transforms. Windowed via a DirectDraw clipper and a manual back-buffer blit.",
    "D3D8.Basic": "DirectDraw is gone: one IDirect3D8 device, present parameters, vertex/index buffers. Uses vendored Wine headers because the modern SDK no longer ships d3d8.h.",
    "D3D9.Basic": "The IDirect3D9 fixed-function cube - the baseline the D3D9 shader samples build on.",
    "D3D10.Basic": "The great redesign: device + DXGI swap chain, render-target views, constant buffers and HLSL compiled at runtime with D3DCompile. No fixed function left.",
    "D3D11.Basic": "Device and immediate context split apart, feature levels, and the same HLSL pipeline as D3D10.",
    "D3D12.Basic": "The minimal explicit cube: command queue, allocator, list, fence, PSO and a root signature with MVP passed as 32-bit root constants.",
    "D3D12.Nuklear": "An immediate-mode GUI (Nuklear) rendered on a Direct3D 12 backend - the full widget overview that every annotated D3D12 sample builds its live panel on.",
}

FAIL_REASONS = {
    "D3D7": "Needs the legacy DirectDraw/D3D7 HAL, which the capture machine no longer exposes.",
    "D3D8": "Needs the legacy d3d8.dll runtime, which the capture machine no longer exposes.",
    "D3D12.DirectStorage": "DirectStorage factory creation returned E_NOTIMPL on the capture machine - see the sample's .log.",
    "D3D12.SamplerFeedback": "Root-signature creation failed on the capture machine's driver - see the sample's .log.",
}

VERSION_ALIASES = {"D3D7": "D3D7.Basic", "D3D8": "D3D8.Basic", "D3D9": "D3D9.Basic",
                   "D3D10": "D3D10.Basic", "D3D11": "D3D11.Basic", "D3D12": "D3D12.Basic"}


@dataclass
class Sample:
    name: str
    path: Path
    rel: str
    main: str
    era: str
    tier: str | None
    lines: int
    lang: str
    desc: str = ""
    stage: str = ""
    shot: dict = field(default_factory=dict)
    parts: list = field(default_factory=list)

    @property
    def url(self):
        return f"sample-{self.name}.html"

    @property
    def group(self):
        if self.tier:
            return self.tier
        return "D3D7-8" if self.era in ("D3D7", "D3D8") else self.era

    @property
    def ok(self):
        return bool(self.shot.get("ok"))

    @property
    def short(self):
        return self.name.split(".", 1)[1] if "." in self.name else self.name


# ----------------------------------------------------------------------------
# Discovery
# ----------------------------------------------------------------------------
def discover_samples() -> list[Sample]:
    status = {}
    if (SHOTS / "shots.json").exists():
        status = json.loads((SHOTS / "shots.json").read_text(encoding="utf-8-sig"))
    samples = []
    for d in sorted(ROOT.rglob("*")):
        if not d.is_dir() or any(p in ("third_party", "Out", "book", "screenshots", "tools", "docs") for p in d.relative_to(ROOT).parts):
            continue
        main = next((m for m in ("main.cpp", "main.c") if (d / m).exists()), None)
        if not main:
            continue
        name = d.name
        era = name.split(".")[0]
        tier = next((p for p in d.relative_to(ROOT).parts if p.startswith("Tier")), None)
        text = (d / main).read_text(encoding="utf-8", errors="replace")
        samples.append(Sample(name=name, path=d, rel=d.relative_to(ROOT).as_posix(), main=main, era=era, tier=tier,
                              lines=text.count("\n") + 1, lang="C" if main.endswith(".c") else "C++",
                              shot=status.get(name, {})))
    era_order = [e["id"] for e in ERAS]
    samples.sort(key=lambda s: (era_order.index(s.era), s.tier or "", 0 if s.name.endswith(".Basic") else 1, s.name))
    return samples


def attach_descriptions(samples: list[Sample]):
    by_name = {s.name: s for s in samples}
    md = (DOCS / "SAMPLES.md").read_text(encoding="utf-8")
    for line in md.splitlines():
        m = re.match(r"^\|\s*`([^`]+)`\s*\|(.*)\|\s*$", line)
        if not m or m.group(1) not in by_name:
            continue
        cells = [c.strip() for c in m.group(2).split("|")]
        s = by_name[m.group(1)]
        s.desc = cells[-1]
        if len(cells) == 2:
            s.stage = cells[0].replace("`", "")
    for n, d in BASE_DESCRIPTIONS.items():
        if n in by_name:
            by_name[n].desc = d
    for s in samples:
        if not s.desc:
            s.desc = f"{s.short} on Direct3D {s.era[3:]}."


def plain(md_text: str) -> str:
    t = re.sub(r"`([^`]*)`", r"\1", md_text)
    t = re.sub(r"\*\*([^*]+)\*\*", r"\1", t)
    t = re.sub(r"\*([^*]+)\*", r"\1", t)
    return re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", t)


def md_inline(text: str) -> str:
    h = html.escape(text, quote=False)
    h = re.sub(r"`([^`]+)`", r"<code>\1</code>", h)
    h = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", h)
    h = re.sub(r"(?<![*\w])\*([^*]+)\*(?!\*)", r"<em>\1</em>", h)
    return h


# ----------------------------------------------------------------------------
# Images
# ----------------------------------------------------------------------------
def build_images(samples: list[Sample]):
    img_dir = OUT / "assets" / "img"
    img_dir.mkdir(parents=True, exist_ok=True)
    for s in samples:
        png = SHOTS / f"{s.name}.png"
        if not s.ok or not png.exists():
            s.shot["ok"] = False
            continue
        im = Image.open(png).convert("RGB")
        full = im.copy()
        full.thumbnail((1600, 1600), Image.LANCZOS)
        full.save(img_dir / f"{s.name}.jpg", quality=88, optimize=True)
        th = im.copy()
        th.thumbnail((640, 640), Image.LANCZOS)
        th.save(img_dir / f"{s.name}.thumb.jpg", quality=84, optimize=True)
        s.shot["aspect"] = round(im.width / im.height, 4)
        anim = SHOTS / f"{s.name}.anim.jpg"
        if anim.exists():
            shutil.copyfile(anim, img_dir / f"{s.name}.anim.jpg")
            s.shot["anim"] = True


# ----------------------------------------------------------------------------
# Markdown -> HTML
# ----------------------------------------------------------------------------
LEXERS = {"c": "c", "cpp": "cpp", "hlsl": "hlsl", "powershell": "powershell", "bat": "batch"}


def render_code_block(lang: str, code: str) -> str:
    if lang in LEXERS:
        body = highlight(code, get_lexer_by_name(LEXERS[lang]), HtmlFormatter(nowrap=True))
        label = {"cpp": "C++", "c": "C", "hlsl": "HLSL", "powershell": "PowerShell", "bat": "Batch"}[lang]
        return (f'<div class="codeblock"><div class="codeblock-bar"><span>{label}</span>'
                f'<button class="copy" type="button" aria-label="Copy code">Copy</button></div>'
                f'<pre class="hl"><code>{body}</code></pre></div>')
    # Unlabeled fences in the course are pseudo-code flow diagrams.
    return (f'<div class="flow"><div class="flow-bar"><span>Flow</span></div>'
            f'<pre><code>{html.escape(code)}</code></pre></div>')


def preprocess_md(text: str, placeholders: dict) -> str:
    lines = text.splitlines()
    out, i = [], 0
    while i < len(lines):
        line = lines[i]
        m = re.match(r"^```(\w*)\s*$", line)
        if m:
            lang, buf = m.group(1).lower(), []
            i += 1
            while i < len(lines) and not lines[i].startswith("```"):
                buf.append(lines[i])
                i += 1
            key = f"CODEBLOCK{len(placeholders)}X"
            placeholders[key] = render_code_block(lang, "\n".join(buf))
            out += ["", key, ""]
            i += 1
            continue
        out.append(line)
        i += 1
    text = "\n".join(out)
    # Drop the markdown breadcrumb/next-links: the book renders its own navigation.
    text = re.sub(r"^\*\[«[^\n]*\*\s*\n+(---\s*\n)?", "", text, flags=re.M)
    text = re.sub(r"\n---\s*\n+\*(Next:|Continue to)[\s\S]*?\*\s*$", "\n", text)
    text = re.sub(r"\n\*(Next:|Continue to)[\s\S]*?\*\s*$", "\n", text)
    return text


def link_rewrite(h: str) -> str:
    def fix(m):
        href = m.group(1)
        href = href.replace("./", "")
        href = re.sub(r"^D3D-Evolution-Course\.md", "course.html", href)
        href = re.sub(r"^SAMPLES\.md", "reference.html", href)
        href = re.sub(r"^([\w-]+)\.md", r"\1.html", href)
        return f'href="{href}"'
    return re.sub(r'href="([^"]+\.md[^"]*)"', fix, h)


def resolve_sample(code_text: str, by_name: dict, by_short: dict):
    t = html.unescape(code_text).strip().replace("/", "\\")
    t = re.sub(r"^RotatingCubes\\", "", t)
    if t in VERSION_ALIASES:
        return by_name.get(VERSION_ALIASES[t])
    for seg in t.split("\\"):
        if seg in by_name:
            return by_name[seg]
    if t in by_short:
        return by_short[t]
    return None


def link_samples(h: str, by_name, by_short, found: list):
    parts = re.split(r"(<pre[\s\S]*?</pre>)", h)
    for i in range(0, len(parts), 2):
        def rep(m):
            s = resolve_sample(m.group(1), by_name, by_short)
            if not s:
                return m.group(0)
            found.append(s.name)
            return f'<a class="sample-ref" href="{s.url}" data-sample="{s.name}">{m.group(0)}</a>'
        parts[i] = re.sub(r"<code>([^<]+)</code>", rep, parts[i])
    return "".join(parts)


def decorate(h: str) -> str:
    h = re.sub(r"<blockquote>\s*<p><strong>(Decision lens[^<]*)</strong>",
               r'<blockquote class="lens"><p class="lens-title">\1</p><p>', h)
    h = re.sub(r"<blockquote>\s*<p><strong>([^<]+)</strong>",
               r'<blockquote class="note"><p><strong>\1</strong>', h)
    h = h.replace("<table>", '<div class="table-wrap"><table>').replace("</table>", "</table></div>")
    return h


def sample_figure(samples: list[Sample]) -> str:
    cards = "".join(sample_card(s, compact=True) for s in samples)
    cls = "fig-row" if len(samples) > 1 else "fig-row single"
    return f'<div class="{cls}">{cards}</div>'


def insert_figures(h: str, by_name):
    """Place each referenced sample's screenshot card once, after the first
    paragraph of the first section (h2/h3) that mentions it. The intro before
    the first h2 is skipped - the part's sample strip already covers it."""
    chunks = re.split(r"(?=<h[23][ >])", h)
    placed = set()
    for i, ch in enumerate(chunks):
        if i == 0 and not re.match(r"<h[23]", ch):
            continue
        names = []
        for n in re.findall(r'data-sample="([^"]+)"', ch):
            if n not in placed and n not in names:
                names.append(n)
        if not names:
            continue
        placed.update(names)
        fig = sample_figure([by_name[n] for n in names])
        # after the first paragraph, or right after the heading if none
        pm = re.search(r"</p>", ch)
        if pm:
            chunks[i] = ch[:pm.end()] + fig + ch[pm.end():]
        else:
            hm = re.search(r"</h[23]>", ch)
            chunks[i] = ch[:hm.end()] + fig + ch[hm.end():] if hm else fig + ch
    return "".join(chunks), placed


def md_to_html(text: str, by_name, by_short):
    placeholders = {}
    src = preprocess_md(text, placeholders)
    md = markdown.Markdown(extensions=["tables", "toc", "sane_lists"],
                           extension_configs={"toc": {"permalink": "#", "permalink_class": "anchor", "toc_depth": "2-3"}})
    h = md.convert(src)
    for k, v in placeholders.items():
        h = h.replace(f"<p>{k}</p>", v).replace(k, v)
    h = link_rewrite(h)
    found = []
    h = link_samples(h, by_name, by_short, found)
    h = decorate(h)
    title_m = re.search(r"<h1[^>]*>([\s\S]*?)</h1>", h)
    title = html.unescape(re.sub(r"<[^>]+>", "", title_m.group(1))).replace("#", "").strip() if title_m else ""
    return h, title, md.toc_tokens, list(dict.fromkeys(found))


# ----------------------------------------------------------------------------
# Page chrome
# ----------------------------------------------------------------------------
LOGO = ('<svg class="logo-mark" viewBox="0 0 40 40" aria-hidden="true"><defs><linearGradient id="lg" x1="0" y1="0" x2="1" y2="1">'
        '<stop offset="0" stop-color="#9BF00B"/><stop offset="1" stop-color="#107C10"/></linearGradient></defs>'
        '<circle cx="20" cy="20" r="18" fill="none" stroke="url(#lg)" stroke-width="3"/>'
        '<path d="M20 9 L30 14.5 L30 25.5 L20 31 L10 25.5 L10 14.5 Z" fill="none" stroke="#9BF00B" stroke-width="1.6"/>'
        '<path d="M20 9 L20 20 M20 20 L30 14.5 M20 20 L10 14.5 M20 20 L20 31" stroke="#52B043" stroke-width="1.2" opacity=".7"/></svg>')


def topbar(active: str) -> str:
    links = [("course.html", "Course", "course"), ("samples.html", "Samples", "samples"),
             ("compare.html", "Compare eras", "compare"), ("reference.html", "Reference", "reference")]
    nav = "".join(f'<a href="{u}" class="{"active" if k == active else ""}">{t}</a>' for u, t, k in links)
    if WEB:
        nav += f'<a href="{REPO_URL}" target="_blank" rel="noopener">GitHub</a>'
    return f"""<header class="topbar">
  <button class="menu-btn" type="button" aria-label="Toggle navigation">&#9776;</button>
  <a class="brand" href="index.html">{LOGO}<span><b>History of</b> Direct3D</span></a>
  <nav class="topnav">{nav}</nav>
  <button class="search-btn" type="button" aria-label="Search"><svg viewBox="0 0 24 24" width="16" height="16"><circle cx="10.5" cy="10.5" r="6.5" fill="none" stroke="currentColor" stroke-width="2"/><path d="M15.5 15.5 L21 21" stroke="currentColor" stroke-width="2" stroke-linecap="round"/></svg><span>Search</span><kbd>Ctrl K</kbd></button>
</header>
<div class="progress"><div class="progress-bar"></div></div>"""


def sidebar(current: str, parts_meta) -> str:
    items = ['<a class="side-link {}" href="course.html"><span class="num">&#9679;</span><span>Welcome &amp; syllabus</span></a>'.format(
        "active" if current == "course" else "")]
    for stem, label, tag, title in parts_meta:
        items.append(f'<a class="side-link {"active" if current == stem else ""}" href="{stem}.html" data-page="{stem}">'
                     f'<span class="num">{label.replace("Part ", "") if label.startswith("Part") else "A"}</span>'
                     f'<span>{html.escape(title)}<small>{tag}</small></span></a>')
    extra = [("samples", "samples.html", "Sample gallery"), ("compare", "compare.html", "Compare eras"),
             ("reference", "reference.html", "Sample reference")]
    ex = "".join(f'<a class="side-link {"active" if current == k else ""}" href="{u}"><span class="num">&#9670;</span><span>{t}</span></a>'
                 for k, u, t in extra)
    return f'<aside class="sidebar"><div class="side-title">The course</div>{"".join(items)}<div class="side-title">Explore</div>{ex}</aside>'


def page(title: str, body: str, active: str = "", extra_head: str = "", body_class: str = "", page_id: str = "",
         extra_body: str = "") -> str:
    return f"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)} · The History of Direct3D</title>
<link rel="icon" href="data:image/svg+xml,{html.escape(LOGO.replace('class="logo-mark" ', 'xmlns="http://www.w3.org/2000/svg" ').replace('#', '%23'))}">
<link rel="stylesheet" href="assets/book.css"><link rel="stylesheet" href="assets/pygments.css">{extra_head}
</head><body class="{body_class}" data-page="{page_id}">
{topbar(active)}
{body}
<div class="search-overlay" hidden><div class="search-panel"><input type="search" placeholder="Search the course and samples…" aria-label="Search"><div class="search-results"></div><div class="search-hint"><kbd>↑</kbd><kbd>↓</kbd> navigate · <kbd>Enter</kbd> open · <kbd>Esc</kbd> close</div></div></div>
<div class="lightbox" hidden><img alt=""><div class="lightbox-cap"></div></div>
<div class="hovercard" hidden></div>
{extra_body}<script src="assets/data.js"></script><script src="assets/book.js"></script>
</body></html>"""


def shot_html(s: Sample, cls="shot", full=False) -> str:
    if s.ok:
        img = f"assets/img/{s.name}.{'jpg' if full else 'thumb.jpg'}"
        anim = f' data-anim="assets/img/{s.name}.anim.jpg" data-frames="{s.shot.get("frames", 12)}" data-dur="{s.shot.get("ms", s.shot.get("frames", 12) * 110)}"' if s.shot.get("anim") else ""
        return (f'<div class="{cls}"{anim} style="--ar:{s.shot.get("aspect", 1.6)}"><img loading="lazy" src="{img}" alt="{s.name} screenshot"'
                f' data-full="assets/img/{s.name}.jpg"><div class="anim"></div></div>')
    reason = FAIL_REASONS.get(s.name) or FAIL_REASONS.get(s.era, "This sample needs a GPU/driver feature the capture machine didn't expose; it shows an init dialog instead.")
    return (f'<div class="{cls} noshot" style="--ar:1.6"><div class="noshot-inner"><div class="noshot-cube"></div>'
            f'<b>No live capture</b><span>{html.escape(reason)}</span></div></div>')


def sample_card(s: Sample, compact=False) -> str:
    tier = f'<span class="chip tier">{s.tier.replace("Tier", "Tier ")}</span>' if s.tier else ""
    desc = plain(s.desc)
    if compact and len(desc) > 150:
        desc = desc[:147].rsplit(" ", 1)[0] + "…"
    return (f'<a class="card sample-card era-{s.era.lower()}" href="{s.url}" data-era="{s.era}" data-group="{s.group}" data-name="{s.name.lower()}">'
            f'{shot_html(s)}<div class="card-body"><div class="chips"><span class="chip era">{s.era}</span>{tier}'
            f'<span class="chip lines">{s.lines:,} lines</span></div><h4>{s.name}</h4><p>{html.escape(desc)}</p></div></a>')


# ----------------------------------------------------------------------------
# Pages
# ----------------------------------------------------------------------------
def toc_html(tokens) -> str:
    items = []
    for t in tokens:
        if t["level"] == 1:
            for c in t.get("children", []):
                items.append((c, 2))
                items += [(g, 3) for g in c.get("children", [])]
        else:
            items.append((t, t["level"]))
            items += [(g, 3) for g in t.get("children", [])]
    lis = "".join(f'<a class="toc-l{lvl}" href="#{t["id"]}">{html.escape(re.sub("<[^>]+>", "", html.unescape(t["name"])))}</a>' for t, lvl in items)
    return f'<nav class="toc"><div class="toc-title">On this page</div>{lis}</nav>' if lis else ""


def reading_page(stem, h, title, tokens, parts_meta, prev, nxt, header_html="", current=None) -> str:
    pn = ""
    if prev:
        pn += f'<a class="pn prev" href="{prev[0]}"><small>← Previous</small><span>{html.escape(prev[1])}</span></a>'
    else:
        pn += "<span></span>"
    if nxt:
        pn += f'<a class="pn next" href="{nxt[0]}"><small>Next →</small><span>{html.escape(nxt[1])}</span></a>'
    body = f"""<div class="layout">
{sidebar(current or stem, parts_meta)}
<main class="content"><article class="prose">{header_html}{h}</article>
<div class="pager">{pn}</div>
<footer class="foot">The History of Direct3D · an Xbox side project · <kbd>←</kbd>/<kbd>→</kbd> page · <kbd>F</kbd> fullscreen · <kbd>/</kbd> search</footer></main>
{toc_html(tokens)}
</div>"""
    return body


def part_header(label, tag, title, samples_here: list[Sample], placed=()) -> str:
    strip = ""
    if samples_here:
        def mini(s):
            anchor = "" if s.name in placed else f' id="s-{s.name}"'
            label = s.name if s.short == "Basic" else s.short
            return (f'<a class="mini" href="{s.url}"{anchor} data-sample="{s.name}" title="{s.name}">'
                    f'{shot_html(s, "shot mini-shot")}<span>{label}</span></a>')
        thumbs = "".join(mini(s) for s in samples_here)
        strip = f'<div class="part-samples"><div class="ps-label">{len(samples_here)} sample{"s" if len(samples_here) != 1 else ""} in this part</div><div class="ps-strip">{thumbs}</div></div>'
    return f'<div class="part-hero"><div class="eyebrow">{label} · {tag}</div></div>{strip}'


TREND_SVG = """<figure class="trend-fig"><svg viewBox="0 0 900 340" class="trend" role="img" aria-label="Programmability rose from D3D7 to D3D11 then plateaued; explicitness stayed flat until D3D12 then climbed.">
<defs><linearGradient id="tg1" x1="0" x2="1"><stop offset="0" stop-color="#2f6f2f"/><stop offset="1" stop-color="#9BF00B"/></linearGradient>
<linearGradient id="tg2" x1="0" x2="1"><stop offset="0" stop-color="#1c3b1c"/><stop offset=".55" stop-color="#52B043"/><stop offset="1" stop-color="#e8ffd0"/></linearGradient>
<filter id="glow"><feGaussianBlur stdDeviation="4" result="b"/><feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter></defs>
<g class="grid">{grid}</g>
<path class="t1" d="M70 280 C 160 272, 200 240, 250 205 S 380 120, 430 95 S 520 70, 590 66 S 760 62, 860 62" fill="none" stroke="url(#tg1)" stroke-width="5" filter="url(#glow)"/>
<path class="t2" d="M70 292 C 200 290, 420 288, 520 282 S 580 250, 640 170 S 760 70, 860 40" fill="none" stroke="url(#tg2)" stroke-width="5" stroke-dasharray="1 0" filter="url(#glow)"/>
{labels}
<g class="legend"><rect x="80" y="18" width="14" height="4" fill="#9BF00B"/><text x="100" y="24">Trend 1 · Programmability — what the GPU computes</text>
<rect x="480" y="18" width="14" height="4" fill="#e8ffd0"/><text x="500" y="24">Trend 2 · Explicitness — who does the bookkeeping</text></g>
<text class="note" x="445" y="138">Shaders can express anything →</text><text class="note" x="445" y="156">the bottleneck moves to the CPU</text>
</svg><figcaption>The two trends from <a href="part-12-synthesis.html">Part XII</a>: D3D7→D3D11 is the story of programmability; D3D11→D3D12→frontier is the story of explicitness.</figcaption></figure>"""


def trend_svg() -> str:
    xs = [("D3D7", 1999, 70), ("D3D8", 2000, 160), ("D3D9", 2002, 250), ("D3D10", 2006, 340), ("D3D11", 2009, 430),
          ("D3D12", 2015, 590), ("Agility", 2020, 725), ("Frontier", 2026, 860)]
    grid = "".join(f'<line x1="{x}" y1="40" x2="{x}" y2="300"/>' for _, _, x in xs)
    labels = "".join(f'<text class="ax" x="{x}" y="322" text-anchor="middle">{n}</text><text class="yr" x="{x}" y="336" text-anchor="middle">{y}</text>' for n, y, x in xs)
    return TREND_SVG.replace("{grid}", grid).replace("{labels}", labels)


def cube_html() -> str:
    faces = "".join(f'<div class="face f{i}"></div>' for i in range(6))
    return f'<div class="cube-stage"><div class="cube-glow"></div><div class="cube">{faces}</div><div class="cube-floor"></div></div>'


HEADPHONES = ('<svg viewBox="0 0 24 24" width="18" height="18" aria-hidden="true"><path d="M4 14v-2a8 8 0 0 1 16 0v2" fill="none" '
              'stroke="currentColor" stroke-width="2" stroke-linecap="round"/><rect x="3" y="13" width="5" height="8" rx="2" fill="currentColor"/>'
              '<rect x="16" y="13" width="5" height="8" rx="2" fill="currentColor"/></svg>')


class Narrator:
    """Per-page read-along: writes the narration script tools/narrate_book.py
    speaks, and - once audio exists - wraps words, aligns timings, and adds the
    player. Pages keep working (just without audio) when nothing is narrated."""

    def __init__(self):
        import narration
        self.nr = narration
        narration.STATS.update(words=0, matched=0)
        self.voice, self.speed = "af_heart", 1.0
        cfg = AUDIO / "narration.json"
        if cfg.exists():
            c = json.loads(cfg.read_text(encoding="utf-8"))
            self.voice, self.speed = c.get("voice", self.voice), float(c.get("speed", self.speed))
        out_audio = OUT / "assets" / "audio"
        shutil.rmtree(out_audio, ignore_errors=True)       # rebuilt from audio/ every time
        out_audio.mkdir(parents=True, exist_ok=True)
        self.available = any(CHAPTER_AUDIO.glob("*.m4a"))
        # The whole-book m4b is a download: the GitHub Release for the web build,
        # a local copy for the offline (zip) edition.
        self.m4b_href = None
        m4b = AUDIO / AUDIO_BOOK_NAME
        if WEB:
            self.m4b_href = RELEASE_M4B
        elif m4b.exists():
            shutil.copyfile(m4b, out_audio / AUDIO_BOOK_NAME)
            self.m4b_href = f"assets/audio/{AUDIO_BOOK_NAME}"
        if (AUDIO / "cover.jpg").exists():
            shutil.copyfile(AUDIO / "cover.jpg", out_audio / "cover.jpg")
        self.order: list[str] = []
        self.stale: list[str] = []
        self.minutes = {}
        (AUDIO / "script").mkdir(parents=True, exist_ok=True)

    def page(self, stem: str, title: str, h: str) -> tuple[str, str]:
        """-> (article html, extra body html for the player)."""
        index = len(self.order)
        self.order.append(stem)
        key = f"{index:02d}_{stem}"
        wrapped, blocks = self.nr.wrap(h)
        digest = self.nr.script_hash(blocks, self.voice, self.speed)
        script = {"title": title, "hash": digest, "voice": self.voice, "speed": self.speed,
                  "blocks": [{"id": b["id"], "text": b["text"]} for b in blocks]}
        (AUDIO / "script" / f"{key}.json").write_text(json.dumps(script, ensure_ascii=False, indent=1), encoding="utf-8")

        timings_path = AUDIO / "timings" / f"{key}.json"
        chapter = CHAPTER_AUDIO / f"{key}.m4a"
        if not (self.available and chapter.exists() and timings_path.exists()):
            if self.available:
                self.stale.append(stem)
            return h, ""
        timings = json.loads(timings_path.read_text(encoding="utf-8"))
        if timings.get("hash") != digest:
            self.stale.append(stem)
        audio_rel = f"assets/audio/{stem}.m4a"
        shutil.copyfile(chapter, OUT / audio_rel)
        by_id = {tb["id"]: tb for tb in timings.get("blocks", [])}
        out = []
        for b in blocks:
            tb = by_id.get(b["id"])
            if not tb or tb.get("start") is None or self.nr.spoken_norm(tb["text"]) != self.nr.spoken_norm(b["text"]):
                continue        # text changed since narration: this block plays but isn't highlighted
            words = self.nr.align(b, tb.get("words", []), float(tb["start"]), float(tb["end"]))
            flat = [round(t, 2) for w in words for t in w]
            out.append([int(b["id"]), round(tb["start"], 2), round(tb["end"], 2), flat])
        end = round(float(timings.get("duration") or (out[-1][2] if out else 0)), 2)
        self.minutes[stem] = max(1, round(end / 60))
        payload = {"page": stem, "title": title, "audio": audio_rel, "start": 0, "end": end, "blocks": out}
        (OUT / "assets" / "audio" / f"{stem}.js").write_text("window.__narr=" + json.dumps(payload, separators=(",", ":")) + ";",
                                                             encoding="utf-8")
        listen = (f'<div class="listen"><button class="listen-btn" type="button">{HEADPHONES}<span>Listen</span></button>'
                  f'<span class="listen-meta">{self.minutes[stem]} min read-along · words highlight as they\'re spoken</span></div>')
        m = re.search(r'<div class="part-hero">.*?</div></div>', wrapped)
        at = m.end() if m else (re.search(r"</h1>", wrapped).end() if re.search(r"</h1>", wrapped) else 0)
        wrapped = wrapped[:at] + listen + wrapped[at:]
        dl = (f'<a class="pl-dl" href="{self.m4b_href}" download title="Download the whole audiobook (.m4b) for an audiobook app">m4b</a>'
              if self.m4b_href else "")
        player = f"""<div class="player" hidden role="region" aria-label="Audiobook player">
 <audio preload="none" src="{audio_rel}"></audio>
 <div class="pl-controls">
  <button class="pl-back" type="button" title="Back 15 s (J)">&#8634;<small>15</small></button>
  <button class="pl-play" type="button" title="Play / pause (K)"><span class="pl-ic"></span></button>
  <button class="pl-fwd" type="button" title="Forward 15 s (L)">&#8635;<small>15</small></button>
 </div>
 <div class="pl-main"><div class="pl-title">{html.escape(title)}</div>
  <div class="pl-bar"><span class="pl-cur">0:00</span><input class="pl-seek" type="range" min="0" max="1000" value="0" aria-label="Seek"><span class="pl-dur">0:00</span></div></div>
 <div class="pl-side">
  <button class="pl-speed" type="button" title="Playback speed">1&times;</button>
  <button class="pl-follow on" type="button" title="Scroll to follow the narration">Follow</button>
  {dl}
  <button class="pl-close" type="button" title="Close player">&times;</button>
 </div>
 <div class="pl-next" hidden></div>
</div>
<script src="assets/audio/{stem}.js"></script>
"""
        return wrapped, player

    def report(self):
        if not self.available:
            print("Narration: no audiobook yet - run tools\\narrate_book.py to add read-along audio")
        elif self.stale:
            print(f"Narration: text changed since last narration on {len(self.stale)} page(s): {', '.join(self.stale)}"
                  " - re-run tools\\narrate_book.py")
        else:
            s = self.nr.STATS
            rate = f", {s['matched'] / s['words']:.1%} of {s['words']:,} spoken words word-synced" if s["words"] else ""
            print(f"Narration: {len(self.order)} pages narrated{rate}")


def audiobook_button() -> str:
    """Home-page link to the read-along audiobook (only once it has been narrated)."""
    chapters = AUDIO / "chapters.json"
    if not (any(CHAPTER_AUDIO.glob("*.m4a")) and chapters.exists()):
        return ""
    total = json.loads(chapters.read_text(encoding="utf-8")).get("duration", 0)
    h, m = divmod(int(round(total / 60)), 60)
    length = f"{h} h {m:02d} min" if h else f"{m} min"
    out = (f'<a class="btn big ghost audiobook" href="course.html?listen=1" title="Every page has a Listen button with word-by-word read-along">'
           f'{HEADPHONES}<span>Listen · {length}</span></a>')
    href = RELEASE_M4B if WEB else (f"assets/audio/{AUDIO_BOOK_NAME}" if (AUDIO / AUDIO_BOOK_NAME).exists() else None)
    if href:
        out += (f'<a class="btn big ghost" href="{href}" download title="Chaptered audiobook for any podcast/audiobook app">'
                f'Download .m4b</a>')
    return out


def build_index(samples, parts_meta, part_samples) -> str:
    by_name = {s.name: s for s in samples}
    basics = [by_name[n] for n in ["D3D7.Basic", "D3D8.Basic", "D3D9.Basic", "D3D10.Basic", "D3D11.Basic", "D3D12.Basic", "D3D12.Fundamentals"] if n in by_name]
    mx = max(s.lines for s in basics)
    bars = "".join(
        f'<a class="bar-row" href="{s.url}"><span class="bar-label">{s.name}</span><span class="bar"><span class="bar-fill" style="--w:{s.lines / mx * 100:.1f}%"></span></span><span class="bar-val" data-count="{s.lines}">{s.lines}</span></a>'
        for s in basics)

    tl_nodes, tl_panels = [], []
    for i, e in enumerate(ERAS):
        b = by_name.get(f"{e['id']}.Basic")
        tl_nodes.append(f'<button class="tl-node{" active" if i == 0 else ""}" data-idx="{i}" type="button"><span class="tl-dot"></span><span class="tl-year">{e["year"]}</span><span class="tl-name">{e["id"]}</span></button>')
        era_samples = [s for s in samples if s.era == e["id"]]
        shot = shot_html(b, "shot tl-shot") if b else ""
        tl_panels.append(f"""<div class="tl-panel{" active" if i == 0 else ""}" data-idx="{i}">
<div class="tl-text"><div class="eyebrow">{e["year"]} · {e["name"]}</div><h3>{e["identity"]}</h3><p class="tl-idea">{e["idea"]}</p>
<p class="tl-hw"><span>Meanwhile, in hardware:</span> {e["hw"]}</p>
<div class="tl-stats"><div><b>{b.lines if b else "–"}</b><small>lines for the cube</small></div><div><b>{len(era_samples)}</b><small>sample{"s" if len(era_samples) != 1 else ""}</small></div></div>
<div class="tl-actions"><a class="btn" href="{e["part"]}.html">Read the chapter</a>{f'<a class="btn ghost" href="{b.url}">Open {b.name}</a>' if b else ""}</div></div>
<a class="tl-media" href="{b.url if b else "#"}">{shot}</a></div>""")

    cards = []
    for stem, label, tag, title in parts_meta:
        ss = part_samples.get(stem, [])
        thumbs = "".join(f'<img loading="lazy" src="assets/img/{s.name}.thumb.jpg" alt="">' for s in ss if s.ok)[:4000]
        thumbs = "".join(re.findall(r"<img[^>]+>", thumbs)[:3])
        cards.append(f"""<a class="card part-card" href="{stem}.html" data-page="{stem}"><div class="pc-thumbs">{thumbs or '<div class="pc-empty"></div>'}</div>
<div class="card-body"><div class="chips"><span class="chip era">{label}</span><span class="chip">{tag}</span>{f'<span class="chip lines">{len(ss)} samples</span>' if ss else ""}</div>
<h4>{html.escape(title)}</h4><span class="visited-badge">✓ Read</span></div></a>""")

    mosaic = "".join(f'<a class="mosaic-tile" href="{s.url}" data-sample="{s.name}">{shot_html(s)}</a>' for s in samples if s.ok)
    n_ok = sum(1 for s in samples if s.ok)
    body = f"""<main class="home">
<section class="hero"><div class="hero-bg"></div><div class="hero-inner">
<div class="hero-text"><div class="eyebrow glow">A code-driven history · 1999 → today</div>
<h1>The History of <span>Direct3D</span></h1>
<p class="lede">Twenty-seven years of Microsoft's graphics API, told through <b>one spinning cube</b>. Every sample renders the exact same thing — so the only thing that changes is the code it takes to get there.</p>
<div class="hero-actions"><a class="btn big" href="course.html">Start reading</a><a class="btn big ghost" href="samples.html">Browse {len(samples)} samples</a>{audiobook_button()}</div>
<p class="hero-quote">The GPU went from a fixed appliance you <em>configure</em>, to a programmable processor you <em>feed</em>, to a raw device you <em>manage</em>.</p>
{hero_setup()}
</div>{cube_html()}</div>
<div class="stats"><div><b data-count="6">6</b><small>API generations</small></div><div><b data-count="{len(samples)}">{len(samples)}</b><small>runnable samples</small></div><div><b data-count="13">13</b><small>parts</small></div><div><b data-count="9">9</b><small>D3D12 learning tiers</small></div><div><b>1</b><small>cube</small></div></div>
</section>

<section class="band"><div class="band-inner"><div class="section-head"><div class="eyebrow">The timeline</div><h2>Six eras, one cube</h2><p>Click an era to see what defined it.</p></div>
<div class="timeline"><div class="tl-track"><div class="tl-line"></div>{"".join(tl_nodes)}</div><div class="tl-panels">{"".join(tl_panels)}</div></div></div></section>

<section class="band alt"><div class="band-inner split"><div><div class="eyebrow">The controlled experiment</div><h2>Same cube. More code.</h2>
<p>When D3D7 needs {by_name["D3D7.Basic"].lines} lines and D3D12's fundamentals need {by_name["D3D12.Fundamentals"].lines}, that difference isn't noise — <b>it is the lesson</b>. Every extra line buys back control the older runtimes were quietly exercising on your behalf.</p>
<a class="btn" href="compare.html">Compare two eras side by side →</a></div>
<div class="bars">{bars}<div class="bars-note">Lines in each sample's single source file.</div></div></div></section>

<section class="band"><div class="band-inner"><div class="section-head"><div class="eyebrow">The arc in one picture</div><h2>Two trends run the whole story</h2></div>{trend_svg()}</div></section>

<section class="band alt"><div class="band-inner"><div class="section-head"><div class="eyebrow">The syllabus</div><h2>Thirteen parts, cumulative</h2><p>Each part is a lecture wrapped around real, buildable programs.</p></div>
<div class="part-grid">{"".join(cards)}</div></div></section>

<section class="band"><div class="band-inner"><div class="section-head"><div class="eyebrow">Live captures</div><h2>{n_ok} cubes, captured from the real samples</h2><p>Hover any tile to watch it spin.</p></div>
<div class="mosaic">{mosaic}</div><div class="center"><a class="btn big" href="samples.html">Open the gallery</a></div></div></section>
<footer class="foot home-foot">{LOGO}<span>The History of Direct3D — an Xbox side project. Built from <code>docs/</code> and the RotatingCubes samples by <code>tools/build_book.py</code>.</span></footer>
</main>"""
    return page("Home", body, active="", body_class="is-home", page_id="index")


def build_gallery(samples, parts_meta) -> str:
    groups = [("all", "All"), ("D3D7-8", "D3D7 · D3D8")] + [(e["id"], e["id"]) for e in ERAS if e["id"] not in ("D3D7", "D3D8", "D3D12")] + \
             [(t, t.replace("Tier", "D3D12 · Tier ")) for t in TIERS]
    btns = "".join(f'<button class="filter{" active" if k == "all" else ""}" data-filter="{k}" type="button">{v}</button>' for k, v in groups)
    sections = []
    era_groups = [("D3D7-8", "Direct3D 7 & 8", "The fixed-function era: the GPU is an appliance you configure.")] + \
                 [(e["id"], e["name"], e["identity"]) for e in ERAS if e["id"] not in ("D3D7", "D3D8", "D3D12")]
    for key, label, desc in era_groups + [(t, f"D3D12 · {t.replace('Tier', 'Tier ')}", d) for t, d in TIERS.items()]:
        ss = [s for s in samples if s.group == key]
        if not ss:
            continue
        sections.append(f'<section class="gal-group" id="{key}" data-group="{key}"><div class="gal-head"><h3>{label}</h3><p>{desc}</p></div>'
                        f'<div class="card-grid">{"".join(sample_card(s) for s in ss)}</div></section>')
    body = f"""<div class="layout wide">{sidebar("samples", parts_meta)}<main class="content">
<div class="page-head"><div class="eyebrow">Sample gallery</div><h1>{len(samples)} programs, one cube</h1>
<p class="lede">Every sample is a single-file Win32 app. Each card plays a live capture of the real program; click one for the story, the build command and the full source.</p>
<div class="gal-tools"><div class="gal-row"><input class="gal-search" type="search" placeholder="Filter samples…" aria-label="Filter samples"><button class="anim-toggle" type="button" aria-pressed="false"><span class="ic"></span><span class="lbl">Pause animations</span></button></div><div class="filters">{btns}</div></div></div>
{"".join(sections)}
<footer class="foot">Captured with <code>tools/capture-screenshots.ps1</code>.</footer></main></div>"""
    return page("Sample gallery", body, active="samples", page_id="samples")


def extra_sources(s: Sample) -> list[Path]:
    files = []
    for f in sorted(s.path.iterdir()):
        if f.is_file() and f.suffix.lower() in (".c", ".cpp", ".h", ".hlsl", ".ps1", ".bat") and f.name != s.main \
                and not f.name.lower().startswith("d3d8"):
            files.append(f)
    return files


def lexer_for(p: Path):
    return get_lexer_by_name({".c": "c", ".h": "c", ".cpp": "cpp", ".hlsl": "hlsl", ".ps1": "powershell", ".bat": "batch"}[p.suffix.lower()])


def hero_setup() -> str:
    if WEB:
        return (f'<p class="hero-setup"><b>Want to run the samples?</b> Every one is a single-file Win32 program. Clone '
                f'<a href="{REPO_URL}">{REPO_URL.replace("https://", "")}</a> and build them with '
                f'<code>.\\build-all.ps1</code> (Visual Studio with C++), or grab the prebuilt offline edition from the '
                f'<a href="{REPO_URL}/releases/latest">latest release</a>.</p>')
    return ('<p class="hero-setup"><b>Reading needs nothing</b>: the book works straight from disk. To launch the real samples from their pages, '
            'run <code>powershell -ExecutionPolicy Bypass -File .\\setup.ps1</code> once in the <code>HistoryOfD3D</code> folder.</p>')


def run_button(s) -> str:
    if WEB:
        return (f'<a class="btn run-btn" href="{TREE_BASE}{s.rel}" target="_blank" rel="noopener">'
                f'<span class="run-ic"></span>Build it on GitHub</a>')
    return f'<a class="btn run-btn" href="d3dbook:run/{s.name}" data-sample="{s.name}"><span class="run-ic"></span>Run sample</a>'


def run_hint(s) -> str:
    if WEB:
        return (f'<p class="run-hint">Source and build script in <code>{s.rel}</code>. Prebuilt exes are in the offline edition on the '
                f'<a href="{REPO_URL}/releases/latest">releases page</a>.</p>')
    return (f'<p class="run-hint">Launches <code>{s.rel.replace("/", chr(92))}\\Out\\Cube{s.name}.exe</code> (built on first run). '
            f'First time? Run <code>setup.ps1</code> once from the <code>HistoryOfD3D</code> folder.</p>')


def build_sample_page(s: Sample, samples, parts_meta, titles) -> str:
    idx = samples.index(s)
    prev_s = samples[idx - 1] if idx > 0 else None
    next_s = samples[idx + 1] if idx + 1 < len(samples) else None
    files = [s.path / s.main] + extra_sources(s)
    tabs, panes = [], []
    for i, f in enumerate(files):
        code = f.read_text(encoding="utf-8", errors="replace")
        hl = highlight(code, lexer_for(f), HtmlFormatter(linenos="table", lineanchors=f"f{i}L", anchorlinenos=True, cssclass="hl src"))
        tabs.append(f'<button class="tab{" active" if i == 0 else ""}" data-tab="{i}" type="button">{f.name}<small>{code.count(chr(10)) + 1} lines</small></button>')
        rel = f.relative_to(ROOT).as_posix()
        panes.append(f'<div class="pane{" active" if i == 0 else ""}" data-tab="{i}"><div class="pane-bar"><span>{rel}</span><span class="pane-actions">'
                     + ("" if WEB else f'<a href="../{rel}">Open local file</a>') + f'<a href="{GITHUB_BASE}{rel}" target="_blank" rel="noopener">View on GitHub</a>'
                     f'<button class="copy-src" type="button">Copy</button></span></div>{hl}</div>')
    tier = f'<span class="chip tier">{s.tier.replace("Tier", "Tier ")} · {TIERS[s.tier]}</span>' if s.tier else ""
    era = ERA_BY_ID[s.era]
    discussed = "".join(f'<a href="{p}.html#s-{s.name}">{html.escape(titles[p])}</a>' for p in s.parts) or "<span>—</span>"
    exe = f"{s.rel.replace('/', chr(92))}\\Out\\Cube{s.name}.exe"
    run_cmd = f".\\build-all.ps1 -Only {s.name}\n.\\{exe}"
    if (s.path / "fetch-deps.ps1").exists():
        run_cmd = f".\\{s.rel.replace('/', chr(92))}\\fetch-deps.ps1\n" + run_cmd
    if s.name == "D3D12.LinearAlgebra":
        run_cmd = f".\\{s.rel.replace('/', chr(92))}\\fetch-deps.ps1\n.\\build-all.ps1 -Only {s.name}\n.\\{s.rel.replace('/', chr(92))}\\run.ps1"
    runbox_label = "Build &amp; run (in a clone of the repo)" if WEB else "Build &amp; run (from HistoryOfD3D\\)"
    related = [x for x in samples if x.group == s.group and x is not s][:4]
    legacy_note = ""
    if s.era in ("D3D7", "D3D8"):
        legacy_note = ('<blockquote class="note legacy"><p><strong>Heads-up: build it x86.</strong> On modern Windows the legacy '
                       f'{"DirectDraw/D3D7 (d3dim700.dll)" if s.era == "D3D7" else "d3d8.dll"} runtime ships only as a 32-bit DLL in '
                       '<code>SysWOW64</code>, so the default x64 build shows “init failed”. Build from a <code>vcvars32.bat</code> '
                       'prompt (<code>cl /EHsc /I. main.cpp user32.lib</code>) — that is how the capture above was made.</p></blockquote>')
    body = f"""<div class="layout wide">{sidebar("samples", parts_meta)}<main class="content sample-page">
<div class="crumbs"><a href="samples.html">Samples</a><span>/</span><a href="samples.html#{s.group}">{s.tier.replace("Tier", "D3D12 · Tier ") if s.tier else s.era}</a><span>/</span><b>{s.name}</b></div>
<div class="sample-hero">
  <div class="sample-media">{shot_html(s, "shot big-shot", full=True)}{'<div class="media-hint">Hover to play the captured animation · click to enlarge</div>' if s.ok else ""}</div>
  <div class="sample-info"><div class="chips"><span class="chip era">{era["name"]} · {era["year"]}</span>{tier}</div>
  <h1>{s.name}</h1><p class="lede">{md_inline(s.desc)}</p>
  <dl class="facts"><div><dt>Source</dt><dd>{s.main} · {s.lang}</dd></div><div><dt>Size</dt><dd>{s.lines:,} lines</dd></div>
  {f'<div><dt>Stage</dt><dd>{html.escape(s.stage)}</dd></div>' if s.stage else ""}<div><dt>Discussed in</dt><dd class="disc">{discussed}</dd></div></dl>
  <div class="runbox"><div class="runbox-bar"><span>{runbox_label}</span><button class="copy" type="button">Copy</button></div><pre><code>{html.escape(run_cmd)}</code></pre></div>{legacy_note}
  <div class="tl-actions">{run_button(s)}<a class="btn ghost" href="#source">Read the source</a><a class="btn ghost" href="compare.html?a={s.name}">Compare with another era</a></div>
  {run_hint(s)}
  </div></div>
<section id="source" class="source"><div class="tabs">{"".join(tabs)}</div>{"".join(panes)}</section>
{f'<section class="related"><h3>More from {s.tier.replace("Tier", "Tier ") if s.tier else s.era}</h3><div class="card-grid">{"".join(sample_card(x) for x in related)}</div></section>' if related else ""}
<div class="pager">{f'<a class="pn prev" href="{prev_s.url}"><small>← Previous sample</small><span>{prev_s.name}</span></a>' if prev_s else "<span></span>"}{f'<a class="pn next" href="{next_s.url}"><small>Next sample →</small><span>{next_s.name}</span></a>' if next_s else ""}</div>
<footer class="foot"><kbd>R</kbd> run sample · <kbd>←</kbd>/<kbd>→</kbd> previous/next sample · <kbd>/</kbd> search</footer></main></div>"""
    return page(s.name, body, active="samples", page_id="sample")


def build_compare(samples, parts_meta) -> str:
    opts = "".join(f'<option value="{s.name}">{s.name} ({s.lines} lines)</option>' for s in samples)
    body = f"""<div class="layout wide">{sidebar("compare", parts_meta)}<main class="content compare-page">
<div class="page-head"><div class="eyebrow">Hold the cube constant</div><h1>Compare two eras</h1>
<p class="lede">Pick any two samples. The output is the same spinning cube — everything you see in the code is the API's price of admission.</p></div>
<div class="cmp-presets"><span>Presets:</span><button data-a="D3D7.Basic" data-b="D3D8.Basic">D3D7 → D3D8</button><button data-a="D3D9.Basic" data-b="D3D10.Basic">D3D9 → D3D10</button><button data-a="D3D11.Basic" data-b="D3D12.Basic">D3D11 → D3D12</button><button data-a="D3D7.Basic" data-b="D3D12.Basic">1999 vs 2015</button><button data-a="D3D12.ComputeShader" data-b="D3D12.WorkGraphs">Compute → Work graphs</button><button data-a="D3D12.Bindless" data-b="D3D12.DynamicResources">SM5.1 → SM6.6 bindless</button></div>
<div class="cmp">
 <div class="cmp-col" data-side="a"><div class="cmp-head"><select aria-label="Left sample">{opts}</select></div><div class="cmp-meta"></div><div class="cmp-code"></div></div>
 <div class="cmp-col" data-side="b"><div class="cmp-head"><select aria-label="Right sample">{opts}</select></div><div class="cmp-meta"></div><div class="cmp-code"></div></div>
</div><footer class="foot">Scrolling is synchronized · <label><input type="checkbox" class="sync" checked> sync scroll</label></footer></main></div>"""
    return page("Compare eras", body, active="compare", page_id="compare")


def write_code_js(s: Sample):
    d = OUT / "assets" / "code"
    d.mkdir(parents=True, exist_ok=True)
    f = s.path / s.main
    code = f.read_text(encoding="utf-8", errors="replace")
    hl = highlight(code, lexer_for(f), HtmlFormatter(nowrap=True))
    lines = hl.split("\n")
    payload = json.dumps({"name": s.name, "lines": lines}, ensure_ascii=False)
    (d / f"{s.name}.js").write_text(f"window.__code=window.__code||{{}};window.__code[{json.dumps(s.name)}]={payload};", encoding="utf-8")


def search_chunks(h: str, url: str, page_title: str):
    chunks = re.split(r"(?=<h[123][ >])", h)
    out = []
    for ch in chunks:
        hm = re.match(r'<h([123])[^>]*id="([^"]+)"[^>]*>([\s\S]*?)</h\1>', ch)
        text = re.sub(r"<pre[\s\S]*?</pre>", " ", ch)
        text = re.sub(r'<div class="fig-row[\s\S]*?</div></a></div>', " ", text)
        text = html.unescape(re.sub(r"<[^>]+>", " ", text))
        text = re.sub(r"\s+", " ", text).strip()
        if not text:
            continue
        if hm:
            head = html.unescape(re.sub(r"<[^>]+>", "", hm.group(3))).replace("#", "").strip()
            u = f"{url}#{hm.group(2)}"
            text = text[len(head):].strip() if text.startswith(head) else text
        else:
            head, u = page_title, url
        out.append({"t": head, "p": page_title, "u": u, "x": text[:1200]})
    return out


def main():
    import argparse
    global WEB
    ap = argparse.ArgumentParser(description="Render the History of Direct3D book into book/.")
    ap.add_argument("--web", action="store_true", help="build for GitHub Pages (no local Run launcher)")
    WEB = ap.parse_known_args()[0].web
    OUT.mkdir(exist_ok=True)
    for sub in ("assets/img", "assets/code"):
        (OUT / sub).mkdir(parents=True, exist_ok=True)
    for f in BOOK_SRC.iterdir():
        shutil.copyfile(f, OUT / "assets" / f.name)
    (OUT / "assets" / "pygments.css").write_text(HtmlFormatter(style="github-dark").get_style_defs(".hl"), encoding="utf-8")

    samples = discover_samples()
    attach_descriptions(samples)
    build_images(samples)
    by_name = {s.name: s for s in samples}
    by_short = {}
    for s in samples:
        if s.era == "D3D12":
            by_short.setdefault(s.short, s)

    # Pass 1: render markdown
    rendered = {}
    for stem, label, tag in PARTS:
        text = (DOCS / f"{stem}.md").read_text(encoding="utf-8")
        h, title, toc, found = md_to_html(text, by_name, by_short)
        short_title = re.sub(r"^(Part [0IVX]+|Appendices)\s*[—-]\s*", "", title) or title
        rendered[stem] = (h, short_title, toc, found, label, tag)
        for n in found:
            if stem not in by_name[n].parts:
                by_name[n].parts.append(stem)
    parts_meta = [(stem, rendered[stem][4], rendered[stem][5], rendered[stem][1]) for stem, _, _ in PARTS]
    titles = {stem: rendered[stem][4] if rendered[stem][4] == rendered[stem][1] else f"{rendered[stem][4]} — {rendered[stem][1]}" for stem, _, _ in PARTS}
    part_samples = {stem: [by_name[n] for n in rendered[stem][3]] for stem in rendered}

    search = []
    narrator = Narrator()
    # Course landing page
    ch, ctitle, ctoc, _ = md_to_html((DOCS / "D3D-Evolution-Course.md").read_text(encoding="utf-8"), by_name, by_short)
    first_stem = PARTS[0][0]
    ch_body, ch_player = narrator.page("course", "Welcome & syllabus", ch)
    (OUT / "course.html").write_text(page("Welcome", reading_page("course", ch_body, ctitle, ctoc, parts_meta, ("index.html", "Home"),
                                                                  (f"{first_stem}.html", titles[first_stem])), page_id="course",
                                          extra_body=ch_player), encoding="utf-8")
    search += search_chunks(ch, "course.html", "Welcome & syllabus")

    for i, (stem, label, tag) in enumerate(PARTS):
        h, title, toc, found, label, tag = rendered[stem]
        h, placed = insert_figures(h, by_name)
        # anchor ids so sample pages can deep-link to the figure in each part
        h = re.sub(r'<a class="card sample-card([^"]*)" href="sample-([^"]+)\.html"',
                   lambda m: f'<a id="s-{m.group(2)}" class="card sample-card{m.group(1)}" href="sample-{m.group(2)}.html"', h)
        if stem == "part-12-synthesis":
            h = re.sub(r"(<h2[^>]*>Module 16[\s\S]*?</h2>)", lambda m: m.group(1) + trend_svg(), h, count=1)
        prev = ("course.html", "Welcome & syllabus") if i == 0 else (f"{PARTS[i - 1][0]}.html", titles[PARTS[i - 1][0]])
        nxt = (f"{PARTS[i + 1][0]}.html", titles[PARTS[i + 1][0]]) if i + 1 < len(PARTS) else ("samples.html", "Sample gallery")
        hdr = part_header(label, tag, title, part_samples[stem], placed)
        # put the eyebrow + sample strip right after the H1
        h = re.sub(r"(</h1>)", lambda m: m.group(1) + hdr, h, count=1)
        body, player = narrator.page(stem, titles[stem], h)
        (OUT / f"{stem}.html").write_text(page(titles[stem], reading_page(stem, body, title, toc, parts_meta, prev, nxt), page_id=stem,
                                               extra_body=player), encoding="utf-8")
        search += search_chunks(h, f"{stem}.html", titles[stem])

    rh, rtitle, rtoc, _ = md_to_html((DOCS / "SAMPLES.md").read_text(encoding="utf-8"), by_name, by_short)
    (OUT / "reference.html").write_text(page("Sample reference", reading_page("reference", rh, rtitle, rtoc, parts_meta, ("samples.html", "Sample gallery"), None),
                                             active="reference", page_id="reference"), encoding="utf-8")
    search += search_chunks(rh, "reference.html", "Sample reference")

    for s in samples:
        (OUT / s.url).write_text(build_sample_page(s, samples, parts_meta, titles), encoding="utf-8")
        write_code_js(s)
        search.append({"t": s.name, "p": "Sample", "u": s.url, "x": plain(s.desc), "k": "sample"})

    (OUT / "index.html").write_text(build_index(samples, parts_meta, part_samples), encoding="utf-8")
    (OUT / "samples.html").write_text(build_gallery(samples, parts_meta), encoding="utf-8")
    (OUT / "compare.html").write_text(build_compare(samples, parts_meta), encoding="utf-8")

    data = {
        "samples": {s.name: {"era": s.era, "tier": s.tier, "desc": plain(s.desc), "lines": s.lines, "url": s.url,
                             "ok": s.ok, "anim": bool(s.shot.get("anim")), "frames": s.shot.get("frames", 12), "ms": s.shot.get("ms", s.shot.get("frames", 12) * 110), "lang": s.lang}
                    for s in samples},
        "pages": [f"{stem}.html" for stem, _, _ in PARTS],
        "search": search,
    }
    (OUT / "assets" / "data.js").write_text("window.BOOK=" + json.dumps(data, ensure_ascii=False) + ";", encoding="utf-8")
    ok = sum(1 for s in samples if s.ok)
    print(f"Built {OUT}  ({len(PARTS)} parts, {len(samples)} samples, {ok} with screenshots, {len(search)} search entries)")
    narrator.report()


if __name__ == "__main__":
    main()
