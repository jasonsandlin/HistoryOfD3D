# Part 0 — Orientation

*[« Back to the syllabus](./D3D-Evolution-Course.md) · Next: [Part I »](./part-01-fixed-function-era.md)*

---

Before we open a single sample, let's establish the experimental setup, the
anatomy every program shares, and a vocabulary of ideas that will survive all
five API rewrites. Spend time here; it pays compound interest.

## Module 0.1 — The rotating cube as a controlled experiment

Every sample in this course renders the same thing: a unit cube, colored at its
eight corners, spinning slowly around two axes against a dark blue background.
The geometry is *identical* everywhere — the same eight vertices, the same 36
indices (12 triangles, 2 per face):

```c
// This same index list appears in D3D7, D3D8, D3D9, ... D3D12. It never changes.
0,1,2, 0,2,3,  4,6,5, 4,7,6,  4,5,1, 4,1,0,
3,2,6, 3,6,7,  1,5,6, 1,6,2,  4,0,3, 4,3,7,
```

Because the *output* is pinned, everything else in a sample is, by definition,
**API overhead** — the machinery a given Direct3D version demands before it will
put those 12 triangles on screen. Reading two samples side by side is therefore
a direct measurement of what changed between two eras, and *why*.

Keep a running question in your head as you read: **"Who is doing this work — the
runtime, the driver, or me?"** The entire history of Direct3D is the answer to
that question shifting, one responsibility at a time, from *them* to *you*.

### Why "shift work onto the programmer" is progress, not regress

It feels backwards that a *newer* API makes you write *more* code. The reason it's
progress is that the work the old runtime did for you was done **generically** —
it had to be correct for every program, so it was conservative, it synchronized
defensively, and it guessed at your intent. When you take that work over, you can
do it **specifically**: you know your frame structure, so you can synchronize
exactly once instead of defensively; you know your memory lifetimes, so you can
pack resources tightly. The old runtime's generality was a tax. D3D12 hands you
the money back and the responsibility with it.

> **Decision lens — "Should I use the newest API?"** More control is only worth
> it if you can *spend* it. A tool that draws a few thousand triangles will not
> beat the D3D11 runtime by managing memory by hand — it'll just have more bugs.
> Reach down an era when you have a bottleneck the runtime's generality is
> causing (draw-call CPU cost, memory residency, submission threading) and a plan
> to exploit the control you gain. We'll return to this lens repeatedly.

## Module 0.2 — The anatomy every sample shares

Strip away the era-specific detail and every sample has the same skeleton. Learn
it once and you can navigate any of them:

1. **Create a window.** Plain Win32: `RegisterClass`, `CreateWindow`, a
   `WndProc` that quits on `ESC`. Identical everywhere; we'll never discuss it
   again.
2. **Initialize the device.** Acquire the GPU abstraction and a surface to draw
   into. *This is where the eras differ most.*
3. **Create resources.** Vertex data, index data, and (from D3D9 on) shaders and
   constant storage.
4. **The frame loop.** Pump window messages; when idle, compute the current
   rotation angle from elapsed time and render a frame.
5. **Render.** Clear, set up transforms, draw the cube, and present the result to
   the screen.

Here's the same frame loop, essentially unchanged from D3D7 to D3D12:

```cpp
DWORD start = GetTickCount();
MSG msg = {};
while (msg.message != WM_QUIT) {
    if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    else Render((GetTickCount() - start) / 1000.0f);   // t = seconds since start
}
```

The `Render(t)` function is where the whole story lives. In D3D7 it's a dozen
lines. In D3D12 it orchestrates command allocators, records a command list,
inserts resource barriers, submits to a queue, waits on a fence, and presents.
Same five words in the loop; wildly different amounts of ceremony inside.

## Module 0.3 — Vocabulary that persists across every era

These concepts predate D3D7 and outlive D3D12. The *names* and *objects* change,
but the ideas are constant. When an era "introduces" something, it's usually
making an old implicit idea explicit.

- **Vertex** — a point with attributes (position, color, texture coordinates).
- **Primitive** — how vertices assemble into shapes; we always use a
  **triangle list**.
- **Index buffer** — reuse shared vertices (a cube has 8 corners but 36 triangle
  corners) by referencing them by number.
- **Transform** — the matrices that move geometry from model space, to the
  camera's view, to the 2D screen. The **model-view-projection (MVP)** matrix is
  the product of all three.
- **Rasterization** — turning a projected triangle into the pixels it covers.
  Always done by fixed-function silicon; never programmable (until ray tracing
  sidesteps it entirely in Part VII).
- **Depth buffer (Z-buffer)** — a per-pixel depth so nearer surfaces hide farther
  ones. Every sample has one.
- **Render target** — the image being drawn into. The **swap chain** (from D3D8
  on) holds the front/back buffers and **presents** the finished frame.
- **The frame** — clear → set transforms → draw → present, sixty times a second.

### The one genuinely new idea: the programmable shader

If you internalize a single arc, make it this one. In D3D7, the pipeline is a
**fixed function**: you set *state* (a matrix here, a lighting flag there) and
the silicon does a hardwired sequence of operations. Starting in D3D8/D3D9,
stages of that pipeline become **programmable** — you supply a small program (a
**shader**) that the GPU runs per vertex or per pixel. Everything after D3D9 is,
in a sense, the consequences of that one idea:

- If vertices and pixels can run programs, **why not whole new stages?** →
  geometry (D3D10), tessellation (D3D11), mesh/amplification (D3D12).
- If the GPU runs programs, **why only graphics programs?** → compute (D3D11).
- If the GPU is a programmable processor, **why does the driver babysit it so
  much?** → the explicit model of D3D12.

Hold that thread. Now let's go back to 1999, when none of it existed yet.

---

*Next: [Part I — The Fixed-Function Era »](./part-01-fixed-function-era.md)*
