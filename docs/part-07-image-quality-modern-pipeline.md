# Part VII — Image Quality & the Modern Pipeline

*[« Part VI](./part-06-feeding-the-gpu.md) · [Syllabus](./D3D-Evolution-Course.md) · Next: [Part VIII »](./part-08-recording-querying-at-scale.md)*

---

Tier 4 covers two different frontiers at once. The first is **image quality** —
the classic techniques that make a render look correct and clean: shadows,
anti-aliasing, and adaptive shading. The second is the **Shader Model 6 pipeline
revolution** — mesh/amplification shaders and hardware ray tracing, which
redefine how geometry reaches the rasterizer and how light is simulated. Both live
in `D3D12\Tier4\`.

Anchoring samples: **`ShadowMap`, `MSAA`, `VariableRateShading`** (image quality)
and **`MeshShader`, `AmplificationShader`, `RayTracing`** (SM6 pipeline).

---

## Module 10a — Image Quality Fundamentals

### Shadow mapping (`D3D12.ShadowMap`)

The oldest trick for real-time shadows, and still the dominant one. Render the
scene **from the light's point of view** into a depth texture; then, when shading
each pixel, transform it into light space and compare its depth against the stored
depth — if it's farther, it's in shadow.

```
Pass 1: render depth from light  → shadowMap (R32_TYPELESS depth, read later as SRV)
Pass 2: render from camera; for each pixel:
        depthInLight > shadowMap.SampleCmp(...) ? in shadow : lit
```

The sample uses a **typeless** depth resource so the same texture can be a DSV
when written and an SRV when read, plus a **comparison sampler** that does the
depth test in hardware (giving smooth PCF edges for free).

**When to use:** essentially always for dynamic shadows from spot/directional
lights. It's the baseline every game builds on.

**When *not* to (naively):** a single shadow map has fixed resolution — great near
the camera, blocky far away. Large scenes need **cascaded** shadow maps (several
maps at different ranges), and point lights need cube maps or other tricks. Shadow
mapping is a family of techniques, not one setting; the sample teaches the core so
the elaborations make sense.

> **Decision lens — shadow technique.** Single map for a small, contained scene.
> Cascades for open worlds (resolution where it matters). Ray-traced shadows
> (below) when you have the hardware budget and want contact-accurate,
> filter-free results.

### MSAA (`D3D12.MSAA`)

Multisample anti-aliasing fixes jagged geometry **edges** by sampling coverage and
depth at multiple points per pixel while shading only once per pixel — cheaper
than supersampling. In D3D12 you render to a multisampled target and then
**resolve** it down to a normal one:

```
render → msaaTarget (e.g. 4x)
ID3D12GraphicsCommandList_ResolveSubresource(dest, msaaTarget)   // average samples → 1
present dest
```

**When to use:** geometry-edge aliasing in forward-rendered scenes. 4× MSAA is a
long-standing sweet spot for quality vs. cost.

**When *not* to:** MSAA only anti-aliases *geometry edges*, not shader aliasing
(specular sparkle) or texture aliasing, and it's expensive on a **deferred**
G-buffer (Part VI). That's why many modern engines prefer post-process AA (FXAA)
or temporal AA (TAA), which handle shader aliasing and cost less on deferred
pipelines — at the price of some blur/ghosting.

> **Decision lens — anti-aliasing.** MSAA for forward rendering with clean edges
> and no ghosting. TAA for deferred/complex pipelines and shader aliasing,
> accepting temporal artifacts. FXAA as a cheap catch-all. Often engines combine
> them.

### Variable-rate shading (`D3D12.VariableRateShading`)

VRS lets you shade **coarser than one invocation per pixel** where detail won't be
missed — e.g. 2×2 pixels share one shading result in blurry, distant, or
motion-blurred regions. It's set on a `ID3D12GraphicsCommandList5`:

```c
ID3D12GraphicsCommandList5_RSSetShadingRate(cmd, D3D12_SHADING_RATE_2X2, NULL);
```

**When to use:** shading-bound scenes where you can afford to shade some regions
coarsely — peripheral vision (foveated VR), out-of-focus areas, fast-moving
regions hidden by motion blur. It's close to free performance when applied where
the eye won't notice.

**When *not* to:** if you're geometry- or bandwidth-bound rather than
shading-bound, VRS buys nothing. And applied carelessly it produces visible
blocky shading. It also requires hardware support (this machine reports VRS Tier
2). Measure that you're shading-bound before reaching for it.

---

## Module 10b — The Shader Model 6 Pipeline Revolution

Everything up to here fed the rasterizer through the same fixed front end:
input-assembler → vertex shader → (optional geometry/tessellation) → rasterizer.
SM6 offers two radical alternatives.

### Mesh & amplification shaders (`D3D12.MeshShader`, `D3D12.AmplificationShader`)

The mesh shader **replaces** the entire input-assembler/vertex/geometry front end
with a *compute-style* program that outputs a small batch of triangles
(a "meshlet") directly:

```hlsl
[numthreads(128,1,1)] [outputtopology("triangle")]
void MSMain(uint tid : SV_GroupThreadID,
            out vertices VOut verts[64], out indices uint3 tris[126]) {
    // compute vertices and connectivity programmatically — no vertex buffer needed
}
```

The **amplification shader** runs *before* it and decides how many mesh-shader
groups to launch — the natural place to do **per-meshlet culling** (skip meshlets
that are off-screen or back-facing before they cost anything):

```
Amplification: cull meshlets → DispatchMesh(survivingCount)
Mesh:          each surviving group emits its triangles
```

**When to use:** extremely dense geometry, GPU-driven culling at meshlet
granularity, and procedural geometry — the mesh pipeline removes the fixed-function
bottlenecks (index-buffer fetch, primitive assembly) that limited the classic
front end. It's the modern answer to "draw enormous amounts of geometry
efficiently."

**When *not* to:** it requires recent hardware and a mesh-friendly content
pipeline (your assets must be split into meshlets). For ordinary geometry the
classic vertex pipeline is simpler and universally supported. Mesh shaders are a
scale-and-modernity play, not a default.

> **Decision lens — classic vs. mesh pipeline.** Classic vertex/index for broad
> compatibility and ordinary meshes. Mesh + amplification when you're geometry
> throughput-bound, want fine-grained GPU culling, or generate geometry
> procedurally — and can require modern GPUs.

### Ray tracing / DXR (`D3D12.RayTracing`)

Rasterization asks "which pixels does this triangle cover?" Ray tracing asks the
inverse: "what does this ray hit?" — the natural formulation for reflections,
refractions, accurate shadows, and global illumination. DXR introduces a whole
sub-model: **acceleration structures** (BLAS/TLAS) that organize geometry for fast
ray queries, and a new family of shaders:

- **ray generation** — casts primary rays,
- **closest-hit / any-hit** — shade what a ray hits,
- **miss** — what a ray hits nothing (sky).

```
Build BLAS (per-mesh) → TLAS (scene) 
DispatchRays(raygen)  → traversal hardware finds hits → hit/miss shaders shade
```

This is why bindless (Part VI) matters: a hit shader may strike *any* object, so
it must reach *any* material's resources by index. Ray tracing is also the payoff
for everything explicit about D3D12 — the shader table, acceleration-structure
memory, and barriers are all yours to manage.

**When to use:** effects that rasterization fakes poorly — mirror-accurate
reflections, soft contact shadows, ambient occlusion, and global illumination.
Often used *hybrid*: rasterize primary visibility, ray-trace specific effects.

**When *not* to:** ray tracing is expensive and needs RT-capable hardware; a fully
path-traced game is still a premium feature. For many effects a well-tuned raster
approximation (SSR, shadow maps, SSAO) looks close enough for a fraction of the
cost. Use DXR where the raster fake visibly fails.

> **Decision lens — raster vs. ray trace.** Raster for primary visibility and
> effects with good approximations — it's dramatically cheaper. Ray-trace the
> specific effects (reflections, GI, contact shadows) where approximations break
> down, and only when the hardware budget allows. Hybrid is the pragmatic norm.

### Historical "why now?"

By the late 2010s GPUs gained dedicated hardware — mesh-shader front ends and
ray-traversal units (RT cores) — and the SM6 (DXIL) compiler could express these
new pipelines. D3D12's explicit model was the prerequisite: managing acceleration
structures, shader tables, and meshlet buffers would be impossible under D3D11's
hidden bookkeeping. These features are where the *pipeline itself* finally
changes shape after two decades of the same rasterizer front end.

---

## Part VII in one paragraph

Tier 4 pairs classic **image quality** (shadow maps for shadows, MSAA-plus-resolve
for edge anti-aliasing, VRS for adaptive shading cost) with the **Shader Model 6
pipeline revolution** (mesh + amplification shaders that replace the fixed
geometry front end with programmable, culling-aware meshlet generation, and DXR
ray tracing that inverts the visibility question for accurate reflections,
shadows, and GI). Each is a deliberate trade — quality vs. cost, compatibility vs.
scale, raster approximation vs. ray-traced accuracy — and each depends on the
explicit foundation D3D12 laid.

---

*Next: [Part VIII — Recording & Querying at Scale (Tier 5) »](./part-08-recording-querying-at-scale.md)*
