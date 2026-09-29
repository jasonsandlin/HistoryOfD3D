// D3D12 Partial Graphics Programs teaching sample (Agility SDK 1.721-preview).
//
// Teaches the "compile once, link many" split of pipeline creation. One VS and one
// PS are compiled by the DRIVER exactly once, as two *partial graphics programs*
// inside a state-object COLLECTION (the expensive step). Five *generic programs*
// are then LINKED from those partials, each adding only the cheap fixed-function
// state that differs (blend / rasterizer / depth-stencil). The scene draws three
// rotating cubes side by side - left OPAQUE, middle ADDITIVE, right ALPHA-blended -
// in front of an opaque colored bar, so you can see the bar glow through the middle
// cube and show through the right one. Every draw is preceded by
// ID3D12GraphicsCommandList10::SetProgram(<generic program identifier>).
//
// The timing lesson is in the title bar: collection compile time vs per-program link
// time vs the classic way (one full CreateGraphicsPipelineState per variant).
//
// Keys: 1/2/3 solo the opaque/additive/alpha cube (press again or 0 for all),
//       W wireframe the opaque cube (a 5th linked program), P draw via generic
//       programs <-> classic PSOs (same image), ESC quits.
// Args: --hw (never fall back to WARP), --warp (force WARP), --debug (D3D12 debug
//       layer; messages go to the log), --pso (start in classic-PSO mode).
//
// REQUIRES the vendored preview Agility SDK + DXC + WARP (run fetch-deps.ps1).
// Hardware support needs a preview driver; on an RTX 2070 (driver 591.86) the tier
// is 0, so the sample falls back to the preview WARP software device, and if even
// that fails it renders the plain single-PSO cube.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>              // resolves to third_party\agility\include (build-all /I)
// Only the d3dx12 pieces we use (the umbrella d3dx12.h also pulls in the state-object
// database helper, which needs C++17 <optional>; this course builds with the default).
#include <d3dx12/d3dx12_core.h>
#include <d3dx12/d3dx12_barriers.h>
#include <d3dx12/d3dx12_root_signature.h>
#include <d3dx12/d3dx12_state_object.h>   // CD3DX12_STATE_OBJECT_DESC + partial-program helpers
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <DirectXMath.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>

using namespace DirectX;

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "version.lib")

// [LEARN] Agility SDK opt-in: the OS d3d12.dll sees these exports and loads the
// preview D3D12Core.dll from .\D3D12\ next to the exe instead of the inbox runtime.
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 721;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

static const char* kLogName = "CubeD3D12.PartialPrograms.log";
static const UINT  kFrames = 2;
static const DXGI_FORMAT kRtFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
static const DXGI_FORMAT kDsFormat = DXGI_FORMAT_D32_FLOAT;

struct Vertex { XMFLOAT3 pos; XMFLOAT4 color; };
struct RootData { XMFLOAT4X4 mvp; XMFLOAT4 params; };   // params.x = rgb scale, .y = alpha
static const UINT kRootConstants = sizeof(RootData) / 4;

// ---------------------------------------------------------------------------------
// The five pipeline variants. They share the VS, PS, input layout, topology and
// RT/DS formats (all of which live in the precompiled partials) and differ ONLY in
// state the PS partial declared "late link": blend, rasterizer and depth-stencil.
// ---------------------------------------------------------------------------------
enum BlendMode { BLEND_OPAQUE, BLEND_ADDITIVE, BLEND_ALPHA };
enum ProgramId { PROG_OPAQUE, PROG_ADDITIVE, PROG_ALPHA_BACK, PROG_ALPHA_FRONT, PROG_WIRE, PROG_COUNT };

struct Variant {
    const wchar_t*  programName;   // generic program export name in the state object
    const char*     label;
    BlendMode       blend;
    D3D12_FILL_MODE fill;
    D3D12_CULL_MODE cull;
    bool            depthWrite;
};

// [LEARN] Transparent cubes: depth TEST stays on (so the opaque cube/bar still hide
// what is behind them) but depth WRITE is off, so blended faces never occlude each
// other. The alpha cube is drawn twice - back faces (cull front) then front faces
// (cull back) - which is the classic correct back-to-front order for a convex mesh.
// Additive blending is order-independent, so it just draws both sides at once.
static const Variant g_variants[PROG_COUNT] = {
    { L"OpaqueProgram",     "opaque",      BLEND_OPAQUE,   D3D12_FILL_MODE_SOLID,     D3D12_CULL_MODE_BACK,  true  },
    { L"AdditiveProgram",   "additive",    BLEND_ADDITIVE, D3D12_FILL_MODE_SOLID,     D3D12_CULL_MODE_NONE,  false },
    { L"AlphaBackProgram",  "alpha-back",  BLEND_ALPHA,    D3D12_FILL_MODE_SOLID,     D3D12_CULL_MODE_FRONT, false },
    { L"AlphaFrontProgram", "alpha-front", BLEND_ALPHA,    D3D12_FILL_MODE_SOLID,     D3D12_CULL_MODE_BACK,  false },
    { L"WireProgram",       "wireframe",   BLEND_OPAQUE,   D3D12_FILL_MODE_WIREFRAME, D3D12_CULL_MODE_NONE,  true  },
};

static UINT g_width = 800, g_height = 600;
static HWND                        g_hwnd = nullptr;
static ID3D12Device*               g_dev = nullptr;
static ID3D12CommandQueue*         g_queue = nullptr;
static IDXGISwapChain3*            g_swap = nullptr;
static ID3D12DescriptorHeap*       g_rtvHeap = nullptr;
static ID3D12DescriptorHeap*       g_dsvHeap = nullptr;
static UINT                        g_rtvSize = 0;
static ID3D12Resource*             g_rtBuffers[kFrames] = {};
static ID3D12Resource*             g_depth = nullptr;
static ID3D12CommandAllocator*     g_alloc = nullptr;
static ID3D12GraphicsCommandList*  g_list = nullptr;
static ID3D12GraphicsCommandList10* g_list10 = nullptr;
static ID3D12RootSignature*        g_rootSig = nullptr;
static ID3D12PipelineState*        g_psos[PROG_COUNT] = {};
static ID3D12StateObject*          g_collection = nullptr;
static ID3D12StateObject*          g_linked[PROG_COUNT] = {};
static D3D12_PROGRAM_IDENTIFIER    g_programIds[PROG_COUNT] = {};
static ID3D12Resource*             g_vb = nullptr;
static ID3D12Resource*             g_ib = nullptr;
static D3D12_VERTEX_BUFFER_VIEW    g_vbv = {};
static D3D12_INDEX_BUFFER_VIEW     g_ibv = {};
static ID3D12Fence*                g_fence = nullptr;
static UINT64                      g_fenceValue = 0;
static HANDLE                      g_fenceEvent = nullptr;
static UINT                        g_frameIndex = 0;
static ID3D12InfoQueue*            g_infoQueue = nullptr;

// Options + state shown in the title.
static bool        g_optHw = false, g_optWarp = false, g_optDebug = false, g_optPso = false;
static bool        g_experimentEnabled = false;
static std::string g_deviceLabel = "no device";
static UINT        g_tier = 0;
static bool        g_partialReady = false;     // partial-program path fully built
static bool        g_classicReady = false;     // all classic PSOs built
static bool        g_usePrograms = true;       // P toggles
static bool        g_wire = false;             // W toggles
static int         g_solo = -1;                // 1/2/3 -> 0/1/2
static double      g_msDxc = 0, g_msCollection = 0, g_msLinkTotal = 0, g_msPsoTotal = 0;
static double      g_msLink[PROG_COUNT] = {}, g_msPso[PROG_COUNT] = {};
static double      g_msFirstDrawProg = 0, g_msFirstDrawPso = 0;
static UINT        g_debugErrors = 0, g_debugWarnings = 0;
static LARGE_INTEGER g_qpcFreq = {};

static void logf_line(const char* fmt, ...) {
    FILE* f = nullptr;
    if (fopen_s(&f, kLogName, "a") != 0 || !f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static double NowMs() {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)g_qpcFreq.QuadPart;
}

static std::string Narrow(const wchar_t* w) {
    char buf[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
    return buf;
}

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf);
    size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : s.substr(0, slash + 1);
}

static std::string FileVersion(const wchar_t* path) {
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(path, &dummy);
    if (!size) return "?";
    std::string data(size, '\0');
    if (!GetFileVersionInfoW(path, 0, size, &data[0])) return "?";
    VS_FIXEDFILEINFO* ffi = nullptr; UINT len = 0;
    if (!VerQueryValueW(&data[0], L"\\", (void**)&ffi, &len) || !ffi) return "?";
    char out[64];
    sprintf_s(out, "%u.%u.%u.%u", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
        HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
    return out;
}

// [LEARN] With --debug every validation message is copied into the log, so a
// run can prove "no debug-layer errors" without attaching a debugger.
static void DrainDebugMessages() {
    if (!g_infoQueue) return;
    UINT64 n = g_infoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        g_infoQueue->GetMessage(i, nullptr, &len);
        std::string buf(len, '\0');
        D3D12_MESSAGE* msg = (D3D12_MESSAGE*)&buf[0];
        if (FAILED(g_infoQueue->GetMessage(i, msg, &len))) continue;
        const char* sev = "INFO";
        switch (msg->Severity) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION: sev = "CORRUPTION"; ++g_debugErrors; break;
        case D3D12_MESSAGE_SEVERITY_ERROR:      sev = "ERROR"; ++g_debugErrors; break;
        case D3D12_MESSAGE_SEVERITY_WARNING:    sev = "WARNING"; ++g_debugWarnings; break;
        case D3D12_MESSAGE_SEVERITY_MESSAGE:    sev = "MESSAGE"; break;
        default: break;
        }
        if (msg->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            logf_line("[debug layer %s id=%d] %s", sev, (int)msg->ID, msg->pDescription);
    }
    g_infoQueue->ClearStoredMessages();
}

// ---------------------------------------------------------------------------------
// Runtime HLSL -> DXIL with the vendored preview DXC (copied next to the exe).
// ---------------------------------------------------------------------------------
// [LEARN] The PS carries a 96-step unrolled "Material" block. It is switched off at
// run time by a uniform branch (params.z == 0) so the cubes look like every other
// sample, but the DRIVER still has to compile all of it - a stand-in for a real
// game material shader. A two-line PS compiles in microseconds and would hide the
// cost this sample is about.
static const char* g_shaderSrc =
"cbuffer CB : register(b0) { float4x4 mvp; float4 params; };\n"
"struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
"VSOut VSMain(float3 pos : POSITION, float4 col : COLOR) {\n"
"  VSOut o; o.pos = mul(float4(pos, 1.0), mvp); o.col = col; return o;\n"
"}\n"
"float3 Material(float3 c) {\n"
"  float3 acc = 0;\n"
"  [unroll] for (int k = 0; k < 96; ++k) {\n"
"    float f = 1.0 + k * 0.37;\n"
"    acc += sin(c * f + acc.zxy) * cos(c.yzx * (f * 0.5) - acc) / f;\n"
"  }\n"
"  return acc;\n"
"}\n"
"float4 PSMain(VSOut i) : SV_TARGET {\n"
"  float3 rgb = i.col.rgb * params.x;\n"
"  [branch] if (params.z != 0) rgb += Material(i.col.rgb) * params.z;\n"
"  return float4(rgb, params.y);\n"
"}\n";

static HMODULE TryLoadDxcFromDir(const std::wstring& dir, const char* what) {
    std::wstring dxil = dir + L"dxil.dll", dxc = dir + L"dxcompiler.dll";
    if (GetFileAttributesW(dxc.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
    LoadLibraryExW(dxil.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);   // validator/signer
    HMODULE m = LoadLibraryExW(dxc.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (m) logf_line("Loaded DXC (%s) from %ls version=%s", what, dxc.c_str(), FileVersion(dxc.c_str()).c_str());
    else   logf_line("LoadLibrary DXC %ls failed err=%lu", dxc.c_str(), GetLastError());
    return m;
}

static HMODULE LoadDxc() {
    std::wstring exe = ExeDir();
    HMODULE m = TryLoadDxcFromDir(exe, "vendored preview, next to exe");
    if (!m) m = TryLoadDxcFromDir(exe + L"..\\third_party\\dxc\\bin\\x64\\", "vendored preview, third_party");
    static const wchar_t* kits[] = {
        L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.26100.0\\x64\\",
        L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.22621.0\\x64\\",
    };
    for (auto d : kits) { if (!m) m = TryLoadDxcFromDir(d, "Windows SDK"); }
    if (!m) logf_line("dxcompiler.dll not found; cannot compile shaders");
    return m;
}

static IDxcBlob* CompileDXC(const wchar_t* entry, const wchar_t* target) {
    static HMODULE dll = LoadDxc();
    if (!dll) return nullptr;
    auto create = (DxcCreateInstanceProc)GetProcAddress(dll, "DxcCreateInstance");
    if (!create) { logf_line("DxcCreateInstance export not found"); return nullptr; }
    IDxcCompiler3* comp = nullptr;
    HRESULT hr = create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), (void**)&comp);
    if (FAILED(hr) || !comp) { logf_line("DxcCreateInstance hr=0x%08X", (unsigned)hr); return nullptr; }
    DxcBuffer buf = { g_shaderSrc, strlen(g_shaderSrc), DXC_CP_UTF8 };
    const wchar_t* args[] = { L"-E", entry, L"-T", target, L"-O3", L"-HV", L"2021" };
    IDxcResult* result = nullptr;
    hr = comp->Compile(&buf, args, _countof(args), nullptr, __uuidof(IDxcResult), (void**)&result);
    comp->Release();
    if (FAILED(hr) || !result) { logf_line("DXC Compile %ls hr=0x%08X", target, (unsigned)hr); return nullptr; }
    HRESULT status = E_FAIL; result->GetStatus(&status);
    if (FAILED(status)) {
        IDxcBlobUtf8* errs = nullptr;
        result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), (void**)&errs, nullptr);
        if (errs && errs->GetStringLength()) logf_line("DXC %ls errors: %s", target, errs->GetStringPointer());
        if (errs) errs->Release();
        result->Release();
        return nullptr;
    }
    IDxcBlob* obj = nullptr;
    result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), (void**)&obj, nullptr);
    result->Release();
    return obj;
}

// ---------------------------------------------------------------------------------
// Fixed-function state builders, shared by the linked programs and the classic PSOs
// so both paths are guaranteed to describe the exact same pipelines.
// ---------------------------------------------------------------------------------
static D3D12_BLEND_DESC MakeBlend(BlendMode mode) {
    D3D12_BLEND_DESC b = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    D3D12_RENDER_TARGET_BLEND_DESC& rt = b.RenderTarget[0];
    rt.BlendEnable = mode != BLEND_OPAQUE;
    rt.SrcBlend  = mode == BLEND_ALPHA ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_ONE;
    rt.DestBlend = mode == BLEND_ALPHA ? D3D12_BLEND_INV_SRC_ALPHA
                 : mode == BLEND_ADDITIVE ? D3D12_BLEND_ONE : D3D12_BLEND_ZERO;
    rt.BlendOp = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D12_BLEND_ONE; rt.DestBlendAlpha = D3D12_BLEND_ZERO; rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    return b;
}

static D3D12_DEPTH_STENCIL_DESC MakeDepth(bool write) {
    D3D12_DEPTH_STENCIL_DESC d = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);   // test on, LESS
    d.DepthWriteMask = write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    return d;
}

static const D3D12_INPUT_ELEMENT_DESC g_inputLayout[] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};

// ---------------------------------------------------------------------------------
// Capability query + device selection (hardware first, preview WARP second).
// ---------------------------------------------------------------------------------
static bool QueryPartialPrograms(ID3D12Device* dev, const char* who, UINT* tierOut) {
    D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_SHADER_MODEL_6_8 };
    HRESULT smHr = dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
    D3D12_FEATURE_DATA_PARTIAL_GRAPHICS_PROGRAMS pgp = {};
    HRESULT hr = dev->CheckFeatureSupport(D3D12_FEATURE_PARTIAL_GRAPHICS_PROGRAMS, &pgp, sizeof(pgp));
    UINT tier = SUCCEEDED(hr) ? (UINT)pgp.PartialGraphicsProgramsTier : 0;
    bool smOk = SUCCEEDED(smHr) && sm.HighestShaderModel >= D3D_SHADER_MODEL_6_8;
    bool ok = tier >= (UINT)D3D12_PARTIAL_GRAPHICS_PROGRAMS_TIER_1_0 && smOk;
    logf_line("[%s] D3D12_FEATURE_SHADER_MODEL hr=0x%08X highest=0x%X (SM 6.8 device required: %s)",
        who, (unsigned)smHr, (unsigned)sm.HighestShaderModel, smOk ? "yes" : "NO");
    logf_line("[%s] D3D12_FEATURE_PARTIAL_GRAPHICS_PROGRAMS hr=0x%08X PartialGraphicsProgramsTier=%u (%s) -> %s",
        who, (unsigned)hr, tier, tier >= 10 ? "TIER_1_0" : "NOT_SUPPORTED", ok ? "SUPPORTED" : "unsupported");
    if (tierOut) *tierOut = tier;
    return ok;
}

static ID3D12Device* CreateHardwareDevice(IDXGIFactory4* factory, std::string* name) {
    IDXGIFactory6* f6 = nullptr;
    factory->QueryInterface(__uuidof(IDXGIFactory6), (void**)&f6);
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* ad = nullptr;
        HRESULT hr = f6 ? f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1), (void**)&ad)
                        : factory->EnumAdapters1(i, &ad);
        if (hr == DXGI_ERROR_NOT_FOUND || !ad) break;
        DXGI_ADAPTER_DESC1 d = {}; ad->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { ad->Release(); continue; }
        ID3D12Device* dev = nullptr;
        hr = D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dev);
        logf_line("D3D12CreateDevice hardware '%ls' (VendorId=0x%04X DeviceId=0x%04X) hr=0x%08X",
            d.Description, d.VendorId, d.DeviceId, (unsigned)hr);
        ad->Release();
        if (SUCCEEDED(hr) && dev) {
            *name = Narrow(d.Description);
            if (f6) f6->Release();
            return dev;
        }
    }
    if (f6) f6->Release();
    return nullptr;
}

static ID3D12Device* CreateWarpDevice(IDXGIFactory4* factory, std::string* name) {
    IDXGIAdapter* warp = nullptr;
    HRESULT hr = factory->EnumWarpAdapter(__uuidof(IDXGIAdapter), (void**)&warp);
    if (FAILED(hr) || !warp) { logf_line("EnumWarpAdapter hr=0x%08X", (unsigned)hr); return nullptr; }
    ID3D12Device* dev = nullptr;
    hr = D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dev);
    warp->Release();
    logf_line("D3D12CreateDevice WARP hr=0x%08X", (unsigned)hr);
    if (FAILED(hr)) return nullptr;
    // [LEARN] WARP lives in d3d10warp.dll. Because the preview copy sits next to
    // the exe, normal DLL search order picks it over System32 - prove it here.
    wchar_t path[MAX_PATH] = {};
    HMODULE m = GetModuleHandleW(L"d3d10warp.dll");
    if (m) GetModuleFileNameW(m, path, MAX_PATH);
    std::wstring exeDir = ExeDir();
    bool preview = m && _wcsnicmp(path, exeDir.c_str(), exeDir.size()) == 0;
    logf_line("WARP module: %ls version=%s (%s)", m ? path : L"<not loaded>",
        m ? FileVersion(path).c_str() : "?", preview ? "vendored PREVIEW WARP" : "system WARP");
    *name = preview ? "WARP (preview)" : "WARP (system)";
    return dev;
}

static bool SelectDevice() {
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void**)&factory);
    if (FAILED(hr)) { logf_line("CreateDXGIFactory2 hr=0x%08X", (unsigned)hr); return false; }

    std::string hwName, warpName;
    UINT hwTier = 0, warpTier = 0;
    ID3D12Device* hw = nullptr;
    bool hwOk = false;
    if (!g_optWarp) {
        hw = CreateHardwareDevice(factory, &hwName);
        hwOk = hw && QueryPartialPrograms(hw, hwName.c_str(), &hwTier);
    }
    if (hwOk || (g_optHw && hw)) {
        g_dev = hw; g_deviceLabel = hwName; g_tier = hwTier;
        if (!hwOk) logf_line("--hw given: staying on '%s' without partial programs (classic PSO cube)", hwName.c_str());
    } else {
        if (hw) logf_line("'%s' lacks partial graphics programs; recreating the device on WARP", hwName.c_str());
        ID3D12Device* warp = CreateWarpDevice(factory, &warpName);
        bool warpOk = warp && QueryPartialPrograms(warp, warpName.c_str(), &warpTier);
        if (warpOk || !hw) {
            if (hw) hw->Release();
            g_dev = warp; g_deviceLabel = warpName; g_tier = warpTier;
        } else {
            logf_line("WARP also lacks partial programs; using '%s' for the classic PSO cube", hwName.c_str());
            if (warp) warp->Release();
            g_dev = hw; g_deviceLabel = hwName; g_tier = hwTier;
        }
    }
    factory->Release();
    if (!g_dev) { logf_line("No D3D12 device could be created"); return false; }
    logf_line("Using device: %s (partial programs tier %u)", g_deviceLabel.c_str(), g_tier);
    return true;
}

// ---------------------------------------------------------------------------------
// THE LESSON: build the collection (compile once), then link generic programs.
// ---------------------------------------------------------------------------------
// [LEARN] Part V recap: a classic PSO bakes EVERYTHING - shaders plus blend, raster,
// depth, formats - into one object, and the driver compiles DXIL -> GPU ISA for
// every PSO even when two PSOs share byte-identical shaders and differ only in a
// blend factor. Partial graphics programs split that into:
//   1. COLLECTION: partial programs that hold only the state that actually changes
//      the generated shader code (input layout + topology for the pre-raster side;
//      RT/DS formats + AlphaToCoverage/DualSource for the PS side). The driver
//      compiles them HERE, once.
//   2. EXECUTABLE: generic programs that name those partials as exports and add
//      the "late-link" state (blend, rasterizer, depth-stencil). That is a link, not
//      a compile - no new shader code should be generated.
// [LEARN] Part X recap: ID3D12PipelineLibrary caches FINISHED PSOs so the second
// launch is fast - but the first launch still compiles every blend x raster x depth
// combination separately. Partial programs shrink the number of real compiles to
// the number of distinct SHADERS; the combinatorial explosion moves into cheap links.
// [LEARN] Advanced Shader Delivery ties it together: the store/cloud precompiles the
// COLLECTIONS once per GPU + driver (a handful of partials instead of every PSO
// permutation), the game downloads them, and at load time only the links run - so
// the precompile farm does less duplicated work and players see no compile stutter.
static bool BuildPartialPrograms(IDxcBlob* vs, IDxcBlob* ps) {
    ID3D12Device7* dev7 = nullptr;
    HRESULT hr = g_dev->QueryInterface(__uuidof(ID3D12Device7), (void**)&dev7);
    if (FAILED(hr) || !dev7) { logf_line("ID3D12Device7 unavailable hr=0x%08X", (unsigned)hr); return false; }

    // ---- 1. Collection: the one expensive compile --------------------------------
    CD3DX12_STATE_OBJECT_DESC col;
    col.SetStateObjectType(D3D12_STATE_OBJECT_TYPE_COLLECTION);
    auto cfg = col.CreateSubobject<CD3DX12_STATE_OBJECT_CONFIG_SUBOBJECT>();
    cfg->SetFlags(D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS);
    auto rs = col.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
    rs->SetRootSignature(g_rootSig);
    // [LEARN] Ordinary vs_6_0 / ps_6_0 blobs are handed over as DXIL libraries; their
    // entry points (VSMain, PSMain) become exports the partials can reference.
    auto libVS = col.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
    CD3DX12_SHADER_BYTECODE bcVS(vs->GetBufferPointer(), vs->GetBufferSize());
    libVS->SetDXILLibrary(&bcVS);
    auto libPS = col.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
    CD3DX12_SHADER_BYTECODE bcPS(ps->GetBufferPointer(), ps->GetBufferSize());
    libPS->SetDXILLibrary(&bcPS);

    auto il = col.CreateSubobject<CD3DX12_INPUT_LAYOUT_SUBOBJECT>();
    for (const auto& e : g_inputLayout) il->AddInputLayoutElementDesc(e);
    auto topo = col.CreateSubobject<CD3DX12_PRIMITIVE_TOPOLOGY_SUBOBJECT>();
    topo->SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    auto rtFmt = col.CreateSubobject<CD3DX12_RENDER_TARGET_FORMATS_SUBOBJECT>();
    rtFmt->SetNumRenderTargets(1);
    rtFmt->SetRenderTargetFormat(0, kRtFormat);
    auto dsFmt = col.CreateSubobject<CD3DX12_DEPTH_STENCIL_FORMAT_SUBOBJECT>();
    dsFmt->SetDepthStencilFormat(kDsFormat);

    // [LEARN] The PS partial's "fields" subobject pins the few blend/raster values
    // that change PS codegen (AlphaToCoverage, DualSourceBlend, LineRasterizationMode,
    // ForcedSampleCount) and declares which whole subobjects will be supplied LATER
    // at link time. Anything not marked late-link and not given here is assumed to be
    // the default and can never be overridden - so we opt in explicitly for blend,
    // rasterizer (cull + wireframe) and depth-stencil (depth write on/off).
    auto psFields = col.CreateSubobject<CD3DX12_PIXEL_SHADER_PARTIAL_PROGRAM_FIELDS_SUBOBJECT>();
    psFields->SetAlphaToCoverageEnable(FALSE);
    psFields->SetDualSourceBlendEnable(FALSE);
    psFields->SetLateLinkBlendSubobject(TRUE);
    psFields->SetLateLinkRasterizerSubobject(TRUE);
    psFields->SetLateLinkDepthStencilSubobject(TRUE);

    auto preRast = col.CreateSubobject<CD3DX12_PARTIAL_GRAPHICS_PROGRAM_SUBOBJECT>();
    preRast->SetProgramName(L"VSPartial");
    preRast->SetPartialGraphicsProgramType(D3D12_PARTIAL_GRAPHICS_PROGRAM_TYPE_PRERASTERIZATION_SHADER);
    preRast->AddExport(L"VSMain");
    preRast->AddSubobject(*il);
    preRast->AddSubobject(*topo);

    auto psProg = col.CreateSubobject<CD3DX12_PARTIAL_GRAPHICS_PROGRAM_SUBOBJECT>();
    psProg->SetProgramName(L"PSPartial");
    psProg->SetPartialGraphicsProgramType(D3D12_PARTIAL_GRAPHICS_PROGRAM_TYPE_PIXEL_SHADER);
    psProg->AddExport(L"PSMain");
    psProg->AddSubobject(*rtFmt);
    psProg->AddSubobject(*dsFmt);
    psProg->AddSubobject(*psFields);

    double t0 = NowMs();
    hr = dev7->CreateStateObject(col, __uuidof(ID3D12StateObject), (void**)&g_collection);
    g_msCollection = NowMs() - t0;
    logf_line("CreateStateObject(COLLECTION: VSPartial + PSPartial) hr=0x%08X  %.3f ms  <- the one real compile",
        (unsigned)hr, g_msCollection);
    DrainDebugMessages();
    if (FAILED(hr) || !g_collection) { dev7->Release(); return false; }

    // ---- 2. Link one generic program per variant --------------------------------
    // [LEARN] The first program creates the EXECUTABLE state object that imports the
    // collection; each further program is appended with AddToStateObject, so the
    // app can link new variants on demand (e.g. the first time a material needs
    // them) without touching programs already in use. PREFER_MINIMAL_LINK asks the
    // driver to reuse the collection's compiled code instead of re-specializing.
    D3D12_STATE_OBJECT_FLAGS linkFlags =
        D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS | D3D12_STATE_OBJECT_FLAG_PREFER_MINIMAL_LINK;
    for (UINT i = 0; i < PROG_COUNT; ++i) {
        const Variant& v = g_variants[i];
        for (int attempt = 0; attempt < 2; ++attempt) {
            CD3DX12_STATE_OBJECT_DESC exe;
            exe.SetStateObjectType(D3D12_STATE_OBJECT_TYPE_EXECUTABLE);
            auto ecfg = exe.CreateSubobject<CD3DX12_STATE_OBJECT_CONFIG_SUBOBJECT>();
            ecfg->SetFlags(linkFlags);
            if (i == 0) {
                auto existing = exe.CreateSubobject<CD3DX12_EXISTING_COLLECTION_SUBOBJECT>();
                existing->SetExistingCollection(g_collection);
            }
            auto blend = exe.CreateSubobject<CD3DX12_BLEND_SUBOBJECT>();
            static_cast<D3D12_BLEND_DESC&>(*blend) = MakeBlend(v.blend);
            auto rast = exe.CreateSubobject<CD3DX12_RASTERIZER_SUBOBJECT>();
            rast->SetFillMode(v.fill);
            rast->SetCullMode(v.cull);
            auto depth = exe.CreateSubobject<CD3DX12_DEPTH_STENCIL_SUBOBJECT>();
            depth->SetDepthEnable(TRUE);
            depth->SetDepthFunc(D3D12_COMPARISON_FUNC_LESS);
            depth->SetDepthWriteMask(v.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO);
            auto prog = exe.CreateSubobject<CD3DX12_GENERIC_PROGRAM_SUBOBJECT>();
            prog->SetProgramName(v.programName);
            prog->AddExport(L"VSPartial");     // partials are referenced by NAME only
            prog->AddExport(L"PSPartial");
            prog->AddSubobject(*blend);
            prog->AddSubobject(*rast);
            prog->AddSubobject(*depth);

            double l0 = NowMs();
            if (i == 0) hr = dev7->CreateStateObject(exe, __uuidof(ID3D12StateObject), (void**)&g_linked[i]);
            else        hr = dev7->AddToStateObject(exe, g_linked[i - 1], __uuidof(ID3D12StateObject), (void**)&g_linked[i]);
            g_msLink[i] = NowMs() - l0;
            logf_line("%s(%ls: blend=%s fill=%s cull=%d depthWrite=%u flags=0x%X) hr=0x%08X  %.3f ms  <- link only",
                i == 0 ? "CreateStateObject(EXECUTABLE" : "AddToStateObject(", v.programName,
                v.blend == BLEND_OPAQUE ? "opaque" : v.blend == BLEND_ADDITIVE ? "additive" : "alpha",
                v.fill == D3D12_FILL_MODE_WIREFRAME ? "wire" : "solid", (int)v.cull, v.depthWrite ? 1u : 0u,
                (unsigned)linkFlags, (unsigned)hr, g_msLink[i]);
            DrainDebugMessages();
            if (SUCCEEDED(hr)) break;
            if (attempt == 0 && i == 0 && (linkFlags & D3D12_STATE_OBJECT_FLAG_PREFER_MINIMAL_LINK)) {
                logf_line("Retrying the link without PREFER_MINIMAL_LINK");
                linkFlags = D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS;
                continue;
            }
            dev7->Release();
            return false;
        }
        ID3D12StateObjectProperties1* props = nullptr;
        hr = g_linked[i]->QueryInterface(__uuidof(ID3D12StateObjectProperties1), (void**)&props);
        if (FAILED(hr) || !props) { logf_line("ID3D12StateObjectProperties1 hr=0x%08X", (unsigned)hr); dev7->Release(); return false; }
        g_programIds[i] = props->GetProgramIdentifier(v.programName);
        props->Release();
        g_msLinkTotal += g_msLink[i];
        logf_line("  program identifier %ls = %016llX...", v.programName, (unsigned long long)g_programIds[i].OpaqueData[0]);
    }
    dev7->Release();
    return true;
}

// [LEARN] The comparison: the classic way needs a FULL PSO per variant, and each
// CreateGraphicsPipelineState hands the driver the whole VS+PS to compile again.
// Part X's pipeline libraries (ID3D12PipelineLibrary) only help on the SECOND run by
// caching finished PSOs on disk; partial programs cut the work on the FIRST run.
// [LEARN] Measured on the RTX 2070 (--hw): the first PSO costs ~80 ms and the next
// four ~0.1 ms, because NVIDIA does blend/raster/depth in fixed function and its
// driver quietly dedupes identical shaders. That is a heuristic that varies per
// vendor (hardware that folds blend or formats into shader code must recompile).
// Partial programs turn "compile the shaders once" into an API CONTRACT that every
// driver, and an offline precompile service, can rely on.
static bool BuildClassicPsos(IDxcBlob* vs, IDxcBlob* ps) {
    bool ok = true;
    for (UINT i = 0; i < PROG_COUNT; ++i) {
        const Variant& v = g_variants[i];
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
        d.pRootSignature = g_rootSig;
        d.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        d.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        d.InputLayout = { g_inputLayout, _countof(g_inputLayout) };
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = kRtFormat;
        d.DSVFormat = kDsFormat;
        d.SampleDesc.Count = 1;
        d.SampleMask = UINT_MAX;
        d.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        d.RasterizerState.FillMode = v.fill;
        d.RasterizerState.CullMode = v.cull;
        d.BlendState = MakeBlend(v.blend);
        d.DepthStencilState = MakeDepth(v.depthWrite);
        double t0 = NowMs();
        HRESULT hr = g_dev->CreateGraphicsPipelineState(&d, __uuidof(ID3D12PipelineState), (void**)&g_psos[i]);
        g_msPso[i] = NowMs() - t0;
        g_msPsoTotal += g_msPso[i];
        logf_line("CreateGraphicsPipelineState(%s) hr=0x%08X  %.3f ms  <- full compile", v.label, (unsigned)hr, g_msPso[i]);
        if (FAILED(hr)) ok = false;
    }
    DrainDebugMessages();
    return ok;
}

// ---------------------------------------------------------------------------------
// Boilerplate: buffers, swap chain, depth, root signature.
// ---------------------------------------------------------------------------------
static void WaitForGpu() {
    const UINT64 v = ++g_fenceValue;
    g_queue->Signal(g_fence, v);
    if (g_fence->GetCompletedValue() < v) {
        g_fence->SetEventOnCompletion(v, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
    g_frameIndex = g_swap->GetCurrentBackBufferIndex();
}

static ID3D12Resource* CreateUploadBuffer(const void* data, UINT64 size) {
    CD3DX12_HEAP_PROPERTIES hp(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(size);
    ID3D12Resource* res = nullptr;
    HRESULT hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&res);
    if (FAILED(hr) || !res) { logf_line("CreateCommittedResource upload hr=0x%08X", (unsigned)hr); return nullptr; }
    void* mapped = nullptr; D3D12_RANGE none = { 0, 0 };
    if (SUCCEEDED(res->Map(0, &none, &mapped)) && mapped) { memcpy(mapped, data, (size_t)size); res->Unmap(0, nullptr); }
    return res;
}

static bool InitD3D(HWND hwnd) {
    if (g_optDebug) {
        // [LEARN] The debug layer must be enabled BEFORE the device is created.
        ID3D12Debug* dbg = nullptr;
        HRESULT hr = D3D12GetDebugInterface(__uuidof(ID3D12Debug), (void**)&dbg);
        logf_line("D3D12GetDebugInterface hr=0x%08X", (unsigned)hr);
        if (dbg) { dbg->EnableDebugLayer(); dbg->Release(); logf_line("D3D12 debug layer ENABLED"); }
    }
    if (!SelectDevice()) return false;
    if (g_optDebug) {
        g_dev->QueryInterface(__uuidof(ID3D12InfoQueue), (void**)&g_infoQueue);
        logf_line("ID3D12InfoQueue %s", g_infoQueue ? "attached" : "unavailable");
    }

    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT hr = g_dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&g_queue);
    if (FAILED(hr)) { logf_line("CreateCommandQueue hr=0x%08X", (unsigned)hr); return false; }

    IDXGIFactory4* factory = nullptr;
    CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), (void**)&factory);
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.BufferCount = kFrames; sd.Width = g_width; sd.Height = g_height;
    sd.Format = kRtFormat; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.SampleDesc.Count = 1;
    IDXGISwapChain1* sc1 = nullptr;
    hr = factory ? factory->CreateSwapChainForHwnd(g_queue, hwnd, &sd, nullptr, nullptr, &sc1) : E_FAIL;
    if (factory) factory->Release();
    if (FAILED(hr) || !sc1) { logf_line("CreateSwapChainForHwnd hr=0x%08X", (unsigned)hr); return false; }
    sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&g_swap);
    sc1->Release();
    if (!g_swap) return false;
    g_frameIndex = g_swap->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rh = {}; rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors = kFrames;
    g_dev->CreateDescriptorHeap(&rh, __uuidof(ID3D12DescriptorHeap), (void**)&g_rtvHeap);
    g_rtvSize = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_DESCRIPTOR_HEAP_DESC dh = {}; dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; dh.NumDescriptors = 1;
    g_dev->CreateDescriptorHeap(&dh, __uuidof(ID3D12DescriptorHeap), (void**)&g_dsvHeap);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrames; i++) {
        g_swap->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&g_rtBuffers[i]);
        g_dev->CreateRenderTargetView(g_rtBuffers[i], nullptr, rtv);
        rtv.ptr += g_rtvSize;
    }

    CD3DX12_HEAP_PROPERTIES dhp(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_RESOURCE_DESC drd = CD3DX12_RESOURCE_DESC::Tex2D(kDsFormat, g_width, g_height, 1, 1, 1, 0,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    CD3DX12_CLEAR_VALUE cv(kDsFormat, 1.0f, 0);
    hr = g_dev->CreateCommittedResource(&dhp, D3D12_HEAP_FLAG_NONE, &drd,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, __uuidof(ID3D12Resource), (void**)&g_depth);
    if (FAILED(hr)) { logf_line("CreateCommittedResource depth hr=0x%08X", (unsigned)hr); return false; }
    g_dev->CreateDepthStencilView(g_depth, nullptr, g_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    // Root signature: 20 root constants (mvp + params). It is shared by the classic
    // PSOs and (via a GLOBAL_ROOT_SIGNATURE subobject) by the collection.
    CD3DX12_ROOT_PARAMETER rp;
    rp.InitAsConstants(kRootConstants, 0);
    CD3DX12_ROOT_SIGNATURE_DESC rsd(1, &rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    ID3DBlob* sig = nullptr; ID3DBlob* sigErr = nullptr;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr);
    if (sigErr) sigErr->Release();
    if (FAILED(hr)) { logf_line("D3D12SerializeRootSignature hr=0x%08X", (unsigned)hr); return false; }
    hr = g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
        __uuidof(ID3D12RootSignature), (void**)&g_rootSig);
    sig->Release();
    if (FAILED(hr)) { logf_line("CreateRootSignature hr=0x%08X", (unsigned)hr); return false; }

    // [LEARN] DXC turns HLSL into DXIL (hardware-neutral bytecode). That is NOT the
    // expensive part being measured below - the driver's DXIL -> GPU-ISA compile is.
    double c0 = NowMs();
    IDxcBlob* vs = CompileDXC(L"VSMain", L"vs_6_0");
    IDxcBlob* ps = CompileDXC(L"PSMain", L"ps_6_0");
    g_msDxc = NowMs() - c0;
    if (!vs || !ps) { logf_line("runtime DXC compile failed"); return false; }
    logf_line("DXC HLSL->DXIL (vs_6_0 %zu bytes, ps_6_0 %zu bytes) %.3f ms (front end, done offline in a shipping game)",
        vs->GetBufferSize(), ps->GetBufferSize(), g_msDxc);

    bool supported = g_experimentEnabled && g_tier >= (UINT)D3D12_PARTIAL_GRAPHICS_PROGRAMS_TIER_1_0;
    if (supported) {
        g_partialReady = BuildPartialPrograms(vs, ps);
        logf_line("Partial graphics program path %s: collection %.3f ms, link %ux total %.3f ms (avg %.3f ms)",
            g_partialReady ? "READY" : "FAILED", g_msCollection, (unsigned)PROG_COUNT, g_msLinkTotal, g_msLinkTotal / PROG_COUNT);
    } else {
        logf_line("Partial graphics programs unavailable (experiment=%u tier=%u); classic PSO cube fallback",
            g_experimentEnabled ? 1u : 0u, g_tier);
    }
    g_classicReady = BuildClassicPsos(vs, ps);
    logf_line("Classic PSO path %s: %ux CreateGraphicsPipelineState total %.3f ms (avg %.3f ms)",
        g_classicReady ? "READY" : "FAILED", (unsigned)PROG_COUNT, g_msPsoTotal, g_msPsoTotal / PROG_COUNT);
    if (g_partialReady && g_classicReady)
        logf_line("SUMMARY: compile once %.2f ms + link %u programs %.2f ms = %.2f ms  vs  %u full PSOs %.2f ms",
            g_msCollection, (unsigned)PROG_COUNT, g_msLinkTotal, g_msCollection + g_msLinkTotal,
            (unsigned)PROG_COUNT, g_msPsoTotal);
    vs->Release(); ps->Release();
    if (!g_classicReady && !g_partialReady) return false;
    if (!g_partialReady) g_usePrograms = false;
    if (g_optPso) g_usePrograms = false;

    hr = g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&g_alloc);
    if (FAILED(hr)) { logf_line("CreateCommandAllocator hr=0x%08X", (unsigned)hr); return false; }
    hr = g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr,
        __uuidof(ID3D12GraphicsCommandList), (void**)&g_list);
    if (FAILED(hr) || !g_list) { logf_line("CreateCommandList hr=0x%08X", (unsigned)hr); return false; }
    g_list->Close();
    // [LEARN] SetProgram lives on ID3D12GraphicsCommandList10 (added for work graphs,
    // now reused for generic graphics programs).
    hr = g_list->QueryInterface(__uuidof(ID3D12GraphicsCommandList10), (void**)&g_list10);
    logf_line("QueryInterface(ID3D12GraphicsCommandList10) hr=0x%08X", (unsigned)hr);
    if (!g_list10 && g_partialReady) { g_partialReady = false; g_usePrograms = false; }

    Vertex verts[] = {
        { {-1,-1,-1}, {0.90f,0.35f,0.35f,1} }, { {-1, 1,-1}, {0.90f,0.70f,0.30f,1} },
        { { 1, 1,-1}, {0.85f,0.85f,0.35f,1} }, { { 1,-1,-1}, {0.40f,0.80f,0.45f,1} },
        { {-1,-1, 1}, {0.35f,0.75f,0.85f,1} }, { {-1, 1, 1}, {0.40f,0.55f,0.90f,1} },
        { { 1, 1, 1}, {0.65f,0.45f,0.90f,1} }, { { 1,-1, 1}, {0.92f,0.92f,0.92f,1} },
    };
    unsigned short idx[] = {
        0,1,2, 0,2,3,  4,6,5, 4,7,6,  4,5,1, 4,1,0,
        3,2,6, 3,6,7,  1,5,6, 1,6,2,  4,0,3, 4,3,7,
    };
    g_vb = CreateUploadBuffer(verts, sizeof(verts));
    g_ib = CreateUploadBuffer(idx, sizeof(idx));
    if (!g_vb || !g_ib) return false;
    g_vbv = { g_vb->GetGPUVirtualAddress(), sizeof(verts), sizeof(Vertex) };
    g_ibv = { g_ib->GetGPUVirtualAddress(), sizeof(idx), DXGI_FORMAT_R16_UINT };

    hr = g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_fence);
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    DrainDebugMessages();
    return SUCCEEDED(hr) && g_fence && g_fenceEvent;
}

// ---------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------
// [LEARN] Binding a pipeline: SetProgram(generic program identifier) replaces
// SetPipelineState(PSO). Everything else about the draw (root signature, root
// constants, IA buffers, viewports) is unchanged, which is why P can flip between
// the two paths and produce the same image.
static void BindVariant(ProgramId p) {
    if (g_usePrograms) {
        D3D12_SET_PROGRAM_DESC sp = {};
        sp.Type = D3D12_PROGRAM_TYPE_GENERIC_PIPELINE;
        sp.GenericPipeline.ProgramIdentifier = g_programIds[p];
        g_list10->SetProgram(&sp);
    } else {
        g_list->SetPipelineState(g_psos[p]);
    }
}

static void DrawCube(ProgramId p, const XMMATRIX& world, const XMMATRIX& viewProj, float rgbScale, float alpha) {
    BindVariant(p);
    RootData rd;
    XMStoreFloat4x4(&rd.mvp, XMMatrixTranspose(world * viewProj));
    rd.params = XMFLOAT4(rgbScale, alpha, 0, 0);
    g_list->SetGraphicsRoot32BitConstants(0, kRootConstants, &rd, 0);
    g_list->DrawIndexedInstanced(36, 1, 0, 0, 0);
}

static void Render(float t) {
    g_alloc->Reset();
    g_list->Reset(g_alloc, nullptr);
    g_list->SetGraphicsRootSignature(g_rootSig);
    D3D12_VIEWPORT vp = { 0, 0, (float)g_width, (float)g_height, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)g_width, (LONG)g_height };
    g_list->RSSetViewports(1, &vp);
    g_list->RSSetScissorRects(1, &sc);

    CD3DX12_RESOURCE_BARRIER toRt = CD3DX12_RESOURCE_BARRIER::Transition(g_rtBuffers[g_frameIndex],
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_list->ResourceBarrier(1, &toRt);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)g_frameIndex * g_rtvSize;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    g_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    float clear[4] = { 0.07f, 0.08f, 0.20f, 1.0f };
    g_list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    g_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_list->IASetVertexBuffers(0, 1, &g_vbv);
    g_list->IASetIndexBuffer(&g_ibv);

    XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 0.8f, -7.5f, 0), XMVectorSet(0, 0, 0.6f, 0), XMVectorSet(0, 1, 0, 0));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, (float)g_width / g_height, 0.1f, 100.0f);
    XMMATRIX viewProj = view * proj;
    XMMATRIX spin = XMMatrixRotationY(t) * XMMatrixRotationX(t * 0.5f);
    XMMATRIX cubeScale = XMMatrixScaling(0.8f, 0.8f, 0.8f);

    if (!g_partialReady && g_classicReady) {
        // Unsupported everywhere: the plain family cube with a single classic PSO.
        DrawCube(PROG_OPAQUE, spin, viewProj, 1.0f, 1.0f);
    } else {
        // Opaque first (they write depth): a long bar BEHIND the cubes so the
        // blended cubes have something visible to show through, then the left cube.
        XMMATRIX bar = XMMatrixScaling(4.9f, 0.32f, 0.32f) * XMMatrixRotationX(t * 0.8f) * XMMatrixTranslation(0, 0, 2.4f);
        DrawCube(PROG_OPAQUE, bar, viewProj, 1.0f, 1.0f);
        if (g_solo < 0 || g_solo == 0)
            DrawCube(g_wire ? PROG_WIRE : PROG_OPAQUE, cubeScale * spin * XMMatrixTranslation(-2.4f, 0, 0), viewProj, 1.0f, 1.0f);
        // Blended last, depth-test on / depth-write off.
        if (g_solo < 0 || g_solo == 1)
            DrawCube(PROG_ADDITIVE, cubeScale * spin * XMMatrixTranslation(0, 0, 0), viewProj, 0.35f, 1.0f);
        if (g_solo < 0 || g_solo == 2) {
            XMMATRIX w = cubeScale * spin * XMMatrixTranslation(2.4f, 0, 0);
            DrawCube(PROG_ALPHA_BACK, w, viewProj, 1.0f, 0.45f);
            DrawCube(PROG_ALPHA_FRONT, w, viewProj, 1.0f, 0.45f);
        }
    }

    CD3DX12_RESOURCE_BARRIER toPresent = CD3DX12_RESOURCE_BARRIER::Transition(g_rtBuffers[g_frameIndex],
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    g_list->ResourceBarrier(1, &toPresent);
    g_list->Close();
    ID3D12CommandList* lists[] = { g_list };
    g_queue->ExecuteCommandLists(1, lists);
    g_swap->Present(1, 0);
    WaitForGpu();
}

// [LEARN] "When is the shader really compiled?" Hardware drivers do the DXIL -> ISA
// work inside CreateStateObject / CreateGraphicsPipelineState, which is what the
// title's creation timings measure. WARP (a CPU JIT) defers its code generation to
// the FIRST DRAW that uses a pipeline, so on WARP the creation calls look nearly
// free. To keep the lesson honest we also time the first draw of every variant for
// both paths ("1st draw programs/PSOs" in the title). Measured on WARP
// 1.65535.20-preview: every blend/depth-unique pipeline JITs for ~70 ms at first
// draw on BOTH paths - WARP implements the partial-program API faithfully but does
// not (yet) reuse the collection's compile across linked programs, so the speedup
// only shows on a driver with native tier 1.0 support.
static double TimeVariantFrame(bool programs, int only) {
    bool saved = g_usePrograms;
    g_usePrograms = programs;
    double t0 = NowMs();
    g_alloc->Reset();
    g_list->Reset(g_alloc, nullptr);
    g_list->SetGraphicsRootSignature(g_rootSig);
    D3D12_VIEWPORT vp = { 0, 0, (float)g_width, (float)g_height, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)g_width, (LONG)g_height };
    g_list->RSSetViewports(1, &vp);
    g_list->RSSetScissorRects(1, &sc);
    CD3DX12_RESOURCE_BARRIER toRt = CD3DX12_RESOURCE_BARRIER::Transition(g_rtBuffers[g_frameIndex],
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    g_list->ResourceBarrier(1, &toRt);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)g_frameIndex * g_rtvSize;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    g_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    g_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_list->IASetVertexBuffers(0, 1, &g_vbv);
    g_list->IASetIndexBuffer(&g_ibv);
    XMMATRIX viewProj = XMMatrixLookAtLH(XMVectorSet(0, 0, -6, 0), XMVectorSet(0, 0, 0, 0), XMVectorSet(0, 1, 0, 0)) *
        XMMatrixPerspectiveFovLH(XM_PIDIV4, (float)g_width / g_height, 0.1f, 100.0f);
    for (UINT i = 0; i < PROG_COUNT; ++i)
        if (only < 0 || only == (int)i)
            DrawCube((ProgramId)i, XMMatrixRotationY(0.5f), viewProj, 1.0f, 0.5f);
    CD3DX12_RESOURCE_BARRIER toPresent = CD3DX12_RESOURCE_BARRIER::Transition(g_rtBuffers[g_frameIndex],
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    g_list->ResourceBarrier(1, &toPresent);
    g_list->Close();
    ID3D12CommandList* lists[] = { g_list };
    g_queue->ExecuteCommandLists(1, lists);
    WaitForGpu();                       // no Present: this frame is never shown
    g_usePrograms = saved;
    return NowMs() - t0;
}

static void MeasureFirstUse() {
    if (!g_partialReady || !g_classicReady) return;
    // One variant per frame, so each number is that pipeline's first-use cost.
    // Programs are measured first, so any cache the device shares between the two
    // paths can only flatter the classic PSOs, never the partial programs.
    const bool order[2] = { true, false };
    for (bool programs : order) {
        double total = 0, each[PROG_COUNT] = {};
        for (UINT i = 0; i < PROG_COUNT; ++i) { each[i] = TimeVariantFrame(programs, (int)i); total += each[i]; }
        double warm = TimeVariantFrame(programs, -1);
        (programs ? g_msFirstDrawProg : g_msFirstDrawPso) = total;
        logf_line("First draw per variant via %-12s: %.2f %.2f %.2f %.2f %.2f ms  (total %.2f ms; warm frame with all 5: %.2f ms)",
            programs ? "SetProgram" : "classic PSOs", each[0], each[1], each[2], each[3], each[4], total, warm);
    }
    logf_line("  (first-draw time includes any shader code generation the device deferred from creation to first use)");
    DrainDebugMessages();
}

static void UpdateTitle(double msPerFrame) {
    char title[512];
    if (g_partialReady) {
        const char* soloNames[] = { "opaque", "additive", "alpha" };
        char extra[96] = "";
        if (g_solo >= 0) sprintf_s(extra, " | solo %s", soloNames[g_solo]);
        if (g_wire) strcat_s(extra, " | wire");
        sprintf_s(title, "D3D12 - Partial Programs | %s | compile %.1f ms, link %ux %.2f ms vs %u PSOs %.1f ms | 1st draw %.0f/%.0f ms | %s%s | %.2f ms/frame",
            g_deviceLabel.c_str(), g_msCollection, (unsigned)PROG_COUNT, g_msLinkTotal / PROG_COUNT,
            (unsigned)PROG_COUNT, g_msPsoTotal, g_msFirstDrawProg, g_msFirstDrawPso,
            g_usePrograms ? "SetProgram" : "classic PSOs", extra, msPerFrame);
    } else {
        sprintf_s(title, "D3D12 - Partial Programs UNSUPPORTED (tier %u) | %s | classic PSO cube | %.2f ms/frame",
            g_tier, g_deviceLabel.c_str(), msPerFrame);
    }
    SetWindowTextA(g_hwnd, title);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) PostQuitMessage(0);
        else if (w >= '1' && w <= '3') { int s = (int)(w - '1'); g_solo = (g_solo == s) ? -1 : s; }
        else if (w == '0') g_solo = -1;
        else if (w == 'W') g_wire = !g_wire;
        else if (w == 'P' && g_partialReady && g_classicReady) {
            g_usePrograms = !g_usePrograms;
            logf_line("P: now drawing via %s", g_usePrograms ? "SetProgram (generic programs)" : "SetPipelineState (classic PSOs)");
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR cmdLine, int) {
    DeleteFileA(kLogName);
    QueryPerformanceFrequency(&g_qpcFreq);
    g_optHw    = strstr(cmdLine, "--hw") != nullptr;
    g_optWarp  = strstr(cmdLine, "--warp") != nullptr;
    g_optDebug = strstr(cmdLine, "--debug") != nullptr;
    g_optPso   = strstr(cmdLine, "--pso") != nullptr;
    logf_line("D3D12.PartialPrograms starting (args: '%s')", cmdLine);

    // [LEARN] Partial graphics programs ship in the 1.721 preview behind the state
    // objects experiment; it must be enabled BEFORE any device exists (Developer Mode
    // required). vs_6_0/ps_6_0 are not experimental, so shader models stay as-is.
    UUID experiments[] = { D3D12StateObjectsExperiment };
    HRESULT expHr = D3D12EnableExperimentalFeatures(_countof(experiments), experiments, nullptr, nullptr);
    g_experimentEnabled = SUCCEEDED(expHr);
    logf_line("D3D12EnableExperimentalFeatures(D3D12StateObjectsExperiment) hr=0x%08X enabled=%u",
        (unsigned)expHr, g_experimentEnabled ? 1u : 0u);

    WNDCLASS wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = "D3D12PartialPrograms";
    RegisterClass(&wc);
    RECT r = { 0, 0, (LONG)g_width, (LONG)g_height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindow("D3D12PartialPrograms", "D3D12 - Partial Programs",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, hInst, nullptr);
    if (!InitD3D(g_hwnd)) { logf_line("D3D12 Partial Programs init failed"); DrainDebugMessages(); return 1; }
    MeasureFirstUse();
    UpdateTitle(0.0);
    ShowWindow(g_hwnd, SW_SHOW);

    double start = NowMs(), lastTitle = start, lastFrame = start, accum = 0;
    UINT frames = 0, totalFrames = 0, titleUpdates = 0;
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); continue; }
        Render((float)((NowMs() - start) / 1000.0));
        double now = NowMs();
        accum += now - lastFrame; lastFrame = now; ++frames; ++totalFrames;
        if (now - lastTitle > 500.0) {
            UpdateTitle(accum / frames);
            if ((titleUpdates++ % 10) == 0)   // every ~5 s
                logf_line("frame %u: %.2f ms/frame via %s", totalFrames, accum / frames,
                    g_partialReady && g_usePrograms ? "SetProgram" : "classic PSOs");
            DrainDebugMessages();
            accum = 0; frames = 0; lastTitle = now;
        }
    }
    WaitForGpu();
    DrainDebugMessages();
    if (g_optDebug) logf_line("Debug layer totals: %u errors, %u warnings", g_debugErrors, g_debugWarnings);
    logf_line("Exiting after %u frames", totalFrames);
    return (int)msg.wParam;
}
