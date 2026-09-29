# Part XI — The Frontier: Where Direct3D Is Heading

*[« Part X](./part-10-presentation-devices-resources.md) · [Syllabus](./D3D-Evolution-Course.md) · Next: [Part XII »](./part-12-synthesis.md)*

> **Roadmap note.** These are **Agility SDK** frontier features — the newest
> additions, delivered out-of-band from Windows via the D3D12 Agility SDK.
>
> **Build note.** These samples live under `D3D12\Tier9\`
> (`D3D12.SamplerFeedback`, `D3D12.DynamicResources`, `D3D12.WorkGraphs`,
> `D3D12.EnhancedBarriers`, `D3D12.DirectStorage`, `D3D12.RayTracing12`,
> `D3D12.LinearAlgebra`, `D3D12.PartialPrograms`). The first five build against
> the Windows 10.0.26100 SDK (DirectStorage vendors its NuGet SDK locally). The
> last three are **preview** samples: each vendors the Agility SDK
> 1.721-preview, the preview DXC and the preview **WARP** software rasterizer via
> its own `fetch-deps.ps1`. Every sample queries its capability tier at runtime.
> The preview samples try the GPU first and, when the driver doesn't expose the
> feature yet, re-run on preview WARP (which implements all of them), so you
> still see the feature working. The others fall back to a plain cube.

---

D3D12 stopped being a fixed target. Through the **Agility SDK**, Microsoft ships
new D3D12 features independently of Windows OS updates, so the API keeps evolving
between "versions." This Part surveys the current frontier — the features shaping
where real-time graphics is going — and, as always, *when* they matter.

Samples (`D3D12\Tier9\`): dynamic resources (SM6.6), enhanced barriers,
sampler feedback, work graphs, DirectStorage, second-generation ray tracing
(DXR 1.2 / SM6.9), linear algebra / neural rendering (SM6.10 preview), and
partial graphics programs (1.721-preview).

---

## Module 15 — The Agility-SDK Frontier

### SM6.6 dynamic resources / `ResourceDescriptorHeap`

Bindless (Part VI) still needed you to declare descriptor tables. **Shader Model
6.6 dynamic resources** go further: shaders index the descriptor heap *directly*
by integer, with no root-signature table at all:

```hlsl
Texture2D t = ResourceDescriptorHeap[materialIndex];   // no binding, just an index
float4 c = t.Sample(s, uv);
```

**When to use:** fully GPU-driven and ray-traced pipelines where "any shader
reaches any resource by index" is the natural model. It's the cleanest, most
flexible binding model D3D12 has offered — the logical end of the road that
started with descriptor heaps in Part V.

**When *not* to:** it requires SM6.6-capable hardware/drivers, and the total lack
of binding declarations makes mistakes harder to catch. Simpler renderers don't
need it. It's the tool for large, dynamic, GPU-driven resource sets.

### Enhanced barriers

The original `ResourceBarrier` model (Part V) is coarse — it conflates several
kinds of synchronization and is easy to over-sync. **Enhanced barriers** split the
concept into precise **layout**, **sync**, and **access** dimensions:

```
Barrier( sync:  DRAW → PIXEL_SHADER,
         access: RENDER_TARGET → SHADER_RESOURCE,
         layout: RENDER_TARGET → SHADER_RESOURCE )   // say exactly what you mean
```

**When to use:** performance-critical engines that were leaving GPU cycles on the
table with the old coarse barriers. Enhanced barriers let you express *exactly*
the synchronization you need and no more — fewer stalls, less cache flushing.

**When *not* to:** the legacy barriers still work and are simpler to reason about.
Enhanced barriers are an optimization for teams profiling barrier overhead — not a
required migration.

> **Decision lens — barrier model.** Legacy `ResourceBarrier` for clarity and
> broad support. Enhanced barriers when the profiler shows you're over-syncing and
> you want fine control. Same correctness goal, finer instrument.

### Sampler feedback

Sampler feedback lets the GPU **record which parts of a texture (which mip tiles)
were actually sampled** while shading. Feed that map back into your streaming
system to load exactly the texture data the frame needed — no more, no less:

```
shade with feedback → feedback map: "tiles X,Y at mip N were touched"
streaming system: load those tiles, evict the rest
```

**When to use:** texture-streaming and virtual-texturing systems (pairs naturally
with reserved resources, Part X). It replaces heuristics ("guess what's visible")
with ground truth ("here's what was sampled"), cutting memory waste dramatically.

**When *not* to:** if you're not streaming textures, there's nothing to feed back.
It's a specialized tool for large-world streaming, not a general feature.

### Work graphs

Perhaps the most forward-looking feature. **Work graphs** let the GPU **generate
and schedule its own work** — nodes that launch other nodes — without round-trips
to the CPU. It generalizes indirect draw and `DispatchMesh` into a full on-GPU
task graph:

```
GPU node A (classify work) → launches → nodes B/C/D (process each class)
                                       → those launch further nodes...
all scheduled by the GPU, no CPU involvement
```

**When to use:** highly dynamic, data-dependent GPU pipelines where the *amount
and kind* of work is decided on the GPU — advanced culling, procedural generation,
GPU-driven everything. It's the frontier of "the GPU drives itself."

**When *not* to:** it's new, hardware/driver support is still spreading, and it's
overkill for pipelines with a fixed, CPU-known structure. Today it's for engine
R&D and cutting-edge titles, not mainstream adoption.

### DirectStorage

Not a rendering feature — an **I/O** one, but transformative. DirectStorage
streams assets from NVMe SSDs to GPU memory with minimal CPU involvement and
**GPU decompression**, bypassing the traditional slow, CPU-heavy load path:

```
NVMe SSD → (GPU-decompressed) → GPU memory        // CPU barely involved
```

**When to use:** fast level loading and seamless open-world streaming — the tech
behind "instant" loads on modern consoles and PCs. It attacks the load-time and
streaming bottleneck that VRAM size and CPU decompression created.

**When *not* to:** small games with tiny assets won't notice. Its benefits scale
with asset size and streaming intensity; it needs an NVMe SSD to shine.

### Ray tracing, second generation: DXR 1.2 (`D3D12.RayTracing12`)

Part VII's `D3D12.RayTracing` cube is DXR 1.0: trace a ray, run the hit shader.
Real scenes expose two costs that the cube hides, and **DXR 1.2** (shipped with
**Shader Model 6.9**) attacks both.

**Cost 1 — alpha-tested geometry.** Foliage, fences and chain-link are triangles
whose texture says "this part is a hole." In DXR 1.0 the traversal hardware can't
know that, so every candidate hit on such a triangle launches an **any-hit
shader** — a full shader invocation just to sample a texture and answer "keep
going." **Opacity micromaps (OMM)** bake that answer into the acceleration
structure: each triangle is subdivided into micro-triangles, each marked
*opaque*, *transparent* or (in the 4-state format) *unknown*. The traversal
hardware resolves the opaque and transparent ones by itself; the any-hit shader
only runs for the "unknown" slivers along the edges.

```
build: OMM array (micro-triangle states) → linked from the BLAS triangles
trace: hit a transparent micro-triangle → traversal continues, no shader launched
```

**Cost 2 — divergence.** A wave runs 32–64 threads in lockstep. After a bounce,
neighbouring rays hit different materials, so the wave executes *every* material's
hit shader in turn with most lanes idle. **Shader Execution Reordering (SER)**
splits `TraceRay` into steps: find the hit, let the GPU **regroup threads by what
they hit**, then shade:

```hlsl
dx::HitObject hit = dx::HitObject::TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);
dx::MaybeReorderThread(hit, materialHint, 2);   // the GPU may reshuffle threads here
dx::HitObject::Invoke(hit, payload);            // shade, now coherently
```

Note the word *Maybe*. At raytracing tier 1.2 the call is always **legal**, but
hardware is allowed to treat it as a no-op; the device reports
`ShaderExecutionReorderingActuallyReorders` so you can tell. Write the code once
and let each GPU decide.

The sample cuts a different hole into each cube face (circle, diamond, four
round holes) as 12 opacity micromaps, and shades through the SER path. `O` cycles
four alpha modes, and the title shows how many any-hit shader calls each costs
per frame:

| Mode | Any-hit calls per frame |
|---|---|
| OMM 2-state (default) | 0 |
| OMM 4-state (any-hit only on the "unknown" edge slivers) | ~3.9K |
| Classic any-hit alpha test, no OMM | ~162K |
| Opaque (no holes) | 0 |

All three cut-out modes produce the *same* holes; OMM just gets there without
running shaders. `S` toggles SER, `H` colours pixels by the reorder hint, and the
title shows the `DispatchRays` time. On a GPU without tier 1.2 (like the
RTX 2070 it was captured on) it runs on preview WARP instead, where SER works but
doesn't reorder (`ShaderExecutionReorderingActuallyReorders = 0`), so don't
expect a speedup there. One practical SM6.9 detail: ray-tracing libraries must
annotate their payloads (`[raypayload]` with per-field read/write qualifiers)
before `HitObject` will compile.

**When to use:** OMM whenever you ray-trace alpha-tested content — it's the
cheapest any-hit shader, the one that never runs. SER whenever your hit shading
diverges (many materials, secondary bounces, path tracing).

**When *not* to:** fully opaque, single-material scenes gain nothing from either.
Both need tier 1.2 hardware to pay off; on older GPUs SER compiles and runs but
doesn't reorder. And reordering has its own cost: for primary rays that are
already coherent it can be a small loss, so measure.

> **Decision lens — who decides the execution order?** DXR 1.0 made you accept
> whatever order rays happened to finish in. SER lets you *hint* and lets the GPU
> *decide*. It's the same move as work graphs, applied to a single dispatch: the
> scheduling decision moves to the hardware, which can see the whole wave.

### Linear algebra / neural rendering (Shader Model 6.10 preview)

The newest frontier is the one that ties graphics to the ML era. **Shader Model
6.10** adds a unified **LinAlg Matrix** API (`#include <dx/linalg.h>`,
`dx::linalg`) that surfaces the GPU's dedicated matrix hardware — the same tensor
units that power ML — directly to HLSL. It subsumes the earlier *Cooperative
Vectors* and *WaveMMA* previews into three **matrix scopes**:

```
MatrixScope::Thread      // per-thread mat×vec — drop ML inference into a shader thread
MatrixScope::Wave        // wave-wide MMA — hardware matrix-matrix multiply (tiling)
MatrixScope::ThreadGroup // large matrices; the DRIVER tiles for you (one impl, optimal)
```

**When to use:** *neural rendering* — replacing a hand-written physically-based
computation (lighting, denoising, upscaling, material evaluation) with a small
trained neural network evaluated **inside** the shader, per pixel, in real time.
Thread-scope matrices make this an incremental drop-in: run inference on a tiny
MLP from a pixel shader thread and let the compiler map it to the matrix
accelerators. Wave and thread-group scopes target heavier ML/image-processing and
LLM-style GEMMs where you want big matrix throughput without hand-tiling for every
GPU.

**Why thread-group scope matters:** the inputs/weights in LLM-like networks exceed
wave-matrix size limits, so a wave-scope solution forces *manual tiling* and a
different kernel per architecture. A thread-group matrix hands the tiling decision
to the **driver**, so you ship one implementation and still get optimal
per-hardware tiling. That is the recurring frontier theme again — push the
decision down to where the best information lives.

**The groundwork: long vectors (SM 6.9).** For twenty years an HLSL vector topped
out at four components — `float4` is the RGBA/XYZW world of graphics. A neural
network layer is 16, 32 or 64 values wide. **Shader Model 6.9 long vectors**
remove the cap (`vector<half, 16>`, `vector<float, 64>`), which is what lets the
LinAlg API pass a whole feature vector in one value. The sample's
`vector<half, 16>` input is exactly that.

**Inference *and* training: `VectorAccumulate`.** The 1.721-preview DXC adds the
last missing LinAlg operation, **VectorAccumulate** (spelled
`InterlockedAccumulate(vector, buffer, offset)` in the shipped `dx/linalg.h`):
many threads atomically add their vector into a shared buffer. Multiply is how a
network is *evaluated*; accumulate is how gradients and statistics are
*gathered*, the building block for on-GPU training and adaptation. The sample has
256 threads each compute a bias gradient and accumulate it into one 64-byte
buffer.

**When *not* to:** classic analytic shading is still cheaper and exact for most
surfaces; reach for neural evaluation only where a learned approximation beats the
math on cost or quality. And note the practical gate: this is a **preview** — it
needs the preview Agility SDK, the SM6.10 preview DXC, Developer Mode, and a
driver that reports `D3D12_LINEAR_ALGEBRA_TIER_1_0`. The `D3D12.LinearAlgebra`
sample vendors the whole preview toolchain (`fetch-deps.ps1`), enables
experimental shader models and queries the tier. If the GPU driver doesn't
expose LinAlg yet, it re-runs on preview WARP. It then runs a matrix×vector
multiply and a `VectorAccumulate`, checks both against CPU math in its log, and
tints the cube and draws bar charts of the two GPU results on its faces to show
the LinAlg path ran. That is how to ship a
preview-gated feature: detect, fall back, and prove the result.

### Partial graphics programs & Advanced Shader Delivery (`D3D12.PartialPrograms`)

Part V's PSOs bake *all* state into one object, which is why they're fast to bind
and slow to create. Multiply shaders by blend modes, render-target formats,
vertex layouts and material options and a big game ends up with **tens of
thousands** of PSOs. Every one is a full compile for your exact GPU and driver.
There are three bad places to pay for that: a long "compiling shaders" screen on
first launch, stutter when a PSO is first needed mid-game, or both. Pipeline
libraries (Part X) help from the *second* run on; the first run still pays.

Consoles never had this problem: the GPU is fixed, so shaders are compiled to
final machine code at build time and shipped in the package. **Advanced Shader
Delivery (ASD)** brings that model to PC. The store compiles a game's shaders
ahead of time for specific GPU + driver combinations and delivers them with the
download, so the first launch doesn't compile at all. It first shipped for the
ROG Xbox Ally handhelds.

ASD only scales if the number of things to precompile is manageable, and that is
what **partial graphics programs** (Agility SDK 1.721-preview) address. Pipeline
creation is split in two:

1. **Compile the shared pieces once**, as partial programs in a *collection*: a
   *pre-rasterization* partial (vertex shader + input layout + topology) and a
   *pixel-shader* partial (pixel shader + render-target formats + a few fields
   that affect its compilation).
2. **Link** them into a **generic program** together with the state that varies
   (here, blend). Linking is cheap because the expensive compile already happened.

```cpp
// Collection: the expensive, shareable compile.
preRast->SetPartialGraphicsProgramType(D3D12_PARTIAL_GRAPHICS_PROGRAM_TYPE_PRERASTERIZATION_SHADER);
pixel  ->SetPartialGraphicsProgramType(D3D12_PARTIAL_GRAPHICS_PROGRAM_TYPE_PIXEL_SHADER);
psFields->SetLateLinkBlendSubobject(TRUE);        // blend is decided at link time
// Executable state object: one cheap link per variant.
program->AddExport(L"VSPartial"); program->AddExport(L"PSPartial"); program->AddSubobject(*blend);
// Draw: select the program instead of a PSO.
list10->SetProgram(&setProgramDesc);
```

The sample draws three cubes (opaque, additive, alpha-blended) in front of an
opaque bar, all from **one** compiled vertex partial and **one** pixel partial.
Those are linked into five generic programs (opaque, additive, two alpha passes,
and a wireframe variant on `W`) and selected with `SetProgram`; `P` swaps in five
equivalent classic PSOs, which draw the same image. The title times the
collection compile, each link, and the classic PSOs.

Read those numbers honestly. On preview WARP the API works, but the real compile
still happens at first draw for *every* variant: WARP doesn't reuse the partials
yet. NVIDIA's retail driver already skips recompiles when only blend or depth
state changes, but that is its own optimization. Partial programs turn that
vendor behavior into an API **guarantee**, and the speedup appears on drivers
with native tier 1.0 support.

**When to use:** large, permutation-heavy PSO sets where variants share shaders
and differ in link-time state, and anywhere you want precompiled pieces to be
reusable (ASD, pre-load compile screens).

**When *not* to:** a renderer with a few dozen PSOs just creates them at load and
caches them in a pipeline library. Partial programs are also still a **preview**:
they need `D3D12StateObjectsExperiment`, the 1.721-preview runtime and a
supporting driver (AMD's developer preview driver today, NVIDIA in a future
driver, WARP always).

> **Decision lens — PSO strategy.** *Few PSOs:* create them at load. *Many,
> recurring:* cache them with a pipeline library. *Massive permutations:* share
> compiled partials and link variants, and let ASD ship the compiled pieces so
> the player never waits.

### Also on the frontier (no sample)

Not everything in the Agility SDK is a rendering technique. These matter, but a
spinning cube can't usefully demonstrate them:

- **UAVs of depth** (1.721-preview) — create unordered-access views of
  depth/stencil resources so compute shaders can read *and write* depth and
  stencil directly.
- **GUID texture layouts** (1.721-preview) — name an exact memory layout (e.g.
  row-major NV12) by GUID so CPU, camera and video blocks can share textures
  without swizzle copies.
- **D3D12 video encode** — steady additions (HEVC reference-list extensions,
  multi-pass lower-resolution encoding, rate-control statistics). This matters
  for streaming and capture, but it's outside a rendering course.
- **Application-specific driver state** — a tools API
  (`ID3D12Tools2::SetApplicationSpecificDriverState`) that lets capture/replay
  tools such as PIX save the driver's per-app state and restore it, so a replay
  behaves like the original run.
- **DirectX dump files** — console-style GPU crash dumps on Windows; covered with
  debugging in [Part X](./part-10-presentation-devices-resources.md).

> **Decision lens — the frontier in general.** These features share a profile:
> big wins at *scale*, real hardware/driver requirements, and added complexity.
> Adopt them when you've hit the specific wall they knock down (binding limits,
> barrier over-sync, streaming waste, CPU-bound work generation, load times,
> alpha-test and divergence cost, PSO compile time) — not because they're newest.

### Historical "why now?"

The Agility SDK exists because tying graphics features to OS releases was too
slow. The features themselves all push the same direction the whole course has
traced: **more of the decision-making moves onto the GPU**, and the CPU's role
shrinks toward "start the GPU and get out of the way." Dynamic resources, work
graphs, and DirectStorage are what a fully GPU-driven, self-scheduling,
self-streaming renderer is built from. With **SER**, the GPU even reorders its
own threads. With **SM6.10 linear algebra** it runs neural networks inline,
folding the ML era directly into the shader. Meanwhile **partial programs** and
**Advanced Shader Delivery** borrow the console's oldest advantage, shipping
compiled shaders instead of compiling them on the player's machine.

---

## Part XI in one paragraph

The Agility-SDK frontier continues D3D12's trajectory toward GPU autonomy: **SM6.6
dynamic resources** finish the bindless journey (index the heap directly, no
tables); **enhanced barriers** give precise, low-waste synchronization; **sampler
feedback** replaces streaming guesswork with ground truth; **work graphs** let the
GPU generate and schedule its own work without the CPU; **DirectStorage**
streams and decompresses assets straight to the GPU; **DXR 1.2** resolves alpha
tests inside traversal (opacity micromaps) and lets the GPU regroup divergent
rays (SER); **SM6.10 linear algebra** runs, and now accumulates into, neural
networks inline; and **partial programs** make precompiled, deliverable shaders
(ASD) practical. Each is a scale-driven, hardware-gated tool — adopt it when you
hit the specific wall it removes.

---

*Next: [Part XII — Synthesis: Seeing the Whole Arc »](./part-12-synthesis.md)*
