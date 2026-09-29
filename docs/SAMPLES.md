# Sample Reference

Supporting reference for the sample set behind
**[The Evolution of Direct3D](./D3D-Evolution-Course.md)** course. The course is
the main text; this file is the catalog of the programs it teaches from — the
build matrix, the per-sample tables, and the build/runtime mechanics.

Every sample is a single-file Win32 app that spins a colored cube. Press **ESC**
(or close the window) to quit any of them.

---

## Build status (Windows SDK 10.0.26100, VS 2022/x64)

| Version | Pipeline            | Builds | Notes |
|---------|---------------------|:------:|-------|
| D3D7    | DirectDraw7 + D3D7, fixed-function | ✅ | Windowed via a DirectDraw clipper + back-buffer blit. |
| D3D8    | IDirect3D8, fixed-function          | ✅ | SDK dropped `d3d8.h`; uses **vendored** Wine headers (see below). |
| D3D9    | IDirect3D9, fixed-function          | ✅ | `d3d9.h` ships in the SDK `shared/` folder. |
| D3D10   | Device + swap chain, HLSL           | ✅ | Shaders compiled at runtime with `D3DCompile`. |
| D3D11   | Device/context + swap chain, HLSL   | ✅ | |
| D3D12   | Queue + swap chain, root constants  | ✅ | Minimal single-threaded fence sync; MVP via 32-bit root constants. |

All six **compile and link** with the stock Visual Studio 2022 toolchain and the
Windows 10/11 SDK — no legacy DirectX SDK required. Matrices come from
`DirectXMath` (`DirectXMath.h`), which every SDK ships.

## Shader showcase samples (D3D9–D3D12)

Alongside the six base games there are focused samples that each highlight **one
programmable shader stage** while keeping the rotating-cube theme. Every sample is
a single-file Win32 app that compiles its HLSL at runtime; folders are named
`<Version>.<ShaderStage>`.

**Organization.** Each base cube now lives in a `<Version>.Basic` folder (e.g.
`D3D7.Basic`, `D3D12\Tier1\D3D12.Basic`). The D3D9/D3D10/D3D11 base cubes and their
shader-stage samples are grouped under per-version parent folders — `D3D9\`,
`D3D10\`, `D3D11\` — while D3D12 is organized by tier under `D3D12\`. Build scripts
discover samples at any depth, so the folder nesting doesn't change how you build.

| Folder | Stage | What it shows |
|--------|-------|---------------|
| `D3D9.VertexShader`  | `vs_3_0` | Vertex shader breathes/deforms the cube. |
| `D3D9.PixelShader`   | `ps_3_0` | Pixel shader paints an animated procedural color. |
| `D3D10.VertexShader` | `vs_4_0` | Vertex shader twists the cube. |
| `D3D10.PixelShader`  | `ps_4_0` | Pixel shader procedural color. |
| `D3D10.GeometryShader` | `gs_4_0` | Geometry shader explodes triangles along face normals. |
| `D3D11.VertexShader` | `vs_5_0` | Vertex shader twist. |
| `D3D11.PixelShader`  | `ps_5_0` | Pixel shader procedural color. |
| `D3D11.GeometryShader` | `gs_5_0` | Geometry shader exploding cube. |
| `D3D11.ComputeShader` | `cs_5_0` | Compute shader writes an animated texture (UAV) the cube then samples. |
| `D3D11.Tessellation` | `hs_5_0` + `ds_5_0` | Hull/domain shaders subdivide the cube and bulge it toward a sphere (wireframe). |
| `D3D12.VertexShader` | `vs_5_0` | Vertex shader twist (root constants). |
| `D3D12.PixelShader`  | `ps_5_0` | Pixel shader procedural color. |
| `D3D12.GeometryShader` | `gs_5_0` | Geometry shader exploding cube. |
| `D3D12.ComputeShader` | `cs_5_0` | Compute PSO writes a UAV texture, graphics PSO samples it via a descriptor table. |
| `D3D12.Tessellation` | `hs_5_0` + `ds_5_0` | Tessellation PSO subdivides + bulges the cube (wireframe). |
| `D3D12.MeshShader` | `ms_6_5` | Mesh shader generates the whole cube on the GPU — no vertex/index buffers or input assembler. |
| `D3D12.AmplificationShader` | `as_6_5` + `ms_6_5` | Amplification shader turns one dispatch into a ring of orbiting cubes, each emitted by a mesh group. |
| `D3D12.RayTracing` | `lib_6_3` (raygen + miss + closest-hit) | Full DXR pipeline: a BLAS/TLAS cube ray-traced per pixel; the TLAS instance is rotated each frame. |

Tessellation is shown as a single combined **hull + domain** sample per API. All 18
samples build with the stock toolchain and their embedded HLSL was validated with
`fxc.exe` (SM 4/5) or `dxc.exe` (SM 6). Build any of them the same way as the base
games, e.g. `.\build-all.ps1 -Only D3D11.ComputeShader`.

### Shader Model 6 samples (mesh / amplification / ray tracing)

`D3D12.MeshShader`, `D3D12.AmplificationShader`, and `D3D12.RayTracing` use
**Shader Model 6.3/6.5**, which the legacy `D3DCompile`/`fxc` path cannot target.
They compile their HLSL at runtime with **DXC** by loading `dxcompiler.dll` (and
`dxil.dll` for signing) — the app searches the Windows SDK `bin\<ver>\x64` folders,
so no extra setup is needed on a machine with the SDK installed. Amplification
shaders only exist to feed a mesh shader, so that sample combines **AS + MS**; ray
tracing's raygen, miss, and closest-hit stages are all part of one pipeline, so it
is a single combined sample. These three need a **mesh-shader / DXR-capable GPU**
(or a recent WARP) at runtime and show a clean "init failed" dialog otherwise; they
still build everywhere.

## GUI sample (Nuklear)

`D3D12.Nuklear` shows an immediate-mode GUI rendered on a Direct3D 12 backend
using **[Nuklear](https://github.com/Immediate-Mode-UI/Nuklear)** (public-domain,
single-header). It renders Nuklear's full widget *overview* — buttons, sliders,
checkboxes, radio buttons, combo boxes, a color picker, tree/tab views, charts,
text edit, property widgets, groups, and popups — plus a small control window.

Vendored from the Nuklear repo into `third_party/nuklear/`: `nuklear.h`, the
D3D12 backend `nuklear_d3d12.h` with its precompiled shader headers
(`nuklear_d3d12_{vertex,pixel}_shader.h`). The `overview.c` widget demo lives in
the sample folder. The backend uses C-style COM (`COBJMACROS`), so this sample
is written in **C** (`main.c`) and `build-all.ps1` compiles it as C (`/Tc`). It
links `d3d12.lib`, `dxgi.lib`, and `dxguid.lib` via `#pragma comment`, and
requires **Windows SDK >= 10.0.22000.0** (a lower version has a known bug that
crashes the backend). Build it like any other sample:
`.\build-all.ps1 -Only D3D12.Nuklear`.

## Learning D3D12 (annotated samples with a Nuklear UI)

A growing set of teaching samples, each a single self-contained `main.c` built
on the shared Nuklear GUI backend (`third_party/nuklear/`). Each renders a
rotating cube plus a Nuklear panel that both explains and shows the live state
of the D3D12 feature it focuses on, with interactive controls. They are written
in C (the Nuklear backend requires C-style COM) and compute matrices with a tiny
local row-vector helper (DirectXMath is C++-only).

Complex samples log errors to a `Cube<Sample>.log` file (written next to the
working directory) instead of popping message-box dialogs, so failures are easy
to read after the fact.

**Directory layout.** All D3D12 samples now live under `RotatingCubes\D3D12\`,
grouped entirely by tier in `D3D12\Tier1\ .. D3D12\Tier9\` — `Tier9` being the
Agility-SDK / preview "frontier" tier — including the base
cube, the shader-showcase samples, the Shader Model 6 samples, and the Nuklear
GUI sample (each is placed in the tier that matches its learning level):

```
RotatingCubes\D3D12\
  Tier1\  D3D12.Basic, D3D12.VertexShader, D3D12.PixelShader, D3D12.Nuklear,
          D3D12.Fundamentals, D3D12.Textures, D3D12.TripleBuffering,
          D3D12.ConstantBuffers, D3D12.RootSignatures
  Tier2\  D3D12.GeometryShader, D3D12.Tessellation,
          D3D12.PlacedResources, D3D12.CopyQueue, D3D12.IndirectDraw
  Tier3\  D3D12.ComputeShader, D3D12.ComputeAsync, D3D12.Bindless, D3D12.DeferredMRT
  Tier4\  D3D12.MeshShader, D3D12.AmplificationShader, D3D12.RayTracing,
          D3D12.ShadowMap, D3D12.MSAA, D3D12.VariableRateShading
  Tier5\  D3D12.MultiThreadedRecording, D3D12.Bundles, D3D12.Queries, D3D12.Predication
  Tier6\  D3D12.Instancing, D3D12.Blending, D3D12.Stencil
  Tier7\  D3D12.HDR, D3D12.MultiAdapter
  Tier8\  D3D12.PipelineLibraries, D3D12.ReservedResources, D3D12.Debugging
  Tier9\  D3D12.SamplerFeedback, D3D12.DynamicResources, D3D12.WorkGraphs,
          D3D12.EnhancedBarriers, D3D12.DirectStorage, D3D12.LinearAlgebra,
          D3D12.RayTracing12, D3D12.PartialPrograms
```

`build-all.ps1` discovers samples at any depth, so `-Only <FolderName>` still
works regardless of nesting (e.g. `-Only D3D12.Textures`). The shader-showcase,
Shader Model 6, and Nuklear samples are described in their own sections above;
the tables below cover the annotated learning samples.

**Tier 1 — fundamentals, resources & pipeline basics**

Also includes the base fixed-function cube (`D3D12.Basic`), `D3D12.VertexShader`,
`D3D12.PixelShader`, and the `D3D12.Nuklear` GUI baseline.

| Sample | Teaches |
|---|---|
| `D3D12.Fundamentals` | The core objects: device, command queue/allocator/list, swap chain, RTV/DSV descriptor heaps, resource barriers, the fence sync model, root signature, and PSO. Live panel shows the fence value (CPU vs GPU-completed), back-buffer index, and barrier count each frame. Controls: pause, speed, wireframe (second PSO), vsync, clear color. |
| `D3D12.Textures` | The full texture-upload path: a DEFAULT-heap texture staged from an UPLOAD buffer via `GetCopyableFootprints` + `CopyTextureRegion`, the `COPY_DEST -> PIXEL_SHADER_RESOURCE` barrier, an SRV in a shader-visible heap bound through a root descriptor table, and static **point (s0)** / **linear (s1)** samplers. |
| `D3D12.TripleBuffering` | Multiple frames in flight: N back buffers + per-frame command allocators + a fence value per frame, so the CPU stops blocking on every Present and instead only waits when it laps the GPU. |
| `D3D12.ConstantBuffers` | An UPLOAD constant buffer bound via a CBV descriptor table (vs. root constants), 256-byte CBV alignment, and per-frame CB regions to avoid overwriting data the GPU is still reading. |
| `D3D12.RootSignatures` | The three root-parameter kinds side by side — root constants, root descriptors (CBV), and descriptor tables — plus how root-signature layout affects binding cost. |

**Tier 2 — memory, submission & extra geometry stages**

Also includes `D3D12.GeometryShader` and `D3D12.Tessellation` (the optional
geometry-pipeline stages), described in the shader-showcase section.

| Sample | Teaches |
|---|---|
| `D3D12.PlacedResources` | Explicit heaps + `CreatePlacedResource`: sub-allocating several resources from one heap, resource-heap tiers, and aliasing considerations. |
| `D3D12.CopyQueue` | A dedicated COPY queue uploading data in parallel with rendering, with a cross-queue fence so the graphics queue waits for the copy to finish. |
| `D3D12.IndirectDraw` | `ExecuteIndirect` with a command signature: draw arguments sourced from a GPU buffer so the GPU drives draw counts/parameters without CPU round-trips. |

**Tier 3 — GPU work & multi-pass**

Also includes `D3D12.ComputeShader` (a graphics + compute PSO pair), described
in the shader-showcase section.

| Sample | Teaches |
|---|---|
| `D3D12.ComputeAsync` | An async COMPUTE queue running a compute shader (RWStructuredBuffer via a root UAV) that animates the cube vertices; the graphics queue `Wait()`s on the compute fence before drawing. |
| `D3D12.Bindless` | SM5.1 dynamic indexing: an unbounded descriptor-table range (`Texture2D g_tex[]`) with a per-draw root-constant index selecting one of many textures — no re-binding between draws. |
| `D3D12.DeferredMRT` | A G-buffer with Multiple Render Targets (albedo + normal in one pass), then a fullscreen-triangle lighting pass that samples them. Combo view of lit/albedo/normal. |

**Tier 4 — advanced rendering & the modern pipeline**

Also includes the Shader Model 6 samples `D3D12.MeshShader`,
`D3D12.AmplificationShader`, and `D3D12.RayTracing`, described in the Shader
Model 6 section.

| Sample | Teaches |
|---|---|
| `D3D12.ShadowMap` | Two-pass shadow mapping: render depth from the light's POV into an `R32_TYPELESS` shadow map (D32 DSV + R32 SRV), then sample it with a `SamplerComparisonState` / `SampleCmpLevelZero`. Depth-bias slider. |
| `D3D12.MSAA` | A multisampled render target (2x/4x/8x, capability-gated via `CheckFeatureSupport`) resolved into the back buffer with `ResolveSubresource`. Live sample-count selector. |
| `D3D12.VariableRateShading` | Per-draw VRS (`RSSetShadingRate` on `ID3D12GraphicsCommandList5`), capability-gated on `D3D12_FEATURE_D3D12_OPTIONS6`. A fine procedural pattern makes coarse rates (2x2, 4x4) visibly blocky. |

**Tier 5 — recording & querying at scale**

| Sample | Teaches |
|---|---|
| `D3D12.MultiThreadedRecording` | Parallel command-list recording: each worker thread records a slice of a cube grid into its own allocator + list, coordinated with Win32 events, then all lists are submitted in order with one `ExecuteCommandLists`. The concrete payoff of the queue/allocator/list split. |
| `D3D12.Bundles` | A `D3D12_COMMAND_LIST_TYPE_BUNDLE` recorded once at init and replayed each frame with `ExecuteBundle`; the direct list sets the inherited root signature + root constants. Toggle between bundle replay and inline re-recording. |
| `D3D12.Queries` | Timestamp queries (GPU pass duration via `GetTimestampFrequency`) and pipeline-statistics queries (IA vertices, VS/PS invocations) resolved to a readback buffer and shown live. |
| `D3D12.Predication` | Occlusion-query-driven predication: a `BINARY_OCCLUSION` query resolves into a predicate buffer, and `SetPredication(..., EQUAL_ZERO)` makes the GPU skip the expensive draw when fully occluded — no CPU round-trip. |

**Tier 6 — classic pipeline mastery**

| Sample | Teaches |
|---|---|
| `D3D12.Instancing` | `DrawIndexedInstanced` with a per-instance vertex slot (`PER_INSTANCE_DATA`, offset + tint), rendering a whole grid of cubes in a single draw call. Instance-count slider. |
| `D3D12.Blending` | Alpha vs. additive blend PSOs, back-to-front sorting (toggle to see sorting artifacts), and depth-write off for transparents. Shows why transparency order matters. |
| `D3D12.Stencil` | A two-pass stencil mask (`D24_UNORM_S8_UINT`): pass 1 writes stencil in a mask shape, pass 2 draws the cube only where `StencilFunc == EQUAL` — the mirror/portal/decal building block. |

**Tier 7 — presentation & devices**

| Sample | Teaches |
|---|---|
| `D3D12.HDR` | An HDR10 swap chain (`R10G10B10A2` + `ST2084` color space via `IDXGISwapChain3::SetColorSpace1`), `CheckColorSpaceSupport` negotiation, `IDXGIOutput6` luminance query, and a paper-white/brightness control. Graceful SDR fallback. |
| `D3D12.MultiAdapter` | Explicit adapter enumeration (`IDXGIFactory6::EnumAdapterByGpuPreference` with fallback), listing each adapter's VRAM/vendor/flags, and selecting the high-performance GPU to render on. Explains cross-adapter shared resources. |

**Tier 8 — big resources & staying correct**

| Sample | Teaches |
|---|---|
| `D3D12.PipelineLibraries` | Caching compiled PSOs to disk with `ID3D12PipelineLibrary` (Load/Store + Serialize to a `.cache` file); logs cold-create vs cached-load timing so you can see the first-run-compiles-then-caches behavior. |
| `D3D12.ReservedResources` | Reserved (tiled/sparse) textures: `CreateReservedResource` + `UpdateTileMappings` binding virtual tiles to a small heap, capability-gated on `TiledResourcesTier` with a committed-resource fallback. |
| `D3D12.Debugging` | The validation tooling that replaces the driver's safety net: debug layer, GPU-based validation, DRED, live `ID3D12InfoQueue` message drain, PIX markers (encoded by hand, no `pix3.h`), and a button that deliberately triggers a benign validation warning. A **DirectX Dump Files (preview)** panel explains console-style GPU crash dumps and probes `D3D12_FEATURE_DUMP_FILE` / `ID3D12DevicePreview`, reporting "not available" on the retail runtime (the API ships in Agility SDK 1.721-preview but isn't usable yet). |

**Tier 9 (Frontier) — the Agility-SDK edge (DirectX 12's 10-year milestones)**

These map to the features celebrated in *"Celebrating 10 Years of DirectX 12."* Each queries its capability tier at runtime and falls back gracefully (still renders a cube) when the GPU/driver lacks support, logging the reason to a `.log` file.

| Sample | Teaches |
|---|---|
| `D3D12.SamplerFeedback` | DirectX 12 Ultimate Sampler Feedback: a paired feedback map (`CreateSamplerFeedbackUnorderedAccessView`, `DXGI_FORMAT_SAMPLER_FEEDBACK_*`) records *which* texture mips a shader actually sampled (via `WriteSamplerFeedback` in SM6.5) — the basis for texture streaming and texture-space shading. Gated on `SamplerFeedbackTier`. |
| `D3D12.DynamicResources` | Shader Model 6.6 dynamic resources / bindless: `HEAP_DIRECTLY_INDEXED` root flags let HLSL index `ResourceDescriptorHeap[]` / `SamplerDescriptorHeap[]` directly with no descriptor tables in the root signature. Gated on shader model + resource-binding tier. |
| `D3D12.WorkGraphs` | GPU-driven Work Graphs (2024): an executable state object of shader nodes launched with `DispatchGraph` (`ID3D12GraphicsCommandList10`) so the GPU spawns its own follow-on work. Gated on `WorkGraphsTier`. |
| `D3D12.EnhancedBarriers` | The modern barrier model: `ID3D12GraphicsCommandList7::Barrier` with `D3D12_TEXTURE_BARRIER` groups splitting SYNC / ACCESS / LAYOUT for precise, less over-synchronizing transitions. Gated on `EnhancedBarriersSupported`, with a legacy `ResourceBarrier` fallback. |
| `D3D12.DirectStorage` | Fast asset streaming (2022): `IDStorageFactory` → queue → request → submit → fence, loading bytes straight from disk into a D3D12 resource. Uses the out-of-band `Microsoft.Direct3D.DirectStorage` NuGet SDK (vendored locally, git-ignored). |
| `D3D12.RayTracing12` | **DXR 1.2 / Shader Model 6.9.** Opacity micromaps cut a different hole into each cube face: 12 OC1 4-state OMMs in an OMM array built with `BuildRaytracingAccelerationStructure`, linked to the BLAS triangles, pipeline flag `ALLOW_OPACITY_MICROMAPS`. `O` cycles OMM 2-state (0 any-hit calls) / OMM 4-state (~4K) / any-hit alpha test (~162K) / opaque. **Shader Execution Reordering**: `dx::HitObject::TraceRay` → `dx::MaybeReorderThread(hit, faceHint, 3)` → `HitObject::Invoke`, toggled with `S`, with the `OPTIONS22.ShaderExecutionReorderingActuallyReorders` cap and GPU timestamps in the title. Hardware → preview WARP → DXR 1.0 fallback. |
| `D3D12.LinearAlgebra` | **SM6.10 preview.** GPU-accelerated matrix math for *neural rendering* (`#include <dx/linalg.h>`, `cs_6_10`): a thread-scope F16 matrix × **SM6.9 long-vector** `Multiply` (inference) plus **VectorAccumulate** (`InterlockedAccumulate`), which sums 256 threads' gradients atomically (training). Both results are checked against the CPU and charted on the cube's faces. Gated on `D3D12_FEATURE_LINEAR_ALGEBRA_SUPPORT`. Needs the **preview** Agility SDK (1.721.2-preview) + preview DXC + Developer Mode, all vendored by `fetch-deps.ps1`. Runs on hardware when the driver exposes LinAlg, otherwise on the vendored **preview WARP** (`--hw` / `--warp` override). |
| `D3D12.PartialPrograms` | **1.721 preview.** Partial graphics programs: one VS and one PS compiled once as pre-rasterization and pixel-shader partials in a state-object **collection**, then late-linked (blend / rasterizer / depth) into five generic programs with `AddToStateObject`. It draws opaque, additive and alpha cubes with `SetProgram`; `P` swaps in equivalent classic PSOs, and the title compares compile, link and first-draw times. The basis for Advanced Shader Delivery. Needs `D3D12StateObjectsExperiment`; falls back to preview WARP, then to a classic PSO. |

## Building

**Option A — build everything (recommended):**

```powershell
powershell -ExecutionPolicy Bypass -File .\build-all.ps1
# or a single one:
powershell -ExecutionPolicy Bypass -File .\build-all.ps1 -Only D3D11.Basic
```

`build-all.ps1` imports the VC x64 environment and invokes `cl.exe` directly.
It writes each sample's `Cube<Version>.exe` and all intermediates into a
per-sample `Out\` subfolder (e.g. `D3D11\D3D11.Basic\Out\CubeD3D11.Basic.exe`, or a nested
`D3D12\Tier1\D3D12.Textures\Out\CubeD3D12.Textures.exe`), which is `.gitignore`d
so build products never clutter the tree. Samples are auto-discovered at any
depth, so `-Only <FolderName>` works no matter how a sample is nested.

**Option B — per project:**

```bat
cd D3D11\D3D11.Basic
build.bat
```

`build.bat` locates Visual Studio with `vswhere`, calls `vcvars64.bat`, then
`cl`. The output is `Cube<Version>.exe` in that folder.

> Tip: `%ProgramFiles(x86)%` contains parentheses that break `for /f (...)`
> parsing, so `build.bat` copies it to a temp variable first. If you ever see a
> build compiling unrelated files, another process on the machine is hooking
> `cl.exe`; `build-all.ps1` avoids that by calling `cl.exe` by full path.

## Runtime notes

- **D3D7 / D3D8** need the legacy Direct3D runtimes, which modern Windows ships
  **only as 32-bit DLLs** (`SysWOW64\d3dim700.dll`, `SysWOW64\d3d8.dll`). The
  default x64 build therefore shows a clean "init failed" dialog; build an **x86**
  copy (`vcvars32.bat`, then `cl /EHsc /I. main.cpp user32.lib`) and it runs.
  `tools\capture-screenshots.ps1` does this automatically (into `Out\x86\`).
- **D3D10–D3D12** run on the WARP software rasterizer if no GPU driver is
  available, but need a desktop session (they won't render over a headless
  service).

## D3D8 vendored headers

`D3D8.Basic/d3d8.h`, `d3d8types.h`, and `d3d8caps.h` are the MinGW/Wine reimplementation
(LGPL, © Jason Edmeades / the Wine project), included because the modern Windows
SDK no longer ships D3D8 headers. `main.cpp` adds two small MSVC compat shims
(`__MSABI_LONG`, `WINBOOL`) and resolves `Direct3DCreate8` at runtime via
`LoadLibrary("d3d8.dll")`, so no D3D8 import library is needed.
