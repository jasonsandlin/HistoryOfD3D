# Appendices

*[« Part XII](./part-12-synthesis.md) · [Syllabus](./D3D-Evolution-Course.md)*

Reference material to support the main lectures: how to build and run the samples,
a glossary, the "same concept, many names" terminology map, and where to go next.

---

## Appendix A — Build & Run Cheat-Sheet

All samples build with a **stock Visual Studio + Windows SDK** toolchain on x64.
The driver script `build-all.ps1` at the repo root imports the VC x64 environment
and invokes `cl.exe` directly (more reliable on shared machines than
`cmd /c build.bat`).

**Build everything:**

```powershell
cd <your clone of HistoryOfD3D>
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-all.ps1
```

**Build one sample** (pass the leaf folder name — it's found at any depth):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-all.ps1 -Only D3D12.Fundamentals
powershell -NoProfile -ExecutionPolicy Bypass -File .\build-all.ps1 -Only D3D11.Basic
```

**Or do everything at once:** `setup.ps1` checks the toolchain, fetches the
preview SDKs, builds every sample (plus the 32-bit D3D7/D3D8 copies) and enables
the HTML book's **Run sample** buttons:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\setup.ps1
```

**Where output goes.** Each sample builds into its own `Out\` subfolder (e.g.
`D3D12\Tier1\D3D12.Fundamentals\Out\CubeD3D12.Fundamentals.exe`). `Out\` is
`.gitignore`d so build products never clutter the tree. Run the `.exe` from that
folder.

**Notes & gotchas.**
- The D3D12 samples compile as **C** (`/Tc`, `COBJMACROS`), so COM calls read
  `ID3D12X_Method(obj, ...)`. The era cubes (D3D7–D3D11) are C++ (`/EHsc`).
- Complex D3D12 samples write a **log file** (`Cube<Sample>.log`) next to the exe
  for debugging instead of popping message boxes — read it if a sample exits
  unexpectedly.
- **D3D7 / D3D8** build as x64 but the legacy runtimes they need only ship as
  32-bit DLLs on modern Windows, so the x64 exe reports "init failed". Build them
  from a `vcvars32.bat` prompt (`cl /EHsc /I. main.cpp user32.lib`) to run them;
  `tools\capture-screenshots.ps1` does this automatically into `Out\x86\`.
- The **preview** Tier 9 samples (`D3D12.RayTracing12`, `D3D12.LinearAlgebra`,
  `D3D12.PartialPrograms`) need their dependencies first: run the sample's
  `fetch-deps.ps1` once (Agility SDK 1.721-preview, preview DXC, preview WARP).
  `build-all.ps1` then copies the runtimes into `Out\` so the exe runs from
  there. They need **Developer Mode** enabled.
- **Regenerating the HTML book:** `tools\make-book.ps1` builds every sample,
  captures screenshots/animations (`tools\capture-screenshots.ps1`) and renders
  `book\` (`tools\build_book.py`; needs `pip install markdown pygments pillow`).
- **Re-narrating the audiobook:** after editing `docs\`, run
  `python tools\narrate_book.py` (or `make-book.ps1 -Narrate`). It speaks only the
  pages whose text changed, using the Kokoro text-to-speech toolkit, and rebuilds
  `book\assets\audio\TheHistoryOfDirect3D.m4b` plus the word timings that drive
  the read-along highlighting. `build_book.py` warns when a page's text has
  drifted from its narration.

---

## Appendix B — Glossary

Terms in the order you meet them in the course.

- **Fixed-function pipeline** — a GPU with configurable but non-programmable stages;
  you set *render states*, not code (D3D7).
- **Render state** — a knob (`SetRenderState`) toggling fixed-function behavior:
  lighting on/off, blend mode, cull mode.
- **FVF (Flexible Vertex Format)** — a bitmask describing what's in each vertex
  (position, normal, color, UV) in the fixed-function era.
- **Shader** — a program that runs on the GPU for each vertex, pixel, etc.
- **Vertex / Pixel (Fragment) shader** — programs that transform vertices and
  color pixels.
- **HLSL** — High-Level Shading Language; the C-like language shaders are written
  in from D3D9 on.
- **Shader Model (SM)** — a version number for shader capability (`vs_3_0`,
  `cs_5_0`, SM6.6).
- **Geometry shader** — an optional stage that can create/destroy primitives.
- **Tessellation (hull/domain shader)** — stages that subdivide geometry on the GPU.
- **Compute shader / DirectCompute** — general parallel GPU programs outside the
  draw pipeline; launched with `Dispatch`.
- **Constant buffer (cbuffer / CBV)** — a block of shader constants (e.g. the MVP
  matrix), introduced as a first-class object in D3D10.
- **View (RTV/DSV/SRV/UAV/CBV)** — a typed *interpretation* of a resource:
  render-target, depth-stencil, shader-resource, unordered-access, constant-buffer.
- **UAV (unordered-access view)** — a view a shader can *write* to (and read).
- **DXGI** — the OS layer owning swap chains, adapters, and output/display
  management (from D3D10).
- **Swap chain** — the set of back buffers you render into and `Present`.
- **Feature level** — a capability tier letting one API (D3D11/12) drive several
  GPU generations.
- **Device vs. context** — D3D11's split of resource creation (free-threaded
  device) from command issue (context).
- **Command queue / allocator / list** — D3D12's three-way split of "issue
  commands": the GPU intake, the backing memory, and the recording surface.
- **Fence** — a shared counter for CPU/GPU synchronization in D3D12.
- **Resource barrier** — an explicit resource-state transition (e.g.
  PRESENT→RENDER_TARGET) you issue in D3D12.
- **PSO (Pipeline State Object)** — an immutable bundle of baked render state +
  shaders created up front in D3D12.
- **Root signature** — the declaration of what inputs (constants, descriptor
  tables, samplers) a D3D12 pipeline's shaders receive.
- **Descriptor heap** — a GPU-visible array of resource descriptors (views) you
  allocate and index in D3D12.
- **Root constants** — a few 32-bit values pushed directly through the root
  signature (cheapest per-draw data).
- **Committed / placed / reserved resource** — memory models: own allocation /
  at an offset in your heap / virtual with tiles mapped on demand.
- **Copy queue** — a dedicated DMA queue for transfers that overlap rendering.
- **Async compute** — running compute work on a separate queue concurrently with
  graphics.
- **Bindless** — indexing resources in a big descriptor heap by integer instead of
  binding each one.
- **MRT / G-buffer** — multiple render targets written at once; the buffer of
  surface attributes used by deferred shading.
- **MSAA / resolve** — multisample anti-aliasing of geometry edges; `Resolve`
  collapses samples to one.
- **VRS (variable-rate shading)** — shading coarser than one invocation per pixel
  where detail is unneeded.
- **Mesh / amplification shader** — SM6 stages that replace the fixed geometry
  front end with programmable meshlet output + per-meshlet culling.
- **DXR / acceleration structure (BLAS/TLAS)** — hardware ray tracing and the
  data structures that make ray queries fast.
- **Agility SDK** — the mechanism shipping new D3D12 features out-of-band from
  Windows updates.
- **Work graphs** — GPU-scheduled task graphs where GPU nodes launch other nodes.
- **DirectStorage** — fast NVMe→GPU asset streaming with GPU decompression.
- **DXR 1.2** — the second generation of D3D12 ray tracing (raytracing tier 1.2,
  Shader Model 6.9): opacity micromaps and shader execution reordering.
- **Opacity micromap (OMM)** — per-triangle grid of micro-triangles marked
  opaque/transparent/unknown, stored in the acceleration structure so traversal
  resolves alpha tests without running an any-hit shader.
- **Shader execution reordering (SER)** — `HitObject` + `MaybeReorderThread`:
  trace first, let the GPU regroup threads by what they hit, then shade.
- **Long vectors** — SM6.9 HLSL vectors wider than four components
  (`vector<half, 16>`), needed to pass neural-network feature vectors.
- **LinAlg / cooperative vectors** — SM6.10 matrix API exposing the GPU's matrix
  hardware to shaders (`Multiply`, `VectorAccumulate`, …) for neural rendering.
- **Partial graphics program** — a precompiled pre-rasterization or pixel-shader
  piece of a pipeline, linked cheaply with the varying state into a *generic
  program*.
- **Advanced Shader Delivery (ASD)** — shaders precompiled for your GPU + driver
  and delivered with the game download, so the first launch doesn't compile.
- **DirectX dump file** — a console-style GPU crash dump written on device
  removal (preview).
- **WARP** — Microsoft's software D3D12 rasterizer; the preview WARP implements
  new features before drivers do, so preview samples fall back to it.

---

## Appendix C — Same Concept, Many Names

The same underlying idea acquired different names as the API evolved. This table
is the "Rosetta Stone" for reading code across eras.

| Concept | D3D7 (fixed) | D3D9 | D3D10/11 | D3D12 |
|---|---|---|---|---|
| Per-draw transform data | `SetTransform(WORLD/VIEW/PROJ)` | shader constants (`SetVertexShaderConstantF`) | **constant buffer** (cbuffer/CBV) | **root constants** or CBV |
| Describe vertex layout | **FVF** bitmask | FVF or vertex declaration | **input layout** (validated vs. shader) | input layout in the **PSO** |
| Configure pipeline state | many `SetRenderState` calls | render states + shaders | state objects set piecemeal | one immutable **PSO** |
| "The GPU" you talk to | `IDirect3DDevice7` | `IDirect3DDevice9` | device **+** context | device + queue + **command list** |
| Present a frame | manual `Blt` of a surface | `Present` | DXGI **swap chain** `Present` | DXGI swap chain `Present` (+ barriers) |
| A texture the shader reads | texture stage | texture sampler | **SRV** | **SRV** in a descriptor heap |
| Who tracks resource state | the driver | the driver | the driver | **you** (resource barriers) |
| Who syncs CPU/GPU | the driver | the driver | the driver | **you** (fences) |

Reading left-to-right across a row *is* the evolution of that concept — usually
from "the driver hides it" toward "you state it explicitly."

---

## Appendix D — Further Reading

- **Microsoft Direct3D 12 programming guide** (learn.microsoft.com) — the
  authoritative reference for the D3D12 object model, barriers, and PSOs.
- **DirectX-Graphics-Samples** (github.com/microsoft/DirectX-Graphics-Samples) —
  Microsoft's official D3D12 samples; the canonical, production-grade counterparts
  to this course's teaching samples.
- **DirectX Developer Blog** — announcements and deep dives on Agility-SDK
  features (mesh shaders, DXR, work graphs, DirectStorage, enhanced barriers).
- **"Introduction to 3D Game Programming with DirectX"** (Frank Luna) — a
  thorough, book-length treatment of D3D11/12 fundamentals.
- **The samples in this repository** — the best next step is to open a sample from
  the tier you're studying, read it top to bottom, then change one thing and
  rebuild. Hold a variable constant and diff; the delta is the lesson.

---

*Return to the [Syllabus »](./D3D-Evolution-Course.md).*
