// D3D12 Ray Tracing 1.2 (DXR 1.2 / Shader Model 6.9) teaching sample.
// Evolves the Tier4 D3D12.RayTracing cube (BLAS/TLAS, raygen+miss+closesthit, TLAS instance rotated
// every frame) with the two headline DXR 1.2 features:
//   * Opacity Micromaps (OMM): every cube face gets a cut-out (circle / diamond / four holes). The
//     cut-out is baked into an OMM array (4 opacity states per micro-triangle) that is built with the
//     normal acceleration-structure build API and linked to the BLAS triangles. Rays fly through the
//     transparent micro-triangles WITHOUT running an any-hit shader, so you see the inside of the cube
//     and the background through the holes.
//   * Shader Execution Reordering (SER): the raygen shader uses dx::HitObject::TraceRay ->
//     dx::MaybeReorderThread(hit, hint, bits) -> dx::HitObject::Invoke so the GPU may regroup its own
//     threads by "which face/material got hit" before running the closest-hit shader.
// Keys: O = cycle alpha mode (OMM 2-state / OMM 4-state + any-hit edges / any-hit only / opaque),
//       S = SER on/off, H = coherence-hint debug view, ESC quits.
// Command line: --hw (never fall back), --warp (force WARP), --debug (D3D12 debug layer),
//               --mode=N (start alpha mode 0..3), --ser=0, --hint.
// Needs the vendored preview Agility SDK + DXC + WARP (run fetch-deps.ps1). On GPUs without
// D3D12_RAYTRACING_TIER_1_2 (e.g. RTX 2070) it re-creates the device on the preview WARP; if even that
// fails it falls back to the plain DXR 1.0 cube.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>          // resolves to third_party\agility\include via build-all.ps1
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <DirectXMath.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

using namespace DirectX;

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "version.lib")

// [LEARN] Agility SDK opt-in: the loader sees these exports and loads D3D12Core.dll from .\D3D12\
// instead of the in-box runtime. 721 = the 1.721 preview, which carries the DXR 1.2 structs.
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 721;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

static const UINT kFrames = 2;
static const UINT STRIDE = 64;           // shader-table record stride (>= identifier size, 32B aligned)
static const UINT kOmmLevel = 7;         // 4^7 = 16384 micro-triangles per cube triangle
static const UINT kMicroTris = 1u << (2 * kOmmLevel);
static const UINT kOmmBytesPerTri = kMicroTris * 2 / 8;   // OC1 4-state = 2 bits per micro-triangle
static const UINT kCubeTris = 12;
static const char* kLogName = "CubeD3D12.RayTracing12.log";

// Alpha modes toggled with 'O'. [LEARN] All four use the SAME BLAS - only the per-instance flags in
// the TLAS change, which is exactly how an engine would do LOD / debug switches for OMM content.
enum AlphaMode { MODE_OMM_2STATE = 0, MODE_OMM_4STATE = 1, MODE_ANYHIT = 2, MODE_OPAQUE = 3 };
static const char* kModeNames[4] = {
    "OMM 2-state (no any-hit)", "OMM 4-state + any-hit edges", "Any-hit alpha test (no OMM)", "Opaque (no cutout)"
};

static UINT g_width = 800, g_height = 600;
static HWND                        g_hwnd = nullptr;
static ID3D12Device5*              g_dev = nullptr;
static ID3D12InfoQueue*            g_infoQueue = nullptr;
static ID3D12CommandQueue*         g_queue = nullptr;
static IDXGISwapChain3*            g_swap = nullptr;
static ID3D12Resource*             g_rtBuffers[kFrames] = {};
static ID3D12CommandAllocator*     g_alloc = nullptr;
static ID3D12GraphicsCommandList4* g_list = nullptr;
static ID3D12RootSignature*        g_globalRS = nullptr;
static ID3D12StateObject*          g_rtpso = nullptr;
static ID3D12Fence*                g_fence = nullptr;
static UINT64                      g_fenceValue = 0;
static HANDLE                      g_fenceEvent = nullptr;
static UINT                        g_frameIndex = 0;

static ID3D12Resource*             g_positions = nullptr;
static ID3D12Resource*             g_indices = nullptr;
static ID3D12Resource*             g_colors = nullptr;
static ID3D12Resource*             g_ommInput = nullptr;
static ID3D12Resource*             g_ommDescs = nullptr;
static ID3D12Resource*             g_ommArray = nullptr;
static ID3D12Resource*             g_blas = nullptr;
static ID3D12Resource*             g_tlas = nullptr;
static ID3D12Resource*             g_scratch = nullptr;
static ID3D12Resource*             g_instanceBuf = nullptr;
static void*                       g_instanceMapped = nullptr;
static ID3D12Resource*             g_shaderTable = nullptr;
static UINT64                      g_stBase = 0;
static ID3D12Resource*             g_output = nullptr;
static ID3D12DescriptorHeap*       g_uavHeap = nullptr;
static ID3D12Resource*             g_cb = nullptr;
static void*                       g_cbMapped = nullptr;
static ID3D12Resource*             g_ahsCounter = nullptr;   // any-hit invocation counter (UAV)
static ID3D12QueryHeap*            g_tsHeap = nullptr;       // 2 timestamps around DispatchRays
static ID3D12Resource*             g_readback = nullptr;     // [0..15] timestamps, [16..19] counter
static UINT64                      g_tsFreq = 1;

// Live state (shown in the title bar).
static bool        g_dxr12 = false;          // true = OMM + SER path, false = DXR 1.0 fallback
static bool        g_isWarp = false;
static std::string g_deviceName = "?";
static D3D12_RAYTRACING_TIER g_rtTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
static D3D_SHADER_MODEL g_shaderModel = D3D_SHADER_MODEL_5_1;
static BOOL        g_serActuallyReorders = FALSE;
static bool        g_experimentalEnabled = false;
static bool        g_debugLayer = false;
static UINT        g_debugErrors = 0;
static int         g_mode = MODE_OMM_2STATE;
static bool        g_serOn = true;
static bool        g_hintView = false;
static UINT        g_lastCounter = 0;
static double      g_accumMs = 0.0;
static UINT64      g_accumAhs = 0;
static UINT        g_accumFrames = 0;

struct CBData { XMFLOAT4X4 invViewProj; XMFLOAT3 camPos; float time; UINT hintView; UINT pad[3]; };

static void logf_line(const char* fmt, ...) {
    FILE* f = nullptr;
    if (fopen_s(&f, kLogName, "a") != 0 || !f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ------------------------------------------------------------------------------------------------
// HLSL. One DXIL library holds every ray-tracing shader. It is compiled twice-capable:
//   lib_6_9 + ENABLE_SER  -> DXR 1.2 path (RayGenSER uses the SM 6.9 dx::HitObject API)
//   lib_6_3               -> DXR 1.0 fallback (RayGenPlain only)
// ------------------------------------------------------------------------------------------------
static const char* g_src =
"RaytracingAccelerationStructure Scene : register(t0);\n"
"StructuredBuffer<float3> Positions : register(t1);\n"
"StructuredBuffer<uint>   Indices   : register(t2);\n"
"StructuredBuffer<float4> Colors    : register(t3);\n"
"RWTexture2D<float4> Output : register(u0);\n"
"RWByteAddressBuffer AnyHitCounter : register(u1);\n"
"cbuffer CB : register(b0) { float4x4 invViewProj; float3 camPos; float g_time; uint g_hintView; };\n"
"#ifdef ENABLE_SER\n"
"// [LEARN] SM 6.7+ libraries turn Payload Access Qualifiers on by default, and SM 6.9 insists on\n"
"// them: every payload field declares which stages read/write it. With HitObject the caller sits\n"
"// between any-hit and closest-hit/miss, so 'caller' reads what closesthit/miss wrote. Any-hit\n"
"// touches neither field, so the driver need not keep them alive during traversal.\n"
"struct [raypayload] Payload {\n"
"  float3 color : read(caller) : write(caller, closesthit, miss);\n"
"  uint   hint  : read(caller) : write(caller, closesthit, miss);\n"
"};\n"
"#else\n"
"struct Payload { float3 color; uint hint; };\n"
"#endif\n"
"\n"
"// [LEARN] The cut-out pattern, in the cube's object space. The CPU evaluates the SAME function per\n"
"// micro-triangle to bake the opacity micromap; the any-hit shader evaluates it per hit. Comparing the\n"
"// two ('O' key) shows OMM reproducing an alpha test with zero shader invocations.\n"
"bool IsHole(float3 p) {\n"
"  float3 a = abs(p); float2 st; uint face;\n"
"  if (a.x >= a.y && a.x >= a.z) { st = p.yz; face = 0 + (p.x > 0 ? 1 : 0); }\n"
"  else if (a.y >= a.z)          { st = p.xz; face = 2 + (p.y > 0 ? 1 : 0); }\n"
"  else                          { st = p.xy; face = 4 + (p.z > 0 ? 1 : 0); }\n"
"  uint style = face % 3;\n"
"  if (style == 0) return length(st) < 0.62;                  // circle\n"
"  if (style == 1) return abs(st.x) + abs(st.y) < 0.78;       // diamond\n"
"  return length(abs(st) - 0.45) < 0.28;                      // four round holes\n"
"}\n"
"\n"
"float3 HintColor(uint h) {\n"
"  static const float3 pal[8] = { float3(1,0.25,0.25), float3(0.25,1,0.25), float3(0.3,0.45,1),\n"
"    float3(1,1,0.2), float3(1,0.3,1), float3(0.2,1,1), float3(0.15,0.15,0.15), float3(1,1,1) };\n"
"  return pal[min(h, 7u)];\n"
"}\n"
"\n"
"RayDesc CameraRay(uint2 px) {\n"
"  float2 d = (float2(px) + 0.5) / float2(DispatchRaysDimensions().xy) * 2.0 - 1.0;\n"
"  d.y = -d.y;\n"
"  float4 world = mul(float4(d, 0, 1), invViewProj); world /= world.w;\n"
"  RayDesc ray; ray.Origin = camPos; ray.Direction = normalize(world.xyz - camPos);\n"
"  ray.TMin = 0.001; ray.TMax = 1000.0;\n"
"  return ray;\n"
"}\n"
"\n"
"float4 Finish(float3 color, uint hint) {\n"
"  return float4(g_hintView ? lerp(color, HintColor(hint), 0.8) : color, 1.0);\n"
"}\n"
"\n"
"// Classic DXR 1.0 raygen: TraceRay runs traversal + any-hit + closest-hit/miss as one fused call.\n"
"[shader(\"raygeneration\")]\n"
"void RayGenPlain() {\n"
"  uint2 px = DispatchRaysIndex().xy;\n"
"  Payload p; p.color = float3(0,0,0); p.hint = 6;\n"
"  TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, CameraRay(px), p);\n"
"  Output[px] = Finish(p.color, p.hint);\n"
"}\n"
"\n"
"#ifdef ENABLE_SER\n"
"// [LEARN] SER raygen (Shader Model 6.9). TraceRay is split in two:\n"
"//   1. dx::HitObject::TraceRay   - traversal + any-hit only; returns WHAT was hit, shades nothing.\n"
"//   2. dx::MaybeReorderThread    - 'GPU, feel free to shuffle threads so that lanes with the same\n"
"//                                  hit group + coherence hint end up in the same wave'.\n"
"//   3. dx::HitObject::Invoke     - now run closest-hit / miss, hopefully in a coherent wave.\n"
"// The hint is a user value: here 'which cube face (material) was hit', 6 = miss, 3 bits.\n"
"// DispatchRaysIndex() is preserved across the reorder, so each thread still writes its own pixel.\n"
"[shader(\"raygeneration\")]\n"
"void RayGenSER() {\n"
"  uint2 px = DispatchRaysIndex().xy;\n"
"  Payload p; p.color = float3(0,0,0); p.hint = 6;\n"
"  dx::HitObject hit = dx::HitObject::TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, CameraRay(px), p);\n"
"  uint hint = hit.IsHit() ? hit.GetPrimitiveIndex() / 2 : 6;\n"
"  dx::MaybeReorderThread(hit, hint, 3);\n"
"  dx::HitObject::Invoke(hit, p);\n"
"  // The HitObject knew the face BEFORE shading; closest-hit reports it AFTER. They must agree -\n"
"  // any mismatch would show up white in the H view.\n"
"  Output[px] = Finish(p.color, hint == p.hint ? hint : 7);\n"
"}\n"
"#endif\n"
"\n"
"[shader(\"miss\")]\n"
"void Miss(inout Payload p) {\n"
"  float t = (float)DispatchRaysIndex().y / (float)DispatchRaysDimensions().y;\n"
"  p.color = lerp(float3(0.10,0.10,0.20), float3(0.02,0.02,0.05), t);\n"
"  p.hint = 6;\n"
"}\n"
"\n"
"[shader(\"closesthit\")]\n"
"void ClosestHit(inout Payload p, BuiltInTriangleIntersectionAttributes attr) {\n"
"  float3 bary = float3(1.0 - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);\n"
"  uint prim = PrimitiveIndex();\n"
"  uint i0 = Indices[prim*3+0], i1 = Indices[prim*3+1], i2 = Indices[prim*3+2];\n"
"  float3 col = bary.x*Colors[i0].rgb + bary.y*Colors[i1].rgb + bary.z*Colors[i2].rgb;\n"
"  float3 p0 = Positions[i0], p1 = Positions[i1], p2 = Positions[i2];\n"
"  float3 n = normalize(cross(p1 - p0, p2 - p0));\n"
"  if (dot(n, p0) < 0) n = -n;                               // outward (cube is centred on the origin)\n"
"  float3 nw = normalize(mul(ObjectToWorld3x4(), float4(n, 0.0)));\n"
"  bool inside = dot(nw, WorldRayDirection()) > 0;           // seen through a hole = inner wall\n"
"  if (inside) nw = -nw;\n"
"  float3 L = normalize(float3(0.5, 0.8, -0.6));\n"
"  float diff = saturate(dot(nw, L)) * 0.7 + 0.3;\n"
"  p.color = col * diff * (inside ? 0.55 : 1.0);\n"
"  p.hint = prim / 2;\n"
"}\n"
"\n"
"// [LEARN] The any-hit alpha test OMM replaces. It counts itself so the title bar can show how many\n"
"// any-hit invocations each mode costs (OMM 2-state: zero; OMM 4-state: only the edge micro-triangles).\n"
"[shader(\"anyhit\")]\n"
"void AnyHit(inout Payload p, BuiltInTriangleIntersectionAttributes attr) {\n"
"  AnyHitCounter.InterlockedAdd(0, 1u);\n"
"  float3 op = ObjectRayOrigin() + ObjectRayDirection() * RayTCurrent();\n"
"  if (IsHole(op)) IgnoreHit();\n"
"}\n";

// ---- runtime DXC ---------------------------------------------------------------------------------
static bool g_previewDxcLoaded = false;

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf);
    size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : s.substr(0, slash + 1);
}

// [LEARN] lib_6_9 + dx::HitObject only exist in the preview DXC. build-all.ps1 copies it next to the
// exe, so load dxil.dll/dxcompiler.dll by full path from there before trying any Windows Kits copy.
static HMODULE LoadDxc() {
    std::wstring dirs[] = {
        ExeDir(),
        L"third_party\\dxc\\bin\\x64\\",
        L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.26100.0\\x64\\",
    };
    for (int i = 0; i < _countof(dirs); ++i) {
        LoadLibraryExW((dirs[i] + L"dxil.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        HMODULE m = LoadLibraryExW((dirs[i] + L"dxcompiler.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (m) {
            wchar_t path[MAX_PATH] = {}; GetModuleFileNameW(m, path, MAX_PATH);
            g_previewDxcLoaded = (i < 2);
            logf_line("Loaded DXC from %ls (vendored preview=%u)", path, g_previewDxcLoaded ? 1u : 0u);
            return m;
        }
    }
    logf_line("dxcompiler.dll not found");
    return nullptr;
}

static IDxcBlob* CompileLib(const char* src, const wchar_t* target, bool enableSer) {
    static HMODULE dll = LoadDxc();
    if (!dll) return nullptr;
    auto create = (DxcCreateInstanceProc)GetProcAddress(dll, "DxcCreateInstance");
    if (!create) { logf_line("DxcCreateInstance export missing"); return nullptr; }
    IDxcCompiler3* comp = nullptr;
    HRESULT hr = create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), (void**)&comp);
    if (FAILED(hr)) { logf_line("DxcCreateInstance hr=0x%08X", (unsigned)hr); return nullptr; }
    DxcBuffer buf = { src, strlen(src), DXC_CP_UTF8 };
    std::vector<const wchar_t*> args = { L"-T", target, L"-O3", L"-HV", L"2021" };
    if (enableSer) { args.push_back(L"-D"); args.push_back(L"ENABLE_SER=1"); }
    IDxcResult* result = nullptr;
    hr = comp->Compile(&buf, args.data(), (UINT32)args.size(), nullptr, __uuidof(IDxcResult), (void**)&result);
    comp->Release();
    if (FAILED(hr) || !result) { logf_line("DXC Compile call hr=0x%08X", (unsigned)hr); return nullptr; }
    HRESULT status = E_FAIL; result->GetStatus(&status);
    IDxcBlobUtf8* errs = nullptr;
    result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), (void**)&errs, nullptr);
    if (errs && errs->GetStringLength())
        logf_line("DXC %ls %s: %s", target, FAILED(status) ? "errors" : "warnings", errs->GetStringPointer());
    if (errs) errs->Release();
    if (FAILED(status)) { result->Release(); return nullptr; }
    IDxcBlob* obj = nullptr;
    result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), (void**)&obj, nullptr);
    result->Release();
    logf_line("Compiled ray-tracing library as %ls (SER=%u), %zu bytes", target, enableSer ? 1u : 0u,
        obj ? (size_t)obj->GetBufferSize() : (size_t)0);
    return obj;
}

// ---- Opacity micromap encoding ------------------------------------------------------------------
// [LEARN] An OMM subdivides each triangle into 4^level micro-triangles on a 2^level x 2^level
// barycentric grid, and stores 1 bit (OC1 2-state) or 2 bits (OC1 4-state) per micro-triangle.
// The micro-triangles are NOT stored row by row: they follow a hierarchical space-filling
// "bird curve" (see the DXR spec's "Opacity micromap encoding" figure; the same curve is used by
// VK_EXT_opacity_micromap). This routine turns a position along that curve into the barycentric
// (u,v) corners of the micro-triangle: de-interleave the index into two bit planes, run a prefix
// XOR from the most significant level down (each level's choice flips the orientation of the
// levels below it), and recover the discrete barycentric cell plus whether it is "upright".
static uint32_t PrefixXorFromTop(uint32_t x) {   // bit i = XOR of bits i..15
    uint32_t r = 0, acc = 0;
    for (int i = 15; i >= 0; --i) { acc ^= (x >> i) & 1u; r |= acc << i; }
    return r;
}

static void MicroTriangleUV(uint32_t index, uint32_t level, float uv[3][2]) {
    if (level == 0) { float t[3][2] = { {0,0}, {1,0}, {0,1} }; memcpy(uv, t, sizeof(t)); return; }
    uint32_t b0 = 0, b1 = 0;
    for (uint32_t i = 0; i < level; ++i) {
        b0 |= ((index >> (2 * i)) & 1u) << i;
        b1 |= ((index >> (2 * i + 1)) & 1u) << i;
    }
    const uint32_t fx = PrefixXorFromTop(b0);
    const uint32_t fy = PrefixXorFromTop(b0 & ~b1);
    const uint32_t t = fy ^ b1;
    const uint32_t mask = (1u << level) - 1u;
    uint32_t u = ((fx & ~t) | (b0 & ~t) | (~b0 & ~fx & t)) & mask;
    uint32_t v = (fy ^ b0) & mask;
    uint32_t w = ((~fx & ~t) | (b0 & ~t) | (~b0 & fx & t)) & mask;
    const bool upright = ((u ^ v ^ w) & 1u) != 0;
    if (!upright) { u += 1; v += 1; }           // inverted cells hang down from the next grid point
    const float s = 1.0f / (float)(1u << level);
    const float d = upright ? s : -s;
    uv[0][0] = u * s;     uv[0][1] = v * s;
    uv[1][0] = u * s + d; uv[1][1] = v * s;
    uv[2][0] = u * s;     uv[2][1] = v * s + d;
}

// C++ twin of the HLSL IsHole(): the pattern must match so OMM and any-hit modes look identical.
static bool IsHoleCpu(int axis, float sign, float s, float t) {
    const UINT face = (UINT)axis * 2 + (sign > 0 ? 1 : 0);
    const UINT style = face % 3;
    if (style == 0) return sqrtf(s * s + t * t) < 0.62f;
    if (style == 1) return fabsf(s) + fabsf(t) < 0.78f;
    const float ds = fabsf(s) - 0.45f, dt = fabsf(t) - 0.45f;
    return sqrtf(ds * ds + dt * dt) < 0.28f;
}

// Sanity check the curve decoder: every index must land on a distinct grid cell inside the triangle.
static bool ValidateBirdCurve(uint32_t level) {
    const uint32_t n = 1u << level, count = 1u << (2 * level);
    std::vector<uint8_t> seen((size_t)n * n * 2, 0);
    for (uint32_t i = 0; i < count; ++i) {
        float uv[3][2]; MicroTriangleUV(i, level, uv);
        const float cu = (uv[0][0] + uv[1][0] + uv[2][0]) / 3.0f, cv = (uv[0][1] + uv[1][1] + uv[2][1]) / 3.0f;
        if (cu < 0 || cv < 0 || cu + cv > 1.0f) return false;
        const uint32_t gu = (uint32_t)(cu * n), gv = (uint32_t)(cv * n);
        const uint32_t up = uv[1][0] > uv[0][0] ? 1u : 0u;
        const size_t key = ((size_t)gv * n + gu) * 2 + up;
        if (gu >= n || gv >= n || seen[key]) return false;
        seen[key] = 1;
    }
    return true;
}

struct OmmStats { UINT transparent = 0, opaque = 0, unknownT = 0, unknownO = 0; };

// Bake one OC1 4-state OMM per cube triangle. Each micro-triangle is sampled at its 3 corners,
// 3 edge midpoints and centroid: all-hole = TRANSPARENT, no-hole = OPAQUE, mixed (the rim of the
// hole) = UNKNOWN_TRANSPARENT/UNKNOWN_OPAQUE depending on the centroid. In 4-state mode the GPU
// calls any-hit only for those rim micro-triangles; forced to 2-state, UNKNOWN_x collapses to x.
static void BakeOmms(const XMFLOAT3* pos, const UINT32* idx, std::vector<uint8_t>& data, OmmStats& st) {
    data.assign((size_t)kCubeTris * kOmmBytesPerTri, 0);
    for (UINT tri = 0; tri < kCubeTris; ++tri) {
        const XMFLOAT3 v[3] = { pos[idx[tri * 3 + 0]], pos[idx[tri * 3 + 1]], pos[idx[tri * 3 + 2]] };
        // The coordinate all three vertices share is the face axis (e.g. x = +1).
        int axis = 0;
        if (v[0].y == v[1].y && v[1].y == v[2].y) axis = 1;
        if (v[0].z == v[1].z && v[1].z == v[2].z) axis = 2;
        const float* f0 = &v[0].x;
        const float sign = f0[axis];
        auto holeAt = [&](float bu, float bv) {
            // DXR barycentrics: u weights vertex 1, v weights vertex 2 (same convention as OMMs).
            float p[3];
            for (int c = 0; c < 3; ++c) {
                const float* a = &v[0].x; const float* b = &v[1].x; const float* d = &v[2].x;
                p[c] = a[c] * (1 - bu - bv) + b[c] * bu + d[c] * bv;
            }
            const float s = axis == 0 ? p[1] : p[0];
            const float t = axis == 2 ? p[1] : p[2];
            return IsHoleCpu(axis, sign, s, t);
        };
        uint8_t* out = &data[(size_t)tri * kOmmBytesPerTri];
        for (UINT i = 0; i < kMicroTris; ++i) {
            float uv[3][2]; MicroTriangleUV(i, kOmmLevel, uv);
            const float su[7] = { uv[0][0], uv[1][0], uv[2][0], (uv[0][0] + uv[1][0]) * 0.5f,
                (uv[1][0] + uv[2][0]) * 0.5f, (uv[2][0] + uv[0][0]) * 0.5f, (uv[0][0] + uv[1][0] + uv[2][0]) / 3.0f };
            const float sv[7] = { uv[0][1], uv[1][1], uv[2][1], (uv[0][1] + uv[1][1]) * 0.5f,
                (uv[1][1] + uv[2][1]) * 0.5f, (uv[2][1] + uv[0][1]) * 0.5f, (uv[0][1] + uv[1][1] + uv[2][1]) / 3.0f };
            UINT holes = 0;
            for (int k = 0; k < 7; ++k) holes += holeAt(su[k], sv[k]) ? 1u : 0u;
            const bool centroidHole = holeAt(su[6], sv[6]);
            UINT state;
            if (holes == 7)      { state = D3D12_RAYTRACING_OPACITY_MICROMAP_STATE_TRANSPARENT; st.transparent++; }
            else if (holes == 0) { state = D3D12_RAYTRACING_OPACITY_MICROMAP_STATE_OPAQUE; st.opaque++; }
            else if (centroidHole) { state = D3D12_RAYTRACING_OPACITY_MICROMAP_STATE_UNKNOWN_TRANSPARENT; st.unknownT++; }
            else                 { state = D3D12_RAYTRACING_OPACITY_MICROMAP_STATE_UNKNOWN_OPAQUE; st.unknownO++; }
            // [LEARN] OC1 4-state packing: micro-triangle i lives in bits [2i, 2i+1], little-endian.
            out[i / 4] |= (uint8_t)(state << ((i % 4) * 2));
        }
    }
}

// ---- small D3D12 helpers --------------------------------------------------------------------------
static void WaitForGpu() {
    const UINT64 v = ++g_fenceValue;
    g_queue->Signal(g_fence, v);
    if (g_fence->GetCompletedValue() < v) {
        g_fence->SetEventOnCompletion(v, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
    if (g_swap) g_frameIndex = g_swap->GetCurrentBackBufferIndex();
}

static ID3D12Resource* CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
    D3D12_RESOURCE_FLAGS flags, const char* what) {
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = heap;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = std::max<UINT64>(size, 256); rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = flags;
    ID3D12Resource* res = nullptr;
    HRESULT hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
        __uuidof(ID3D12Resource), (void**)&res);
    if (FAILED(hr)) logf_line("CreateCommittedResource(%s, %llu bytes) hr=0x%08X", what, (unsigned long long)size, (unsigned)hr);
    return res;
}

static ID3D12Resource* CreateUpload(const void* data, UINT64 size, const char* what) {
    ID3D12Resource* res = CreateBuffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
        D3D12_RESOURCE_FLAG_NONE, what);
    if (res && data) {
        void* m = nullptr; D3D12_RANGE none = { 0, 0 };
        res->Map(0, &none, &m); memcpy(m, data, (size_t)size); res->Unmap(0, nullptr);
    }
    return res;
}

static ID3D12Resource* CreateUAVBuffer(UINT64 size, D3D12_RESOURCE_STATES state, const char* what) {
    return CreateBuffer(size, D3D12_HEAP_TYPE_DEFAULT, state, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, what);
}

static void UAVBarrier(ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = r;
    g_list->ResourceBarrier(1, &b);
}

static void Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r; b.Transition.StateBefore = before; b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_list->ResourceBarrier(1, &b);
}

// Pull any debug-layer messages into the log (only populated when run with --debug).
static void DumpDebugMessages(const char* when) {
    if (!g_infoQueue) return;
    const UINT64 n = g_infoQueue->GetNumStoredMessages();
    UINT errs = 0;
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        g_infoQueue->GetMessage(i, nullptr, &len);
        std::vector<char> buf(len);
        D3D12_MESSAGE* msg = (D3D12_MESSAGE*)buf.data();
        if (FAILED(g_infoQueue->GetMessage(i, msg, &len))) continue;
        const bool err = msg->Severity <= D3D12_MESSAGE_SEVERITY_ERROR;
        if (err) errs++;
        logf_line("[debug layer] sev=%d id=%d %s", (int)msg->Severity, (int)msg->ID, msg->pDescription);
    }
    g_infoQueue->ClearStoredMessages();
    g_debugErrors += errs;
    if (n || when[0] == '!') logf_line("[debug layer] %s: %llu message(s), %u error/corruption (total errors %u)",
        when[0] == '!' ? when + 1 : when, (unsigned long long)n, errs, g_debugErrors);
}

// ---- device selection ----------------------------------------------------------------------------
static std::string Narrow(const wchar_t* w) {
    char buf[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
    return buf;
}

static std::string FileVersionOf(const wchar_t* path) {
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(path, &dummy);
    if (!size) return "?";
    std::vector<BYTE> data(size);
    VS_FIXEDFILEINFO* ffi = nullptr; UINT len = 0;
    if (!GetFileVersionInfoW(path, 0, size, data.data()) || !VerQueryValueW(data.data(), L"\\", (void**)&ffi, &len) || !ffi)
        return "?";
    char s[64];
    sprintf_s(s, "%u.%u.%u.%u", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
        HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
    return s;
}

struct Caps { D3D12_RAYTRACING_TIER tier; D3D_SHADER_MODEL sm; BOOL serReorders; };

static const char* TierName(D3D12_RAYTRACING_TIER t) {
    switch (t) {
    case D3D12_RAYTRACING_TIER_NOT_SUPPORTED: return "NOT_SUPPORTED";
    case D3D12_RAYTRACING_TIER_1_0: return "1.0";
    case D3D12_RAYTRACING_TIER_1_1: return "1.1";
    case D3D12_RAYTRACING_TIER_1_2: return "1.2";
    default: return "unknown(>1.2)";
    }
}

// [LEARN] The three questions a DXR 1.2 app asks:
//   1. OPTIONS5.RaytracingTier >= TIER_1_2  -> opacity micromaps + SER are available.
//   2. SHADER_MODEL >= 6.9                   -> the HLSL dx::HitObject / MaybeReorderThread API compiles in.
//   3. OPTIONS22.ShaderExecutionReorderingActuallyReorders -> does MaybeReorderThread DO anything here?
// SER is always *legal* at tier 1.2 / SM 6.9 - a device that does not reorder simply treats
// MaybeReorderThread as a no-op - so apps write one code path and query (3) only for diagnostics
// or to decide whether to fall back to their own manual sorting.
static Caps QueryCaps(ID3D12Device5* dev, const char* who) {
    Caps c = { D3D12_RAYTRACING_TIER_NOT_SUPPORTED, D3D_SHADER_MODEL_5_1, FALSE };
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
    HRESULT hr5 = dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5));
    if (SUCCEEDED(hr5)) c.tier = o5.RaytracingTier;
    D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_SHADER_MODEL_6_9 };   // "the highest I care about"
    HRESULT hrSm = dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
    if (SUCCEEDED(hrSm)) c.sm = sm.HighestShaderModel;
    D3D12_FEATURE_DATA_D3D12_OPTIONS22 o22 = {};
    HRESULT hr22 = dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS22, &o22, sizeof(o22));
    if (SUCCEEDED(hr22)) c.serReorders = o22.ShaderExecutionReorderingActuallyReorders;
    logf_line("[%s] OPTIONS5 hr=0x%08X RaytracingTier=%s (%d)", who, (unsigned)hr5, TierName(c.tier), (int)c.tier);
    logf_line("[%s] SHADER_MODEL hr=0x%08X HighestShaderModel=%d.%d", who, (unsigned)hrSm, c.sm >> 4, c.sm & 0xF);
    logf_line("[%s] OPTIONS22 hr=0x%08X ShaderExecutionReorderingActuallyReorders=%d", who, (unsigned)hr22, (int)c.serReorders);
    return c;
}

static bool IsDxr12(const Caps& c) { return c.tier >= D3D12_RAYTRACING_TIER_1_2 && c.sm >= D3D_SHADER_MODEL_6_9; }

static ID3D12Device5* CreateDeviceOn(IDXGIFactory4* factory, bool warp, std::string& shortName) {
    IDXGIAdapter1* adapter = nullptr;
    if (warp) {
        HRESULT hr = factory->EnumWarpAdapter(__uuidof(IDXGIAdapter1), (void**)&adapter);
        if (FAILED(hr)) { logf_line("EnumWarpAdapter hr=0x%08X", (unsigned)hr); return nullptr; }
    } else {
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d = {}; adapter->GetDesc1(&d);
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
            adapter->Release(); adapter = nullptr;
        }
        if (!adapter) { logf_line("No hardware adapter found"); return nullptr; }
    }
    DXGI_ADAPTER_DESC1 desc = {}; adapter->GetDesc1(&desc);
    std::string name = Narrow(desc.Description);
    ID3D12Device5* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device5), (void**)&dev);
    adapter->Release();
    logf_line("D3D12CreateDevice(%s \"%s\") hr=0x%08X", warp ? "WARP" : "hardware", name.c_str(), (unsigned)hr);
    if (FAILED(hr)) return nullptr;
    if (warp) {
        // [LEARN] EnumWarpAdapter loads d3d10warp.dll through the normal DLL search order, so the
        // preview copy sitting next to the exe wins over System32. Prove it by asking the loader.
        wchar_t path[MAX_PATH] = L"(not loaded)";
        HMODULE m = GetModuleHandleW(L"d3d10warp.dll");
        if (m) GetModuleFileNameW(m, path, MAX_PATH);
        const bool preview = _wcsnicmp(path, ExeDir().c_str(), ExeDir().size()) == 0;
        logf_line("WARP module: %ls version %s (%s)", path, FileVersionOf(path).c_str(),
            preview ? "vendored preview" : "IN-BOX - preview WARP not found next to exe");
        shortName = preview ? "WARP (preview)" : "WARP (in-box)";
    } else {
        shortName = name;
        const char* strip[] = { "NVIDIA GeForce ", "NVIDIA ", "AMD Radeon ", "Intel(R) " };
        for (auto s : strip) if (shortName.rfind(s, 0) == 0) { shortName = shortName.substr(strlen(s)); break; }
    }
    return dev;
}

// Tries, in order: hardware -> preview WARP -> preview WARP with experimental shader models.
// Experimental features are only switched on if a plain device does not already report SM 6.9.
static bool SelectDevice(IDXGIFactory4* factory, bool forceHw, bool forceWarp) {
    struct Attempt { bool warp; bool experimental; };
    std::vector<Attempt> attempts;
    if (!forceWarp) attempts.push_back({ false, false });
    if (!forceHw) { attempts.push_back({ true, false }); attempts.push_back({ true, true }); }
    else attempts.push_back({ false, true });

    bool experimentalCouldHelp[2] = { false, false };   // [hw, warp]: tier 1.2 present but SM < 6.9
    for (const Attempt& a : attempts) {
        if (a.experimental) {
            if (!experimentalCouldHelp[a.warp ? 1 : 0]) continue;   // experimental SMs can't add a tier
            if (!g_experimentalEnabled) {
                // [LEARN] Must happen before the device is created, and it invalidates existing devices.
                HRESULT hr = D3D12EnableExperimentalFeatures(1, &D3D12ExperimentalShaderModels, nullptr, nullptr);
                g_experimentalEnabled = SUCCEEDED(hr);
                logf_line("D3D12EnableExperimentalFeatures(D3D12ExperimentalShaderModels) hr=0x%08X (needed: tier 1.2 but no SM 6.9)",
                    (unsigned)hr);
                if (!g_experimentalEnabled) continue;
            }
        }
        std::string name;
        ID3D12Device5* dev = CreateDeviceOn(factory, a.warp, name);
        if (!dev) continue;
        Caps c = QueryCaps(dev, name.c_str());
        if (!a.experimental && c.tier >= D3D12_RAYTRACING_TIER_1_2 && c.sm < D3D_SHADER_MODEL_6_9)
            experimentalCouldHelp[a.warp ? 1 : 0] = true;
        if (IsDxr12(c)) {
            g_dev = dev; g_isWarp = a.warp; g_deviceName = name; g_dxr12 = true;
            g_rtTier = c.tier; g_shaderModel = c.sm; g_serActuallyReorders = c.serReorders;
            logf_line("Selected %s for the DXR 1.2 path (experimental shader models %s)", name.c_str(),
                g_experimentalEnabled ? "ON" : "not needed");
            return true;
        }
        logf_line("%s lacks DXR 1.2 (needs RaytracingTier >= 1.2 and SM >= 6.9)%s", name.c_str(),
            forceHw ? "" : "; trying next device");
        dev->Release();
    }

    // Fallback: the plain DXR 1.0 rotating cube (lib_6_3, TraceRay, no OMM / SER).
    std::vector<bool> fallbacks;
    if (!forceWarp) fallbacks.push_back(false);
    if (!forceHw) fallbacks.push_back(true);
    for (bool warp : fallbacks) {
        std::string name;
        ID3D12Device5* dev = CreateDeviceOn(factory, warp, name);
        if (!dev) continue;
        Caps c = QueryCaps(dev, name.c_str());
        if (c.tier >= D3D12_RAYTRACING_TIER_1_0) {
            g_dev = dev; g_isWarp = warp; g_deviceName = name; g_dxr12 = false;
            g_rtTier = c.tier; g_shaderModel = c.sm; g_serActuallyReorders = c.serReorders;
            logf_line("FALLBACK: no DXR 1.2 device - rendering the plain DXR 1.0 cube on %s", name.c_str());
            return true;
        }
        dev->Release();
    }
    logf_line("No device with any ray-tracing support");
    return false;
}

static bool g_serAvailable = false;   // SER raygen compiled + in the state object

// Builds (once): OMM array -> BLAS that links it. The TLAS is rebuilt every frame in Render().
static bool BuildAccelerationStructures(const XMFLOAT3* positions, const UINT32* indices) {
    D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC tri = {};
    tri.VertexBuffer.StartAddress = g_positions->GetGPUVirtualAddress();
    tri.VertexBuffer.StrideInBytes = sizeof(XMFLOAT3);
    tri.VertexCount = 8;
    tri.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    tri.IndexBuffer = g_indices->GetGPUVirtualAddress();
    tri.IndexCount = 36;
    tri.IndexFormat = DXGI_FORMAT_R32_UINT;

    // ---- 1. Opacity micromap array ----
    // [LEARN] An OMM array is a third kind of "acceleration structure": same prebuild-info query,
    // same BuildRaytracingAccelerationStructure call, same RAYTRACING_ACCELERATION_STRUCTURE state.
    // Inputs: raw bits (InputBuffer), one D3D12_RAYTRACING_OPACITY_MICROMAP_DESC per OMM saying
    // where its bits start + level + format, and a CPU-side histogram so the driver can size it.
    D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY hist = {};
    D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC ommArrayDesc = {};
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ommIn = {};
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO ommPre = {};
    if (g_dxr12) {
        const bool curveOk = ValidateBirdCurve(kOmmLevel);
        logf_line("OMM bird-curve decoder self-check (level %u, %u micro-triangles): %s", kOmmLevel, kMicroTris,
            curveOk ? "every index maps to a unique cell" : "FAILED");
        std::vector<uint8_t> bits; OmmStats st;
        BakeOmms(positions, indices, bits, st);
        logf_line("Baked %u OC1 4-state OMMs, %u bytes: transparent=%u opaque=%u unknown-transparent=%u unknown-opaque=%u",
            kCubeTris, (unsigned)bits.size(), st.transparent, st.opaque, st.unknownT, st.unknownO);
        static_assert(sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_DESC) == 8, "OMM desc layout");
        D3D12_RAYTRACING_OPACITY_MICROMAP_DESC descs[kCubeTris] = {};
        for (UINT i = 0; i < kCubeTris; ++i) {
            descs[i].ByteOffset = i * kOmmBytesPerTri;
            descs[i].SubdivisionLevel = kOmmLevel;
            descs[i].Format = D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE;
        }
        // Upload heap = GENERIC_READ, which includes the required NON_PIXEL_SHADER_RESOURCE state;
        // committed buffers are 64 KB aligned so the 128-byte InputBuffer alignment is satisfied.
        g_ommInput = CreateUpload(bits.data(), bits.size(), "OMM input bits");
        g_ommDescs = CreateUpload(descs, sizeof(descs), "OMM descs");
        if (!g_ommInput || !g_ommDescs) return false;
        hist.Count = kCubeTris; hist.SubdivisionLevel = kOmmLevel;
        hist.Format = D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE;
        ommArrayDesc.NumOmmHistogramEntries = 1;
        ommArrayDesc.pOmmHistogram = &hist;
        ommArrayDesc.InputBuffer = g_ommInput->GetGPUVirtualAddress();
        ommArrayDesc.PerOmmDescs.StartAddress = g_ommDescs->GetGPUVirtualAddress();
        ommArrayDesc.PerOmmDescs.StrideInBytes = sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_DESC);
        ommIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY;
        ommIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        ommIn.NumDescs = 1;   // one OMM-array desc (which itself describes all 12 OMMs)
        ommIn.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        ommIn.pOpacityMicromapArrayDesc = &ommArrayDesc;
        g_dev->GetRaytracingAccelerationStructurePrebuildInfo(&ommIn, &ommPre);
        logf_line("OMM array prebuild: result=%llu scratch=%llu bytes",
            (unsigned long long)ommPre.ResultDataMaxSizeInBytes, (unsigned long long)ommPre.ScratchDataSizeInBytes);
        if (!ommPre.ResultDataMaxSizeInBytes) { logf_line("OMM array prebuild returned 0 bytes"); return false; }
        g_ommArray = CreateUAVBuffer(ommPre.ResultDataMaxSizeInBytes,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, "OMM array");
        if (!g_ommArray) return false;
    }

    // ---- 2. BLAS: the same triangles, now wrapped in an OMM_TRIANGLES geometry ----
    // [LEARN] D3D12_RAYTRACING_GEOMETRY_OMM_TRIANGLES_DESC = pointer to the usual triangle desc +
    // pointer to an OMM linkage desc. With no OMM index buffer (DXGI_FORMAT_UNKNOWN) triangle N uses
    // OMM N (+ OpacityMicromapBaseLocation). An index buffer would let triangles share OMMs or use
    // the special indices -1..-4 (fully transparent / opaque / unknown) without storing any bits.
    // The geometry is deliberately NOT flagged OPAQUE: with OMMs linked the flag is ignored, and
    // when an instance disables OMMs we want the any-hit shader to run (mode 2).
    D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC link = {};
    link.OpacityMicromapIndexFormat = DXGI_FORMAT_UNKNOWN;
    link.OpacityMicromapBaseLocation = 0;
    link.OpacityMicromapArray = g_ommArray ? g_ommArray->GetGPUVirtualAddress() : 0;
    D3D12_RAYTRACING_GEOMETRY_DESC geom = {};
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    if (g_dxr12) {
        geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES;
        geom.OmmTriangles.pTriangles = &tri;
        geom.OmmTriangles.pOmmLinkage = &link;
    } else {
        geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geom.Triangles = tri;
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blIn = {};
    blIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    blIn.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    // ALLOW_DISABLE_OMMS is what makes the per-instance DISABLE_OMMS flag (modes 2/3) legal.
    blIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    if (g_dxr12) blIn.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_DISABLE_OMMS;
    blIn.NumDescs = 1; blIn.pGeometryDescs = &geom;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blPre = {};
    g_dev->GetRaytracingAccelerationStructurePrebuildInfo(&blIn, &blPre);
    logf_line("BLAS (%s) prebuild: result=%llu scratch=%llu bytes", g_dxr12 ? "OMM_TRIANGLES" : "TRIANGLES",
        (unsigned long long)blPre.ResultDataMaxSizeInBytes, (unsigned long long)blPre.ScratchDataSizeInBytes);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlIn = {};
    tlIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlIn.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tlIn.NumDescs = 1;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlPre = {};
    g_dev->GetRaytracingAccelerationStructurePrebuildInfo(&tlIn, &tlPre);

    const UINT64 scratchSize = std::max({ ommPre.ScratchDataSizeInBytes, blPre.ScratchDataSizeInBytes, tlPre.ScratchDataSizeInBytes });
    // Buffers ignore InitialState (they start COMMON and are implicitly promoted on first GPU use).
    g_scratch = CreateUAVBuffer(scratchSize, D3D12_RESOURCE_STATE_COMMON, "scratch");
    g_blas = CreateUAVBuffer(blPre.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, "BLAS");
    g_tlas = CreateUAVBuffer(tlPre.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, "TLAS");
    g_instanceBuf = CreateUpload(nullptr, sizeof(D3D12_RAYTRACING_INSTANCE_DESC), "instance");
    if (!g_scratch || !g_blas || !g_tlas || !g_instanceBuf) return false;
    g_instanceBuf->Map(0, nullptr, &g_instanceMapped);

    g_alloc->Reset();
    g_list->Reset(g_alloc, nullptr);
    if (g_dxr12) {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC ommBuild = {};
        ommBuild.Inputs = ommIn;
        ommBuild.ScratchAccelerationStructureData = g_scratch->GetGPUVirtualAddress();
        ommBuild.DestAccelerationStructureData = g_ommArray->GetGPUVirtualAddress();
        g_list->BuildRaytracingAccelerationStructure(&ommBuild, 0, nullptr);
        UAVBarrier(nullptr);   // OMM array written + scratch reused -> the BLAS build must wait
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blBuild = {};
    blBuild.Inputs = blIn;
    blBuild.ScratchAccelerationStructureData = g_scratch->GetGPUVirtualAddress();
    blBuild.DestAccelerationStructureData = g_blas->GetGPUVirtualAddress();
    g_list->BuildRaytracingAccelerationStructure(&blBuild, 0, nullptr);
    UAVBarrier(nullptr);
    HRESULT hr = g_list->Close();
    ID3D12CommandList* lists[] = { g_list };
    g_queue->ExecuteCommandLists(1, lists);
    WaitForGpu();
    HRESULT removed = g_dev->GetDeviceRemovedReason();
    logf_line("Acceleration-structure build submitted: Close hr=0x%08X, device status=0x%08X", (unsigned)hr, (unsigned)removed);
    return SUCCEEDED(hr) && SUCCEEDED(removed);
}

static bool CreatePipeline(bool withSer) {
    IDxcBlob* lib = CompileLib(g_src, withSer ? L"lib_6_9" : L"lib_6_3", withSer);
    if (!lib) return false;

    D3D12_STATE_SUBOBJECT sub[5] = {};
    D3D12_DXIL_LIBRARY_DESC libDesc = {};
    libDesc.DXILLibrary.pShaderBytecode = lib->GetBufferPointer();
    libDesc.DXILLibrary.BytecodeLength = lib->GetBufferSize();
    sub[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY; sub[0].pDesc = &libDesc;

    D3D12_HIT_GROUP_DESC hg = {};
    hg.HitGroupExport = L"HitGroup";
    hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hg.ClosestHitShaderImport = L"ClosestHit";
    hg.AnyHitShaderImport = L"AnyHit";
    sub[1].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP; sub[1].pDesc = &hg;

    D3D12_RAYTRACING_SHADER_CONFIG shaderCfg = {};
    shaderCfg.MaxPayloadSizeInBytes = sizeof(float) * 4;     // float3 color + uint hint
    shaderCfg.MaxAttributeSizeInBytes = sizeof(float) * 2;
    sub[2].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG; sub[2].pDesc = &shaderCfg;

    D3D12_GLOBAL_ROOT_SIGNATURE grs = {}; grs.pGlobalRootSignature = g_globalRS;
    sub[3].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE; sub[3].pDesc = &grs;

    // [LEARN] DXR 1.2 opt-in: PIPELINE_CONFIG1 carries flags, and ALLOW_OPACITY_MICROMAPS tells the
    // driver traversal may meet OMM-linked triangles. Without it, hitting an OMM is undefined.
    D3D12_RAYTRACING_PIPELINE_CONFIG pipeCfg = {}; pipeCfg.MaxTraceRecursionDepth = 1;
    D3D12_RAYTRACING_PIPELINE_CONFIG1 pipeCfg1 = {};
    pipeCfg1.MaxTraceRecursionDepth = 1;
    pipeCfg1.Flags = D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_OPACITY_MICROMAPS;
    if (g_dxr12) { sub[4].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1; sub[4].pDesc = &pipeCfg1; }
    else         { sub[4].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;  sub[4].pDesc = &pipeCfg; }

    D3D12_STATE_OBJECT_DESC soDesc = {};
    soDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    soDesc.NumSubobjects = _countof(sub); soDesc.pSubobjects = sub;
    HRESULT hr = g_dev->CreateStateObject(&soDesc, __uuidof(ID3D12StateObject), (void**)&g_rtpso);
    lib->Release();
    logf_line("CreateStateObject(%s, %s) hr=0x%08X", withSer ? "lib_6_9 + SER" : "lib_6_3",
        g_dxr12 ? "PIPELINE_CONFIG1 ALLOW_OPACITY_MICROMAPS" : "PIPELINE_CONFIG", (unsigned)hr);
    if (FAILED(hr)) return false;

    // Shader table: [0] RayGenSER (or Plain), [1] RayGenPlain, [2] Miss, [3] HitGroup.
    ID3D12StateObjectProperties* props = nullptr;
    g_rtpso->QueryInterface(__uuidof(ID3D12StateObjectProperties), (void**)&props);
    const UINT idSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    BYTE table[STRIDE * 4] = {};
    memcpy(table + 0 * STRIDE, props->GetShaderIdentifier(withSer ? L"RayGenSER" : L"RayGenPlain"), idSize);
    memcpy(table + 1 * STRIDE, props->GetShaderIdentifier(L"RayGenPlain"), idSize);
    memcpy(table + 2 * STRIDE, props->GetShaderIdentifier(L"Miss"), idSize);
    memcpy(table + 3 * STRIDE, props->GetShaderIdentifier(L"HitGroup"), idSize);
    props->Release();
    g_shaderTable = CreateUpload(table, sizeof(table), "shader table");
    if (!g_shaderTable) return false;
    g_stBase = g_shaderTable->GetGPUVirtualAddress();
    g_serAvailable = withSer;
    return true;
}

static bool InitD3D(HWND hwnd, bool forceHw, bool forceWarp) {
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&factory);
    if (FAILED(hr)) { logf_line("CreateDXGIFactory1 hr=0x%08X", (unsigned)hr); return false; }
    if (!SelectDevice(factory, forceHw, forceWarp)) { factory->Release(); return false; }
    if (g_debugLayer) g_dev->QueryInterface(__uuidof(ID3D12InfoQueue), (void**)&g_infoQueue);
    if (!g_dxr12) { g_mode = MODE_OPAQUE; g_serOn = false; }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g_dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&g_queue);
    g_queue->GetTimestampFrequency(&g_tsFreq);

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.BufferCount = kFrames;
    sd.Width = g_width; sd.Height = g_height;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc.Count = 1;
    IDXGISwapChain1* sc1 = nullptr;
    hr = factory->CreateSwapChainForHwnd(g_queue, hwnd, &sd, nullptr, nullptr, &sc1);
    factory->Release();
    if (FAILED(hr)) { logf_line("CreateSwapChainForHwnd hr=0x%08X", (unsigned)hr); return false; }
    sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&g_swap);
    sc1->Release();
    g_frameIndex = g_swap->GetCurrentBackBufferIndex();
    for (UINT i = 0; i < kFrames; i++)
        g_swap->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&g_rtBuffers[i]);

    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&g_alloc);
    ID3D12GraphicsCommandList* list0 = nullptr;
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr,
        __uuidof(ID3D12GraphicsCommandList), (void**)&list0);
    list0->QueryInterface(__uuidof(ID3D12GraphicsCommandList4), (void**)&g_list);
    list0->Release();
    g_list->Close();
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_fence);
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // ---- cube geometry (identical to every other sample in the course) ----
    static const XMFLOAT3 positions[8] = {
        {-1,-1,-1}, {-1,1,-1}, {1,1,-1}, {1,-1,-1},
        {-1,-1, 1}, {-1,1, 1}, {1,1, 1}, {1,-1, 1},
    };
    static const XMFLOAT4 colors[8] = {
        {0,0,0,1}, {0,1,0,1}, {1,1,0,1}, {1,0,0,1},
        {0,0,1,1}, {0,1,1,1}, {1,1,1,1}, {1,0,1,1},
    };
    static const UINT32 indices[36] = {
        0,1,2, 0,2,3, 4,6,5, 4,7,6, 4,5,1, 4,1,0,
        3,2,6, 3,6,7, 1,5,6, 1,6,2, 4,0,3, 4,3,7,
    };
    g_positions = CreateUpload(positions, sizeof(positions), "positions");
    g_colors = CreateUpload(colors, sizeof(colors), "colors");
    g_indices = CreateUpload(indices, sizeof(indices), "indices");
    if (!g_positions || !g_colors || !g_indices) return false;
    if (!BuildAccelerationStructures(positions, indices)) { logf_line("Acceleration-structure build failed"); return false; }

    // ---- global root signature: u0 table, t0..t3 root SRVs, b0 root CBV, u1 root UAV ----
    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uavRange.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER rp[7] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1; rp[0].DescriptorTable.pDescriptorRanges = &uavRange;
    for (int i = 1; i <= 4; i++) {
        rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rp[i].Descriptor.ShaderRegister = i - 1;
    }
    rp[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rp[5].Descriptor.ShaderRegister = 0;
    rp[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; rp[6].Descriptor.ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = _countof(rp); rsd.pParameters = rp;
    ID3DBlob* sig = nullptr; ID3DBlob* sigErr = nullptr;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr);
    if (FAILED(hr)) { logf_line("D3D12SerializeRootSignature hr=0x%08X", (unsigned)hr); if (sigErr) sigErr->Release(); return false; }
    hr = g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
        __uuidof(ID3D12RootSignature), (void**)&g_globalRS);
    sig->Release();
    if (FAILED(hr)) { logf_line("CreateRootSignature hr=0x%08X", (unsigned)hr); return false; }

    // ---- ray-tracing pipeline: SER library first, lib_6_3 if that is impossible ----
    if (!(g_dxr12 && CreatePipeline(true))) {
        if (g_dxr12) logf_line("SER pipeline unavailable; retrying with plain lib_6_3 raygen");
        if (!CreatePipeline(false)) return false;
        g_serOn = false;
    }

    // ---- output UAV texture + descriptor heap ----
    D3D12_HEAP_PROPERTIES dhp = {}; dhp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC od = {};
    od.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    od.Width = g_width; od.Height = g_height; od.DepthOrArraySize = 1; od.MipLevels = 1;
    od.Format = DXGI_FORMAT_R8G8B8A8_UNORM; od.SampleDesc.Count = 1;
    od.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr = g_dev->CreateCommittedResource(&dhp, D3D12_HEAP_FLAG_NONE, &od,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource), (void**)&g_output);
    if (FAILED(hr)) { logf_line("CreateCommittedResource(output) hr=0x%08X", (unsigned)hr); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g_uavHeap);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    g_dev->CreateUnorderedAccessView(g_output, nullptr, &uav, g_uavHeap->GetCPUDescriptorHandleForHeapStart());

    g_cb = CreateUpload(nullptr, 256, "constants");
    g_ahsCounter = CreateUAVBuffer(256, D3D12_RESOURCE_STATE_COMMON, "any-hit counter");
    g_readback = CreateBuffer(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, "readback");
    if (!g_cb || !g_ahsCounter || !g_readback) return false;
    g_cb->Map(0, nullptr, &g_cbMapped);

    // [LEARN] Two GPU timestamps bracket DispatchRays so SER on/off cost is measured on the GPU
    // timeline, not by the CPU (which is dominated by vsync + WaitForGpu here).
    D3D12_QUERY_HEAP_DESC qh = {}; qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qh.Count = 2;
    hr = g_dev->CreateQueryHeap(&qh, __uuidof(ID3D12QueryHeap), (void**)&g_tsHeap);
    logf_line("CreateQueryHeap(timestamp) hr=0x%08X, timestamp frequency=%llu Hz", (unsigned)hr, (unsigned long long)g_tsFreq);
    return SUCCEEDED(hr);
}

// [LEARN] Every alpha mode is just a different instance flag on the SAME BLAS:
//   FORCE_OMM_2_STATE : UNKNOWN_x micro-triangles behave as x  -> traversal never calls any-hit.
//   (none)            : 4-state - only UNKNOWN (rim) micro-triangles call any-hit for an exact edge.
//   DISABLE_OMMS      : ignore the OMM, use the geometry's non-opaque flag -> any-hit on every hit.
//   + FORCE_OPAQUE    : no alpha at all. (FORCE_OPAQUE alone would NOT fill the holes - instance and
//                       ray flags apply after OMM classification, so transparent stays transparent.)
static UINT InstanceFlagsFor(int mode) {
    switch (mode) {
    case MODE_OMM_2STATE: return D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OMM_2_STATE;
    case MODE_OMM_4STATE: return D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
    case MODE_ANYHIT:     return g_dxr12 ? D3D12_RAYTRACING_INSTANCE_FLAG_DISABLE_OMMS : D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
    default:              return D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE |
                              (g_dxr12 ? D3D12_RAYTRACING_INSTANCE_FLAG_DISABLE_OMMS : 0);
    }
}

static void UpdateTitle() {
    const double ms = g_accumFrames ? g_accumMs / g_accumFrames : 0.0;
    const double ahs = g_accumFrames ? (double)g_accumAhs / g_accumFrames : 0.0;
    char serText[96];
    if (g_serAvailable)
        sprintf_s(serText, "SER %s (HW reorders: %s)", g_serOn ? "on" : "off", g_serActuallyReorders ? "yes" : "no");
    else
        sprintf_s(serText, "SER n/a");
    char dbg[48] = "";
    if (g_debugLayer) sprintf_s(dbg, " | debug errors %u", g_debugErrors);
    char title[512];
    sprintf_s(title, "D3D12 %s | %s, %.0f any-hit/frame | %s | %s | rays %.2f ms%s%s | O S H",
        g_dxr12 ? "DXR 1.2" : "DXR 1.0 FALLBACK (no tier 1.2)", kModeNames[g_mode], ahs, serText,
        g_deviceName.c_str(), ms, g_hintView ? " | hint view" : "", dbg);
    SetWindowTextA(g_hwnd, title);
    static DWORD lastLog = 0;
    if (GetTickCount() - lastLog > 2000) { lastLog = GetTickCount(); logf_line("title: %s", title); }
    g_accumMs = 0; g_accumAhs = 0; g_accumFrames = 0;
}

static void Render(float t) {
    // Rotating instance transform (object -> world), row-major 3x4, plus this mode's flags.
    XMMATRIX world = XMMatrixRotationY(t) * XMMatrixRotationX(t * 0.5f);
    XMFLOAT4X4 wt; XMStoreFloat4x4(&wt, XMMatrixTranspose(world));
    D3D12_RAYTRACING_INSTANCE_DESC inst = {};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++)
            inst.Transform[r][c] = wt.m[r][c];
    inst.InstanceMask = 1;
    inst.Flags = InstanceFlagsFor(g_mode);
    inst.AccelerationStructure = g_blas->GetGPUVirtualAddress();
    memcpy(g_instanceMapped, &inst, sizeof(inst));

    XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 0, -6, 0), XMVectorSet(0, 0, 0, 0), XMVectorSet(0, 1, 0, 0));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, (float)g_width / g_height, 0.1f, 100.0f);
    XMMATRIX invVP = XMMatrixInverse(nullptr, view * proj);
    CBData cb = {}; XMStoreFloat4x4(&cb.invViewProj, XMMatrixTranspose(invVP));
    cb.camPos = XMFLOAT3(0, 0, -6); cb.time = t; cb.hintView = g_hintView ? 1u : 0u;
    memcpy(g_cbMapped, &cb, sizeof(cb));

    g_alloc->Reset();
    g_list->Reset(g_alloc, nullptr);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlIn = {};
    tlIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlIn.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tlIn.NumDescs = 1;
    tlIn.InstanceDescs = g_instanceBuf->GetGPUVirtualAddress();
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlBuild = {};
    tlBuild.Inputs = tlIn;
    tlBuild.ScratchAccelerationStructureData = g_scratch->GetGPUVirtualAddress();
    tlBuild.DestAccelerationStructureData = g_tlas->GetGPUVirtualAddress();
    g_list->BuildRaytracingAccelerationStructure(&tlBuild, 0, nullptr);
    UAVBarrier(g_tlas);

    ID3D12DescriptorHeap* heaps[] = { g_uavHeap };
    g_list->SetDescriptorHeaps(1, heaps);
    g_list->SetComputeRootSignature(g_globalRS);
    g_list->SetComputeRootDescriptorTable(0, g_uavHeap->GetGPUDescriptorHandleForHeapStart());
    g_list->SetComputeRootShaderResourceView(1, g_tlas->GetGPUVirtualAddress());
    g_list->SetComputeRootShaderResourceView(2, g_positions->GetGPUVirtualAddress());
    g_list->SetComputeRootShaderResourceView(3, g_indices->GetGPUVirtualAddress());
    g_list->SetComputeRootShaderResourceView(4, g_colors->GetGPUVirtualAddress());
    g_list->SetComputeRootConstantBufferView(5, g_cb->GetGPUVirtualAddress());
    g_list->SetComputeRootUnorderedAccessView(6, g_ahsCounter->GetGPUVirtualAddress());
    g_list->SetPipelineState1(g_rtpso);

    // [LEARN] SER on/off is just which raygen record DispatchRays points at: record 0 is RayGenSER,
    // record 1 is RayGenPlain. Same pipeline, same hit groups, same payload.
    D3D12_DISPATCH_RAYS_DESC drd = {};
    drd.RayGenerationShaderRecord.StartAddress = g_stBase + (g_serOn ? 0 : 1) * STRIDE;
    drd.RayGenerationShaderRecord.SizeInBytes = STRIDE;
    drd.MissShaderTable.StartAddress = g_stBase + 2 * STRIDE;
    drd.MissShaderTable.SizeInBytes = STRIDE;
    drd.MissShaderTable.StrideInBytes = STRIDE;
    drd.HitGroupTable.StartAddress = g_stBase + 3 * STRIDE;
    drd.HitGroupTable.SizeInBytes = STRIDE;
    drd.HitGroupTable.StrideInBytes = STRIDE;
    drd.Width = g_width; drd.Height = g_height; drd.Depth = 1;
    // Note: on WARP (a CPU rasterizer) SER will not make this faster - MaybeReorderThread has no
    // hardware scheduler to feed. The lesson is the API shape: the app says WHAT is coherent, the
    // GPU decides whether shuffling its own threads is worth it (OPTIONS22 tells you if it does).
    g_list->EndQuery(g_tsHeap, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    g_list->DispatchRays(&drd);
    g_list->EndQuery(g_tsHeap, D3D12_QUERY_TYPE_TIMESTAMP, 1);
    g_list->ResolveQueryData(g_tsHeap, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, g_readback, 0);
    Transition(g_ahsCounter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g_list->CopyBufferRegion(g_readback, 16, g_ahsCounter, 0, sizeof(UINT));
    Transition(g_ahsCounter, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Copy the ray-traced image to the back buffer.
    Transition(g_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(g_rtBuffers[g_frameIndex], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    g_list->CopyResource(g_rtBuffers[g_frameIndex], g_output);
    Transition(g_output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(g_rtBuffers[g_frameIndex], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);

    g_list->Close();
    ID3D12CommandList* lists[] = { g_list };
    g_queue->ExecuteCommandLists(1, lists);
    g_swap->Present(1, 0);
    WaitForGpu();

    // Read back GPU time and the any-hit counter (monotonic; per-frame value = delta).
    BYTE* rb = nullptr; D3D12_RANGE range = { 0, 20 };
    if (SUCCEEDED(g_readback->Map(0, &range, (void**)&rb)) && rb) {
        UINT64 ts[2]; UINT counter;
        memcpy(ts, rb, sizeof(ts)); memcpy(&counter, rb + 16, sizeof(counter));
        D3D12_RANGE none = { 0, 0 }; g_readback->Unmap(0, &none);
        if (ts[1] > ts[0]) g_accumMs += (double)(ts[1] - ts[0]) * 1000.0 / (double)g_tsFreq;
        g_accumAhs += counter - g_lastCounter;
        g_lastCounter = counter;
        g_accumFrames++;
    }
    static UINT frameNo = 0;
    if (++frameNo == 1) {
        logf_line("First frame rendered: mode=%s, SER=%s, instance flags=0x%X, device status=0x%08X", kModeNames[g_mode],
            g_serOn ? "on" : "off", InstanceFlagsFor(g_mode), (unsigned)g_dev->GetDeviceRemovedReason());
        DumpDebugMessages("!after first frame");
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) PostQuitMessage(0);
        else if (w == 'O') {
            g_mode = g_dxr12 ? (g_mode + 1) % 4 : (g_mode == MODE_OPAQUE ? MODE_ANYHIT : MODE_OPAQUE);
            logf_line("Key O -> alpha mode %s", kModeNames[g_mode]);
        } else if (w == 'S' && g_serAvailable) {
            g_serOn = !g_serOn;
            logf_line("Key S -> SER %s", g_serOn ? "on" : "off");
        } else if (w == 'H') {
            g_hintView = !g_hintView;
            logf_line("Key H -> hint view %s", g_hintView ? "on" : "off");
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR cmdLine, int) {
    DeleteFileA(kLogName);
    logf_line("D3D12.RayTracing12 starting (cmdline: \"%s\")", cmdLine ? cmdLine : "");
    const bool forceHw = cmdLine && strstr(cmdLine, "--hw");
    const bool forceWarp = cmdLine && strstr(cmdLine, "--warp");
    g_debugLayer = cmdLine && strstr(cmdLine, "--debug");
    if (const char* m = cmdLine ? strstr(cmdLine, "--mode=") : nullptr) g_mode = std::min(3, std::max(0, atoi(m + 7)));
    if (cmdLine && strstr(cmdLine, "--ser=0")) g_serOn = false;
    if (cmdLine && strstr(cmdLine, "--hint")) g_hintView = true;

    if (g_debugLayer) {
        ID3D12Debug* dbg = nullptr;
        HRESULT hr = D3D12GetDebugInterface(__uuidof(ID3D12Debug), (void**)&dbg);
        if (SUCCEEDED(hr)) { dbg->EnableDebugLayer(); dbg->Release(); }
        logf_line("D3D12 debug layer requested: hr=0x%08X", (unsigned)hr);
    }

    WNDCLASS wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = "D3D12RT12";
    RegisterClass(&wc);
    RECT r = { 0, 0, (LONG)g_width, (LONG)g_height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindow("D3D12RT12", "D3D12 - Ray Tracing 1.2 (OMM + SER)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, hInst, nullptr);
    if (!InitD3D(g_hwnd, forceHw, forceWarp)) {
        logf_line("D3D12 ray tracing init failed");
        DumpDebugMessages("!init failure");
        return 1;
    }
    logf_line("Init OK: path=%s device=%s tier=%s SM=%d.%d SER available=%u actually-reorders=%d start mode=%s",
        g_dxr12 ? "DXR 1.2 (OMM + SER)" : "DXR 1.0 fallback", g_deviceName.c_str(), TierName(g_rtTier),
        g_shaderModel >> 4, g_shaderModel & 0xF, g_serAvailable ? 1u : 0u, (int)g_serActuallyReorders, kModeNames[g_mode]);
    DumpDebugMessages("!after init");
    ShowWindow(g_hwnd, SW_SHOW);

    DWORD start = GetTickCount(), lastTitle = start;
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        else {
            Render((GetTickCount() - start) / 1000.0f);
            if (GetTickCount() - lastTitle >= 500) { lastTitle = GetTickCount(); DumpDebugMessages("periodic"); UpdateTitle(); }
        }
    }
    WaitForGpu();
    DumpDebugMessages("!exit");
    return (int)msg.wParam;
}
