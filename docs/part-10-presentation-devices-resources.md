# Part X — Presentation, Devices & Advanced Resources (Tiers 7–8)

*[« Part IX](./part-09-classic-pipeline-mastery.md) · [Syllabus](./D3D-Evolution-Course.md) · Next: [Part XI »](./part-11-the-frontier.md)*

> **Build note.** The Tier 7 and Tier 8 samples described here now **exist** under
> `D3D12\Tier7\` and `D3D12\Tier8\` and build with `build-all.ps1`. Code sketches
> below are simplified for teaching; read each sample's `main.c` for the full
> implementation and its capability-fallback paths.

---

The final two "classic-D3D12" tiers deal with the edges of the system: how frames
reach a real (possibly HDR, possibly multi-GPU) display, how to manage resources
too big to fit in memory, and how to keep the whole explicit machine *correct*
during development. These are the topics that separate a sample from a shippable
product.

Anchoring samples: **Tier 7** (`D3D12\Tier7\`) — `HDR`, `MultiAdapter`; **Tier 8**
(`D3D12\Tier8\`) — `PipelineLibraries`, `ReservedResources`, `Debugging`.

---

## Module 13 — Tier 7: Getting Frames to the Display

### HDR output (`HDR`)

Standard rendering targets an 8-bit SDR display. **HDR** output presents in a
wide-gamut, high-dynamic-range format (e.g. `R10G10B10A2` with the HDR10 color
space, or `R16G16B16A16_FLOAT` with scRGB) so bright highlights and deep colors
survive to a capable monitor:

```
swapChain color space = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020   // HDR10
render in linear → tone-map to the display's luminance range → present
```

**When to use:** when you want your carefully lit, physically-based scene to look
right on HDR TVs and monitors — the bright sun, glinting metal, and dark shadows
that SDR crushes. Requires querying the display's actual capabilities and
tone-mapping to them.

**When *not* to:** HDR adds real pipeline and testing complexity (you must handle
SDR *and* HDR paths, and tone-mapping is easy to get wrong). If your art is
authored for SDR and your audience is mostly SDR, it may not be worth it. HDR is a
quality investment, not a checkbox.

### Multi-adapter (`MultiAdapter`)

Some systems have more than one GPU — an integrated + discrete pair, or multiple
discrete cards. D3D12 exposes each **adapter** explicitly and lets you split work
across them:

```
adapter0 (discrete): render the scene
adapter1 (integrated): run post-processing on the previous frame
cross-adapter shared resources hand data between them
```

**When to use:** rare, specialized cases — offloading post to the iGPU,
professional multi-GPU rendering, or explicit alternate-frame setups. When it
fits, it's free hardware.

**When *not* to:** for almost all games. Cross-adapter synchronization and data
transfer are complex and fragile, driver support varies, and the classic SLI/CF
alternate-frame model has largely faded. Treat multi-adapter as a niche tool, not
a scaling strategy.

> **Decision lens — presentation & devices.** Add HDR when your lighting is
> HDR-authored and the audience has the displays. Reach for multi-adapter only
> for a concrete, measured offload; otherwise target one adapter and keep the
> engine simple.

---

## Module 14 — Tier 8: Big Resources & Staying Correct

### Pipeline libraries (`PipelineLibraries`)

Recall the PSO explosion from Part V: every shader × state combination is a
separate object, and each is expensive to compile. **Pipeline libraries** let you
**cache** compiled PSOs to disk and reload them next run, skipping recompilation:

```
first run:  create PSOs → library.StorePipeline(...) → write library blob to disk
next run:   load blob → library.LoadGraphicsPipeline(...)   // no recompile, no hitch
```

**When to use:** any real game with many PSOs and long shader-compile times.
Caching turns a multi-second (or multi-minute) first-run compile into an instant
warm start, and helps eliminate the in-game hitches of compiling PSOs on demand.

**When *not* to:** a tiny app with two PSOs doesn't need the machinery. Also, the
cache is driver/hardware-specific and must be invalidated when drivers change —
manage it, don't blindly trust it.

### Reserved (tiled) resources (`ReservedResources`)

A **reserved** resource has a virtual address range but **no physical memory** until
you map tiles into it on demand — the D3D12 mechanism behind megatextures and
sparse virtual texturing:

```
create a 16K×16K reserved texture (virtually huge, physically empty)
map only the tiles the camera can currently see → physical memory
unmap tiles that scroll out of view
```

**When to use:** resources far larger than VRAM where only a fraction is needed at
once — virtual texturing, sparse voxel volumes, streaming terrain. It's the way to
have a "16K texture" without paying 1 GB.

**When *not* to:** ordinary textures that fit in memory — reserved resources add
significant management complexity (tile pools, mapping, residency) for no benefit
unless you're genuinely memory-constrained at huge scale.

> **Decision lens — resource residency.** Committed/placed for things that fit.
> Reserved/tiled only when the resource is bigger than memory and naturally
> sparse. The complexity is only worth it at extreme scale.

### Debugging (`Debugging`)

D3D12 took away the driver's safety net (Part V), so **you** must catch the errors
it used to prevent. The tooling that makes explicit D3D12 survivable:

- the **debug layer** (`ID3D12Debug`) — validates every call, catches bad barriers,
  wrong states, and resource misuse;
- **GPU-based validation** — deeper checks that run on the GPU;
- **DRED** (Device Removed Extended Data) — post-mortem info when the GPU hangs or
  is removed;
- **PIX** markers — named regions for the profiler/debugger.
- **DirectX dump files** *(preview)* — full GPU crash dumps, see below.

```c
ID3D12Debug_EnableDebugLayer(debug);   // ALWAYS on in development builds
```

**When to use:** *always*, during development. The debug layer will tell you
exactly which barrier is wrong or which resource is in the wrong state — the class
of bug that otherwise shows up as random corruption or a hang on someone else's
GPU.

**When *not* to:** ship with it off (it's slow). But developing D3D12 *without* the
debug layer is choosing to debug blind — don't.

> **Decision lens — this one's not a trade-off.** Enable the debug layer and
> validation in every development build. The only "when not to" is the shipping
> build, for performance. Explicit APIs make validation tooling essential, not
> optional.

#### What's next after DRED: DirectX dump files (preview)

DRED tells you *roughly* where the GPU was (the last breadcrumb it passed, the
page fault address). Console developers have long had more: when the GPU crashes,
the devkit writes a **GPU crash dump** — the in-flight commands, shader state and
resources — that you open later, like a CPU minidump. **DirectX dump files** bring
that to Windows. The runtime and driver write a dump when the device is removed.
The app configures how much the driver captures (no, medium or high overhead;
shader registers, resources, event markers), attaches its own blobs (build IDs,
the frame's scene description) and asks for the dump to be retained.

```cpp
// Agility SDK 1.721-preview, on ID3D12DevicePreview
D3D12_FEATURE_DATA_DUMP_FILE caps = {};    // SupportedByOS, DumpFileDriverTier, DumpFileDriverOptionsMask
device->CheckFeatureSupport(D3D12_FEATURE_DUMP_FILE, &caps, sizeof(caps));
devicePreview->ConfigureDumpFile(D3D12_DUMP_FILE_DRIVER_OPTION_MEDIUM_OVERHEAD);
devicePreview->AddBlobToDumpFile(/* your own crash context */);
```

The honest status: the API and file format (`DXDumpFileFormat.h`) ship in the
1.721-preview headers, but Microsoft says there's "no way to use it just yet". So
the `Debugging` sample's panel explains the model and probes for the capability,
and shows "not available" on today's retail runtime. This is the feature to watch
for anyone who has had to debug a TDR from a user's bug report.

### Historical "why now?"

Tiers 7–8 are the **production-hardening** layer: the features you need once a
D3D12 renderer stops being a demo and has to run on real users' varied displays
and GPUs, load quickly, stream huge assets, and be debuggable when it breaks. They
close the loop on explicitness — having taken the driver's help away, D3D12 gives
you the *tools* (pipeline caches, validation, DRED) to live without it.

---

## Part X in one paragraph

Tier 7 gets frames onto real hardware: **HDR** presentation for wide-gamut
displays and **multi-adapter** for the rare multi-GPU offload. Tier 8 hardens the
engine: **pipeline libraries** cache compiled PSOs to kill first-run compile
hitches, **reserved/tiled resources** give virtually-huge sparse textures without
the memory, and the **debug layer + validation + DRED** replace the safety net
D3D12 removed. Together they mark the transition from "it renders" to "it ships" —
and they exist precisely because explicitness demanded them.

---

*Next: [Part XI — The Frontier: Where Direct3D Is Heading »](./part-11-the-frontier.md)*
