# The Evolution of Direct3D — a code-driven course

> **This repository is a course.** Its point is
> **[docs/D3D-Evolution-Course.md](./docs/D3D-Evolution-Course.md)** — an
> instructor-style walkthrough of how Microsoft's Direct3D API evolved from the
> fixed-function **D3D7** of 1999 to the explicit, GPU-driven **D3D12** of
> today. Everything else in this repo — dozens of buildable samples — is the
> *supporting material* the course teaches from.

## ▶ Read it online: **[jasonsandlin.github.io/HistoryOfD3D](https://jasonsandlin.github.io/HistoryOfD3D/)**

The interactive edition has live captures of every sample, the full highlighted
source, an era timeline, side-by-side era comparison, search, and a **2-hour
read-along audiobook**. Press **Listen** (or `K`) on any chapter and each word
highlights as it's spoken; click a word to jump there. Keys: `←`/`→` page,
`/` search, `F` presentation mode.

Prefer plain Markdown? **[Read the course »](./docs/D3D-Evolution-Course.md)**

### Downloads ([latest release](https://github.com/jasonsandlin/HistoryOfD3D/releases/latest))

- **Offline edition** (`HistoryOfD3D-<date>.zip`): the whole book plus every
  sample **prebuilt**. Extract it, open `START HERE.html`, and run `setup.ps1` once
  to enable the **Run sample** buttons (no Visual Studio needed).
- **Audiobook** (`TheHistoryOfDirect3D.m4b`): one chapter per page, with an
  embedded read-along subtitle track, for any audiobook app.

### Building the samples

Every sample is a single-file Win32 program. With Visual Studio 2022+ (C++
workload) and the Windows 10/11 SDK:

```powershell
powershell -ExecutionPolicy Bypass -File .\setup.ps1      # checks tools, fetches SDKs, builds all 59
powershell -ExecutionPolicy Bypass -File .\build-all.ps1 -Only D3D12.Fundamentals
```

The Tier 9 samples that use preview or out-of-band SDKs (`D3D12.RayTracing12`,
`D3D12.LinearAlgebra`, `D3D12.PartialPrograms`, `D3D12.DirectStorage`) download
them with their `fetch-deps.ps1` (`setup.ps1` runs these for you).

### Regenerating the book (maintainers)

| Task | Command |
|---|---|
| Build, capture screenshots, render `book\` | `.\tools\make-book.ps1` (`-NoBuild`, `-NoCapture`, `-Only <Sample>`) |
| Re-narrate pages whose text changed | `python .\tools\narrate_book.py` (needs a Kokoro TTS toolkit: `tts.py`/`make_m4b.py`, via `--tools` or `%AUDIO_TOOLS%`) |
| Mirror sources into the public repo | `.\tools\publish-repo.ps1` |
| Publish the website (`gh-pages` branch) | `.\tools\publish-site.ps1 -Push` |
| Offline zip for a release | `.\tools\pack-share.ps1` |

Rendering needs `pip install markdown pygments pillow`.
The course teaches by keeping one thing constant: **every sample spins the exact
same colored cube.** Because the result never changes, the only thing that
differs from one era to the next is *the API you must use to achieve it*. The
cube is the control variable in a controlled experiment — when D3D7 needs 160
lines and D3D12 needs 700, that difference *is* the lesson.

Most API tutorials answer **"how?"** This one insists on also answering
**"when?"** and **"why?"** — the problem each feature was born to solve, when it's
the right tool, when the older/simpler way still wins, and the engineering
trade-offs involved.

### What the course covers

The five eras, each an era-defining shift in what the GPU *is*:

| Era | Year | Identity | Big idea |
|-----|------|----------|----------|
| **D3D7**  | 1999 | The GPU is an appliance you *configure*. | Hardware T&L; fixed-function render states. |
| **D3D8**  | 2000 | Consolidation + the first programmable shaders. | Vertex/index buffers; shader assembly. |
| **D3D9**  | 2002 | The programmable pipeline matures. | HLSL; vertex & pixel shaders you write. |
| **D3D10** | 2006 | Tear it down, rebuild it clean. | No fixed function; DXGI; constant buffers; geometry shader. |
| **D3D11** | 2009 | The GPU becomes a general compute device. | Multithreading; tessellation; DirectCompute. |
| **D3D12** | 2015 | You manage the GPU yourself. | Command lists/queues, PSOs, root signatures, explicit memory & sync. |

The through-line: **the GPU went from a fixed appliance you *configure*, to a
programmable processor you *feed*, to a raw device you *manage*.**

The syllabus runs **Part 0 → Part XII** (orientation, the fixed-function era, the
programmable-shading revolution, the D3D10 redesign, D3D11 maturity, going
explicit with D3D12, and on through the modern GPU-driven frontier — mesh
shaders, ray tracing, work graphs, and the SM6.10 neural-rendering preview). See
the [full syllabus](./docs/D3D-Evolution-Course.md#syllabus).

## The samples (supporting material)

The course is taught through ~60 small, complete, buildable programs — the six
base cubes (D3D7–D3D12), per-stage shader showcases (vertex/pixel/geometry/
compute/tessellation/mesh/amplification/ray tracing), and a tiered D3D12 learning
track (Tier 1–9) that walks from fundamentals to the Agility-SDK frontier.

**The catalog, per-sample tables, build instructions, and runtime notes live in
[docs/SAMPLES.md](./docs/SAMPLES.md).** In brief, from this folder:

```powershell
# Build everything:
powershell -ExecutionPolicy Bypass -File .\build-all.ps1

# Build one sample (works no matter how deeply it is nested):
powershell -ExecutionPolicy Bypass -File .\build-all.ps1 -Only D3D12.Fundamentals
```

Each sample's executable lands in its own `Out\` folder as `Cube<Name>.exe`.
Run it, watch the cube, then read the source alongside the matching course Module
— the source is the primary text; the course is the lecture around it.

## Repository layout

```
HistoryOfD3D\
  docs\                     ← THE COURSE (start with D3D-Evolution-Course.md)
    D3D-Evolution-Course.md   syllabus + landing page
    part-00 … part-12.md      the twelve parts
    appendices.md             build/run cheat-sheet, glossary, further reading
    SAMPLES.md                sample catalog & build reference
  D3D7.Basic\  D3D8.Basic\   fixed-function base cubes
  D3D9\  D3D10\  D3D11\      per-version base cube + shader-stage samples
  D3D12\                     tiered learning track (Tier1 … Tier9)
  build-all.ps1             builds every sample (auto-discovers at any depth)
```

## Requirements

Windows, Visual Studio 2022 (or Build Tools) with the Windows 10/11 SDK
(10.0.22000+; developed against 10.0.26100). No legacy DirectX SDK is needed. A
few frontier samples want a recent GPU/driver and fall back gracefully when a
feature isn't supported — details in [docs/SAMPLES.md](./docs/SAMPLES.md).
