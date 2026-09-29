"""Narrates the HTML book into a chaptered read-along audiobook.

    python tools/narrate_book.py                  # narrate changed pages, rebuild audio + book
    python tools/narrate_book.py --voice bf_lily  # different Kokoro voice (re-narrates everything)
    python tools/narrate_book.py --only part-05-going-explicit-d3d12 --force

Pipeline (uses a Kokoro TTS toolkit - tts.py, make_m4b.py, subtitles.py - found via
--tools, %AUDIO_TOOLS%, or C:\\_AudioTODO):
  1. tools/build_book.py renders every page and writes its narration script to
     audio/script/NN_<page>.json - the page's prose blocks only (no code).
  2. For each page whose script changed:  tts.py --blocks ... -f wav --word-srt
     -> audio/work/NN_<page>.wav/.srt/.words.srt/.timings.json
  3. Each page's WAV is encoded to audio/chapters/NN_<page>.m4a - what the book's
     per-page player streams (small files, fine for GitHub Pages).
  4. make_m4b.py joins the WAVs (sample-exact chapter offsets) into
     audio/TheHistoryOfDirect3D.m4b with one chapter per page, a cover and an
     embedded read-along subtitle track, for audiobook apps; a whole-book
     word-level SRT is written beside it.
  5. build_book runs again to wrap words and add the player to every page.

audio/work/ is a cache (git-ignored). audio/script, audio/timings, audio/chapters/
are what the book needs; the m4b is a download (GitHub Release / offline zip).
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import build_book  # noqa: E402

ROOT = build_book.ROOT
AUDIO = build_book.AUDIO
WORK = AUDIO / "work"
CHAPTERS = build_book.CHAPTER_AUDIO
M4B = AUDIO / build_book.AUDIO_BOOK_NAME


def audio_tools(explicit: str | None) -> Path:
    for cand in (explicit, os.environ.get("AUDIO_TOOLS"), r"C:\_AudioTODO"):
        if cand and (Path(cand) / "tts.py").exists() and (Path(cand) / "make_m4b.py").exists():
            return Path(cand)
    sys.exit("Kokoro toolkit not found: pass --tools <folder with tts.py> or set AUDIO_TOOLS.")


def author() -> str:
    try:
        name = subprocess.run(["git", "config", "user.name"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    except OSError:
        name = ""
    return name or "The History of Direct3D"


def make_cover(path: Path) -> None:
    """1400x1400 cover: Xbox-green glow, title, and a captured cube."""
    from PIL import Image, ImageDraw, ImageFilter, ImageFont

    W = 1400
    img = Image.new("RGB", (W, W), (8, 11, 8))
    glow = Image.new("RGB", (W, W), (0, 0, 0))
    d = ImageDraw.Draw(glow)
    d.ellipse((150, 380, 1250, 1300), fill=(16, 124, 16))
    img = Image.blend(img, glow.filter(ImageFilter.GaussianBlur(160)), 0.55)
    shot = ROOT / "screenshots" / "D3D12.RayTracing12.png"
    if shot.exists():
        cube = Image.open(shot).convert("RGB")
        side = int(min(cube.size) * 0.78)
        cx, cy = cube.width // 2, cube.height // 2
        cube = cube.crop((cx - side // 2, cy - side // 2, cx + side // 2, cy + side // 2)).resize((820, 820))
        mask = Image.new("L", cube.size, 0)
        ImageDraw.Draw(mask).ellipse((20, 20, 800, 800), fill=255)
        img.paste(cube, (290, 470), mask.filter(ImageFilter.GaussianBlur(40)))
    d = ImageDraw.Draw(img)
    fonts = Path(os.environ.get("WINDIR", r"C:\Windows")) / "Fonts"

    def font(name, size):
        try:
            return ImageFont.truetype(str(fonts / name), size)
        except OSError:
            return ImageFont.load_default()

    d.text((W // 2, 170), "THE HISTORY OF", font=font("segoeui.ttf", 64), fill=(152, 168, 152), anchor="mm")
    d.text((W // 2, 290), "Direct3D", font=font("segoeuib.ttf", 170), fill=(155, 240, 11), anchor="mm")
    d.text((W // 2, 1320), "1999 → today  ·  one spinning cube", font=font("segoeui.ttf", 50), fill=(200, 215, 200), anchor="mm")
    path.parent.mkdir(parents=True, exist_ok=True)
    img.save(path, quality=90)


def run(cmd: list[str]) -> None:
    print("  $ " + " ".join(f'"{c}"' if " " in c else c for c in cmd), flush=True)
    subprocess.run(cmd, check=True)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--voice", help="Kokoro voice (default: audio/narration.json, else af_heart)")
    ap.add_argument("--speed", type=float, help="Speech speed (default 1.0)")
    ap.add_argument("--tools", help="Folder containing tts.py / make_m4b.py (default C:\\_AudioTODO)")
    ap.add_argument("--only", action="append", default=[], help="Only (re)narrate this page stem; repeatable")
    ap.add_argument("--force", action="store_true", help="Re-narrate even if the text is unchanged")
    ap.add_argument("--bitrate", default="64k", help="AAC bitrate for the m4b")
    args = ap.parse_args()

    tools = audio_tools(args.tools)
    cfg_path = AUDIO / "narration.json"
    cfg = json.loads(cfg_path.read_text(encoding="utf-8")) if cfg_path.exists() else {}
    cfg["voice"] = args.voice or cfg.get("voice", "af_heart")
    cfg["speed"] = args.speed or cfg.get("speed", 1.0)
    AUDIO.mkdir(parents=True, exist_ok=True)
    cfg_path.write_text(json.dumps(cfg, indent=1), encoding="utf-8")

    print("== 1/5 Rendering pages and writing narration scripts")
    build_book.main()

    print(f"\n== 2/5 Synthesizing changed pages with Kokoro ({cfg['voice']}, {cfg['speed']}x)")
    WORK.mkdir(parents=True, exist_ok=True)
    (AUDIO / "timings").mkdir(exist_ok=True)
    scripts = sorted((AUDIO / "script").glob("*.json"))
    keys = {s.stem for s in scripts}
    for stale in list(WORK.iterdir()) + list((AUDIO / "timings").iterdir()):
        if stale.name.split(".")[0] not in keys and stale.name != "playlist.m3u":
            stale.unlink()
    playlist = ["#EXTM3U", "#PLAYLIST:The History of Direct3D"]
    for s in scripts:
        script = json.loads(s.read_text(encoding="utf-8"))
        key = s.stem
        stem = key.split("_", 1)[1]
        wav, hash_file = WORK / f"{key}.wav", WORK / f"{key}.hash"
        playlist += [f"#EXTINF:-1,{script['title']}", wav.name]
        fresh = wav.exists() and hash_file.exists() and hash_file.read_text().strip() == script["hash"]
        if fresh and not args.force and not (args.only and stem in args.only):
            print(f"  = {key}  (unchanged)")
            continue
        if args.only and stem not in args.only and wav.exists():
            print(f"  ~ {key}  (changed, skipped by --only)")
            continue
        print(f"  + {key}  ({len(script['blocks'])} blocks, {sum(len(b['text']) for b in script['blocks']):,} chars)")
        blocks = WORK / f"{key}.blocks.json"
        blocks.write_text(json.dumps(script["blocks"], ensure_ascii=False), encoding="utf-8")
        run([sys.executable, str(tools / "tts.py"), "--blocks", str(blocks), "-o", str(wav), "-f", "wav",
             "--word-srt", "-v", cfg["voice"], "-s", str(cfg["speed"])])
        timings = json.loads((WORK / f"{key}.timings.json").read_text(encoding="utf-8"))
        timings["hash"] = script["hash"]
        (AUDIO / "timings" / f"{key}.json").write_text(json.dumps(timings, ensure_ascii=False), encoding="utf-8")
        hash_file.write_text(script["hash"])
    (WORK / "playlist.m3u").write_text("\n".join(playlist) + "\n", encoding="utf-8")

    print("\n== 3/5 Encoding per-page audio (what the book streams)")
    ffmpeg = shutil.which("ffmpeg") or sys.exit("ffmpeg not found on PATH (winget install Gyan.FFmpeg)")
    CHAPTERS.mkdir(parents=True, exist_ok=True)
    keep = set()
    changed = False
    for s in scripts:
        key = s.stem
        wav, m4a = WORK / f"{key}.wav", CHAPTERS / f"{key}.m4a"
        keep.add(m4a.name)
        if not wav.exists():
            continue
        if m4a.exists() and m4a.stat().st_mtime >= wav.stat().st_mtime and not args.force:
            continue
        title = json.loads(s.read_text(encoding="utf-8"))["title"]
        tmp = m4a.with_name(m4a.stem + ".partial.m4a")
        run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", str(wav), "-c:a", "aac", "-b:a", args.bitrate,
             "-ac", "1", "-ar", "24000", "-metadata", f"title={title}", "-metadata", "album=The History of Direct3D",
             "-movflags", "+faststart", str(tmp)])
        os.replace(tmp, m4a)
        changed = True
    for old in CHAPTERS.glob("*.m4a"):
        if old.name not in keep:
            old.unlink()
            changed = True

    print("\n== 4/5 Building the chaptered m4b (download for audiobook apps)")
    cover = AUDIO / "cover.jpg"
    if not cover.exists():
        make_cover(cover)
    wavs = list(WORK.glob("*.wav"))
    stale_m4b = not M4B.exists() or any(w.stat().st_mtime > M4B.stat().st_mtime for w in wavs)
    if stale_m4b or not (AUDIO / "chapters.json").exists():
        # Encode to a temp name and swap it in only on success, so an interrupted
        # run never leaves a half-written (unplayable) m4b behind.
        tmp = M4B.with_name(M4B.stem + ".partial.m4b")
        tmp_chapters = AUDIO / "chapters.partial.json"
        run([sys.executable, str(tools / "make_m4b.py"), str(WORK), "-o", str(tmp),
             "--title", "The History of Direct3D", "--album", "The History of Direct3D",
             "--author", author(), "--year", "2026", "--cover", str(cover), "-b", args.bitrate,
             "--chapters-json", str(tmp_chapters)])
        meta = json.loads(tmp_chapters.read_text(encoding="utf-8"))
        meta["audio"] = M4B.name
        (AUDIO / "chapters.json").write_text(json.dumps(meta, ensure_ascii=False, indent=1), encoding="utf-8")
        tmp_chapters.unlink()
        if tmp.with_suffix(".srt").exists():
            os.replace(tmp.with_suffix(".srt"), M4B.with_suffix(".srt"))
        os.replace(tmp, M4B)

        # Whole-book word-level SRT: each page's words.srt shifted to its chapter start.
        sys.path.insert(0, str(tools))
        import subtitles  # noqa: E402  (stdlib-only module from the Kokoro toolkit)
        merged = []
        for ch in meta["chapters"]:
            words = WORK / (Path(ch["file"]).stem + ".words.srt")
            if words.exists():
                merged += subtitles.shift(subtitles.read_srt(words), float(ch["start"]))
        if merged:
            subtitles.write_srt(M4B.with_suffix(".words.srt"), merged)
    else:
        print("  = unchanged")
    chapters = json.loads((AUDIO / "chapters.json").read_text(encoding="utf-8"))["chapters"]

    print("\n== 5/5 Rebuilding the book with read-along audio")
    build_book.main()
    total = chapters[-1]["end"] if chapters else 0
    per_page = sum(p.stat().st_size for p in CHAPTERS.glob("*.m4a")) / 1048576
    print(f"\nAudiobook: {M4B}  ({len(chapters)} chapters, {total / 60:.0f} min, "
          f"{M4B.stat().st_size / 1048576:.0f} MB)\nPer-page : {CHAPTERS}  ({per_page:.0f} MB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
