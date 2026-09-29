// D3D12 Enhanced Barriers teaching sample.
// This draws the family rotating cube while teaching the modern Barrier() model:
// SYNC says which pipeline work must be ordered, ACCESS says how the resource is
// read or written, and LAYOUT says the texture layout. That replaces the older
// single D3D12_RESOURCE_STATES transition model and avoids over-synchronizing.
// Requires Windows SDK 10.0.26100.0 headers and a D3D12 runtime/driver. If the
// device reports no enhanced-barrier support, the sample logs and falls back to
// legacy ResourceBarrier transitions so it still renders. ESC quits.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxcapi.h>
#include <DirectXMath.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>

using namespace DirectX;

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

static const UINT kFrames = 2;
struct Vertex { XMFLOAT3 pos; XMFLOAT4 color; };
struct RootData { XMFLOAT4X4 mvp; float time; };

static UINT g_width = 800, g_height = 600;
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
static ID3D12GraphicsCommandList7* g_list7 = nullptr;
static ID3D12RootSignature*        g_rootSig = nullptr;
static ID3D12PipelineState*        g_pso = nullptr;
static ID3D12Resource*             g_vb = nullptr;
static ID3D12Resource*             g_ib = nullptr;
static D3D12_VERTEX_BUFFER_VIEW    g_vbv = {};
static D3D12_INDEX_BUFFER_VIEW     g_ibv = {};
static ID3D12Fence*                g_fence = nullptr;
static UINT64                      g_fenceValue = 0;
static HANDLE                      g_fenceEvent = nullptr;
static UINT                        g_frameIndex = 0;
static bool                        g_enhancedBarriersSupported = false;
static bool                        g_useEnhancedBarriers = false;

static void logf_line(const char* fmt, ...) {
    FILE* f = nullptr;
    if (fopen_s(&f, "CubeD3D12.EnhancedBarriers.log", "a") != 0 || !f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// [LEARN] The cube remains ordinary VS+PS work so the only new concept in this
// sample is the barrier model around the swap-chain texture.
static const char* g_shaderSrc =
"cbuffer CB : register(b0) { float4x4 mvp; float g_time; };\n"
"struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
"VSOut VSMain(float3 pos : POSITION, float4 col : COLOR) {\n"
"  VSOut o; o.pos = mul(float4(pos, 1.0), mvp); o.col = col; return o;\n"
"}\n"
"float4 PSMain(VSOut i) : SV_TARGET {\n"
"  float pulse = 0.78 + 0.22 * sin(g_time * 2.0 + i.col.r * 6.28318);\n"
"  return float4(i.col.rgb * pulse, 1.0);\n"
"}\n";

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
    logf_line("dxcompiler.dll not found");
    return nullptr;
}

static IDxcBlob* CompileDXC(const char* src, const wchar_t* entry, const wchar_t* target) {
    static HMODULE dll = LoadDxc();
    if (!dll) return nullptr;
    auto create = (DxcCreateInstanceProc)GetProcAddress(dll, "DxcCreateInstance");
    if (!create) { logf_line("DxcCreateInstance export not found"); return nullptr; }
    IDxcCompiler3* comp = nullptr;
    if (FAILED(create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), (void**)&comp))) {
        logf_line("DxcCreateInstance(CLSID_DxcCompiler) failed");
        return nullptr;
    }
    DxcBuffer buf = { src, strlen(src), DXC_CP_UTF8 };
    const wchar_t* args[] = { L"-E", entry, L"-T", target, L"-O3" };
    IDxcResult* result = nullptr;
    HRESULT hr = comp->Compile(&buf, args, _countof(args), nullptr, __uuidof(IDxcResult), (void**)&result);
    comp->Release();
    if (FAILED(hr) || !result) { logf_line("DXC compile call failed hr=0x%08X", (unsigned)hr); return nullptr; }
    HRESULT status = E_FAIL; result->GetStatus(&status);
    if (FAILED(status)) {
        IDxcBlobUtf8* errs = nullptr;
        result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), (void**)&errs, nullptr);
        if (errs && errs->GetStringLength()) logf_line("DXC errors: %s", errs->GetStringPointer());
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

static ID3D12Resource* CreateUploadBuffer(const void* data, UINT size) {
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* res = nullptr;
    HRESULT hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&res);
    if (FAILED(hr) || !res) { logf_line("CreateCommittedResource upload hr=0x%08X", (unsigned)hr); return nullptr; }
    void* mapped = nullptr; D3D12_RANGE none = { 0, 0 };
    res->Map(0, &none, &mapped); memcpy(mapped, data, size); res->Unmap(0, nullptr);
    return res;
}

// [LEARN] Capability queries are not optional for frontier features: the SDK can
// expose the symbols even when the current GPU/driver cannot execute them.
static void QueryEnhancedBarrierSupport() {
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 opt12 = {};
    HRESULT hr = g_dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &opt12, sizeof(opt12));
    g_enhancedBarriersSupported = SUCCEEDED(hr) && opt12.EnhancedBarriersSupported;
    logf_line("D3D12_OPTIONS12 hr=0x%08X EnhancedBarriersSupported=%u",
        (unsigned)hr, g_enhancedBarriersSupported ? 1u : 0u);
    if (!g_enhancedBarriersSupported)
        logf_line("Enhanced barriers unsupported; using legacy ResourceBarrier transitions");
}

// [LEARN] Barrier() is a newer command-list method, so QueryInterface keeps the
// sample graceful on runtimes that create only the older list interface.
static void QueryCommandList7() {
    HRESULT hr = g_list->QueryInterface(__uuidof(ID3D12GraphicsCommandList7), (void**)&g_list7);
    if (FAILED(hr) || !g_list7) {
        logf_line("ID3D12GraphicsCommandList7 unavailable hr=0x%08X; using legacy ResourceBarrier transitions", (unsigned)hr);
        g_list7 = nullptr;
    }
    g_useEnhancedBarriers = g_enhancedBarriersSupported && g_list7;
    logf_line("UsingEnhancedBarriers=%u", g_useEnhancedBarriers ? 1u : 0u);
}

// [LEARN] Enhanced barriers split a transition into SYNC, ACCESS, and LAYOUT:
// tell the GPU when to wait, what access changes, and which texture layout to use.
static void EnhancedTextureTransition(D3D12_BARRIER_SYNC syncBefore, D3D12_BARRIER_SYNC syncAfter,
    D3D12_BARRIER_ACCESS accessBefore, D3D12_BARRIER_ACCESS accessAfter,
    D3D12_BARRIER_LAYOUT layoutBefore, D3D12_BARRIER_LAYOUT layoutAfter) {
    D3D12_TEXTURE_BARRIER barrier = {};
    barrier.SyncBefore = syncBefore;
    barrier.SyncAfter = syncAfter;
    barrier.AccessBefore = accessBefore;
    barrier.AccessAfter = accessAfter;
    barrier.LayoutBefore = layoutBefore;
    barrier.LayoutAfter = layoutAfter;
    barrier.pResource = g_rtBuffers[g_frameIndex];
    barrier.Subresources.IndexOrFirstMipLevel = 0;
    barrier.Subresources.NumMipLevels = 1;
    barrier.Subresources.FirstArraySlice = 0;
    barrier.Subresources.NumArraySlices = 1;
    barrier.Subresources.FirstPlane = 0;
    barrier.Subresources.NumPlanes = 1;
    barrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;

    D3D12_BARRIER_GROUP group = {};
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = 1;
    group.pTextureBarriers = &barrier;
    g_list7->Barrier(1, &group);
}

static void LegacyTransition(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_rtBuffers[g_frameIndex];
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_list->ResourceBarrier(1, &barrier);
}

static bool InitD3D(HWND hwnd) {
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&factory);
    if (FAILED(hr)) { logf_line("CreateDXGIFactory1 hr=0x%08X", (unsigned)hr); return false; }
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&g_dev);
    if (FAILED(hr)) { logf_line("D3D12CreateDevice hr=0x%08X", (unsigned)hr); factory->Release(); return false; }
    QueryEnhancedBarrierSupport();

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
    rp.Constants.Num32BitValues = 17;
    rp.Constants.ShaderRegister = 0;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 1; rsd.pParameters = &rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* sig = nullptr; ID3DBlob* sigErr = nullptr;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr);
    if (FAILED(hr)) {
        logf_line("D3D12SerializeRootSignature hr=0x%08X", (unsigned)hr);
        if (sigErr) sigErr->Release();
        return false;
    }
    g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
        __uuidof(ID3D12RootSignature), (void**)&g_rootSig);
    sig->Release();

    IDxcBlob* vs = CompileDXC(g_shaderSrc, L"VSMain", L"vs_6_0");
    IDxcBlob* ps = CompileDXC(g_shaderSrc, L"PSMain", L"ps_6_0");
    if (!vs || !ps) { logf_line("runtime DXC shader compile failed"); return false; }

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
    for (UINT i = 0; i < 8; i++)
        pso.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    hr = g_dev->CreateGraphicsPipelineState(&pso, __uuidof(ID3D12PipelineState), (void**)&g_pso);
    vs->Release(); ps->Release();
    if (FAILED(hr)) { logf_line("CreateGraphicsPipelineState hr=0x%08X", (unsigned)hr); return false; }

    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        __uuidof(ID3D12CommandAllocator), (void**)&g_alloc);
    hr = g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, g_pso,
        __uuidof(ID3D12GraphicsCommandList), (void**)&g_list);
    if (FAILED(hr) || !g_list) { logf_line("CreateCommandList hr=0x%08X", (unsigned)hr); return false; }
    QueryCommandList7();
    g_list->Close();

    Vertex verts[] = {
        { {-1,-1,-1}, {0.05f,0.05f,0.08f,1} }, { {-1, 1,-1}, {0.10f,0.80f,0.25f,1} },
        { { 1, 1,-1}, {0.95f,0.88f,0.20f,1} }, { { 1,-1,-1}, {0.95f,0.20f,0.18f,1} },
        { {-1,-1, 1}, {0.15f,0.35f,0.95f,1} }, { {-1, 1, 1}, {0.20f,0.90f,0.95f,1} },
        { { 1, 1, 1}, {0.95f,0.95f,0.95f,1} }, { { 1,-1, 1}, {0.95f,0.20f,0.90f,1} },
    };
    unsigned short idx[] = {
        0,1,2, 0,2,3,  4,6,5, 4,7,6,  4,5,1, 4,1,0,
        3,2,6, 3,6,7,  1,5,6, 1,6,2,  4,0,3, 4,3,7,
    };
    g_vb = CreateUploadBuffer(verts, sizeof(verts));
    g_vbv.BufferLocation = g_vb->GetGPUVirtualAddress();
    g_vbv.StrideInBytes = sizeof(Vertex);
    g_vbv.SizeInBytes = sizeof(verts);
    g_ib = CreateUploadBuffer(idx, sizeof(idx));
    g_ibv.BufferLocation = g_ib->GetGPUVirtualAddress();
    g_ibv.Format = DXGI_FORMAT_R16_UINT;
    g_ibv.SizeInBytes = sizeof(idx);

    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_fence);
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    return g_vb && g_ib && g_fence && g_fenceEvent;
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

    // [LEARN] The old barrier says PRESENT -> RENDER_TARGET as one STATE enum;
    // the enhanced path below separately names no prior GPU access, RT writes,
    // and the layout change to D3D12_BARRIER_LAYOUT_RENDER_TARGET.
    if (g_useEnhancedBarriers) {
        EnhancedTextureTransition(D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_RENDER_TARGET,
            D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_RENDER_TARGET,
            D3D12_BARRIER_LAYOUT_PRESENT, D3D12_BARRIER_LAYOUT_RENDER_TARGET);
    } else {
        LegacyTransition(D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

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

    // [LEARN] The fallback keeps the same visible cube on unsupported machines,
    // but the enhanced path is more precise about RT writes finishing before present.
    if (g_useEnhancedBarriers) {
        EnhancedTextureTransition(D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_SYNC_NONE,
            D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_ACCESS_NO_ACCESS,
            D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_LAYOUT_PRESENT);
    } else {
        LegacyTransition(D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    }

    g_list->Close();
    ID3D12CommandList* lists[] = { g_list };
    g_queue->ExecuteCommandLists(1, lists);
    g_swap->Present(1, 0);
    WaitForGpu();
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    WNDCLASS wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = "D3D12EnhancedBarriers";
    RegisterClass(&wc);
    RECT r = { 0, 0, (LONG)g_width, (LONG)g_height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindow("D3D12EnhancedBarriers", "D3D12 - Enhanced Barriers",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, hInst, nullptr);
    if (!InitD3D(hwnd)) {
        logf_line("D3D12 Enhanced Barriers init failed");
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
