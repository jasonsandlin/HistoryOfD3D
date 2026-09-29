# The Evolution of Direct3D
### From Fixed-Function to GPU-Driven Rendering (D3D7 → D3D12)
*A code-driven course, taught through the RotatingCubes sample set.*

---

## Welcome to the course

This is a self-paced course on how Microsoft's Direct3D API evolved across two
decades — from the fixed-function **D3D7** of 1999 to the explicit, GPU-driven
**D3D12** of today. Rather than march through API reference pages, we learn by
reading a family of small, complete programs that all do the *exact same thing*:
spin a colored cube on the screen.

That shared goal is the whole trick. Because the **result never changes**, the
only thing that differs from one sample to the next is *the API you must use to
achieve it*. The cube is a **control variable** in a controlled experiment. When
D3D7 needs 160 lines and D3D12's fundamentals need 700, that difference isn't
noise — it *is* the lesson. Every extra line D3D12 asks of you buys back some
control the older runtimes were quietly exercising on your behalf.

### What makes this course different

Most API tutorials answer **"how?"** This one insists on also answering
**"when?"** and **"why?"** For every feature we study, we ask:

- **The problem** — what real need forced this into existence?
- **How it works** — the mechanics, with short excerpts from the sample.
- **When to use it** — the situations where it's the right tool.
- **When *not* to use it** — the costs, and when the older/simpler way wins.
- **Trade-offs & decision guidance** — the engineering judgment involved.
- **Historical "why now?"** — why it arrived in *this* era and not earlier.

You'll also find recurring **Decision lens** callouts: short, opinionated
frameworks for choosing between competing options ("committed vs. placed vs.
reserved resources," "bundle vs. indirect vs. re-record," and so on).

### Who this is for

You should be comfortable reading C/C++ and have a working mental model of the
GPU as "a thing that turns triangles into pixels." You do **not** need prior
Direct3D experience — we build it up era by era. Linear algebra (matrices,
vectors) is assumed at the level of "I know what a model-view-projection matrix
is for," but every sample uses the same tiny matrix helpers, so you can treat
them as a black box if you like.

---

## How to use this course

Each **Part** is a separate file in this `docs/` folder. Work through them in
order — the narrative is cumulative, and later Parts assume the vocabulary built
earlier. Each Part contains one or more **Modules**, and each Module studies one
or more samples.

Every sample is buildable and runnable. From `RotatingCubes/`:

```powershell
# Build one sample (works no matter how deeply it is nested):
powershell -ExecutionPolicy Bypass -File .\build-all.ps1 -Only D3D12.Fundamentals

# Build everything:
powershell -ExecutionPolicy Bypass -File .\build-all.ps1
```

Each sample's executable lands in its own `Out\` subfolder as `Cube<Name>.exe`.
Run it, watch the cube, then read the source alongside the Module. The source is
the primary text; these notes are the lecture that surrounds it.

> **A note on the D3D12 layout.** All D3D12 samples live under
> `RotatingCubes\D3D12\`, grouped by learning tier in `Tier1\ … Tier4\`. The
> tier a sample lives in tells you roughly how far into the D3D12 journey it
> belongs.

---

## The five eras at a glance

| Era | Year | One-sentence identity | Big idea introduced |
|-----|------|-----------------------|---------------------|
| **D3D7**  | 1999 | The GPU is an appliance you *configure*. | Hardware transform & lighting; fixed-function render states. |
| **D3D8**  | 2000 | Consolidation, and the first programmable shaders. | Vertex/index buffers; shader assembly (SM 1.x). |
| **D3D9**  | 2002 | The programmable pipeline matures. | HLSL; vertex & pixel shaders you write. |
| **D3D10** | 2006 | Tear it down and rebuild it clean. | No fixed function; DXGI; constant buffers; geometry shader. |
| **D3D11** | 2009 | The GPU becomes a general compute device. | Multithreading; tessellation; DirectCompute. |
| **D3D12** | 2015 | You manage the GPU yourself. | Command lists/queues, PSOs, root signatures, explicit memory & sync. |

The single through-line: **the GPU went from a fixed appliance you *configure*,
to a programmable processor you *feed*, to a raw device you *manage*.** Keep that
sentence in mind; every Part is one more step along it.

---

## Syllabus

### Part 0 — [Orientation](./part-00-orientation.md)
The rotating-cube experiment, the anatomy every sample shares, and the
vocabulary that survives all five rewrites.

### Part I — [The Fixed-Function Era](./part-01-fixed-function-era.md)
*D3D7, D3D8.* Configuring the GPU by render state; the birth (and mortality) of
APIs; where shaders first appear.

### Part II — [The Programmable Shading Revolution](./part-02-programmable-shading.md)
*D3D9 + VertexShader + PixelShader.* HLSL arrives; you take ownership of vertex
transform and per-pixel color.

### Part III — [The Great Redesign](./part-03-great-redesign.md)
*D3D10 + Vertex/Pixel/Geometry.* Fixed function removed; DXGI factored out;
constant buffers; the geometry shader.

### Part IV — [Maturity & Generality](./part-04-maturity-generality.md)
*D3D11 + Vertex/Pixel/Geometry/Compute/Tessellation.* Contexts and
multithreading; tessellation; DirectCompute; feature levels.

### Part V — [Going Explicit: The D3D12 Object Model](./part-05-going-explicit-d3d12.md)
*D3D12 base + Tier 1.* Why D3D12 exists; the new object model; fundamentals,
resources, and pipeline basics.

### Part VI — [Feeding the GPU Efficiently](./part-06-feeding-the-gpu.md)
*Tier 2 + Tier 3.* Explicit memory & submission; multi-engine queues;
GPU-driven draws; async compute; bindless; multi-pass rendering.

### Part VII — [Image Quality & the Modern Pipeline](./part-07-image-quality-modern-pipeline.md)
*Tier 4 + Shader Model 6.* Shadow mapping, MSAA, variable-rate shading; mesh &
amplification shaders; DXR ray tracing.

### Part VIII — [Recording & Querying at Scale](./part-08-recording-querying-at-scale.md)
*Tier 5.* Multi-threaded recording, bundles, timestamp/statistics
queries, predication.

### Part IX — [Classic Pipeline Mastery](./part-09-classic-pipeline-mastery.md)
*Tier 6.* Instancing, blending & transparency, the stencil buffer —
the output-merger details every real renderer needs.

### Part X — [Presentation, Devices & Advanced Resources](./part-10-presentation-devices-resources.md)
*Tier 7 & 8.* HDR & swap-chain color; multi-adapter; pipeline
libraries; reserved/tiled resources; debugging & tooling.

### Part XI — [The Frontier](./part-11-the-frontier.md)
*Tier 9 / Agility SDK.* SM6.6 dynamic resources, enhanced barriers, sampler
feedback, work graphs, DirectStorage, DXR 1.2 (opacity micromaps + shader
execution reordering), SM6.10 linear algebra / neural rendering, partial graphics
programs & Advanced Shader Delivery.

### Part XII — [Synthesis](./part-12-synthesis.md)
The arc in one picture; a cross-era retrospective; where the API goes next.

### [Appendices](./appendices.md)
Build/run cheat-sheet · glossary · "same concept, five names" terminology table ·
further reading.

---

> **On the frontier samples.** Part XI covers the Agility-SDK **frontier** samples
> in `D3D12\Tier9\`. The retail ones (SM6.6 dynamic resources, enhanced barriers,
> sampler feedback, work graphs, DirectStorage) query their capability tier and
> fall back to a plain cube when the GPU/driver lacks support. The **preview** ones
> (`D3D12.RayTracing12`, `D3D12.LinearAlgebra`, `D3D12.PartialPrograms`) vendor the
> Agility SDK 1.721-preview, preview DXC and preview WARP via `fetch-deps.ps1`. When
> the GPU driver doesn't expose the feature yet, they re-run on WARP so the feature
> still works on screen. Tiers 1–8 (Parts V–X) all exist and build as well.

*Next: [Part 0 — Orientation »](./part-00-orientation.md)*
