"""Read-along narration support for the HTML book.

Two jobs, both driven by the *rendered* page HTML so what is spoken and what is
highlighted can never drift apart:

1. `wrap(html)` finds the narratable blocks (headings, paragraphs, list items,
   table cells - never code blocks, flow diagrams or sample cards), tags each
   block with `data-b`, wraps every word in `<span class="w" data-i>`, and
   returns the spoken text for each block (with pronunciation fixes).
2. `align(block, words)` maps the word timings Kokoro reports for a block's
   spoken text back onto the block's on-page words.
"""
from __future__ import annotations

import hashlib
import html as htmllib
import json
import re
from html.parser import HTMLParser

BLOCK_TAGS = {"h1", "h2", "h3", "h4", "p", "li", "td", "dt", "dd"}
SKIP_TAGS = {"pre", "svg", "figure", "script", "style", "button", "nav", "th", "select", "textarea"}
SKIP_CLASSES = {"codeblock", "flow", "fig-row", "part-samples", "part-hero", "runbox", "anchor",
                "toc", "trend-fig", "listen", "narr-skip"}
VOID_TAGS = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param",
             "source", "track", "wbr"}

# Pronunciation fixes, in misaki's inline "[text](/phonemes/)" form so the token
# text Kokoro reports is still the on-page word (alignment depends on that).
LEXICON = {
    "Xbox": "[Xbox](/ˈɛksbˌɑks/)",
    "NVMe": "[NVMe](/ˌɛnvˌiˌɛmˈi/)",
    "LinAlg": "[LinAlg](/lˈɪn ˈælʤ/)",
    "SER": "[SER](/ˌɛsˌiˈɑɹ/)",
    "WaveMMA": "[WaveMMA](/wˈAv ˌɛmˌɛmˈA/)",
    "RaytracingTier": "[RaytracingTier](/ɹˈAtɹˌAsɪŋ tˈɪɹ/)",
    "Nuklear": "[Nuklear](/nˈukliəɹ/)",
    "DirectStorage": "[DirectStorage](/dəɹˈɛkt stˈɔɹɪʤ/)",
    "DirectCompute": "[DirectCompute](/dəɹˈɛkt kəmpjˈut/)",
    "DRED": "[DRED](/dɹˈɛd/)",
    "WARP": "[WARP](/wˈɔɹp/)",
    "PIX": "[PIX](/pˈɪks/)",
    "HDR10": "[HDR10](/ˌAʧdˌiˈɑɹ tˈɛn/)",
    "ps1": "[ps1](/pˌiˌɛs wˈʌn/)",
    "cpp": "[cpp](/sˌipˌipˈi/)",
    "dll": "[dll](/dˌiˌɛlˈɛl/)",
    "exe": "[exe](/ˈɛksi/)",
    "fxc": "[fxc](/ˌɛfˌɛksˈi/)",
    "dxc": "[dxc](/dˌiˌɛksˈi/)",
    "EHsc": "[EHsc](/ˌiˌAʧˌɛssˈi/)",
    "cbuffer": "[cbuffer](/sˈi bˌʌfəɹ/)",
    "scRGB": "[scRGB](/ˌɛssˈi ˌɑɹʤˌibˈi/)",
    "PCIe": "[PCIe](/pˌisˌiˌIˈi/)",
    "devkit": "[devkit](/dˈɛvkˌɪt/)",
}
LETTERS = {"a": "ˈA", "b": "bˈi", "c": "sˈi", "d": "dˈi", "g": "ʤˈi", "h": "ˈAʧ", "l": "ˈɛl",
           "m": "ˈɛm", "p": "pˈi", "s": "ˈɛs", "v": "vˈi", "i": "ˈI", "b.": "bˈi"}
ROMAN = {"I": 1, "II": 2, "III": 3, "IV": 4, "V": 5, "VI": 6, "VII": 7, "VIII": 8, "IX": 9,
         "X": 10, "XI": 11, "XII": 12}
PROFILE = re.compile(r"^(vs|ps|gs|hs|ds|cs|ms|as)_(\d+)_(\d+|x)$")
IPA_LINK = re.compile(r"\[([^\]]*)\]\(/[^)]*/\)")
STATS = {"words": 0, "matched": 0}   # spoken on-page words / those that got their own timing


def _spell(prefix: str) -> str:
    return f"[{prefix}](/{' '.join(LETTERS.get(c, c) for c in prefix)}/)"


def speech_form(word: str, prev: str | None) -> str:
    """How one on-page word should be spoken ("" = say nothing)."""
    core = word.strip("()[]{}\"'“”‘’")
    lead = word[: word.find(core)] if core else ""
    trail = word[word.find(core) + len(core):] if core else word
    if not core:
        return word if not re.search(r"[A-Za-z0-9]", word) else ""
    bare = core.rstrip(".,;:!?")
    punct = core[len(bare):]
    if prev and prev.strip("*") == "Part" and bare in ROMAN:
        return lead + str(ROMAN[bare]) + punct + trail
    if bare in LEXICON:
        return lead + LEXICON[bare] + punct + trail
    m = PROFILE.match(bare)
    if m:
        return f"{lead}{_spell(m.group(1))} {m.group(2)} {m.group(3)}{punct}{trail}"
    s = core
    s = s.replace("→", " to ").replace("←", " from ").replace("↔", " and ").replace("≈", " about ")
    s = re.sub(r"(?<=\d)[–-](?=\d)", " to ", s)                    # 7–8 -> 7 to 8
    s = re.sub(r"^~", "about ", s)
    s = re.sub(r"(?<=\d)K\b", " thousand", s)
    s = re.sub(r"(?<=\d)×(?=\d)", " by ", s).replace("×", " times ")
    s = s.replace("::", " ").replace("\\", " ").replace("/", " ").replace("|", " ")
    s = re.sub(r"(?<=[A-Za-z0-9])\.(?=[A-Za-z])", " ", s)          # D3D12.Basic -> D3D12 Basic
    if "_" in s:
        parts = [p for p in s.split("_") if p]
        # D3D12_FEATURE_DUMP_FILE -> "D3D12 feature dump file"; keep short acronyms.
        s = " ".join(p.lower() if (p.isupper() and len(p) > 4) else p for p in parts)
    s = re.sub(r"[`*]", "", s)
    # Second pass: pieces created above (main cpp, build-all ps1) can hit the lexicon too.
    out = []
    for piece in s.split(" "):
        b = piece.rstrip(".,;:!?)")
        out.append(LEXICON[b] + piece[len(b):] if b in LEXICON else piece)
    return lead + " ".join(out) + trail


def spoken_norm(text: str) -> str:
    """Lower-case letters/digits only; drops the phoneme half of [x](/ipa/)."""
    return re.sub(r"[^a-z0-9]", "", IPA_LINK.sub(r"\1", text).lower())


class _Wrapper(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=False)
        self.out: list[str] = []
        self.stack: list[tuple[str, bool, bool]] = []  # tag, opened skip, opened block
        self.skip = 0
        self.block: dict | None = None
        self.blocks: list[dict] = []
        self.buf: list[str] = []
        self.pending_space = True

    # text is buffered so entity refs (&amp;) stay inside their word
    def handle_data(self, data): self.buf.append(data)
    def handle_entityref(self, name): self.buf.append(f"&{name};")
    def handle_charref(self, name): self.buf.append(f"&#{name};")
    def handle_comment(self, data): self._flush(); self.out.append(f"<!--{data}-->")
    def handle_decl(self, decl): self._flush(); self.out.append(f"<!{decl}>")

    def _flush(self):
        if not self.buf:
            return
        text = "".join(self.buf)
        self.buf = []
        if self.block is None or self.skip:
            self.out.append(text)
            return
        for piece in re.split(r"(\s+)", text):
            if not piece:
                continue
            if piece.isspace():
                self.out.append(piece)
                self.pending_space = True
                continue
            b = self.block
            k = len(b["words"])
            b["words"].append(htmllib.unescape(piece))
            b["space"].append(self.pending_space)
            self.pending_space = False
            self.out.append(f'<span class="w" data-i="{k}">{piece}</span>')

    def handle_starttag(self, tag, attrs):
        self._flush()
        raw = self.get_starttag_text() or f"<{tag}>"
        classes = set((dict(attrs).get("class") or "").split())
        opens_skip = tag in SKIP_TAGS or bool(classes & SKIP_CLASSES)
        opens_block = False
        if not opens_skip and not self.skip and self.block is None and tag in BLOCK_TAGS:
            self.block = {"id": str(len(self.blocks)), "tag": tag, "words": [], "space": []}
            self.blocks.append(self.block)
            self.pending_space = True
            raw = raw[:-1].rstrip("/") + f' data-b="{self.block["id"]}">'
            opens_block = True
        if tag in BLOCK_TAGS or tag == "br":
            self.pending_space = True
        self.out.append(raw)
        if tag in VOID_TAGS:
            return
        if opens_skip:
            self.skip += 1
        self.stack.append((tag, opens_skip, opens_block))

    def handle_startendtag(self, tag, attrs):
        self._flush()
        self.out.append(self.get_starttag_text() or f"<{tag}/>")

    def handle_endtag(self, tag):
        self._flush()
        if any(t == tag for t, _, _ in self.stack):
            while self.stack:
                t, s, b = self.stack.pop()
                if s:
                    self.skip -= 1
                if b:
                    self.block = None
                if t == tag:
                    break
        self.out.append(f"</{tag}>")

    def close(self):
        super().close()
        self._flush()


def wrap(html: str) -> tuple[str, list[dict]]:
    """-> (html with data-b blocks + word spans, narratable blocks).

    Each returned block: {"id", "tag", "words", "space", "speech", "text"}, where
    `speech[i]` is how on-page word i is spoken and `text` is the block's full
    spoken text. Blocks with nothing to say are left out (but keep data-b).
    """
    p = _Wrapper()
    p.feed(html)
    p.close()
    blocks = []
    for b in p.blocks:
        speech, prev = [], None
        for w in b["words"]:
            speech.append(speech_form(w, prev))
            prev = w
        text = ""
        for form, space in zip(speech, b["space"]):
            if form:
                text += (" " if space and text else "") + form
        text = re.sub(r"\s+", " ", text).strip()
        if not re.search(r"[A-Za-z0-9]", IPA_LINK.sub(r"\1", text)):
            continue
        if b["tag"] in ("h1", "h2", "h3", "h4", "td", "dt") and re.search(r"[A-Za-z0-9)\]]$", text):
            text += "."          # a falling full stop after headings and cells
        blocks.append(dict(b, speech=speech, text=text))
    return "".join(p.out), blocks


def script_hash(blocks: list[dict], voice: str, speed: float) -> str:
    h = hashlib.sha1(json.dumps([[b["id"], b["text"]] for b in blocks] + [voice, speed],
                                ensure_ascii=False).encode("utf-8"))
    return h.hexdigest()[:16]


def align(block: dict, tokens: list[dict], block_start: float, block_end: float) -> list[list[float]]:
    """Per on-page word [start, end] (seconds, same clock as `tokens`).

    Tokens are matched to words through a shared letters-and-digits stream, so
    Kokoro splitting "D3D12's" or merging punctuation doesn't matter. Words
    that produced no speech (symbols, skipped text) inherit their neighbour's
    time so the highlight simply holds.
    """
    ranges, stream = [], ""
    for form in block["speech"]:
        n = spoken_norm(form)
        ranges.append((len(stream), len(stream) + len(n)))
        stream += n
    chars: list[tuple[float, float] | None] = [None] * len(stream)
    pos = 0
    for tok in tokens:
        n = spoken_norm(tok.get("text", ""))
        if not n:
            continue
        at = pos if stream.startswith(n, pos) else stream.find(n, pos, pos + 80)
        if at < 0:
            continue
        for c in range(at, at + len(n)):
            chars[c] = (float(tok["start"]), float(tok["end"]))
        pos = at + len(n)
    out: list[list[float] | None] = []
    for a, b in ranges:
        hit = [chars[c] for c in range(a, b) if chars[c]]
        out.append([min(h[0] for h in hit), max(h[1] for h in hit)] if hit else None)
        if b > a:
            STATS["words"] += 1
            STATS["matched"] += bool(hit)
    last = block_start
    for i, v in enumerate(out):
        if v is None:
            out[i] = [last, last]
        else:
            last = v[1]
    for v in out:
        v[0] = round(min(max(v[0], block_start), block_end), 3)
        v[1] = round(min(max(v[1], v[0]), block_end), 3)
    return out  # type: ignore[return-value]
