# Part II — The Programmable Shading Revolution

*[« Part I](./part-01-fixed-function-era.md) · [Syllabus](./D3D-Evolution-Course.md) · Next: [Part III »](./part-03-great-redesign.md)*

---

This is the pivot on which the whole course turns. In Part I you *configured* a
fixed pipeline. Starting now, you **write programs the GPU runs** — one per
vertex, one per pixel. Direct3D 9 (2002) is where this becomes practical and
pleasant, thanks to **HLSL**, the High-Level Shading Language, which lets you
write shaders in C-like code instead of GPU assembly.

Three samples anchor this Part, all in `RotatingCubes\D3D12\Tier1`'s ancestors at
the repo root: **`D3D9`** (fixed-function baseline), **`D3D9.VertexShader`**, and
**`D3D9.PixelShader`**.

---

## Module 3 — D3D9: HLSL and the Programmable Pipeline

### The problem it solved

Shader assembly (D3D8's `vs_1_1`) was miserable to write and impossible to
maintain — register allocation by hand, no functions, no readable math. D3D9's
answer was twofold: richer shader models (`vs_2_0`/`vs_3_0`, `ps_2_0`/`ps_3_0`)
*and* HLSL with a runtime compiler (`D3DCompile`/`fxc`) so you could write:

```hlsl
o.pos = mul(float4(p, 1.0), g_mvp);   // instead of a dozen dp4 assembly ops
```

D3D9 is also the era's workhorse: it powered a decade of games and is the point
where "modern" real-time graphics thinking begins.

### The baseline: `D3D9` (still fixed-function)

Crucially, D3D9 *kept* the fixed-function pipeline. The base `D3D9\D3D9.Basic\main.cpp` looks
almost identical to D3D8 — set matrices, set an FVF, draw:

```cpp
g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
// ...
g_dev->SetFVF(FVF);                 // fixed-function vertex processing
g_dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 8, 0, 12);
```

This matters: D3D9 is a **transitional** API. Fixed function and programmable
shaders coexist, and you could adopt shaders incrementally. That gentle on-ramp
is a big reason D3D9 was adopted so widely and lived so long.

### How programmable shading works: `D3D9.VertexShader`

Now compare `D3D9.VertexShader\main.cpp`. Instead of `SetFVF`, we compile a
vertex shader from HLSL at runtime and bind it. The shader **replaces** the
fixed-function transform — computing clip-space position is now *our* job:

```hlsl
// [LEARN] This vs_3_0 shader REPLACES D3D9's fixed-function transform.
float4x4 g_mvp : register(c0);
float    g_time : register(c4);
VSOut main(VSIn i) {
  float d = 0.20 * sin(g_time * 2.0 + i.pos.y * 3.0);
  float3 p = i.pos + normalize(i.pos) * d;   // radial "breathing"
  o.pos = mul(float4(p, 1.0), g_mvp);
  o.col = i.col;
  return o;
}
```

Because we own the vertex program, we can do things the fixed pipeline *never
could* — like displacing every vertex by a time-varying sine wave so the cube
appears to breathe. That's the payoff: **expressiveness**. The fixed pipeline had
a menu; the shader is a blank page.

The host code changes to match. We compile HLSL, create shader objects, describe
the vertex layout with a *vertex declaration* (FVF's more flexible successor), and
feed the shader its constants:

```cpp
D3DCompile(src, strlen(src), ..., "main", "vs_3_0", 0, 0, &code, &err);
g_dev->CreateVertexShader((const DWORD*)code->GetBufferPointer(), &g_vs);
// ...per frame:
g_dev->SetVertexShader(g_vs);                             // a real shader now!
g_dev->SetVertexShaderConstantF(0, &mvp, 4);              // c0..c3 = MVP matrix
g_dev->SetVertexShaderConstantF(4, timeC, 1);             // c4 = time
```

That `SetVertexShader(g_vs)` is the same method that took an FVF bitmask in D3D8.
The socket now has a program in it. The revolution, in one line.

Note the **constant registers** (`c0`, `c4`). Shader inputs that aren't
per-vertex — matrices, time, light directions — are uploaded into numbered
constant registers. This is clumsy (you count float4 slots by hand) and D3D10
will replace it with **constant buffers**. But the *idea* — "shaders need
uniform inputs" — is born here.

### `D3D9.PixelShader`: owning the per-pixel result

The pixel-shader sample tells the mirror-image story for color. The
fixed-function pipeline interpolated vertex colors and did a fixed set of texture
blends; a `ps_3_0` pixel shader lets you compute each pixel's color with
arbitrary math (an animated procedural pattern, in the sample). Same lesson,
different stage: **a menu becomes a blank page.**

### When to use programmable shaders

By D3D9's design, the honest answer became "almost always." Any per-vertex
deformation (skinning, morphing, wind, water), any per-pixel effect (lighting
models beyond the fixed one, normal mapping, procedural texturing) *requires*
shaders. The fixed pipeline remained only for the simplest UI/blit work.

### When *not* to (in the D3D9 era specifically)

- **Old hardware.** In 2002–2005, plenty of cards had weak or no `ps_2_0+`
  support. Shipping code often kept a fixed-function fallback path. (This
  concern died with D3D10, which *mandated* shaders.)
- **Trivial rendering.** If you're just blitting a fullscreen image, the
  fixed-function path was less code. That calculus flips in D3D10 too.

### Trade-offs & decision guidance

The cost of shaders is a new **compilation and asset pipeline** (HLSL must be
compiled, cached, and versioned), plus the discipline of feeding constants
correctly. The benefit is unbounded expressiveness and moving work onto
massively parallel GPU units. In the D3D9 era the trade was "more pipeline
complexity for effects you literally cannot do otherwise" — an easy yes for
anything visually ambitious.

> **Decision lens — fixed-function vs. programmable (D3D9 framing).** Ask: *does
> the effect fit the fixed-function menu?* Flat or vertex-colored geometry,
> single-texture, standard lighting → fixed function is fine and simpler.
> Anything else → shaders. After D3D10 this lens is retired, because fixed
> function is *gone* and the question becomes moot.

### Historical "why now?"

By 2002 programmable hardware was mainstream (GeForce 3/4, Radeon 8500+), and the
assembly-only D3D8 model had proven too painful for real production. HLSL (and
NVIDIA's parallel Cg) made shaders *writable by normal engineers*. That
usability breakthrough — not the hardware alone — is what made the revolution
stick.

---

## Part II in one paragraph

D3D9 turns the shader socket D3D8 installed into a practical, high-level tool.
HLSL lets you *write* the per-vertex and per-pixel programs the GPU runs,
replacing fixed-function transform and coloring with blank-page expressiveness —
a breathing cube, a procedural surface. Constants enter shaders through numbered
registers (soon to become constant buffers), and — critically — fixed function
still coexists, making D3D9 a gentle, long-lived on-ramp into the programmable
world. Next, D3D10 will take the radical step of throwing the fixed pipeline away
entirely.

---

*Next: [Part III — The Great Redesign »](./part-03-great-redesign.md)*
