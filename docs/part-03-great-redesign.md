# Part III — The Great Redesign

*[« Part II](./part-02-programmable-shading.md) · [Syllabus](./D3D-Evolution-Course.md) · Next: [Part IV »](./part-04-maturity-generality.md)*

---

Direct3D 10 (2006, Windows Vista) is the boldest release in the whole timeline.
It didn't extend the D3D9 model — it **replaced** it. Fixed function: gone.
Legacy state juggling: gone. In their place, a clean, strict, shader-only
pipeline built on a new foundation (**DXGI**) that survives, largely unchanged,
into D3D12 today. If D3D9 was a gentle on-ramp, D3D10 was a hard reset.

Anchoring samples (repo root): **`D3D10`**, plus the stage showcases
**`D3D10.VertexShader`**, **`D3D10.PixelShader`**, and **`D3D10.GeometryShader`**.

---

## Module 4 — D3D10: Tearing It Down and Rebuilding

### The problem it solved

D3D9's API had accumulated a decade of barnacles: fixed-function *and*
programmable paths, dozens of loosely-validated render states, per-call driver
overhead from re-validating everything constantly. D3D10 (which *required* new
hardware and a new driver model, WDDM) took the opportunity to design the API
Microsoft wished they'd had all along:

- **No fixed function.** Every draw *must* have a vertex and pixel shader. This
  halved the API surface and the driver's validation burden.
- **Constant buffers** replace numbered constant registers.
- **DXGI** (DirectX Graphics Infrastructure) is factored out as a separate,
  version-independent layer for adapters, swap chains, and presentation.
- **Strict, up-front validation.** State is grouped and validated when created,
  not re-checked every draw.
- **A new stage:** the geometry shader.

### How it works: the new foundation

The very first thing you notice in `D3D10\D3D10.Basic\main.cpp` is **DXGI**. The swap chain
is described by a `DXGI_SWAP_CHAIN_DESC` and created alongside the device:

```cpp
DXGI_SWAP_CHAIN_DESC sd = {};
sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
sd.OutputWindow = hwnd;  sd.SampleDesc.Count = 1;  sd.Windowed = TRUE;
D3D10CreateDeviceAndSwapChain(nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr,
    0, D3D10_SDK_VERSION, &sd, &g_swap, &g_dev);
```

Why does this matter? **DXGI is the piece that outlives every D3D version.** The
`DXGI_FORMAT_R8G8B8A8_UNORM`, the swap chain, `Present()` — you'll see the *exact
same DXGI types* in the D3D12 samples in Part V. Microsoft factored out the
"talk to the display and the adapter" concern so it could evolve the *rendering*
API (D3D10 → 11 → 12) without re-inventing *presentation* each time. When you
learn DXGI once here, you've learned it for good.

### Render targets as views

D3D10 also formalizes the **view** concept. A resource (a texture) is separate
from how you *interpret* it. You don't render "to a texture"; you render to a
**render-target view** of it, and you'd sample it through a **shader-resource
view**:

```cpp
g_swap->GetBuffer(0, __uuidof(ID3D10Texture2D), (void**)&back);
g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);       // how we WRITE it
// (depth buffer gets a CreateDepthStencilView the same way)
g_dev->OMSetRenderTargets(1, &g_rtv, g_dsv);
```

This resource/view split is fundamental and permanent — it's exactly how D3D12's
descriptor heaps work, just made explicit and manual there.

### Constant buffers: the end of numbered registers

Remember D3D9's `SetVertexShaderConstantF(0, &mvp, 4)` — counting float4 slots by
hand? D3D10 replaces it with a real object. The shader declares a `cbuffer`; the
host fills a buffer and binds it:

```hlsl
cbuffer CB : register(b0) { float4x4 mvp; };
```
```cpp
bd.BindFlags = D3D10_BIND_CONSTANT_BUFFER;  g_dev->CreateBuffer(&bd, nullptr, &g_cb);
// per frame:
g_dev->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
g_dev->VSSetConstantBuffers(0, 1, &g_cb);
```

**Why this is better:** you update a whole struct of related constants in one
shot, the layout is described once by the shader, and the runtime can manage the
upload efficiently. Constant buffers are so right that they're essentially
unchanged in D3D11 and D3D12.

### The input layout is validated against the shader

D3D10 ties the vertex layout to the compiled shader's expected inputs, catching
mismatches up front:

```cpp
g_dev->CreateInputLayout(il, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_layout);
```

This "validate against the shader signature" idea is the seed of D3D12's
`ID3D12PipelineState`, which bundles the layout, shaders, and all render state
into one pre-validated immutable object.

### The new stage: the geometry shader (`D3D10.GeometryShader`)

D3D10's headline *new capability* is a programmable stage **between** vertex and
pixel shading that operates on whole primitives and can **emit** new ones. Our
sample uses it to explode the cube's faces along their normals:

```hlsl
// [LEARN] The geometry shader (gs_4_0) sees a whole triangle and can emit vertices.
[maxvertexcount(3)]
void GSMain(triangle VSOut input[3], inout TriangleStream<GSOut> stream) {
  float3 n = normalize(cross(input[1].opos - input[0].opos,
                             input[2].opos - input[0].opos));   // face normal
  // push each vertex outward along n by a time-varying amount, then emit
}
```

### When to use the geometry shader

Genuinely useful for: point-sprite expansion (1 vertex → a quad), generating
normals/adjacency, single-pass rendering to cube-map faces, and small
primitive-level tricks. But note the caveat that shaped the next decade:

### When *not* to use it — a cautionary tale

The geometry shader turned out to be **slow** on most hardware. Its ability to
amplify geometry (emit more vertices than it received) maps badly onto GPU
architectures — it forces serialization and buffering. In practice, teams learned
to *avoid* the GS for anything performance-sensitive. This is why D3D12 later
introduced **mesh & amplification shaders** (Part VII): a redesign of
programmable geometry amplification that actually runs fast. The geometry shader
is a rare example of a feature that shipped, disappointed, and was eventually
*superseded* rather than extended.

> **Decision lens — should I reach for the geometry shader?** Default to **no**
> for hot paths. Consider it only when (a) the amplification factor is tiny or
> zero, (b) the alternative (extra draw calls, compute pre-pass) is clearly
> worse, and (c) you've measured. If you're on D3D12-class hardware and need real
> geometry amplification, jump straight to mesh shaders instead.

### Trade-offs of the whole D3D10 redesign

- **Gained:** a clean, strict, lower-overhead API; constant buffers; DXGI;
  shader-validated layouts; the resource/view model. A far better *foundation*.
- **Paid:** no backward compatibility with fixed-function code (a hard port for
  legacy engines), and it required Vista + new hardware, which slowed adoption.
- **Verdict:** history vindicated it. Nearly every good idea in D3D10 is still
  present in D3D12. The redesign was worth the disruption.

### Historical "why now?"

Vista's new driver model (WDDM) was a once-in-a-decade chance to break
compatibility. Microsoft spent it deliberately, shipping the API that hardware
had outgrown D3D9's abstractions for. D3D10 is the "measure twice, cut once"
release — and the cut has held for nearly twenty years.

---

## Part III in one paragraph

D3D10 rebuilds Direct3D from the studs: fixed function is deleted, every draw is
shader-driven, constants become first-class **constant buffers**, resources are
accessed through **views**, layouts are **validated against shaders**, and
presentation is factored into **DXGI** — the durable layer you'll still be using
in D3D12. Its new geometry shader is a lesson in humility (powerful but slow,
later superseded). The era's real gift is a clean foundation; D3D11 will build
generality and multithreading on top of it.

---

*Next: [Part IV — Maturity & Generality »](./part-04-maturity-generality.md)*
