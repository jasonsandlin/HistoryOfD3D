// D3D12 Mesh Shader showcase.
// A mesh shader (ms_6_5) generates the entire cube on the GPU — there is no input
// assembler, vertex buffer, or index buffer. The mesh shader emits 8 vertices and
// 12 triangles per thread group and pulses the cube with time. HLSL is compiled at
// runtime with DXC (dxcompiler.dll). Requires a GPU/WARP with mesh-shader support.
// ESC quits.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxcapi.h>
#include <DirectXMath.h>
#include <string>

using namespace DirectX;

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

static const UINT kFrames = 2;
struct RootData { XMFLOAT4X4 mvp; float time; };

static UINT g_width = 800, g_height = 600;
static ID3D12Device*              g_dev = nullptr;
static ID3D12Device2*             g_dev2 = nullptr;
static ID3D12CommandQueue*        g_queue = nullptr;
static IDXGISwapChain3*           g_swap = nullptr;
static ID3D12DescriptorHeap*      g_rtvHeap = nullptr;
static ID3D12DescriptorHeap*      g_dsvHeap = nullptr;
static UINT                       g_rtvSize = 0;
static ID3D12Resource*            g_rtBuffers[kFrames] = {};
static ID3D12Resource*            g_depth = nullptr;
static ID3D12CommandAllocator*    g_alloc = nullptr;
static ID3D12GraphicsCommandList6* g_list = nullptr;
static ID3D12RootSignature*       g_rootSig = nullptr;
static ID3D12PipelineState*       g_pso = nullptr;
static ID3D12Fence*               g_fence = nullptr;
static UINT64                     g_fenceValue = 0;
static HANDLE                     g_fenceEvent = nullptr;
static UINT                       g_frameIndex = 0;

// [LEARN] Spotlight: the mesh shader (ms_6_5) - it REPLACES the IA/VS/GS stages,
// emitting vertices and primitives directly from a compute-style thread group.
static const char* g_src =
"cbuffer CB : register(b0) { float4x4 mvp; float g_time; };\n"
"struct VOut { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
"static const float3 CUBE[8] = {\n"
"  float3(-1,-1,-1), float3(-1,1,-1), float3(1,1,-1), float3(1,-1,-1),\n"
"  float3(-1,-1, 1), float3(-1,1, 1), float3(1,1, 1), float3(1,-1, 1) };\n"
"static const float4 COL[8] = {\n"
"  float4(0,0,0,1), float4(0,1,0,1), float4(1,1,0,1), float4(1,0,0,1),\n"
"  float4(0,0,1,1), float4(0,1,1,1), float4(1,1,1,1), float4(1,0,1,1) };\n"
"static const uint3 TRIS[12] = {\n"
"  uint3(0,1,2), uint3(0,2,3), uint3(4,6,5), uint3(4,7,6),\n"
"  uint3(4,5,1), uint3(4,1,0), uint3(3,2,6), uint3(3,6,7),\n"
"  uint3(1,5,6), uint3(1,6,2), uint3(4,0,3), uint3(4,3,7) };\n"
"[outputtopology(\"triangle\")]\n"
"[numthreads(12,1,1)]\n"
"void MSMain(uint tid : SV_GroupThreadID,\n"
"            out vertices VOut verts[8], out indices uint3 tris[12]) {\n"
"  SetMeshOutputCounts(8, 12);\n"
"  if (tid < 8) {\n"
"    float s = 1.0 + 0.12 * sin(g_time * 2.0);\n"
"    verts[tid].pos = mul(float4(CUBE[tid] * s, 1.0), mvp);\n"
"    verts[tid].col = COL[tid];\n"
"  }\n"
"  tris[tid] = TRIS[tid];\n"
"}\n"
"float4 PSMain(VOut i) : SV_TARGET { return i.col; }\n";

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
    if (!dll) return nullptr;
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
        if (errs && errs->GetStringLength()) OutputDebugStringA(errs->GetStringPointer());
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

static bool InitD3D(HWND hwnd) {
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&factory))) return false;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
        __uuidof(ID3D12Device), (void**)&g_dev))) { factory->Release(); return false; }
    if (FAILED(g_dev->QueryInterface(__uuidof(ID3D12Device2), (void**)&g_dev2))) { factory->Release(); return false; }

    // Require mesh shader support.
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 opt7 = {};
    if (FAILED(g_dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opt7, sizeof(opt7))) ||
        opt7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED) {
        factory->Release(); return false;
    }

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

    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.Num32BitValues = 17;
    rp.Constants.ShaderRegister = 0;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 1; rsd.pParameters = &rp;
    ID3DBlob* sig = nullptr; ID3DBlob* sigErr = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr))) {
        if (sigErr) sigErr->Release(); return false;
    }
    g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
        __uuidof(ID3D12RootSignature), (void**)&g_rootSig);
    sig->Release();

    IDxcBlob* ms = CompileDXC(g_src, L"MSMain", L"ms_6_5");
    IDxcBlob* ps = CompileDXC(g_src, L"PSMain", L"ps_6_5");
    if (!ms || !ps) return false;

    D3D12_RT_FORMAT_ARRAY rtFmt = {};
    rtFmt.NumRenderTargets = 1; rtFmt.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D12_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D12_FILL_MODE_SOLID; raster.CullMode = D3D12_CULL_MODE_NONE;
    raster.DepthClipEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = TRUE; depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    struct MeshStream {
        Sub<ID3D12RootSignature*, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> rootSig;
        Sub<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS> ms;
        Sub<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS> ps;
        Sub<D3D12_RT_FORMAT_ARRAY, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS> rtv;
        Sub<DXGI_FORMAT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT> dsv;
        Sub<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> raster;
        Sub<D3D12_DEPTH_STENCIL_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL> depth;
        Sub<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> sample;
    } stream;
    stream.rootSig = g_rootSig;
    stream.ms = D3D12_SHADER_BYTECODE{ ms->GetBufferPointer(), ms->GetBufferSize() };
    stream.ps = D3D12_SHADER_BYTECODE{ ps->GetBufferPointer(), ps->GetBufferSize() };
    stream.rtv = rtFmt;
    stream.dsv = DXGI_FORMAT_D32_FLOAT;
    stream.raster = raster;
    stream.depth = depth;
    stream.sample = DXGI_SAMPLE_DESC{ 1, 0 };

    D3D12_PIPELINE_STATE_STREAM_DESC psd = { sizeof(stream), &stream };
    if (FAILED(g_dev2->CreatePipelineState(&psd, __uuidof(ID3D12PipelineState), (void**)&g_pso)))
        return false;

    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        __uuidof(ID3D12CommandAllocator), (void**)&g_alloc);
    ID3D12GraphicsCommandList* list0 = nullptr;
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, g_pso,
        __uuidof(ID3D12GraphicsCommandList), (void**)&list0);
    if (FAILED(list0->QueryInterface(__uuidof(ID3D12GraphicsCommandList6), (void**)&g_list)))
        return false;
    list0->Release();
    g_list->Close();

    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_fence);
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    return true;
}

static void Render(float t) {
    g_alloc->Reset();
    g_list->Reset(g_alloc, g_pso);

    g_list->SetGraphicsRootSignature(g_rootSig);
    XMMATRIX world = XMMatrixRotationY(t) * XMMatrixRotationX(t * 0.5f);
    XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 0, -6, 0), XMVectorSet(0, 0, 0, 0), XMVectorSet(0, 1, 0, 0));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, (float)g_width / g_height, 0.1f, 100.0f);
    RootData rd; XMStoreFloat4x4(&rd.mvp, XMMatrixTranspose(world * view * proj)); rd.time = t;
    g_list->SetGraphicsRoot32BitConstants(0, 17, &rd, 0);

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

    float clear[4] = { 0.1f, 0.1f, 0.2f, 1.0f };
    g_list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    g_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    g_list->DispatchMesh(1, 1, 1);

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
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = "D3D12Mesh";
    RegisterClass(&wc);
    RECT r = { 0, 0, (LONG)g_width, (LONG)g_height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindow("D3D12Mesh", "D3D12 - Mesh Shader (GPU-generated cube)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, hInst, nullptr);
    if (!InitD3D(hwnd)) {
        MessageBox(hwnd, "D3D12 mesh shader init failed (needs a mesh-shader capable GPU/WARP "
                         "and dxcompiler.dll/dxil.dll).", "Error", MB_OK);
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
