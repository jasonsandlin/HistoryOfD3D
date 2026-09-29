# Part IX — Classic Pipeline Mastery (Tier 6)

*[« Part VIII](./part-08-recording-querying-at-scale.md) · [Syllabus](./D3D-Evolution-Course.md) · Next: [Part X »](./part-10-presentation-devices-resources.md)*

> **Build note.** The Tier 6 samples described here now **exist** under
> `D3D12\Tier6\` and build with `build-all.ps1`. Code sketches below are
> simplified for teaching; read each sample's `main.c` for the full version.

---

The exotic features (mesh shaders, ray tracing) get the attention, but shipping
games spend most frames on three humble techniques that every renderer needs and
that beginners routinely get subtly wrong: **instancing**, **blending**, and
**stencil**. Tier 6 slows down on these fundamentals — done in the explicit D3D12
model — because mastering them separates a renderer that *works* from one that
looks *correct*.

Anchoring samples (`D3D12\Tier6\`): `Instancing`, `Blending`, `Stencil`.

---

## Module 12 — Fundamentals That Ship Every Game

### Instancing (`Instancing`)

Draw the **same mesh many times** in one call, with per-instance data (transform,
color) pulled from a second vertex stream or a buffer indexed by
`SV_InstanceID`:

```hlsl
// per-instance transform selected by the instance index
float4 world = mul(instanceTransforms[iid], localPos);
```
```
DrawIndexedInstanced(indexCount, instanceCount = 10000, ...)   // one call, 10k cubes
```

**When to use:** forests, crowds, particles, debris, grass — anything with many
copies of one mesh. Instancing collapses thousands of draws into one, slashing CPU
overhead. It's the single most important draw-call optimization.

**When *not* to:** if every object is a *different* mesh, instancing doesn't apply
(look at indirect draw / GPU-driven instead). And extreme instance counts of
complex meshes can become GPU-bound — instancing fixes CPU cost, not GPU cost.

> **Decision lens — reducing draw calls.** Instancing for many copies of one
> mesh. Merged/static batching for many static different meshes. Indirect draw
> (Part VI) when the GPU decides counts. Pick by *what varies* between your
> objects.

### Blending (`Blending`)

Blending combines a fragment's output with what's already in the render target —
the basis of transparency, glass, smoke, and UI compositing:

```
finalColor = srcColor * srcAlpha + dstColor * (1 - srcAlpha)   // standard alpha
```

The catch that trips everyone: **order**. Alpha blending is not commutative, so
transparent surfaces must be drawn **back-to-front**, and depth *writes* usually
disabled for them (depth *test* still on). Additive blending (for fire, glow) is
order-independent, which is part of why it's popular.

**When to use:** any translucency — windows, water surfaces, particles, UI. Bake
the blend state into the PSO for the transparent pass.

**When *not* to (carelessly):** don't blend opaque geometry (it's wasteful and
order-dependent); render opaques first with depth writes on, then transparents
sorted. And heavy overdraw from many blended layers is a common, silent
performance sink.

> **Decision lens — transparency strategy.** Sorted back-to-front alpha for
> general translucency (accept the sort cost). Additive for glows/particles
> (order-free). Order-independent transparency (OIT) techniques only when sorting
> genuinely can't produce a correct order and you can afford the extra passes.

### Stencil (`Stencil`)

The stencil buffer is a small per-pixel integer you write and test alongside
depth — a general-purpose "mask" for the screen. Write a value in one pass, then
restrict a later pass to only the pixels carrying that value:

```
Pass 1: draw mirror shape → write stencil = 1 where it covers
Pass 2: draw reflected scene, stencil-test == 1  → reflection appears only in the mirror
```

**When to use:** mirrors and portals (mask drawing to a region), decals, outline
effects, constructive masking, and clever multi-pass tricks. Stencil is cheap
(it rides along with the depth buffer) and remarkably versatile.

**When *not* to:** for simple rectangular clipping a scissor rect is simpler; for
complex compositing a compute pass may be clearer. Stencil shines for
*geometry-defined* masks resolved during rasterization.

> **Decision lens — masking a region.** Scissor rect for axis-aligned rectangles.
> Stencil for arbitrary geometry-shaped masks resolved in the raster pipeline.
> Compute/UAV for programmatic masks you compute yourself.

### Historical "why now?" (in the course, not in the API)

These features are as old as the fixed-function era — D3D7 had blending and
stencil. They appear *here* in the course, not because they're new, but because
seeing them in the **explicit D3D12 model** (baked into PSOs, sequenced with
barriers, driven with instanced/indirect draws) ties the fundamentals to
everything the modern API added. Mastery is knowing that the newest GPU still
lives or dies by how well you sort your transparents.

---

## Part IX in one paragraph

Tier 6 returns to the fundamentals every renderer ships: **instancing** (one call,
thousands of copies — the top draw-call optimization), **blending** (translucency,
with the ever-present ordering and overdraw caveats), and **stencil** (a
per-pixel geometry mask for mirrors, decals, and outlines). None are new to D3D12,
but seeing them in the explicit model — baked into PSOs, ordered deliberately —
is what turns a renderer that runs into one that looks correct.

---

*Next: [Part X — Presentation, Devices & Advanced Resources (Tiers 7–8) »](./part-10-presentation-devices-resources.md)*
