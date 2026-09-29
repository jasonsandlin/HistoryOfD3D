# Part VIII — Recording & Querying at Scale (Tier 5)

*[« Part VII](./part-07-image-quality-modern-pipeline.md) · [Syllabus](./D3D-Evolution-Course.md) · Next: [Part IX »](./part-09-classic-pipeline-mastery.md)*

> **Build note.** The Tier 5 samples described here now **exist** under
> `D3D12\Tier5\` and build with `build-all.ps1`. The short code sketches below are
> simplified for teaching; read each sample's `main.c` for the full, annotated
> implementation.

---

D3D12's headline promise back in Part V was **lower, more predictable CPU overhead
and multithreaded submission**. Tier 1–4 mostly stayed single-threaded to keep the
teaching clear. Tier 5 finally cashes that promise: recording commands across many
threads, pre-recording reusable command sequences, and letting the GPU report back
what actually happened so you can make decisions from real data.

Anchoring samples (`D3D12\Tier5\`): `MultiThreadedRecording`, `Bundles`, `Queries`,
`Predication`.

---

## Module 11 — Command Recording & GPU Feedback

### Multithreaded recording (`MultiThreadedRecording`)

This is *the* reason the D3D12 object model splits queue / allocator / list. Give
each worker thread its **own** allocator and command list, record in parallel,
then submit all the lists on one queue:

```
thread 0: allocator0/list0  → record objects   0..N/4
thread 1: allocator1/list1  → record objects N/4..N/2
...
main: queue.ExecuteCommandLists([list0, list1, list2, list3])   // one submit
```

**When to use:** scenes with thousands of draws where a single thread can't record
fast enough to keep the GPU fed. This is the payoff that justifies D3D12's whole
complexity budget for large titles.

**When *not* to:** small scenes gain nothing — the threading overhead and the
per-thread allocator memory outweigh the benefit. And correctness is subtle: each
list needs its own allocator, and you can't reset an allocator while the GPU is
still consuming its commands (fences again).

> **Decision lens — go multithreaded when the profiler says the submit thread is
> the bottleneck.** If you're GPU-bound or draw counts are modest, single-threaded
> recording is simpler and just as fast. Threading is a scaling tool, not a
> default.

### Bundles (`Bundles`)

A **bundle** is a small, pre-recorded command list you build **once** and replay
**many** times, cheaply, across frames. Unlike a full command list, a bundle
inherits most state from the caller and is optimized for reuse:

```
// once, at load:
bundle = record( SetVB; SetIB; DrawIndexed )   // a reusable draw sequence
// every frame:
list.ExecuteBundle(bundle)                      // near-zero CPU cost to replay
```

**When to use:** repeated, unchanging draw sequences — the same mesh drawn every
frame, UI elements, static props. Bundles shave CPU recording cost off the hot
path.

**When *not* to:** anything whose commands change per frame can't be a static
bundle, and bundles have restrictions (limited state inheritance, no resource
transitions inside). For dynamic work, record normally. Bundles are a
micro-optimization that matters only when recording cost is measurable.

### Queries (`Queries`)

The GPU runs asynchronously, so "how long did that pass take?" or "how many pixels
passed the depth test?" can't be answered by CPU timing. **Query heaps** let the
GPU write those answers into a buffer you read back a frame or two later:

```
BeginQuery(timestamp/occlusion/pipeline-statistics)
... draw ...
EndQuery → ResolveQueryData → readback buffer (read next frame)
```

**When to use:** GPU profiling (timestamp queries for pass durations), **occlusion
culling** (occlusion queries: did anything pass, so should I bother drawing the
detailed version?), and pipeline-statistics for understanding vertex/pixel counts.
Real data beats guessing.

**When *not* to:** queries add synchronization and readback latency; sprinkling
them everywhere hurts. Use them deliberately for profiling or a specific culling
decision, and always read results **latently** (never stall the GPU waiting for a
query this frame).

### Predication (`Predication`)

**Predication** lets the GPU **skip** commands based on a value in a buffer —
without a CPU round-trip. Pair it with an occlusion query: draw a cheap bounding
box, and predicate the expensive draw on whether the box was visible:

```
predicate on {occlusionResult != 0}
DrawExpensiveMesh()          // GPU itself skips this if the box was occluded
```

**When to use:** GPU-driven culling where you want the *GPU* to decide whether to
execute work, avoiding the latency of reading a query back to the CPU and
re-submitting. It keeps culling decisions on-GPU and pipelined.

**When *not* to:** if the CPU already knows visibility, cull on the CPU and don't
submit the draw at all — that's strictly cheaper than submitting a predicated
draw. Predication earns its keep specifically when the deciding data lives on the
GPU.

> **Decision lens — where does the cull decision live?** CPU-side cull when the
> CPU has the data (don't submit at all). GPU-side predication/indirect when the
> visibility data is produced on the GPU and a CPU round-trip would cost more than
> it saves.

### Historical "why now?"

These aren't new *capabilities* so much as the **maturation** of D3D12's model.
Once command recording is explicit and multithreaded, bundles and multithreaded
recording are the obvious tools to exploit it; once the GPU is fully asynchronous,
queries and predication are how you observe and steer it without dragging work
back to the CPU. Tier 5 is D3D12 used the way it was designed to be used.

---

## Part VIII in one paragraph

Tier 5 delivers on D3D12's original promise: **multithreaded recording** (each
thread its own allocator/list, one submit) scales draw submission across cores;
**bundles** pre-record reusable sequences to cut hot-path CPU cost; **queries**
let the asynchronous GPU report timing, occlusion, and statistics back to you; and
**predication** keeps culling decisions on the GPU when that's where the data
lives. The unifying theme is *scale and feedback* — using the explicit model
deliberately, guided by measurement rather than assumption.

---

*Next: [Part IX — Classic Pipeline Mastery (Tier 6) »](./part-09-classic-pipeline-mastery.md)*
