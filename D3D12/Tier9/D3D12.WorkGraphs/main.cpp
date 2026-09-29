// D3D12 Work Graphs (2024) teaching sample.
// Work graphs let shader nodes launch more shader-node work without a CPU
// round-trip, which is useful for adaptive GPU-driven workloads such as
// procedural expansion, recursive refinement, and culling that spawns follow-up
// work. Requires Windows SDK 10.0.26100.0 for the API names. Work Graphs Tier 1
// support is optional here: unsupported devices log and still render a rotating
// cube so the sample remains runnable. HLSL is compiled at runtime with DXC.
// ESC quits.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxcapi.h>
#include <DirectXMath.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

using namespace DirectX;

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

static const UINT kFrames = 2;
struct Vertex { XMFLOAT3 pos; XMFLOAT4 col; };
struct RootData { XMFLOAT4X4 mvp; };

static UINT g_width = 800, g_height = 600;
static ID3D12Device*               g_dev = nullptr;
static ID3D12Device2*              g_dev2 = nullptr;
static ID3D12Device9*              g_dev9 = nullptr;
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
static ID3D12RootSignature*        g_cubeRootSig = nullptr;
static ID3D12PipelineState*        g_pso = nullptr;
static ID3D12Resource*             g_vb = nullptr;
static ID3D12Resource*             g_ib = nullptr;
static D3D12_VERTEX_BUFFER_VIEW    g_vbv = {};
static D3D12_INDEX_BUFFER_VIEW     g_ibv = {};
static ID3D12Fence*                g_fence = nullptr;
static UINT64                      g_fenceValue = 0;
static HANDLE                      g_fenceEvent = nullptr;
static UINT                        g_frameIndex = 0;

static D3D12_WORK_GRAPHS_TIER      g_workGraphsTier = D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED;
static ID3D12RootSignature*        g_wgRootSig = nullptr;
static ID3D12StateObject*          g_wgState = nullptr;
static D3D12_PROGRAM_IDENTIFIER    g_wgProgram = {};
static ID3D12Resource*             g_wgBacking = nullptr;
static ID3D12Resource*             g_wgOutput = nullptr;
static UINT64                      g_wgBackingSize = 0;
static bool                        g_workGraphReady = false;
static bool                        g_wgNeedsInitialize = true;
static bool                        g_wgLoggedDispatch = false;

static const Vertex g_cubeVerts[8] = {
    { XMFLOAT3(-1,-1,-1), XMFLOAT4(0,0,0,1) }, { XMFLOAT3(-1, 1,-1), XMFLOAT4(0,1,0,1) },
    { XMFLOAT3( 1, 1,-1), XMFLOAT4(1,1,0,1) }, { XMFLOAT3( 1,-1,-1), XMFLOAT4(1,0,0,1) },
    { XMFLOAT3(-1,-1, 1), XMFLOAT4(0,0,1,1) }, { XMFLOAT3(-1, 1, 1), XMFLOAT4(0,1,1,1) },
    { XMFLOAT3( 1, 1, 1), XMFLOAT4(1,1,1,1) }, { XMFLOAT3( 1,-1, 1), XMFLOAT4(1,0,1,1) },
};
static const UINT16 g_cubeIdx[36] = {
    0,1,2, 0,2,3,  4,6,5, 4,7,6,  4,5,1, 4,1,0,
    3,2,6, 3,6,7,  1,5,6, 1,6,2,  4,0,3, 4,3,7,
};

static void logf_line(const char* fmt, ...) {
    FILE* f = nullptr;
    fopen_s(&f, "CubeD3D12.WorkGraphs.log", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

static const char* g_cubeSrc =
"cbuffer CB : register(b0) { float4x4 mvp; };\n"
"struct VIn { float3 pos : POSITION; float4 col : COLOR; };\n"
"struct VOut { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
"VOut VSMain(VIn i) { VOut o; o.pos = mul(float4(i.pos, 1.0), mvp); o.col = i.col; return o; }\n"
"float4 PSMain(VOut i) : SV_TARGET { return i.col; }\n";

// [LEARN] A work graph is compiled as a DXIL library. The node shader is not
// dispatched by name from the CPU; it becomes a node in a named GPU program.
static const char* g_wgSrc =
"RWByteAddressBuffer gOutput : register(u0);\n"
"[Shader(\"node\")]\n"
"[NodeLaunch(\"broadcasting\")]\n"
"[NodeIsProgramEntry]\n"
"[NodeDispatchGrid(1, 1, 1)]\n"
"[NumThreads(4, 1, 1)]\n"
"void WGWriteNode(uint3 tid : SV_DispatchThreadID) {\n"
"  gOutput.Store(tid.x * 4, 0x57473000u + tid.x);\n"
"}\n";

// ---- Minimal alignment-correct pipeline-state stream subobject wrapper ----
template<typename T, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type>
struct alignas(void*) Sub {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE _t = Type;
    T _v{};
    Sub() = default;
    Sub(const T& v) : _v(v) {}
    Sub& operator=(const T& v) { _v = v; return *this; }
};

static HMODULE LoadDxc() {
    static const wchar_t* dirs[] = {
        L"",
        L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.26100.0\\x64\\",
        L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.22621.0\\x64\\",
        L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.20348.0\\x64\\",
        L"C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.19041.0\\x64\\",
    };
    for (auto d : dirs) {
        std::wstring dir(d);
        LoadLibraryExW((dir + L"dxil.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        HMODULE m = LoadLibraryExW((dir + L"dxcompiler.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (m) return m;
    }
    return nullptr;
}

static IDxcBlob* CompileDXC(const char* src, const wchar_t* entry, const wchar_t* target) {
    static HMODULE dll = LoadDxc();
    if (!dll) { logf_line("DXC load failed"); return nullptr; }
    auto create = (DxcCreateInstanceProc)GetProcAddress(dll, "DxcCreateInstance");
    if (!create) return nullptr;
    IDxcCompiler3* comp = nullptr;
    if (FAILED(create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), (void**)&comp))) return nullptr;
    DxcBuffer buf = { src, strlen(src), DXC_CP_UTF8 };
    const wchar_t* args[] = { L"-E", entry, L"-T", target, L"-O3" };
    IDxcResult* result = nullptr;
    HRESULT hr = comp->Compile(&buf, args, _countof(args), nullptr, __uuidof(IDxcResult), (void**)&result);
    comp->Release();
    if (FAILED(hr) || !result) return nullptr;
    HRESULT status = E_FAIL; result->GetStatus(&status);
    if (FAILED(status)) {
        IDxcBlobUtf8* errs = nullptr;
        result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), (void**)&errs, nullptr);
        if (errs && errs->GetStringLength()) logf_line("DXC %S/%S: %s", entry, target, errs->GetStringPointer());
        if (errs) errs->Release();
        result->Release();
        return nullptr;
    }
    IDxcBlob* obj = nullptr;
    result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), (void**)&obj, nullptr);
    result->Release();
    return obj;
}

static IDxcBlob* CompileLib68(const char* src) {
    static HMODULE dll = LoadDxc();
    if (!dll) { logf_line("DXC load failed for work graph"); return nullptr; }
    auto create = (DxcCreateInstanceProc)GetProcAddress(dll, "DxcCreateInstance");
    if (!create) return nullptr;
    IDxcCompiler3* comp = nullptr;
    if (FAILED(create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), (void**)&comp))) return nullptr;
    DxcBuffer buf = { src, strlen(src), DXC_CP_UTF8 };
    const wchar_t* args[] = { L"-T", L"lib_6_8", L"-O3" };
    IDxcResult* result = nullptr;
    HRESULT hr = comp->Compile(&buf, args, _countof(args), nullptr, __uuidof(IDxcResult), (void**)&result);
    comp->Release();
    if (FAILED(hr) || !result) return nullptr;
    HRESULT status = E_FAIL; result->GetStatus(&status);
    if (FAILED(status)) {
        IDxcBlobUtf8* errs = nullptr;
        result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), (void**)&errs, nullptr);
        if (errs && errs->GetStringLength()) logf_line("DXC lib_6_8: %s", errs->GetStringPointer());
        if (errs) errs->Release();
        result->Release();
        return nullptr;
    }
    IDxcBlob* obj = nullptr;
    result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), (void**)&obj, nullptr);
    result->Release();
    return obj;
}

static void WaitForGpu() {
    const UINT64 v = ++g_fenceValue;
    g_queue->Signal(g_fence, v);
    if (g_fence->GetCompletedValue() < v) {
        g_fence->SetEventOnCompletion(v, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
    g_frameIndex = g_swap->GetCurrentBackBufferIndex();
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN: if (w == VK_ESCAPE) PostQuitMessage(0); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, w, l);
}

static ID3D12Resource* CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heapType,
    D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags, const void* data = nullptr) {
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = heapType;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size ? size : 1;
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = flags;
    ID3D12Resource* res = nullptr;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state,
        nullptr, __uuidof(ID3D12Resource), (void**)&res))) return nullptr;
    if (data) {
        void* mapped = nullptr; D3D12_RANGE none = { 0, 0 };
        if (SUCCEEDED(res->Map(0, &none, &mapped))) {
            memcpy(mapped, data, (size_t)size);
            res->Unmap(0, nullptr);
        }
    }
    return res;
}

static bool CreateCubePipeline() {
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.Num32BitValues = 16;
    rp.Constants.ShaderRegister = 0;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 1; rsd.pParameters = &rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* sig = nullptr; ID3DBlob* sigErr = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr))) {
        if (sigErr) { logf_line("cube root signature failed"); sigErr->Release(); }
        return false;
    }
    g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
        __uuidof(ID3D12RootSignature), (void**)&g_cubeRootSig);
    sig->Release();

    IDxcBlob* vs = CompileDXC(g_cubeSrc, L"VSMain", L"vs_6_0");
    IDxcBlob* ps = CompileDXC(g_cubeSrc, L"PSMain", L"ps_6_0");
    if (!vs || !ps) return false;

    D3D12_INPUT_ELEMENT_DESC il[2] = {};
    il[0] = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    il[1] = { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    D3D12_INPUT_LAYOUT_DESC input = { il, _countof(il) };
    D3D12_RT_FORMAT_ARRAY rtFmt = {};
    rtFmt.NumRenderTargets = 1; rtFmt.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D12_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D12_FILL_MODE_SOLID; raster.CullMode = D3D12_CULL_MODE_NONE;
    raster.DepthClipEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = TRUE; depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    // [LEARN] The cube uses a conventional graphics PSO so Work Graph support can
    // be taught independently from whether the GPU can execute the graph path.
    struct CubeStream {
        Sub<ID3D12RootSignature*, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> rootSig;
        Sub<D3D12_INPUT_LAYOUT_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT> input;
        Sub<D3D12_PRIMITIVE_TOPOLOGY_TYPE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY> topology;
        Sub<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS> vs;
        Sub<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS> ps;
        Sub<D3D12_RT_FORMAT_ARRAY, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS> rtv;
        Sub<DXGI_FORMAT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT> dsv;
        Sub<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> raster;
        Sub<D3D12_DEPTH_STENCIL_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL> depth;
        Sub<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> sample;
    } stream;
    stream.rootSig = g_cubeRootSig;
    stream.input = input;
    stream.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    stream.vs = D3D12_SHADER_BYTECODE{ vs->GetBufferPointer(), vs->GetBufferSize() };
    stream.ps = D3D12_SHADER_BYTECODE{ ps->GetBufferPointer(), ps->GetBufferSize() };
    stream.rtv = rtFmt;
    stream.dsv = DXGI_FORMAT_D32_FLOAT;
    stream.raster = raster;
    stream.depth = depth;
    stream.sample = DXGI_SAMPLE_DESC{ 1, 0 };

    D3D12_PIPELINE_STATE_STREAM_DESC psd = { sizeof(stream), &stream };
    HRESULT hr = g_dev2->CreatePipelineState(&psd, __uuidof(ID3D12PipelineState), (void**)&g_pso);
    vs->Release(); ps->Release();
    if (FAILED(hr)) { logf_line("cube pipeline hr=0x%08X", (unsigned)hr); return false; }

    g_vb = CreateBuffer(sizeof(g_cubeVerts), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
        D3D12_RESOURCE_FLAG_NONE, g_cubeVerts);
    g_ib = CreateBuffer(sizeof(g_cubeIdx), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
        D3D12_RESOURCE_FLAG_NONE, g_cubeIdx);
    if (!g_vb || !g_ib) return false;
    g_vbv.BufferLocation = g_vb->GetGPUVirtualAddress();
    g_vbv.StrideInBytes = sizeof(Vertex);
    g_vbv.SizeInBytes = sizeof(g_cubeVerts);
    g_ibv.BufferLocation = g_ib->GetGPUVirtualAddress();
    g_ibv.Format = DXGI_FORMAT_R16_UINT;
    g_ibv.SizeInBytes = sizeof(g_cubeIdx);
    return true;
}

static void QueryWorkGraphsSupport() {
    D3D12_FEATURE_DATA_D3D12_OPTIONS21 opt21 = {};
    HRESULT hr = g_dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21, &opt21, sizeof(opt21));
    if (SUCCEEDED(hr)) g_workGraphsTier = opt21.WorkGraphsTier;
    logf_line("WorkGraphsTier=%u CheckFeatureSupport=0x%08X", (unsigned)g_workGraphsTier, (unsigned)hr);
    if (g_workGraphsTier == D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED)
        logf_line("Work Graphs unsupported: falling back to plain rotating cube.");
}

static void TryCreateWorkGraph() {
    QueryWorkGraphsSupport();
    if (g_workGraphsTier == D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED) return;
    if (FAILED(g_dev->QueryInterface(__uuidof(ID3D12Device9), (void**)&g_dev9))) {
        logf_line("ID3D12Device9 unavailable: falling back to plain rotating cube.");
        return;
    }
    if (!g_list10) {
        logf_line("ID3D12GraphicsCommandList10 unavailable: falling back to plain rotating cube.");
        return;
    }

    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp.Descriptor.ShaderRegister = 0;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 1; rsd.pParameters = &rp;
    ID3DBlob* sig = nullptr; ID3DBlob* sigErr = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr))) {
        if (sigErr) { logf_line("work graph root signature failed"); sigErr->Release(); }
        return;
    }
    g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
        __uuidof(ID3D12RootSignature), (void**)&g_wgRootSig);
    sig->Release();

    IDxcBlob* lib = CompileLib68(g_wgSrc);
    if (!lib) { logf_line("work graph DXIL library compile failed; falling back to cube only."); return; }

    D3D12_EXPORT_DESC exp = {};
    exp.Name = L"WGWriteNode";
    D3D12_DXIL_LIBRARY_DESC libDesc = {};
    libDesc.DXILLibrary = D3D12_SHADER_BYTECODE{ lib->GetBufferPointer(), lib->GetBufferSize() };
    libDesc.NumExports = 1; libDesc.pExports = &exp;
    D3D12_GLOBAL_ROOT_SIGNATURE grs = {}; grs.pGlobalRootSignature = g_wgRootSig;

    D3D12_NODE_ID entry = { L"WGWriteNode", 0 };
    D3D12_NODE node = {};
    node.NodeType = D3D12_NODE_TYPE_SHADER;
    node.Shader.Shader = L"WGWriteNode";
    node.Shader.OverridesType = D3D12_NODE_OVERRIDES_TYPE_NONE;
    D3D12_WORK_GRAPH_DESC wgDesc = {};
    wgDesc.ProgramName = L"WorkGraphsDemo";
    wgDesc.Flags = D3D12_WORK_GRAPH_FLAG_INCLUDE_ALL_AVAILABLE_NODES;
    wgDesc.NumEntrypoints = 1; wgDesc.pEntrypoints = &entry;
    wgDesc.NumExplicitlyDefinedNodes = 1; wgDesc.pExplicitlyDefinedNodes = &node;

    D3D12_STATE_SUBOBJECT sub[3] = {};
    sub[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY; sub[0].pDesc = &libDesc;
    sub[1].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE; sub[1].pDesc = &grs;
    sub[2].Type = D3D12_STATE_SUBOBJECT_TYPE_WORK_GRAPH; sub[2].pDesc = &wgDesc;
    D3D12_STATE_OBJECT_DESC soDesc = {};
    soDesc.Type = D3D12_STATE_OBJECT_TYPE_EXECUTABLE;
    soDesc.NumSubobjects = _countof(sub); soDesc.pSubobjects = sub;

    // [LEARN] CreateStateObject packages the DXIL library plus a WORK_GRAPH
    // subobject into an executable program, similar in spirit to a DXR pipeline.
    HRESULT hr = g_dev9->CreateStateObject(&soDesc, __uuidof(ID3D12StateObject), (void**)&g_wgState);
    lib->Release();
    if (FAILED(hr)) { logf_line("CreateStateObject(work graph) hr=0x%08X; falling back to cube only.", (unsigned)hr); return; }

    ID3D12WorkGraphProperties* wgProps = nullptr;
    if (FAILED(g_wgState->QueryInterface(__uuidof(ID3D12WorkGraphProperties), (void**)&wgProps))) {
        logf_line("ID3D12WorkGraphProperties unavailable; falling back to cube only.");
        return;
    }
    UINT wgIndex = wgProps->GetWorkGraphIndex(L"WorkGraphsDemo");
    D3D12_WORK_GRAPH_MEMORY_REQUIREMENTS memReq = {};
    wgProps->GetWorkGraphMemoryRequirements(wgIndex, &memReq);
    wgProps->Release();
    g_wgBackingSize = memReq.MinSizeInBytes;
    logf_line("WorkGraph memory min=%llu max=%llu granularity=%u",
        (unsigned long long)memReq.MinSizeInBytes, (unsigned long long)memReq.MaxSizeInBytes,
        memReq.SizeGranularityInBytes);

    ID3D12StateObjectProperties1* props1 = nullptr;
    if (FAILED(g_wgState->QueryInterface(__uuidof(ID3D12StateObjectProperties1), (void**)&props1))) {
        logf_line("ID3D12StateObjectProperties1 unavailable; falling back to cube only.");
        return;
    }
    g_wgProgram = props1->GetProgramIdentifier(L"WorkGraphsDemo");
    props1->Release();

    // [LEARN] The driver owns the graph scheduler's temporary record storage, but
    // the app owns and binds the backing memory so lifetimes and synchronization
    // are explicit like any other D3D12 resource.
    if (g_wgBackingSize) {
        g_wgBacking = CreateBuffer(g_wgBackingSize, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!g_wgBacking) { logf_line("work graph backing allocation failed"); return; }
    }
    g_wgOutput = CreateBuffer(16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!g_wgOutput) { logf_line("work graph UAV output allocation failed"); return; }

    g_workGraphReady = true;
    logf_line("Work graph path ready.");
}

static bool InitD3D(HWND hwnd) {
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&factory))) return false;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
        __uuidof(ID3D12Device), (void**)&g_dev))) { factory->Release(); return false; }
    if (FAILED(g_dev->QueryInterface(__uuidof(ID3D12Device2), (void**)&g_dev2))) { factory->Release(); return false; }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g_dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&g_queue);

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.BufferCount = kFrames;
    sd.Width = g_width; sd.Height = g_height;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc.Count = 1;
    IDXGISwapChain1* sc1 = nullptr;
    if (FAILED(factory->CreateSwapChainForHwnd(g_queue, hwnd, &sd, nullptr, nullptr, &sc1))) {
        factory->Release(); return false;
    }
    sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&g_swap);
    sc1->Release(); factory->Release();
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
    g_dev->CreateCommittedResource(&dhp, D3D12_HEAP_FLAG_NONE, &drd,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, __uuidof(ID3D12Resource), (void**)&g_depth);
    g_dev->CreateDepthStencilView(g_depth, nullptr, g_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    if (!CreateCubePipeline()) return false;

    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        __uuidof(ID3D12CommandAllocator), (void**)&g_alloc);
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, g_pso,
        __uuidof(ID3D12GraphicsCommandList), (void**)&g_list);
    if (FAILED(g_list->QueryInterface(__uuidof(ID3D12GraphicsCommandList10), (void**)&g_list10))) {
        g_list10 = nullptr;
        logf_line("ID3D12GraphicsCommandList10 unavailable.");
    }
    g_list->Close();

    // [LEARN] Work Graphs are a feature-tier path. The sample logs the tier and
    // keeps rendering the cube if the graph program cannot be created here.
    TryCreateWorkGraph();

    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_fence);
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    return true;
}

static void DispatchTeachingWorkGraph() {
    if (!g_workGraphReady || !g_list10) return;

    g_list->SetComputeRootSignature(g_wgRootSig);
    g_list->SetComputeRootUnorderedAccessView(0, g_wgOutput->GetGPUVirtualAddress());

    D3D12_SET_PROGRAM_DESC set = {};
    set.Type = D3D12_PROGRAM_TYPE_WORK_GRAPH;
    set.WorkGraph.ProgramIdentifier = g_wgProgram;
    set.WorkGraph.Flags = g_wgNeedsInitialize ? D3D12_SET_WORK_GRAPH_FLAG_INITIALIZE : D3D12_SET_WORK_GRAPH_FLAG_NONE;
    if (g_wgBacking) {
        set.WorkGraph.BackingMemory.StartAddress = g_wgBacking->GetGPUVirtualAddress();
        set.WorkGraph.BackingMemory.SizeInBytes = g_wgBackingSize;
    }
    g_list10->SetProgram(&set);
    g_wgNeedsInitialize = false;

    D3D12_DISPATCH_GRAPH_DESC dispatch = {};
    dispatch.Mode = D3D12_DISPATCH_MODE_NODE_CPU_INPUT;
    dispatch.NodeCPUInput.EntrypointIndex = 0;
    dispatch.NodeCPUInput.NumRecords = 1;
    dispatch.NodeCPUInput.pRecords = nullptr;
    dispatch.NodeCPUInput.RecordStrideInBytes = 0;

    // [LEARN] DispatchGraph feeds entry-node records to the GPU scheduler. A
    // one-node graph is intentionally tiny so the API mechanics are the focus.
    g_list10->DispatchGraph(&dispatch);

    D3D12_RESOURCE_BARRIER uav = {};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = g_wgOutput;
    g_list->ResourceBarrier(1, &uav);
    if (!g_wgLoggedDispatch) {
        logf_line("DispatchGraph recorded: WGWriteNode writes four UAV dwords.");
        g_wgLoggedDispatch = true;
    }
}

static void Render(float t) {
    g_alloc->Reset();
    g_list->Reset(g_alloc, g_pso);

    DispatchTeachingWorkGraph();

    g_list->SetPipelineState(g_pso);
    g_list->SetGraphicsRootSignature(g_cubeRootSig);
    XMMATRIX world = XMMatrixRotationY(t) * XMMatrixRotationX(t * 0.5f);
    XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 0, -6, 0), XMVectorSet(0, 0, 0, 0), XMVectorSet(0, 1, 0, 0));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, (float)g_width / g_height, 0.1f, 100.0f);
    RootData rd; XMStoreFloat4x4(&rd.mvp, XMMatrixTranspose(world * view * proj));
    g_list->SetGraphicsRoot32BitConstants(0, 16, &rd, 0);

    D3D12_VIEWPORT vp = { 0, 0, (float)g_width, (float)g_height, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)g_width, (LONG)g_height };
    g_list->RSSetViewports(1, &vp);
    g_list->RSSetScissorRects(1, &sc);

    D3D12_RESOURCE_BARRIER toRT = {};
    toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRT.Transition.pResource = g_rtBuffers[g_frameIndex];
    toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_list->ResourceBarrier(1, &toRT);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += g_frameIndex * g_rtvSize;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    g_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    float clear[4] = { 0.07f, 0.09f, 0.14f, 1.0f };
    g_list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    g_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_list->IASetVertexBuffers(0, 1, &g_vbv);
    g_list->IASetIndexBuffer(&g_ibv);
    g_list->DrawIndexedInstanced(36, 1, 0, 0, 0);

    D3D12_RESOURCE_BARRIER toPresent = toRT;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_list->ResourceBarrier(1, &toPresent);

    g_list->Close();
    ID3D12CommandList* lists[] = { g_list };
    g_queue->ExecuteCommandLists(1, lists);
    g_swap->Present(1, 0);
    WaitForGpu();
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    WNDCLASS wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = "D3D12WorkGraphs";
    RegisterClass(&wc);
    RECT r = { 0, 0, (LONG)g_width, (LONG)g_height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindow("D3D12WorkGraphs", "D3D12 - Work Graphs (GPU-driven node launch)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, hInst, nullptr);
    if (!InitD3D(hwnd)) {
        logf_line("D3D12 WorkGraphs sample init failed before fallback cube could be created.");
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);

    DWORD start = GetTickCount();
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        else Render((GetTickCount() - start) / 1000.0f);
    }
    WaitForGpu();
    return (int)msg.wParam;
}
