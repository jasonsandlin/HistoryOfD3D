# Part IV — Maturity & Generality

*[« Part III](./part-03-great-redesign.md) · [Syllabus](./D3D-Evolution-Course.md) · Next: [Part V »](./part-05-going-explicit-d3d12.md)*

---

Direct3D 11 (2009, Windows 7) is not a redesign — it's a **refinement and
expansion** of D3D10's clean foundation. It's arguably the most beloved release
in the timeline: modern enough to do everything, forgiving enough to be
productive, and still in heavy use today. Two additions define it — the pipeline
grows two new tricks (**tessellation** and **compute**), and the API grows up to
support **multithreaded** rendering and **feature levels**.

Anchoring samples (repo root): **`D3D11`** plus the showcases
**`D3D11.VertexShader`**, **`.PixelShader`**, **`.GeometryShader`**,
**`.ComputeShader`**, and **`.Tessellation`**.

---

## Module 5 — D3D11: The GPU as a General Compute Device

### The problem it solved

D3D10 was clean but had two notable gaps. First, its rendering was
**single-threaded** at the API level — a real bottleneck as CPUs went multi-core
and scenes grew to tens of thousands of draw calls. Second, using the GPU for
*non-graphics* work was awkward. D3D11 addresses both, and adds hardware
tessellation for good measure.

### Device vs. context: the split that enables threading

D3D10 had one object that was both "the GPU" and "the thing you issue commands
to." D3D11 **splits** them:

- The **device** (`ID3D11Device`) creates resources. It's free-threaded — any
  thread can call it.
- The **immediate context** (`ID3D11DeviceContext`) issues commands to the GPU.
- Optional **deferred contexts** record command lists on worker threads, to be
  replayed later on the immediate context.

You can see the split at creation in `D3D11.ComputeShader\main.cpp`:

```cpp
D3D11CreateDeviceAndSwapChain(..., &g_swap, &g_device, &fl, &g_ctx);
//                                          ^device        ^feature level ^context
```

**Why the split matters:** it's the conceptual seed of D3D12. Once "create a
resource" and "issue a command" are different objects, you can record commands on
many threads at once. D3D11's deferred contexts were a first, somewhat limited
attempt; D3D12's command lists (Part V) are the full realization. If you
understand *why* D3D11 split device from context, D3D12's command-list model will
feel inevitable rather than alien.

### Feature levels: one API, many hardware generations

D3D11 introduced **feature levels** — a brilliant pragmatic idea. The D3D11 API
can drive D3D9-, D3D10-, or D3D11-class hardware; you query what the actual GPU
supports and scale accordingly:

```cpp
D3D_FEATURE_LEVEL fl;
D3D11CreateDeviceAndSwapChain(..., &fl, ...);   // tells you what this GPU can do
```

This decoupled *API version* from *hardware capability*, so developers could
adopt the modern API without abandoning older GPUs. Feature levels carry forward
into D3D12.

---

### New trick #1: Tessellation (`D3D11.Tessellation`)

Tessellation adds **three** pipeline stages that subdivide geometry on the GPU:
the **hull shader** (sets how finely to subdivide), the fixed-function
**tessellator**, and the **domain shader** (positions each generated vertex). Our
sample subdivides each cube face and bulges it toward a sphere:

```hlsl
// Hull shader: how many pieces to cut each edge/interior into.
struct PatchConst { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
[domain("tri")] [outputcontrolpoints(3)] [patchconstantfunc("HSConst")]
VSOut HSMain(InputPatch<VSOut,3> ip, uint id : SV_OutputControlPointID) { return ip[id]; }

// Domain shader: place each newly-generated vertex (here, pushed toward a sphere).
[domain("tri")]
DSOut DSMain(PatchConst pc, float3 bary : SV_DomainLocation, const OutputPatch<VSOut,3> patch) { ... }
```

**When to use tessellation:** dynamic level-of-detail (more triangles up close,
fewer far away), displacement mapping (a heightmap turns a flat quad into terrain
on the GPU), and smooth curved surfaces from coarse control meshes. It shines
when you want *adaptive* geometry density without storing or transferring the
dense mesh.

**When *not* to:** if your geometry density is uniform and known, just author the
mesh at the right density — tessellation adds pipeline complexity for no gain.
And like the geometry shader, over-tessellating (tiny sub-pixel triangles) wrecks
performance because GPUs shade pixels in 2×2 quads.

> **Decision lens — adaptive geometry.** Choose tessellation for *continuous*
> LOD driven by distance/screen-size. Choose discrete authored LOD meshes when
> you have a few known levels and want predictable cost. Choose mesh shaders
> (Part VII) if you're on modern hardware and want programmable, culling-aware
> geometry generation.

---

### New trick #2: DirectCompute (`D3D11.ComputeShader`)

This is D3D11's most far-reaching addition: the **compute shader** (`cs_5_0`),
which runs general-purpose parallel programs on the GPU **outside** the draw
pipeline. No vertices, no pixels — just threads, thread-groups, and read/write
buffers/textures (**UAVs**, unordered-access views).

Our sample dispatches a compute shader that writes an animated pattern into a
texture, which the cube then samples:

```hlsl
RWTexture2D<float4> Output : register(u0);       // a UAV: shader can WRITE it
[numthreads(8,8,1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
  float2 uv = id.xy / 256.0;
  Output[id.xy] = float4(/* procedural color from uv and time */);
}
```
```cpp
// Two passes in one frame: compute fills the texture, then graphics samples it.
g_ctx->CSSetUnorderedAccessViews(0, 1, &g_uav, nullptr);
g_ctx->Dispatch(TEX / 8, TEX / 8, 1);            // launch (256/8)^2 thread groups
ID3D11UnorderedAccessView* nullUav = nullptr;
g_ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);  // unbind before sampling!
```

Note the **explicit unbind** of the UAV before the texture is sampled: the same
resource can't be bound for writing and reading simultaneously. D3D11 tracks
these hazards *for you* (it inserts the necessary sync) — remember that, because
in D3D12 tracking that hazard becomes **your** job via resource barriers.

**When to use compute:** post-processing (blur, bloom, tone-mapping), particle
simulation, physics, culling, image processing, building acceleration structures
— anything data-parallel that isn't naturally "draw triangles." Compute is the
foundation of modern GPU-driven rendering.

**When *not* to:** for work that maps cleanly onto the raster pipeline (e.g. a
fullscreen pass that reads and writes one target), a pixel shader over a
fullscreen triangle can be simpler and equally fast. Reach for compute when you
need cross-pixel communication (shared memory), scatter writes, or work that
isn't per-output-pixel.

> **Decision lens — compute vs. pixel-shader pass.** Use a **pixel shader** when
> the work is embarrassingly per-pixel and read-only across neighbors. Use a
> **compute shader** when you need groupshared memory, scatter/gather, arbitrary
> output sizes, or you're chaining many stages that never touch the screen.

### Historical "why now?"

Multi-core CPUs made single-threaded submission the bottleneck, so the
device/context split and deferred contexts appeared. GPUs had become fully
general parallel processors, so DirectCompute exposed that generality. And a
fragmented hardware market made feature levels a necessity. D3D11 is what
"programmable GPU" looks like once the idea is fully mature — which is precisely
why the *next* step had to be about **overhead**, not features.

---

## Part IV in one paragraph

D3D11 perfects the programmable-GPU model: it splits the free-threaded **device**
from the command-issuing **context** (enabling multithreaded submission), adds
**tessellation** for adaptive GPU geometry, and — most importantly — adds
**compute shaders**, turning the GPU into a general parallel processor for work
that has nothing to do with triangles. **Feature levels** let one API span many
GPU generations. Notice the recurring theme: D3D11 still tracks resource hazards
and synchronizes *for you*. That convenience is the last thing D3D12 will take
back — in exchange for the CPU efficiency that features alone could no longer buy.

---

*Next: [Part V — Going Explicit: The D3D12 Object Model »](./part-05-going-explicit-d3d12.md)*
