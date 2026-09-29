# Part V — Going Explicit: The D3D12 Object Model

*[« Part IV](./part-04-maturity-generality.md) · [Syllabus](./D3D-Evolution-Course.md) · Next: [Part VI »](./part-06-feeding-the-gpu.md)*

---

Everything so far has followed one arc: give the programmer more control over
*what the GPU computes*. Direct3D 12 (2015, Windows 10) changes the subject
entirely. The pipeline barely changes. What changes is **who does the
bookkeeping**. For twenty years the driver tracked resource states, synchronized
the CPU and GPU, validated your calls, and managed memory *for you*. D3D12 hands
all of that back. This Part explains why anyone would want that, and introduces
the object model through our Tier 1 samples.

Anchoring samples: **`D3D12\Tier1\`** — `D3D12.Fundamentals` (the template),
`D3D12.Textures`, `D3D12.VertexShader`, `D3D12.PixelShader`, `D3D12.Nuklear`.

---

## Module 6 — Why D3D12 Exists (the case for explicitness)

### The problem it solved

By 2015 the bottleneck in high-end rendering was no longer the GPU — it was the
**CPU-side driver**. Every D3D11 draw call ran through driver code that validated
arguments, tracked which resources were bound where, decided when to allocate or
rename memory, and inserted synchronization. On a scene with tens of thousands of
draws, that overhead dominated, and it was **single-threaded and unpredictable**
(the driver might decide to compile a shader or allocate memory mid-frame,
causing a hitch).

D3D12's thesis: *the application knows more than the driver.* You know which
resources you'll use, when you're done with them, and how your frames are
structured. So D3D12 removes the driver's guesswork and makes you state your
intentions explicitly. In return you get **lower, more predictable overhead** and
**true multithreaded submission**.

### The four things you now own

1. **Resource state** — you transition resources between states with **barriers**
   (the driver no longer tracks it).
2. **CPU/GPU synchronization** — you signal and wait on **fences** (no automatic
   sync).
3. **Memory** — you (can) place resources in heaps you allocate.
4. **Pipeline configuration** — you bake almost all render state into an immutable
   **pipeline state object** up front, instead of setting it piecemeal per draw.

### When to use D3D12 (and when *not* to)

**Use D3D12 when:** you're CPU-bound on draw-call submission; you need
multithreaded command recording; you want predictable frame times (no driver
hitches); you need features only D3D12 exposes (mesh shaders, DXR raytracing,
work graphs); or you're building an engine that will amortize the complexity over
many titles.

**Do *not* use D3D12 when:** you're GPU-bound (D3D12 won't make the GPU faster);
your scene is small (a few hundred draws — D3D11's overhead is irrelevant); or
you're a small team optimizing for time-to-ship over last-10%-performance. D3D11
remains an excellent, fully modern choice. **Explicitness is a cost you pay to
buy CPU efficiency and control — only worthwhile if you need what it buys.**

> **Decision lens — D3D11 vs. D3D12.** The honest default is *D3D11 unless you
> can name the specific thing D3D12 gives you.* "It's newer" is not a reason.
> "We're CPU-bound at 12,000 draw calls and need to record them on 8 threads" is.

---

## Module 7 — Tier 1: The D3D12 Skeleton

Every D3D12 program shares a skeleton. `D3D12.Fundamentals` is our reference
implementation of it (written in C with `COBJMACROS`, so calls read
`ID3D12X_Method(obj, ...)`). Learn this skeleton once and every other D3D12
sample becomes readable.

### Queues, allocators, and command lists

D3D11's single "context" splits into three D3D12 objects:

- **Command queue** — the GPU's actual work intake. You `ExecuteCommandLists`
  onto it.
- **Command allocator** — the backing memory that commands are recorded into.
- **Command list** — the recording surface you write commands to, then `Close`
  and submit.

```c
ID3D12Device_CreateCommandQueue(device, &qd, &IID_ID3D12CommandQueue, (void**)&command_queue);
// ... record into command_list ...
static void execute_commands(void) {
    ID3D12GraphicsCommandList_Close(command_list);
    ID3D12CommandList *lists[1] = { (ID3D12CommandList*)command_list };
    ID3D12CommandQueue_ExecuteCommandLists(command_queue, 1, lists);
    signal_and_wait();                                   // see below
    ID3D12CommandAllocator_Reset(command_allocator);     // reuse the memory
    ID3D12GraphicsCommandList_Reset(command_list, command_allocator, NULL);
}
```

**Why three objects?** Because the split is what enables threading and reuse. Many
threads each record into their *own* allocator/list, then one queue consumes them
all. Our teaching sample stays single-threaded for clarity, but the object model
is the multithreaded one.

### Fences: you synchronize now

There is no automatic "wait until the GPU is done." You do it yourself with a
**fence** — a shared counter the GPU increments and the CPU polls:

```c
static void signal_and_wait(void) {
    ID3D12CommandQueue_Signal(command_queue, queue_fence, ++fence_value);   // GPU will set this
    while (ID3D12Fence_GetCompletedValue(queue_fence) != fence_value)
        SwitchToThread();                                                   // CPU waits
}
```

This busy-wait-per-frame is deliberately the *simplest correct* thing (a real
engine waits on an event and pipelines 2–3 frames deep instead of stalling). The
lesson: **CPU/GPU synchronization is now application code.** Get it wrong and you
either corrupt data (CPU races ahead) or destroy performance (CPU stalls too
often).

### Resource barriers: you track state now

The single most characteristic D3D12 code is the **resource barrier**. Each frame
the back buffer must move from `PRESENT` to `RENDER_TARGET` to draw, then back:

```c
barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
barrier.Transition.pResource   = rtv_buffers[rtv_index];
barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barrier);
// ... draw ...
barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barrier);
```

Remember D3D11's compute sample, where the runtime unbound the UAV and inserted
sync *for you*? This is that same hazard tracking — now handed to you. Barriers
are also where you tell the GPU to flush caches and wait for prior work, so
getting them wrong causes flickering, corruption, or GPU hangs. (The Tier-1 UI
even counts barriers per frame to make the cost visible.)

### PSOs and root signatures: bake state up front

D3D11 set rasterizer state, blend state, shaders, and input layout as separate
calls that the driver reconciled at draw time (sometimes recompiling shaders
mid-frame — a hitch). D3D12 bakes **almost all of it** into one immutable
**pipeline state object** created ahead of time:

```c
ID3D12Device_CreateGraphicsPipelineState(device, &pd, &IID_ID3D12PipelineState, (void**)&pso);
```

The **root signature** is the separate, small declaration of *what inputs* the
shaders receive (constants, descriptor tables, samplers) — think of it as the
function signature for your pipeline. Fundamentals uses the cheapest possible
input: 16 **root constants** for the MVP matrix, no constant buffer at all:

```c
rp.ParameterType        = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
rp.Constants.Num32BitValues = 16;                 // the whole 4x4 matrix
rp.ShaderVisibility     = D3D12_SHADER_VISIBILITY_VERTEX;
// ... at draw time:
ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 16, mvp.m, 0);
```

**Why bake state?** Because compiling a full pipeline is expensive, and doing it
*predictably at load time* eliminates the mid-frame hitches D3D11 could suffer.
The cost: a combinatorial explosion of PSOs (every shader × blend × topology
combination is a separate object), which becomes a real content-pipeline concern
(addressed later by pipeline libraries, Part X).

> **Decision lens — how to feed shader data in D3D12.** Root constants for tiny,
> per-draw data (a matrix, a few floats) — cheapest, no descriptor. Root
> descriptors for a single CBV/SRV/UAV used often. Descriptor tables for
> everything else (textures, arrays of resources). Choosing wrong doesn't break
> correctness, but wastes the tiny, precious root-signature space.

### Descriptor heaps

Views (RTV, DSV, SRV, CBV) live in **descriptor heaps** — GPU-visible arrays of
resource descriptors that you allocate and index yourself:

```c
ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void**)&rtv_heap);
```

`D3D12.Textures` is the first sample to use a shader-visible SRV heap (to bind a
texture); the base cube only needs RTV/DSV heaps. This heap-and-index model is the
seed of **bindless** rendering (Part VI).

### Historical "why now?"

Multithreaded CPUs, ballooning draw-call counts, and the console world (where
fixed hardware made low-level APIs like the PlayStation's GNM hugely successful)
all pushed toward explicitness. AMD's Mantle proved developers wanted it; D3D12,
Vulkan, and Metal all landed within a couple of years carrying the same
philosophy. D3D12 is D3D11's pipeline with the driver's safety rails removed — a
trade you make consciously, feature by feature.

---

## Part V in one paragraph

D3D12 keeps the pipeline and changes the *contract*: you now own resource-state
transitions (**barriers**), CPU/GPU sync (**fences**), memory, and up-front
pipeline baking (**PSOs** + **root signatures** + **descriptor heaps**). The
payoff is lower, predictable CPU overhead and multithreaded submission; the price
is that mistakes which D3D11 quietly prevented now corrupt frames or hang the
GPU. The `D3D12.Fundamentals` skeleton — queue/allocator/list, `signal_and_wait`,
the PRESENT↔RENDER_TARGET barrier pair, root-constant MVP — is the vocabulary
every later Part builds on.

---

*Next: [Part VI — Feeding the GPU: Memory, Copies, and GPU-Driven Work »](./part-06-feeding-the-gpu.md)*
