// D3D12 Linear Algebra teaching sample (Shader Model 6.10 preview).
// Teaches the SM 6.10 dx::linalg API that neural rendering is built on:
//   1. Multiply  - a 16x16 F16 thread-scope weight matrix times a 16-wide input
//                  vector (one tiny MLP layer, the "inference" half).
//   2. VectorAccumulate - dx::linalg::InterlockedAccumulate(vector, buffer, offset):
//                  256 threads each compute a per-sample gradient and atomically
//                  add it into ONE shared buffer (the "training" half: summing
//                  bias/gradient updates across a batch). New in DXC 1.10.2605.24-preview.
// Both GPU results are read back and verified against a CPU reference, logged, and
// then drawn on the cube faces as bar charts (bottom half = Multiply output, top
// half = accumulated gradient) with a warm tint. If LinAlg cannot run, the plain
// cool-tinted cube renders and the title/log say why.
//
// Device selection: hardware adapter first; if it reports LinAlg tier 0 (e.g. an
// RTX 2070 on driver 591.86) the device is recreated on the preview WARP software
// rasterizer (d3d10warp.dll 1.65535.20-preview staged next to the exe), which
// implements every Agility SDK 1.721-preview feature.
//   --hw     never fall back to WARP      --warp   force WARP
//   --debug  enable the D3D12 debug layer and log every debug-layer message
//
// REQUIRES: .\fetch-deps.ps1 (preview Agility SDK 1.721.2, preview DXC
// 1.10.2605.24, preview WARP 1.65535.20) then build-all, which stages the DLLs into
// Out\. Developer Mode must be on for experimental shader models. ESC quits.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "third_party/agility/include/d3d12.h"
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>

using namespace DirectX;

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "version.lib")

// [LEARN] These two exports are how an app opts into a redistributable Agility SDK:
// d3d12.dll (the OS loader stub) sees version 721 and loads D3D12Core.dll from
// .\D3D12\ next to the exe instead of the OS copy. Preview features live there.
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 721;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

static const UINT kFrames = 2;
static const UINT kDim = 16;                                       // vector width / matrix edge
static const UINT kMatrixBytes = kDim * kDim * sizeof(uint16_t);   // 512: 16x16 F16 weights
static const UINT kVectorOffset = kMatrixBytes;                    // 512: 16 F16 inputs
static const UINT kZeroOffset = 576;                               // 64-byte aligned zero block
static const UINT kInputBytes = kZeroOffset + kDim * sizeof(float);// 640
static const UINT kResultBytes = kDim * sizeof(float);             // 64: one vector<float,16>
static const UINT kAccThreadsPerGroup = 64;
static const UINT kAccGroups = 4;
static const UINT kAccThreads = kAccThreadsPerGroup * kAccGroups;  // 256 "training samples"

struct Vertex { XMFLOAT3 pos; XMFLOAT4 color; };
// 16 mvp + time/linalgOn/pad/pad + 16 Multiply bars + 16 Accumulate bars = 52 DWORDs.
struct RootData {
    XMFLOAT4X4 mvp; float time; float linalgOn; float pad0; float pad1;
    float mulBars[kDim]; float accBars[kDim];
};
static const UINT kRootDwords = sizeof(RootData) / 4;

static UINT g_width = 800, g_height = 600;
static ID3D12Device*              g_dev = nullptr;
static ID3D12CommandQueue*        g_queue = nullptr;
static IDXGISwapChain3*           g_swap = nullptr;
static ID3D12DescriptorHeap*      g_rtvHeap = nullptr;
static ID3D12DescriptorHeap*      g_dsvHeap = nullptr;
static UINT                       g_rtvSize = 0;
static ID3D12Resource*            g_rtBuffers[kFrames] = {};
static ID3D12Resource*            g_depth = nullptr;
static ID3D12CommandAllocator*    g_alloc = nullptr;
static ID3D12GraphicsCommandList* g_list = nullptr;
static ID3D12RootSignature*       g_rootSig = nullptr;
static ID3D12PipelineState*       g_pso = nullptr;
static ID3D12RootSignature*       g_computeRootSig = nullptr;
static ID3D12PipelineState*       g_mulPso = nullptr;
static ID3D12PipelineState*       g_accPso = nullptr;
static ID3D12Resource*            g_vb = nullptr;
static ID3D12Resource*            g_ib = nullptr;
static D3D12_VERTEX_BUFFER_VIEW   g_vbv = {};
static D3D12_INDEX_BUFFER_VIEW    g_ibv = {};
static ID3D12Fence*               g_fence = nullptr;
static UINT64                     g_fenceValue = 0;
static HANDLE                     g_fenceEvent = nullptr;
static UINT                       g_frameIndex = 0;
static ID3D12InfoQueue*           g_infoQueue = nullptr;

static bool                       g_argHw = false, g_argWarp = false, g_argDebug = false;
static bool                       g_onWarp = false;
static char                       g_deviceName[128] = "no device";
static std::wstring               g_exeDir;
static std::wstring               g_dxcIncludeDir;
static bool                       g_experimentalShadersEnabled = false;
static bool                       g_previewDxcLoaded = false;
static bool                       g_linalgSupported = false;
static bool                       g_linalgRan = false;
static bool                       g_mulOk = false, g_accOk = false;
static float                      g_gpuMul[kDim] = {}, g_gpuAcc[kDim] = {};
static D3D12_LINEAR_ALGEBRA_TIER  g_linalgTier = D3D12_LINEAR_ALGEBRA_TIER_NOT_SUPPORTED;
static const char*                g_fallbackReason = "not attempted";
static UINT                       g_debugErrors = 0, g_debugWarnings = 0;

static void logf_line(const char* fmt, ...) {
    FILE* f = nullptr;
    if (fopen_s(&f, "CubeD3D12.LinearAlgebra.log", "a") != 0 || !f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// [LEARN] The cube stays a normal VS+PS pipeline so the LinAlg compute dispatches
// read as a drop-in neural-rendering sidecar. The pixel shader only *visualizes*
// the verified GPU vectors: each face is split into 16 columns; the bottom half
// charts the Multiply output, the top half the VectorAccumulate gradient sum.
static const char* g_shaderSrc =
"cbuffer CB : register(b0) {\n"
"  float4x4 mvp; float g_time; float g_linalgOn; float2 g_pad;\n"
"  float4 g_mul[4]; float4 g_acc[4];\n"
"};\n"
"struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; float3 opos : TEXCOORD0; };\n"
"VSOut VSMain(float3 pos : POSITION, float4 col : COLOR) {\n"
"  VSOut o; o.pos = mul(float4(pos, 1.0), mvp); o.col = col; o.opos = pos; return o;\n"
"}\n"
"float4 PSMain(VSOut i) : SV_TARGET {\n"
"  float pulse = 0.78 + 0.22 * sin(g_time * 2.0 + i.col.r * 6.28318);\n"
"  float3 tint = lerp(float3(0.62, 0.72, 1.0), float3(1.05, 0.92, 0.55), g_linalgOn);\n"
"  float3 c = saturate(i.col.rgb * pulse * tint);\n"
"  float3 a = abs(i.opos);\n"
"  float2 uv = (a.x >= a.y && a.x >= a.z) ? i.opos.zy : ((a.y >= a.z) ? i.opos.xz : i.opos.xy);\n"
"  uv = uv * 0.5 + 0.5;\n"
"  uint colIdx = min((uint)(uv.x * 16.0), 15u);\n"
"  float v = (uv.y < 0.5) ? g_mul[colIdx >> 2][colIdx & 3] : g_acc[colIdx >> 2][colIdx & 3];\n"
"  float h = frac(uv.y * 2.0);\n"
"  float fx = frac(uv.x * 16.0);\n"
"  float bar = (h > 0.06 && h < 0.06 + 0.86 * v && fx > 0.18 && fx < 0.82) ? 1.0 : 0.0;\n"
"  float3 barCol = (uv.y < 0.5) ? float3(1.0, 0.85, 0.25) : float3(0.25, 1.0, 0.75);\n"
"  return float4(lerp(c, barCol, bar * g_linalgOn * 0.8), 1.0);\n"
"}\n";

// [LEARN] LONG VECTORS (Shader Model 6.9). Before SM 6.9, HLSL vectors topped out at
// 4 components (float4) because that is what shading registers looked like. SM 6.9
// introduced "long vectors": vector<T, N> with N up to 1024, as first-class values
// you can load, store, add and pass to intrinsics. LinAlg *needs* them: a neural
// layer's activations are 16/32/64 wide, and Multiply/MultiplyAdd/InterlockedAccumulate
// take and return whole vector<half,16>/vector<float,16> values so the driver can map
// them onto tensor/matrix hardware as one operation instead of 16 scalar dot products.
// Everything below (vector<half,16> x, vector<float,16> y, y - 1.0) is SM 6.9 syntax.
//
// [LEARN] Thread-scope matrices are the cooperative-vector fit for tiny MLP layers:
// each invocation multiplies its own feature vector by a shared weight matrix.
static const char* g_linalgSrc =
"#include <dx/linalg.h>\n"
"using namespace dx::linalg;\n"
"ByteAddressBuffer   g_input : register(t0);\n"
"RWByteAddressBuffer g_output : register(u0);\n"
"RWByteAddressBuffer g_grad : register(u1);\n"
"typedef Matrix<ComponentType::F16, 16, 16, MatrixUse::A, MatrixScope::Thread> Weights16;\n"
"\n"
"// Inference: y = W * x for one input vector, stored for readback.\n"
"[numthreads(8, 1, 1)]\n"
"void MulMain(uint3 tid : SV_DispatchThreadID) {\n"
"  if (tid.x != 0) return;\n"
"  Weights16 W = Weights16::Load<MatrixLayout::RowMajor>(g_input, 0, 32, 128);\n"
"  vector<half, 16> x = g_input.Load<vector<half, 16> >(512);\n"
"  vector<float, 16> y = Multiply<float, half>(W, x);\n"
"  g_output.Store<vector<float, 16> >(0, y);\n"
"}\n"
"\n"
"// Training: every thread is one sample in a batch. Forward pass y = W * x, loss\n"
"// L = 0.5*|y - 1|^2, so dL/dbias = y - 1. InterlockedAccumulate (the new\n"
"// VectorAccumulate op) atomically adds the whole 16-wide gradient into g_grad.\n"
"[numthreads(64, 1, 1)]\n"
"void AccMain(uint3 tid : SV_DispatchThreadID) {\n"
"  Weights16 W = Weights16::Load<MatrixLayout::RowMajor>(g_input, 0, 32, 128);\n"
"  vector<half, 16> x;\n"
"  [unroll] for (uint i = 0; i < 16; ++i) x[i] = (half)(((tid.x + i) & 3u) + 1u);\n"
"  vector<float, 16> y = Multiply<float, half>(W, x);\n"
"  vector<float, 16> grad = y - 1.0;\n"
"  InterlockedAccumulate(grad, g_grad, 0);\n"
"}\n";

static std::wstring FullPath(const std::wstring& path) {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetFullPathNameW(path.c_str(), MAX_PATH, buf, nullptr);
    if (n == 0 || n >= MAX_PATH) return path;
    return buf;
}

static bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Returns the DLL's file version as 4 WORDs (major.minor.build.rev); false if none.
static bool GetFileVersion(const std::wstring& path, WORD v[4]) {
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size) return false;
    std::string data(size, '\0');
    if (!GetFileVersionInfoW(path.c_str(), 0, size, &data[0])) return false;
    VS_FIXEDFILEINFO* ffi = nullptr; UINT len = 0;
    if (!VerQueryValueW(&data[0], L"\\", (void**)&ffi, &len) || !ffi) return false;
    v[0] = HIWORD(ffi->dwFileVersionMS); v[1] = LOWORD(ffi->dwFileVersionMS);
    v[2] = HIWORD(ffi->dwFileVersionLS); v[3] = LOWORD(ffi->dwFileVersionLS);
    return true;
}

static HMODULE TryLoadDxcFromDir(const std::wstring& dir) {
    std::wstring dxil = FullPath(dir + L"dxil.dll");
    std::wstring dxc = FullPath(dir + L"dxcompiler.dll");
    if (!FileExists(dxc)) return nullptr;
    HMODULE dxilMod = LoadLibraryExW(dxil.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    HMODULE dxcMod = LoadLibraryExW(dxc.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dxcMod) {
        logf_line("LoadLibrary %ls failed err=%lu (dxil=%p)", dxc.c_str(), GetLastError(), dxilMod);
        return nullptr;
    }
    WORD v[4] = {};
    bool hasVer = GetFileVersion(dxc, v);
    // [LEARN] cs_6_10 + dx::linalg (and VectorAccumulate) need DXC 1.10.2605.24-preview
    // or newer; Windows-Kits compilers stop at older shader models.
    g_previewDxcLoaded = hasVer && (v[0] > 1 || (v[0] == 1 && v[1] >= 10));
    logf_line("Loaded DXC %ls version=%u.%u.%u.%u previewCapable=%u", dxc.c_str(),
        v[0], v[1], v[2], v[3], g_previewDxcLoaded ? 1u : 0u);
    return dxcMod;
}

// [LEARN] build-all stages the vendored preview DXC next to the exe; when run from
// the sample folder, the un-staged third_party copy is found instead.
static HMODULE LoadDxc() {
    std::wstring dirs[] = {
        g_exeDir,
        g_exeDir + L"..\\third_party\\dxc\\bin\\x64\\",
        L"third_party\\dxc\\bin\\x64\\",
        L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.26100.0\\x64\\",
        L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.22621.0\\x64\\",
    };
    for (auto& d : dirs) {
        HMODULE m = TryLoadDxcFromDir(d);
        if (m) return m;
    }
    logf_line("dxcompiler.dll not found; cannot compile shaders at runtime");
    return nullptr;
}

static HMODULE DxcModule() { static HMODULE dll = LoadDxc(); return dll; }

static void ResolveDxcIncludeDir() {
    std::wstring candidates[] = {
        g_exeDir + L"..\\third_party\\dxc\\include",
        L"third_party\\dxc\\include",
    };
    for (auto& c : candidates) {
        if (FileExists(c + L"\\dx\\linalg.h")) { g_dxcIncludeDir = FullPath(c); break; }
    }
    logf_line("dx/linalg.h include dir: %ls",
        g_dxcIncludeDir.empty() ? L"(not found)" : g_dxcIncludeDir.c_str());
}

static IDxcBlob* CompileDXC(const char* src, const wchar_t* entry, const wchar_t* target, bool linalg) {
    HMODULE dll = DxcModule();
    if (!dll) return nullptr;
    auto create = (DxcCreateInstanceProc)GetProcAddress(dll, "DxcCreateInstance");
    if (!create) { logf_line("DxcCreateInstance export not found"); return nullptr; }

    IDxcCompiler3* comp = nullptr;
    HRESULT hr = create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), (void**)&comp);
    if (FAILED(hr) || !comp) { logf_line("DxcCreateInstance compiler hr=0x%08X", (unsigned)hr); return nullptr; }

    IDxcUtils* utils = nullptr;
    IDxcIncludeHandler* includeHandler = nullptr;
    hr = create(CLSID_DxcUtils, __uuidof(IDxcUtils), (void**)&utils);
    if (SUCCEEDED(hr) && utils) utils->CreateDefaultIncludeHandler(&includeHandler);

    DxcBuffer buf = { src, strlen(src), DXC_CP_UTF8 };
    const wchar_t* gfxArgs[] = { L"-E", entry, L"-T", target, L"-O3", L"-HV", L"2021" };
    // [LEARN] -enable-16bit-types makes `half` a real 16-bit float (F16 weights and
    // inputs); without it `half` silently means float and the matrix types mismatch.
    const wchar_t* linalgArgs[] = {
        L"-E", entry, L"-T", target, L"-O3", L"-HV", L"2021",
        L"-I", g_dxcIncludeDir.c_str(), L"-enable-16bit-types"
    };
    const wchar_t** args = linalg ? linalgArgs : gfxArgs;
    UINT argCount = linalg ? _countof(linalgArgs) : _countof(gfxArgs);

    IDxcResult* result = nullptr;
    hr = comp->Compile(&buf, args, argCount, includeHandler, __uuidof(IDxcResult), (void**)&result);
    if (includeHandler) includeHandler->Release();
    if (utils) utils->Release();
    comp->Release();
    if (FAILED(hr) || !result) { logf_line("DXC compile call failed target=%ls hr=0x%08X", target, (unsigned)hr); return nullptr; }

    HRESULT status = E_FAIL; result->GetStatus(&status);
    IDxcBlobUtf8* errs = nullptr;
    result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), (void**)&errs, nullptr);
    if (errs && errs->GetStringLength())
        logf_line("DXC %ls %ls %s: %s", entry, target, FAILED(status) ? "errors" : "warnings", errs->GetStringPointer());
    if (errs) errs->Release();
    if (FAILED(status)) { result->Release(); return nullptr; }
    IDxcBlob* obj = nullptr;
    result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), (void**)&obj, nullptr);
    result->Release();
    logf_line("DXC compiled %ls %ls (%zu bytes DXIL)", entry, target, obj ? obj->GetBufferSize() : (size_t)0);
    return obj;
}

// [LEARN] With --debug every debug-layer message is copied into the log, so "no
// debug-layer errors" is something you can grep for rather than something you hope.
// Expect exactly one WARNING id=1243 per cs_6_10 PSO: the debug layer's DXIL validator
// tops out at SM 6.9, so it declines to validate experimental 6.10 shaders.
static void DrainDebugMessages(const char* when) {
    if (!g_infoQueue) return;
    UINT64 n = g_infoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(g_infoQueue->GetMessage(i, nullptr, &len)) || !len) continue;
        std::string storage(len, '\0');
        D3D12_MESSAGE* msg = (D3D12_MESSAGE*)&storage[0];
        if (FAILED(g_infoQueue->GetMessage(i, msg, &len))) continue;
        const char* sev = "INFO";
        switch (msg->Severity) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION: sev = "CORRUPTION"; ++g_debugErrors; break;
        case D3D12_MESSAGE_SEVERITY_ERROR:      sev = "ERROR"; ++g_debugErrors; break;
        case D3D12_MESSAGE_SEVERITY_WARNING:    sev = "WARNING"; ++g_debugWarnings; break;
        case D3D12_MESSAGE_SEVERITY_MESSAGE:    sev = "MESSAGE"; break;
        default: break;
        }
        logf_line("[debug-layer %s] %s id=%d: %s", when, sev, (int)msg->ID, msg->pDescription);
    }
    g_infoQueue->ClearStoredMessages();
}

static void WaitForGpu() {
    const UINT64 v = ++g_fenceValue;
    g_queue->Signal(g_fence, v);
    if (g_fence->GetCompletedValue() < v) {
        g_fence->SetEventOnCompletion(v, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
    if (g_swap) g_frameIndex = g_swap->GetCurrentBackBufferIndex();
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN: if (w == VK_ESCAPE) PostQuitMessage(0); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, w, l);
}

// [LEARN] Buffers are always created in COMMON (the runtime ignores any other initial
// state for buffers and the debug layer warns about it); copies implicitly promote.
static ID3D12Resource* CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heapType,
    D3D12_RESOURCE_STATES initialState, D3D12_RESOURCE_FLAGS flags) {
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = heapType;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    ID3D12Resource* res = nullptr;
    HRESULT hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        initialState, nullptr, __uuidof(ID3D12Resource), (void**)&res);
    if (FAILED(hr) || !res) logf_line("CreateCommittedResource buffer heap=%u size=%llu hr=0x%08X",
        (unsigned)heapType, (unsigned long long)size, (unsigned)hr);
    return res;
}

static ID3D12Resource* CreateUploadBuffer(const void* data, UINT64 size) {
    ID3D12Resource* res = CreateBuffer(size, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    if (!res) return nullptr;
    void* mapped = nullptr; D3D12_RANGE none = { 0, 0 };
    HRESULT hr = res->Map(0, &none, &mapped);
    if (FAILED(hr) || !mapped) { logf_line("Map upload hr=0x%08X", (unsigned)hr); res->Release(); return nullptr; }
    memcpy(mapped, data, (size_t)size);
    res->Unmap(0, nullptr);
    return res;
}

static void ResourceTransition(ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = res;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_list->ResourceBarrier(1, &barrier);
}

static void UavBarrier(ID3D12Resource* res) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = res;
    g_list->ResourceBarrier(1, &barrier);
}

// Weights: upper-bidiagonal W[i][i] = 1 + i/16, W[i][i+1] = 0.25 (non-symmetric, so a
// transposed or mis-strided load would fail the CPU check). Input x[i] = i + 1.
// Every value is exact in F16, so the CPU reference is exact too.
static float WeightAt(UINT row, UINT col) {
    if (row == col) return 1.0f + row * 0.0625f;
    if (col == row + 1) return 0.25f;
    return 0.0f;
}

static void FillLinalgInput(uint8_t* bytes) {
    memset(bytes, 0, kInputBytes);
    uint16_t* w = (uint16_t*)bytes;
    for (UINT row = 0; row < kDim; ++row)
        for (UINT col = 0; col < kDim; ++col)
            w[row * kDim + col] = PackedVector::XMConvertFloatToHalf(WeightAt(row, col));
    uint16_t* vec = (uint16_t*)(bytes + kVectorOffset);
    for (UINT i = 0; i < kDim; ++i) vec[i] = PackedVector::XMConvertFloatToHalf((float)(i + 1));
    // bytes[kZeroOffset..] stays zero: copied into the gradient buffer to "zero the grads".
}

// CPU reference for both kernels (double precision; all inputs are exact).
static void CpuReference(double mul[kDim], double acc[kDim]) {
    for (UINT i = 0; i < kDim; ++i) {
        mul[i] = 0.0; acc[i] = 0.0;
        for (UINT k = 0; k < kDim; ++k) mul[i] += (double)WeightAt(i, k) * (double)(k + 1);
    }
    for (UINT t = 0; t < kAccThreads; ++t) {
        double x[kDim];
        for (UINT k = 0; k < kDim; ++k) x[k] = (double)(((t + k) & 3u) + 1u);
        for (UINT i = 0; i < kDim; ++i) {
            double y = 0.0;
            for (UINT k = 0; k < kDim; ++k) y += (double)WeightAt(i, k) * x[k];
            acc[i] += y - 1.0;
        }
    }
}

static bool CompareVectors(const char* name, const float* gpu, const double* cpu) {
    double maxErr = 0.0; UINT bad = 0;
    for (UINT i = 0; i < kDim; ++i) {
        double err = fabs((double)gpu[i] - cpu[i]);
        double tol = 1e-3 * (fabs(cpu[i]) > 1.0 ? fabs(cpu[i]) : 1.0);
        if (err > maxErr) maxErr = err;
        if (err > tol) ++bad;
    }
    logf_line("%s GPU : %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f", name,
        gpu[0], gpu[1], gpu[2], gpu[3], gpu[4], gpu[5], gpu[6], gpu[7],
        gpu[8], gpu[9], gpu[10], gpu[11], gpu[12], gpu[13], gpu[14], gpu[15]);
    logf_line("%s CPU : %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f", name,
        cpu[0], cpu[1], cpu[2], cpu[3], cpu[4], cpu[5], cpu[6], cpu[7],
        cpu[8], cpu[9], cpu[10], cpu[11], cpu[12], cpu[13], cpu[14], cpu[15]);
    logf_line("%s verify: %s (%u/16 components match, max abs error %.6g)", name,
        bad == 0 ? "MATCH" : "MISMATCH", kDim - bad, maxErr);
    return bad == 0;
}

// [LEARN] The tier is the coarse gate; the operation-support query is the fine one.
// ATOMIC_ACCUMULATE_STORE is the capability behind InterlockedAccumulate: the driver
// reports per component type whether it can atomically add into an RWByteAddressBuffer.
static void QueryLinearAlgebraSupport() {
    D3D12_FEATURE_DATA_LINEAR_ALGEBRA_SUPPORT data = {};
    HRESULT hr = g_dev->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_SUPPORT, &data, sizeof(data));
    g_linalgTier = SUCCEEDED(hr) ? data.LinearAlgebraTier : D3D12_LINEAR_ALGEBRA_TIER_NOT_SUPPORTED;
    g_linalgSupported = SUCCEEDED(hr) && g_linalgTier != D3D12_LINEAR_ALGEBRA_TIER_NOT_SUPPORTED;
    logf_line("[%s] D3D12_FEATURE_LINEAR_ALGEBRA_SUPPORT hr=0x%08X LinearAlgebraTier=0x%X supported=%u",
        g_deviceName, (unsigned)hr, (unsigned)g_linalgTier, g_linalgSupported ? 1u : 0u);
    if (!g_linalgSupported) return;

    D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT op = {};
    op.OperationType = D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_THREAD_VECTOR_MATRIX_MULTIPLY;
    op.ThreadVectorMatrixMultiply.VectorInputType = D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16;
    op.ThreadVectorMatrixMultiply.MatrixInputType = D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT16;
    op.ThreadVectorMatrixMultiply.BiasInputType = D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT32;
    op.ThreadVectorMatrixMultiply.VectorResultType = D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT32;
    hr = g_dev->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT, &op, sizeof(op));
    logf_line("[%s] MATRIX_OPERATION_SUPPORT THREAD_VECTOR_MATRIX_MULTIPLY F16xF16->F32 hr=0x%08X SupportFlags=0x%X",
        g_deviceName, (unsigned)hr, (unsigned)op.ThreadVectorMatrixMultiply.SupportFlags);

    D3D12_FEATURE_DATA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT acc = {};
    acc.OperationType = D3D12_LINEAR_ALGEBRA_OPERATION_TYPE_ATOMIC_ACCUMULATE_STORE;
    acc.AccumulateStore.ComponentType = D3D12_LINEAR_ALGEBRA_DATATYPE_FLOAT32;
    hr = g_dev->CheckFeatureSupport(D3D12_FEATURE_LINEAR_ALGEBRA_LINEAR_ALGEBRA_MATRIX_OPERATION_SUPPORT, &acc, sizeof(acc));
    logf_line("[%s] MATRIX_OPERATION_SUPPORT ATOMIC_ACCUMULATE_STORE F32 hr=0x%08X RWByteAddressBuffer=%d GroupShared=%d",
        g_deviceName, (unsigned)hr, (int)acc.AccumulateStore.RWByteAddressBufferSupported,
        (int)acc.AccumulateStore.GroupSharedSupported);
}

static ID3D12PipelineState* CreateComputePso(const wchar_t* entry) {
    IDxcBlob* cs = CompileDXC(g_linalgSrc, entry, L"cs_6_10", true);
    if (!cs) { logf_line("LinAlg %ls cs_6_10 compile failed", entry); return nullptr; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = g_computeRootSig;
    pso.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    ID3D12PipelineState* p = nullptr;
    HRESULT hr = g_dev->CreateComputePipelineState(&pso, __uuidof(ID3D12PipelineState), (void**)&p);
    cs->Release();
    logf_line("CreateComputePipelineState %ls hr=0x%08X", entry, (unsigned)hr);
    return SUCCEEDED(hr) ? p : nullptr;
}

// [LEARN] Root SRV/UAV descriptors keep the sample descriptor-heap free. Note the
// spec: bounds checking is not required for root descriptors, so offsets must be right.
static bool CreateLinalgPipelines() {
    if (!g_experimentalShadersEnabled) { g_fallbackReason = "experimental shader models off"; return false; }
    if (!g_linalgSupported) { g_fallbackReason = "LinAlg tier 0"; return false; }
    DxcModule();
    if (!g_previewDxcLoaded) { g_fallbackReason = "no preview DXC"; logf_line("Skipping LinAlg: no SM 6.10-capable DXC"); return false; }
    if (g_dxcIncludeDir.empty()) { g_fallbackReason = "dx/linalg.h missing"; logf_line("Skipping LinAlg: dx/linalg.h not found"); return false; }

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor.ShaderRegister = 0;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor.ShaderRegister = 1;
    for (auto& p : params) p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = _countof(params);
    rsd.pParameters = params;
    ID3DBlob* sig = nullptr; ID3DBlob* sigErr = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr);
    if (FAILED(hr)) {
        logf_line("D3D12SerializeRootSignature compute hr=0x%08X", (unsigned)hr);
        if (sigErr) sigErr->Release();
        g_fallbackReason = "root signature failed";
        return false;
    }
    hr = g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
        __uuidof(ID3D12RootSignature), (void**)&g_computeRootSig);
    sig->Release();
    if (FAILED(hr) || !g_computeRootSig) { logf_line("CreateRootSignature compute hr=0x%08X", (unsigned)hr); g_fallbackReason = "root signature failed"; return false; }

    g_mulPso = CreateComputePso(L"MulMain");
    g_accPso = CreateComputePso(L"AccMain");
    if (!g_mulPso || !g_accPso) { g_fallbackReason = "cs_6_10 PSO failed"; return false; }
    return true;
}

// [LEARN] The logged readback is the payoff: real GPU LinAlg math, checked against
// the CPU, not just a capability bit. Order in one command list:
//   zero gradient buffer -> Multiply dispatch -> VectorAccumulate dispatch -> readback.
static bool ExecuteLinalgProbe() {
    if (!CreateLinalgPipelines()) {
        logf_line("LinAlg path skipped (%s); rendering plain rotating cube fallback", g_fallbackReason);
        return false;
    }

    uint8_t inputBytes[kInputBytes] = {};
    FillLinalgInput(inputBytes);
    ID3D12Resource* upload = CreateUploadBuffer(inputBytes, kInputBytes);
    ID3D12Resource* input = CreateBuffer(kInputBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* output = CreateBuffer(kResultBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ID3D12Resource* grad = CreateBuffer(kResultBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ID3D12Resource* readback = CreateBuffer(kResultBytes * 2, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    if (!upload || !input || !output || !grad || !readback) { g_fallbackReason = "buffer alloc failed"; return false; }

    g_alloc->Reset();
    g_list->Reset(g_alloc, g_mulPso);
    g_list->CopyBufferRegion(input, 0, upload, 0, kInputBytes);
    // [LEARN] Accumulation targets must start at zero - the GPU equivalent of
    // optimizer.zero_grad() before a training step.
    g_list->CopyBufferRegion(grad, 0, upload, kZeroOffset, kResultBytes);
    ResourceTransition(input, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ResourceTransition(grad, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ResourceTransition(output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    g_list->SetComputeRootSignature(g_computeRootSig);
    g_list->SetComputeRootShaderResourceView(0, input->GetGPUVirtualAddress());
    g_list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
    g_list->SetComputeRootUnorderedAccessView(2, grad->GetGPUVirtualAddress());
    g_list->SetPipelineState(g_mulPso);
    g_list->Dispatch(1, 1, 1);
    // [LEARN] 4 groups x 64 threads = 256 samples all hammering the same 64 bytes.
    // InterlockedAccumulate makes each 16-wide add atomic per component, so no
    // groupshared reduction or second pass is needed to sum the batch.
    g_list->SetPipelineState(g_accPso);
    g_list->Dispatch(kAccGroups, 1, 1);
    UavBarrier(nullptr);
    ResourceTransition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ResourceTransition(grad, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g_list->CopyBufferRegion(readback, 0, output, 0, kResultBytes);
    g_list->CopyBufferRegion(readback, kResultBytes, grad, 0, kResultBytes);
    HRESULT hr = g_list->Close();
    if (FAILED(hr)) { logf_line("Close LinAlg command list hr=0x%08X", (unsigned)hr); g_fallbackReason = "command list failed"; return false; }
    ID3D12CommandList* lists[] = { g_list };
    g_queue->ExecuteCommandLists(1, lists);
    WaitForGpu();
    hr = g_dev->GetDeviceRemovedReason();
    if (FAILED(hr)) { logf_line("Device removed after LinAlg dispatch reason=0x%08X", (unsigned)hr); g_fallbackReason = "device removed"; return false; }

    float* mapped = nullptr;
    D3D12_RANGE range = { 0, kResultBytes * 2 };
    hr = readback->Map(0, &range, (void**)&mapped);
    if (FAILED(hr) || !mapped) { logf_line("Map LinAlg readback hr=0x%08X", (unsigned)hr); g_fallbackReason = "readback failed"; return false; }
    memcpy(g_gpuMul, mapped, kResultBytes);
    memcpy(g_gpuAcc, mapped + kDim, kResultBytes);
    D3D12_RANGE none = { 0, 0 };
    readback->Unmap(0, &none);
    upload->Release(); input->Release(); output->Release(); grad->Release(); readback->Release();

    double cpuMul[kDim], cpuAcc[kDim];
    CpuReference(cpuMul, cpuAcc);
    g_mulOk = CompareVectors("Multiply(W,x)", g_gpuMul, cpuMul);
    g_accOk = CompareVectors("VectorAccumulate(sum of 256 grads)", g_gpuAcc, cpuAcc);
    g_linalgRan = true;
    logf_line("LinAlg executed on %s: Multiply=%s VectorAccumulate=%s", g_deviceName,
        g_mulOk ? "OK" : "MISMATCH", g_accOk ? "OK" : "MISMATCH");
    return g_mulOk && g_accOk;
}

static ID3D12Device* CreateDeviceOn(IDXGIAdapter1* adapter, const char* what) {
    ID3D12Device* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&dev);
    logf_line("D3D12CreateDevice(%s) hr=0x%08X", what, (unsigned)hr);
    return SUCCEEDED(hr) ? dev : nullptr;
}

static void SetDeviceNameFromAdapter(IDXGIAdapter1* adapter) {
    DXGI_ADAPTER_DESC1 desc = {};
    adapter->GetDesc1(&desc);
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, g_deviceName, sizeof(g_deviceName), nullptr, nullptr);
    logf_line("Adapter: %s VendorId=0x%04X DeviceId=0x%04X DedicatedVideoMemory=%llu MB",
        g_deviceName, desc.VendorId, desc.DeviceId, (unsigned long long)(desc.DedicatedVideoMemory >> 20));
}

// [LEARN] EnumWarpAdapter loads d3d10warp.dll by name, so ordinary DLL search order
// applies: the preview copy staged next to the exe wins over System32. We prove it
// by asking the loader which file actually got mapped.
static void LogLoadedWarp() {
    HMODULE warp = GetModuleHandleW(L"d3d10warp.dll");
    wchar_t path[MAX_PATH] = L"(not loaded)";
    if (warp) GetModuleFileNameW(warp, path, MAX_PATH);
    WORD v[4] = {};
    GetFileVersion(path, v);
    bool preview = _wcsnicmp(path, g_exeDir.c_str(), g_exeDir.size()) == 0;
    logf_line("WARP module: %ls version=%u.%u.%u.%u (%s)", path, v[0], v[1], v[2], v[3],
        preview ? "vendored preview WARP next to exe" : "system WARP");
    strcpy_s(g_deviceName, preview ? "WARP (preview)" : "WARP (system)");
}

// [LEARN] Hardware first, WARP second. The capability tier decides, not the vendor:
// the same exe lights up LinAlg on real tensor hardware the day the driver reports it.
static bool CreateBestDevice(IDXGIFactory4* factory) {
    if (!g_argWarp) {
        IDXGIAdapter1* hw = nullptr;
        IDXGIFactory6* f6 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory6), (void**)&f6)) && f6) {
            f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter1), (void**)&hw);
            f6->Release();
        }
        if (!hw) factory->EnumAdapters1(0, &hw);
        if (hw) {
            SetDeviceNameFromAdapter(hw);
            g_dev = CreateDeviceOn(hw, "hardware");
            hw->Release();
        }
        if (g_dev) {
            QueryLinearAlgebraSupport();
            if (g_linalgSupported) return true;
            if (g_argHw) {
                logf_line("--hw: hardware reports LinAlg tier 0; staying on hardware (no WARP fallback)");
                return true;
            }
            logf_line("Hardware LinAlg tier 0 -> recreating the device on preview WARP");
        } else if (g_argHw) {
            logf_line("--hw: hardware device creation failed and WARP fallback is disabled");
            return false;
        }
    }

    IDXGIAdapter1* warp = nullptr;
    HRESULT hr = factory->EnumWarpAdapter(__uuidof(IDXGIAdapter1), (void**)&warp);
    logf_line("EnumWarpAdapter hr=0x%08X", (unsigned)hr);
    ID3D12Device* warpDev = (SUCCEEDED(hr) && warp) ? CreateDeviceOn(warp, "WARP") : nullptr;
    if (warp) warp->Release();
    if (!warpDev) {
        logf_line("WARP device unavailable; %s", g_dev ? "keeping hardware device (plain cube)" : "no device at all");
        return g_dev != nullptr;
    }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
    g_dev = warpDev;
    g_onWarp = true;
    LogLoadedWarp();
    QueryLinearAlgebraSupport();
    return true;
}

static bool InitD3D(HWND hwnd) {
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&factory);
    if (FAILED(hr)) { logf_line("CreateDXGIFactory1 hr=0x%08X", (unsigned)hr); return false; }
    if (!CreateBestDevice(factory)) { factory->Release(); return false; }

    if (g_argDebug && SUCCEEDED(g_dev->QueryInterface(__uuidof(ID3D12InfoQueue), (void**)&g_infoQueue)) && g_infoQueue) {
        logf_line("ID3D12InfoQueue acquired; debug-layer messages will be logged");
        DrainDebugMessages("device-create");
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = g_dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&g_queue);
    if (FAILED(hr)) { logf_line("CreateCommandQueue hr=0x%08X", (unsigned)hr); factory->Release(); return false; }

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.BufferCount = kFrames;
    sd.Width = g_width; sd.Height = g_height;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc.Count = 1;
    IDXGISwapChain1* sc1 = nullptr;
    hr = factory->CreateSwapChainForHwnd(g_queue, hwnd, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr)) { logf_line("CreateSwapChainForHwnd hr=0x%08X", (unsigned)hr); factory->Release(); return false; }
    sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&g_swap);
    sc1->Release(); factory->Release();
    if (!g_swap) { logf_line("IDXGISwapChain3 unavailable"); return false; }
    g_frameIndex = g_swap->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rh = {};
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors = kFrames;
    g_dev->CreateDescriptorHeap(&rh, __uuidof(ID3D12DescriptorHeap), (void**)&g_rtvHeap);
    g_rtvSize = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC dh = {};
    dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; dh.NumDescriptors = 1;
    g_dev->CreateDescriptorHeap(&dh, __uuidof(ID3D12DescriptorHeap), (void**)&g_dsvHeap);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrames; i++) {
        g_swap->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&g_rtBuffers[i]);
        g_dev->CreateRenderTargetView(g_rtBuffers[i], nullptr, rtv);
        rtv.ptr += g_rtvSize;
    }

    D3D12_HEAP_PROPERTIES dhp = {}; dhp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC drd = {};
    drd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    drd.Width = g_width; drd.Height = g_height; drd.DepthOrArraySize = 1; drd.MipLevels = 1;
    drd.Format = DXGI_FORMAT_D32_FLOAT; drd.SampleDesc.Count = 1;
    drd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 1.0f;
    hr = g_dev->CreateCommittedResource(&dhp, D3D12_HEAP_FLAG_NONE, &drd,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, __uuidof(ID3D12Resource), (void**)&g_depth);
    if (FAILED(hr)) { logf_line("CreateCommittedResource depth hr=0x%08X", (unsigned)hr); return false; }
    g_dev->CreateDepthStencilView(g_depth, nullptr, g_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.Num32BitValues = kRootDwords;
    rp.Constants.ShaderRegister = 0;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 1; rsd.pParameters = &rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* sig = nullptr; ID3DBlob* sigErr = nullptr;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr);
    if (FAILED(hr)) {
        logf_line("D3D12SerializeRootSignature graphics hr=0x%08X", (unsigned)hr);
        if (sigErr) sigErr->Release();
        return false;
    }
    hr = g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
        __uuidof(ID3D12RootSignature), (void**)&g_rootSig);
    sig->Release();
    if (FAILED(hr)) { logf_line("CreateRootSignature graphics hr=0x%08X", (unsigned)hr); return false; }

    IDxcBlob* vs = CompileDXC(g_shaderSrc, L"VSMain", L"vs_6_0", false);
    IDxcBlob* ps = CompileDXC(g_shaderSrc, L"PSMain", L"ps_6_0", false);
    if (!vs || !ps) { logf_line("runtime DXC graphics shader compile failed"); return false; }

    D3D12_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = g_rootSig;
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout = { il, 2 };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    for (UINT i = 0; i < 8; i++) pso.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    hr = g_dev->CreateGraphicsPipelineState(&pso, __uuidof(ID3D12PipelineState), (void**)&g_pso);
    vs->Release(); ps->Release();
    if (FAILED(hr)) { logf_line("CreateGraphicsPipelineState hr=0x%08X", (unsigned)hr); return false; }

    hr = g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        __uuidof(ID3D12CommandAllocator), (void**)&g_alloc);
    if (FAILED(hr)) { logf_line("CreateCommandAllocator hr=0x%08X", (unsigned)hr); return false; }
    hr = g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, g_pso,
        __uuidof(ID3D12GraphicsCommandList), (void**)&g_list);
    if (FAILED(hr) || !g_list) { logf_line("CreateCommandList hr=0x%08X", (unsigned)hr); return false; }
    g_list->Close();

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
    g_vbv.BufferLocation = g_vb ? g_vb->GetGPUVirtualAddress() : 0;
    g_vbv.StrideInBytes = sizeof(Vertex);
    g_vbv.SizeInBytes = sizeof(verts);
    g_ib = CreateUploadBuffer(idx, sizeof(idx));
    g_ibv.BufferLocation = g_ib ? g_ib->GetGPUVirtualAddress() : 0;
    g_ibv.Format = DXGI_FORMAT_R16_UINT;
    g_ibv.SizeInBytes = sizeof(idx);

    hr = g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_fence);
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr) || !g_fence || !g_fenceEvent || !g_vb || !g_ib) return false;

    ExecuteLinalgProbe();
    DrainDebugMessages("init");
    return true;
}

static void Render(float t) {
    g_alloc->Reset();
    g_list->Reset(g_alloc, g_pso);

    g_list->SetGraphicsRootSignature(g_rootSig);
    XMMATRIX world = XMMatrixRotationY(t) * XMMatrixRotationX(t * 0.5f);
    XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 0, -6, 0), XMVectorSet(0, 0, 0, 0), XMVectorSet(0, 1, 0, 0));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, (float)g_width / g_height, 0.1f, 100.0f);
    RootData rd = {};
    XMStoreFloat4x4(&rd.mvp, XMMatrixTranspose(world * view * proj));
    rd.time = t;
    // [LEARN] The warm tint and the bars are driven by the *verified GPU readback*:
    // full warmth only if both vectors matched the CPU; half if LinAlg ran but disagreed.
    rd.linalgOn = (g_mulOk && g_accOk) ? 1.0f : (g_linalgRan ? 0.5f : 0.0f);
    float mulMax = 1e-6f, accMax = 1e-6f;
    for (UINT i = 0; i < kDim; ++i) { mulMax = fmaxf(mulMax, fabsf(g_gpuMul[i])); accMax = fmaxf(accMax, fabsf(g_gpuAcc[i])); }
    for (UINT i = 0; i < kDim; ++i) { rd.mulBars[i] = fabsf(g_gpuMul[i]) / mulMax; rd.accBars[i] = fabsf(g_gpuAcc[i]) / accMax; }
    g_list->SetGraphicsRoot32BitConstants(0, kRootDwords, &rd, 0);

    D3D12_VIEWPORT vp = { 0, 0, (float)g_width, (float)g_height, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)g_width, (LONG)g_height };
    g_list->RSSetViewports(1, &vp);
    g_list->RSSetScissorRects(1, &sc);

    ResourceTransition(g_rtBuffers[g_frameIndex], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += g_frameIndex * g_rtvSize;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    g_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    float clear[4] = { 0.07f, 0.08f, 0.12f, 1.0f };
    g_list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    g_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_list->IASetVertexBuffers(0, 1, &g_vbv);
    g_list->IASetIndexBuffer(&g_ibv);
    g_list->DrawIndexedInstanced(36, 1, 0, 0, 0);

    ResourceTransition(g_rtBuffers[g_frameIndex], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    g_list->Close();
    ID3D12CommandList* lists[] = { g_list };
    g_queue->ExecuteCommandLists(1, lists);
    g_swap->Present(1, 0);
    WaitForGpu();
}

static void UpdateTitle(HWND hwnd, double msPerFrame) {
    char title[256];
    if (g_linalgRan) {
        sprintf_s(title, "D3D12 - SM 6.10 LinAlg ON | %s | Multiply %s, VectorAccumulate %s | %.2f ms/frame",
            g_deviceName, g_mulOk ? "OK" : "MISMATCH", g_accOk ? "OK" : "MISMATCH", msPerFrame);
    } else {
        sprintf_s(title, "D3D12 - SM 6.10 LinAlg OFF (%s) | %s | plain cube | %.2f ms/frame",
            g_fallbackReason, g_deviceName, msPerFrame);
    }
    SetWindowTextA(hwnd, title);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR cmdLine, int) {
    DeleteFileA("CubeD3D12.LinearAlgebra.log");
    logf_line("D3D12.LinearAlgebra starting (args: %s)", cmdLine && *cmdLine ? cmdLine : "none");
    g_argHw = cmdLine && strstr(cmdLine, "--hw") != nullptr;
    g_argWarp = cmdLine && strstr(cmdLine, "--warp") != nullptr;
    g_argDebug = cmdLine && strstr(cmdLine, "--debug") != nullptr;
    if (g_argHw && g_argWarp) { logf_line("--hw and --warp both given; --warp wins"); g_argHw = false; }

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    g_exeDir = exePath;
    g_exeDir = g_exeDir.substr(0, g_exeDir.find_last_of(L"\\/") + 1);
    ResolveDxcIncludeDir();

    // [LEARN] The debug layer must be enabled before the device exists.
    if (g_argDebug) {
        ID3D12Debug* dbg = nullptr;
        HRESULT dhr = D3D12GetDebugInterface(__uuidof(ID3D12Debug), (void**)&dbg);
        logf_line("D3D12GetDebugInterface hr=0x%08X", (unsigned)dhr);
        if (SUCCEEDED(dhr) && dbg) { dbg->EnableDebugLayer(); dbg->Release(); logf_line("D3D12 debug layer enabled"); }
    }

    // [LEARN] SM 6.10 is still "experimental": the runtime refuses cs_6_10 DXIL unless
    // this opt-in happens before device creation (and Developer Mode is on).
    HRESULT experimentalHr = D3D12EnableExperimentalFeatures(1, &D3D12ExperimentalShaderModels, nullptr, nullptr);
    g_experimentalShadersEnabled = SUCCEEDED(experimentalHr);
    logf_line("D3D12EnableExperimentalFeatures(D3D12ExperimentalShaderModels) hr=0x%08X enabled=%u",
        (unsigned)experimentalHr, g_experimentalShadersEnabled ? 1u : 0u);
    if (!g_experimentalShadersEnabled) logf_line("Experimental shader models unavailable (Developer Mode off?); LinAlg path will be skipped");

    WNDCLASS wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = "D3D12LinearAlgebra";
    RegisterClass(&wc);
    RECT r = { 0, 0, (LONG)g_width, (LONG)g_height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindow("D3D12LinearAlgebra", "D3D12 - SM 6.10 Linear Algebra",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, hInst, nullptr);
    if (!InitD3D(hwnd)) {
        logf_line("D3D12 Linear Algebra init failed");
        DrainDebugMessages("init-failed");
        return 1;
    }
    logf_line("Running on %s; LinAlg %s", g_deviceName, g_linalgRan ? "ON" : "OFF (plain cube fallback)");
    UpdateTitle(hwnd, 0.0);
    ShowWindow(hwnd, SW_SHOW);

    LARGE_INTEGER freq, start, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start); last = start;
    UINT framesSinceTitle = 0;
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); continue; }
        QueryPerformanceCounter(&now);
        Render((float)((now.QuadPart - start.QuadPart) / (double)freq.QuadPart));
        ++framesSinceTitle;
        double since = (now.QuadPart - last.QuadPart) / (double)freq.QuadPart;
        if (since >= 0.5) {
            UpdateTitle(hwnd, since * 1000.0 / framesSinceTitle);
            DrainDebugMessages("frame");
            framesSinceTitle = 0; last = now;
        }
    }
    WaitForGpu();
    DrainDebugMessages("shutdown");
    if (g_infoQueue) logf_line("Debug layer summary: %u errors/corruptions, %u warnings", g_debugErrors, g_debugWarnings);
    logf_line("D3D12.LinearAlgebra exiting");
    return (int)msg.wParam;
}
