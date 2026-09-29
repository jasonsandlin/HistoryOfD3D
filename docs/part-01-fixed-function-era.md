# Part I — The Fixed-Function Era

*[« Part 0](./part-00-orientation.md) · [Syllabus](./D3D-Evolution-Course.md) · Next: [Part II »](./part-02-programmable-shading.md)*

---

In this era the GPU is an **appliance**. You don't program it; you *configure*
it. There's a fixed sequence of operations baked into silicon — transform,
light, rasterize, blend — and your job is to set the knobs: this matrix, that
lighting flag, this cull mode. Then you hand it triangles and it does the rest.

Two samples anchor this Part: **D3D7** (`RotatingCubes\D3D7`) and **D3D8**
(`RotatingCubes\D3D8`).

---

## Module 1 — D3D7: Configuring an Appliance

### The problem it solved

In 1999, consumer GPUs had just gained **hardware transform & lighting (T&L)** —
the ability to do vertex math on the card instead of the CPU. Direct3D 7 exists
to expose that. Its entire worldview: the application describes *what state the
pipeline should be in*, and the fixed-function hardware executes a predetermined
program using that state.

### How it works

D3D7 is joined at the hip to **DirectDraw**, the older 2D surface API. You don't
get a tidy "swap chain"; you allocate raw surfaces and blit them yourself. Look
at the setup in `D3D7.Basic\main.cpp`:

```cpp
DirectDrawCreateEx(nullptr, (void**)&g_dd, IID_IDirectDraw7, nullptr);
g_dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
// A primary surface (the screen), a clipper (so we stay in our window),
// an offscreen 3D-capable back buffer, and a manually-attached Z-buffer:
g_dd->CreateSurface(&ddsd, &g_primary, nullptr);
g_dd->CreateClipper(0, &g_clipper, nullptr);
g_back->AddAttachedSurface(g_zbuffer);
// Finally, a Direct3D device that renders onto that back-buffer surface:
g_d3d->CreateDevice(IID_IDirect3DHALDevice, g_back, &g_device);
```

Notice there is **no shader, no input layout, no constant buffer**. Instead, the
pipeline is steered by *render states* and *transforms*:

```cpp
// [LEARN] The whole pipeline is configured by STATE, not shaders:
g_device->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
g_device->SetRenderState(D3DRENDERSTATE_ZENABLE, D3DZB_TRUE);
g_device->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
```

Drawing is likewise "here are vertices, here's their format, go":

```cpp
g_device->SetTransform(D3DTRANSFORMSTATE_WORLD, &world);
g_device->SetTransform(D3DTRANSFORMSTATE_VIEW, &view);
g_device->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &proj);
g_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, D3DFVF_LVERTEX,
    g_verts, 8, g_idx, 36, 0);
```

The `D3DFVF_LVERTEX` is a **Flexible Vertex Format** flag: a bitmask that tells
the fixed-function pipeline what's in each vertex (here: position + a pre-lit
color). The pipeline has hardcoded slots for "position," "color," "normal,"
"texture coords" — you can only describe vertices in *its* vocabulary.

And because DirectDraw doesn't present for you, the frame ends with a manual
blit from the back buffer to the window's client rectangle:

```cpp
g_primary->Blt(&dest, g_back, &src, DDBLT_WAIT, nullptr);
```

### When to use it (and when not)

This is a history lesson, not a recommendation: **never** start new work in D3D7.
It's here to establish the baseline. But the *mental model* — "configure a fixed
pipeline with state" — is worth understanding, because it's exactly what the
programmable pipeline replaced, and traces of it survive (depth state, blend
state, and rasterizer state in D3D12 are still "render states," just grouped into
immutable objects).

### Historical "why now?"

D3D7 couldn't have shaders because the *hardware* couldn't run them yet. The API
is a faithful mirror of 1999 silicon: fixed T&L, fixed lighting, fixed blending.
The DirectDraw coupling reflects an even older world where 2D and 3D were
separate concerns bolted together.

> **Decision lens — reading legacy fixed-function code.** When you meet a wall of
> `SetRenderState` calls in old code, don't try to map each one to a modern
> equivalent line-for-line. Instead ask "what *stage* is this configuring?"
> Lighting states → what a vertex/pixel shader would now compute. Texture-stage
> states → what a pixel shader would now do. Framing it by pipeline stage makes
> ancient code legible.

---

## Module 2 — D3D8: Consolidation and the First Cracks

### The problem it solved

D3D8 (2000) had two jobs. First, **consolidation**: it merged DirectDraw and
Direct3D into a single "DirectX Graphics" API, killing the awkward surface
juggling of Module 1. Second — and this is the earthquake — it introduced the
**first programmable shaders** (vertex and pixel shaders, in assembly, "shader
model 1.x"). Our sample deliberately stays fixed-function so you can see the
consolidation cleanly; we'll meet real shaders in Part II.

### How it works

The surface circus is gone. You describe how you want to present, and the runtime
manages the buffers:

```cpp
D3DPRESENT_PARAMETERS pp = {};
pp.Windowed = TRUE;
pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
pp.EnableAutoDepthStencil = TRUE;          // the runtime makes the Z-buffer for us
pp.AutoDepthStencilFormat = D3DFMT_D16;
g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
    D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &g_dev);
```

Compare that to D3D7's manual primary/back/Z-buffer/clipper dance — a whole
category of code simply evaporated. Presenting is now one call:

```cpp
g_dev->Present(nullptr, nullptr, nullptr, nullptr);
```

D3D8 also formalizes **vertex and index buffers** as GPU resources you fill once
and reuse, instead of passing raw pointers each draw:

```cpp
g_dev->CreateVertexBuffer(sizeof(verts), 0, FVF, D3DPOOL_MANAGED, &g_vb);
g_vb->Lock(0, sizeof(verts), &p, 0); memcpy(p, verts, sizeof(verts)); g_vb->Unlock();
```

There's a wonderful fossil in the draw code that foreshadows the whole next era:

```cpp
// [LEARN] D3D8 quirk: SetVertexShader(FVF) selects the FIXED-FUNCTION "shader"
// by passing an FVF code. The same entry point will soon take a real shader.
g_dev->SetVertexShader(FVF);
```

The method is literally called `SetVertexShader`, but here we hand it an FVF
bitmask — "use the fixed-function vertex pipeline." The socket for programmable
shaders has been wired in; we're just not plugging anything into it yet.

### A side lesson: APIs are mortal

Our D3D8 sample can't use the Windows SDK's headers, because **`d3d8.h` was
removed** from the modern SDK. The sample vendors the Wine project's headers and
loads `d3d8.dll` at runtime:

```cpp
#include "d3d8.h"   // vendored Wine/MinGW header; the SDK no longer ships one
PFN_Direct3DCreate8 create =
    (PFN_Direct3DCreate8)GetProcAddress(LoadLibraryA("d3d8.dll"), "Direct3DCreate8");
```

The DLL still ships with Windows for compatibility, but Microsoft stopped
supporting *development* against it. That's a real engineering lesson: **an API's
runtime and its SDK have different lifetimes.** Code you ship may keep running for
decades after you can no longer easily *build* against that API.

### When to use it

Again — never, for new work. But D3D8 marks the exact inflection point of this
entire course: it's the last era where "just configure the fixed pipeline" is the
whole story, and the first where the alternative exists. Everything from Part II
onward is life after that fork.

### Historical "why now?"

Two forces converged around 2000. Hardware gained programmable vertex units
(NVIDIA's GeForce 3 shipped pixel shaders in 2001), so the API needed a way to
express programs. And the DirectDraw/Direct3D split had become pure friction now
that all rendering went through the 3D pipeline. D3D8 resolved both at once.

> **Decision lens — recognizing a "consolidation" release.** D3D8 is a pattern
> you'll see again (D3D10, D3D12): a release that *removes* an entire subsystem
> and folds its job into a cleaner model. When evaluating whether to adopt such a
> release, the cost isn't learning the new features — it's that the old escape
> hatches are gone. D3D8 apps *couldn't* fall back to raw DirectDraw tricks. That
> forced simplicity is the point, but it's also the migration pain.

---

## Part I in one paragraph

The fixed-function era treats the GPU as a configurable appliance: you set render
states and transforms, hand over triangles, and hardwired silicon does the rest.
D3D7 shows the model at its most raw (manual DirectDraw surfaces); D3D8 cleans it
up (swap-chain-like presentation, vertex/index buffers) and quietly installs the
socket — `SetVertexShader` — into which the next era will plug real programs. The
stage is set for the revolution.

---

*Next: [Part II — The Programmable Shading Revolution »](./part-02-programmable-shading.md)*
