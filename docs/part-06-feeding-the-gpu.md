# Part VI — Feeding the GPU: Memory, Copies, and GPU-Driven Work

*[« Part V](./part-05-going-explicit-d3d12.md) · [Syllabus](./D3D-Evolution-Course.md) · Next: [Part VII »](./part-07-image-quality-modern-pipeline.md)*

---

Tier 1 got a triangle on screen the explicit way. But real engines spend most of
their D3D12 effort on two problems the skeleton ignored: **where resources live in
memory and how data gets there**, and **how to issue thousands of draws
efficiently — ideally from the GPU itself.** Tier 2 is about memory and copies;
Tier 3 is about compute-driven rendering. Both are where D3D12's explicitness
starts paying real dividends.

Anchoring samples: **`D3D12\Tier2\`** (`PlacedResources`, `CopyQueue`,
`IndirectDraw`, `GeometryShader`, `Tessellation`) and **`D3D12\Tier3\`**
(`ComputeShader`, `ComputeAsync`, `Bindless`, `DeferredMRT`).

---

## Module 8 — Tier 2: Memory & Data Movement

### The problem it solved

D3D11 allocated memory for every resource you created and copied your data up for
you — convenient, but opaque and often wasteful. D3D12 exposes the machinery so
you can **sub-allocate**, **alias**, and **stream** data on your own terms, and
move copies onto dedicated hardware so they don't stall rendering.

### Placed resources (`D3D12.PlacedResources`)

A **committed** resource (the Tier-1 default) gets its own implicit heap
allocation. A **placed** resource is created *at an offset into a heap you
allocated yourself*:

```
heap = CreateHeap(64 MB)
bufferA = CreatePlacedResource(heap, offset = 0,        ...)
bufferB = CreatePlacedResource(heap, offset = 1 MB,     ...)   // same heap
```

**When to use:** sub-allocating many small resources from one big heap (far
cheaper than many committed allocations); **aliasing** two resources that are
never used at the same time onto the same memory (a transient shadow buffer and a
transient blur buffer can share bytes); and any streaming system where you manage
a memory budget yourself.

**When *not* to:** for a handful of long-lived resources, committed resources are
simpler and the allocation overhead is irrelevant. Placement is a tool for
*scale* and *memory pressure*, not a default.

> **Decision lens — committed vs. placed.** Committed for "a few big things that
> live forever." Placed (or a sub-allocator built on it) for "many things,"
> "tight memory budget," or "resources that can share space because their
> lifetimes don't overlap."

### The copy queue (`D3D12.CopyQueue`)

D3D12 has three queue types: **direct** (graphics + everything), **compute**, and
**copy**. The copy queue maps to dedicated DMA hardware that shuttles data over
PCIe **in parallel with** the GPU rendering:

```
copyQueue.ExecuteCommandLists(uploadCmds)    // DMA engine streams texture up...
directQueue.ExecuteCommandLists(drawCmds)    // ...while the GPU keeps drawing
fence: direct queue waits until the copy fence signals before using the texture
```

**When to use:** background streaming of textures/meshes in an open-world game,
uploading next frame's data while this frame renders — anything where you don't
want a big transfer blocking the graphics queue. The cross-queue **fence** is how
you make the graphics queue wait for the copy to finish.

**When *not* to:** a small one-time upload at load time doesn't need its own
queue; just upload on the direct queue. The copy queue earns its complexity when
transfers are large, frequent, and overlap with rendering.

### Indirect draw (`D3D12.IndirectDraw`)

`ExecuteIndirect` reads its draw parameters **from a GPU buffer** instead of from
CPU arguments. The GPU can fill that buffer itself (via compute), so the CPU never
sees the individual draws:

```
computeShader → writes {indexCount, instanceCount, ...} into argsBuffer
ExecuteIndirect(commandSignature, maxCount, argsBuffer)   // GPU decides the draws
```

**When to use:** GPU-driven rendering — the GPU culls objects and builds its own
draw list, so a scene with 100,000 candidate objects costs the CPU *one* call.
This is the backbone of modern GPU-driven pipelines.

**When *not* to:** if the CPU already knows the draws and there aren't many,
ordinary `DrawIndexedInstanced` is simpler and easier to debug. Indirect draw is
for *massive* counts or when draw parameters are computed on the GPU.

### Geometry & tessellation, D3D12 edition

`Tier2` also re-homes the **geometry** and **tessellation** stages (same `gs_5_0`
/ `hs_5_0`+`ds_5_0` concepts from Parts III–IV) into the explicit D3D12 model —
same shaders, now driven through PSOs and root signatures. The *pipeline* is
unchanged; only the *plumbing* is explicit. That's the recurring D3D12 lesson.

---

## Module 9 — Tier 3: Compute-Driven Rendering

### The problem it solved

Compute shaders (Part IV) let the GPU do general work; D3D12 lets you schedule
that work *explicitly* — on a separate queue, overlapping graphics — and bind
resources *bindlessly*, removing the descriptor-shuffling that limited D3D11.

### Compute in D3D12 (`D3D12.ComputeShader`)

Same `cs_5_0`/`Dispatch` model as D3D11, but **you** insert the UAV barriers that
D3D11 inserted for you. The write-then-read hazard from Part IV is now an explicit
`D3D12_RESOURCE_BARRIER` (UAV or transition) between the dispatch and the draw
that samples the result. Convenience became responsibility — the through-line of
the whole D3D12 story.

### Async compute (`D3D12.ComputeAsync`)

This is the payoff for having separate queues. Submit compute work to a **compute
queue** that runs *concurrently* with the graphics queue, so the GPU's compute
units stay busy during graphics bubbles:

```
computeQueue: simulate particles / cull / build args     ─┐ run at the
graphicsQueue: shade the previous frame's geometry        ─┘ same time
fence: graphics waits on compute only where it must consume the result
```

**When to use:** overlapping independent workloads — e.g. run next frame's
light-culling compute *while* this frame's shadow passes render. On GPUs with
spare compute capacity during graphics-heavy phases, this is free performance.

**When *not* to:** if compute and graphics both saturate the same units, running
them "concurrently" just time-slices with extra sync overhead — you can lose
performance. Async compute needs *measurement*; it is not automatically a win.

> **Decision lens — async compute.** Profile first. It helps when one queue has
> idle units the other can use (memory-bound graphics + ALU-bound compute, or
> vice versa). It hurts when both fight for the same resource. The cross-queue
> fences also add real complexity — only adopt it if the profiler shows the gap.

### Bindless (`D3D12.Bindless`)

Classic binding names each texture in the root signature — a hard limit and
constant descriptor shuffling. **Bindless** puts *all* resources in one big
descriptor heap and lets the shader index it with an integer (using SM5.1's
unbounded arrays / dynamic indexing):

```hlsl
Texture2D textures[] : register(t0);        // unbounded array
float4 c = textures[materialIndex].Sample(s, uv);   // pick a texture by index
```

**When to use:** many materials/textures, GPU-driven rendering (the GPU picks
which texture per object), ray tracing (a hit shader must reach *any* material).
Bindless is a prerequisite for most modern GPU-driven and raytraced pipelines.

**When *not* to:** a simple forward renderer with a few textures doesn't need it;
traditional binding is easier to reason about. Bindless trades a little safety and
clarity for enormous flexibility and scale.

### Deferred shading / MRT (`D3D12.DeferredMRT`)

**Multiple render targets** let one pixel-shader pass write several outputs at
once — position, normal, albedo — into a **G-buffer**. A later fullscreen pass
reads the G-buffer and computes lighting once per screen pixel:

```
Geometry pass  → writes  RT0=albedo, RT1=normal, RT2=position   (MRT)
Lighting pass  → reads   those 3 targets, shades each pixel with all lights
```

**When to use:** scenes with **many** dynamic lights. Forward shading costs
`objects × lights`; deferred decouples them to `objects + lights`, so hundreds of
lights become affordable.

**When *not* to:** deferred struggles with **transparency** (a G-buffer stores one
surface per pixel) and **MSAA** (expensive on a fat G-buffer), and it burns
memory bandwidth on the G-buffer itself. Forward+ / clustered forward are often
better for transparency-heavy or bandwidth-limited targets.

> **Decision lens — forward vs. deferred.** Deferred when light count dominates
> and materials are fairly uniform. Forward (or Forward+) when you have heavy
> transparency, need cheap MSAA, or run on bandwidth-constrained hardware.
> There's no universally right answer — it's a bandwidth-vs-light-count trade.

### Historical "why now?"

Once the GPU could run general compute (D3D11) *and* the API stopped hiding
queues, memory, and descriptors (D3D12), "let the GPU drive itself" became
practical: cull, build draw lists, and shade — all on the GPU, with the CPU
merely kicking it off. Tier 2 and Tier 3 are where D3D12 stops being "D3D11 with
more typing" and starts enabling things D3D11 simply couldn't.

---

## Part VI in one paragraph

Tier 2 exposes **memory** (placed resources, heap aliasing) and **data movement**
(the parallel copy queue, GPU-authored draws via `ExecuteIndirect`). Tier 3 turns
the GPU into the director: **async compute** overlaps independent work across
queues, **bindless** removes binding limits so shaders index resources by integer,
and **deferred MRT** decouples lighting cost from object count. Every one of these
is a *when-to-use* decision with a real downside — the point of D3D12 isn't that
these are better, it's that you're now the one choosing.

---

*Next: [Part VII — Image Quality & the Modern Pipeline »](./part-07-image-quality-modern-pipeline.md)*
