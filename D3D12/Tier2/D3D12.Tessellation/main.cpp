// D3D12 Tessellation showcase.
// The hull shader (hs_5_0) sets tessellation factors and the domain shader
// (ds_5_0) subdivides each cube face triangle, bulging the new vertices toward a
// sphere by a time-varying amount. Rendered as wireframe so the added geometry is
// visible. ESC quits.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

using namespace DirectX;

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

static const UINT kFrames = 2;
struct Vertex { XMFLOAT3 pos; XMFLOAT4 color; };
struct RootData { XMFLOAT4X4 mvp; float time; float tess; };

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
static ID3D12Resource*            g_vb = nullptr;
static ID3D12Resource*            g_ib = nullptr;
static D3D12_VERTEX_BUFFER_VIEW   g_vbv = {};
static D3D12_INDEX_BUFFER_VIEW    g_ibv = {};
static ID3D12Fence*               g_fence = nullptr;
static UINT64                     g_fenceValue = 0;
static HANDLE                     g_fenceEvent = nullptr;
static UINT                       g_frameIndex = 0;

// [LEARN] Spotlight: the hull + domain tessellation shaders that subdivide the
// patch on the GPU into a smoother surface.
static const char* g_shaderSrc =
"cbuffer CB : register(b0) { float4x4 mvp; float g_time; float g_tess; };\n"
"struct VSOut { float3 opos : POSITION; float4 col : COLOR; };\n"
"VSOut VSMain(float3 pos : POSITION, float4 col : COLOR) { VSOut o; o.opos = pos; o.col = col; return o; }\n"
"struct PatchConst { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };\n"
"PatchConst HSConst(InputPatch<VSOut,3> ip) {\n"
"  PatchConst p; p.edges[0] = g_tess; p.edges[1] = g_tess; p.edges[2] = g_tess; p.inside = g_tess; return p;\n"
"}\n"
"[domain(\"tri\")]\n"
"[partitioning(\"fractional_odd\")]\n"
"[outputtopology(\"triangle_cw\")]\n"
"[outputcontrolpoints(3)]\n"
"[patchconstantfunc(\"HSConst\")]\n"
"VSOut HSMain(InputPatch<VSOut,3> ip, uint id : SV_OutputControlPointID) { return ip[id]; }\n"
"struct DSOut { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
"[domain(\"tri\")]\n"
"DSOut DSMain(PatchConst pc, float3 bary : SV_DomainLocation, const OutputPatch<VSOut,3> patch) {\n"
"  float3 p = bary.x*patch[0].opos + bary.y*patch[1].opos + bary.z*patch[2].opos;\n"
"  float4 c = bary.x*patch[0].col + bary.y*patch[1].col + bary.z*patch[2].col;\n"
"  float bulge = 0.5 + 0.5*sin(g_time*1.5);\n"
"  float3 sphere = normalize(p) * 1.7;\n"
"  p = lerp(p, sphere, bulge);\n"
"  DSOut o; o.pos = mul(float4(p,1.0), mvp); o.col = c; return o;\n"
"}\n"
"float4 PSMain(DSOut i) : SV_TARGET { return i.col; }\n";

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
    g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&res);
    void* mapped = nullptr; D3D12_RANGE none = { 0, 0 };
    res->Map(0, &none, &mapped); memcpy(mapped, data, size); res->Unmap(0, nullptr);
    return res;
}

static bool InitD3D(HWND hwnd) {
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&factory))) return false;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
        __uuidof(ID3D12Device), (void**)&g_dev))) { factory->Release(); return false; }

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
    rp.Constants.Num32BitValues = 18;
    rp.Constants.ShaderRegister = 0;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 1; rsd.pParameters = &rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* sig = nullptr; ID3DBlob* sigErr = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr))) {
        if (sigErr) sigErr->Release(); return false;
    }
    g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
        __uuidof(ID3D12RootSignature), (void**)&g_rootSig);
    sig->Release();

    ID3DBlob* vsb = nullptr; ID3DBlob* hsb = nullptr; ID3DBlob* dsb = nullptr;
    ID3DBlob* psb = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(g_shaderSrc, strlen(g_shaderSrc), nullptr, nullptr, nullptr,
        "VSMain", "vs_5_0", 0, 0, &vsb, &err))) { if (err) err->Release(); return false; }
    if (FAILED(D3DCompile(g_shaderSrc, strlen(g_shaderSrc), nullptr, nullptr, nullptr,
        "HSMain", "hs_5_0", 0, 0, &hsb, &err))) { if (err) err->Release(); return false; }
    if (FAILED(D3DCompile(g_shaderSrc, strlen(g_shaderSrc), nullptr, nullptr, nullptr,
        "DSMain", "ds_5_0", 0, 0, &dsb, &err))) { if (err) err->Release(); return false; }
    if (FAILED(D3DCompile(g_shaderSrc, strlen(g_shaderSrc), nullptr, nullptr, nullptr,
        "PSMain", "ps_5_0", 0, 0, &psb, &err))) { if (err) err->Release(); return false; }

    D3D12_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = g_rootSig;
    pso.VS = { vsb->GetBufferPointer(), vsb->GetBufferSize() };
    pso.HS = { hsb->GetBufferPointer(), hsb->GetBufferSize() };
    pso.DS = { dsb->GetBufferPointer(), dsb->GetBufferSize() };
    pso.PS = { psb->GetBufferPointer(), psb->GetBufferSize() };
    pso.InputLayout = { il, 2 };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    for (UINT i = 0; i < 8; i++)
        pso.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    g_dev->CreateGraphicsPipelineState(&pso, __uuidof(ID3D12PipelineState), (void**)&g_pso);
    vsb->Release(); hsb->Release(); dsb->Release(); psb->Release();

    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        __uuidof(ID3D12CommandAllocator), (void**)&g_alloc);
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, g_pso,
        __uuidof(ID3D12GraphicsCommandList), (void**)&g_list);
    g_list->Close();

    Vertex verts[] = {
        { {-1,-1,-1}, {0,0,0,1} }, { {-1, 1,-1}, {0,1,0,1} },
        { { 1, 1,-1}, {1,1,0,1} }, { { 1,-1,-1}, {1,0,0,1} },
        { {-1,-1, 1}, {0,0,1,1} }, { {-1, 1, 1}, {0,1,1,1} },
        { { 1, 1, 1}, {1,1,1,1} }, { { 1,-1, 1}, {1,0,1,1} },
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
    return true;
}

static void Render(float t) {
    g_alloc->Reset();
    g_list->Reset(g_alloc, g_pso);

    g_list->SetGraphicsRootSignature(g_rootSig);
    XMMATRIX world = XMMatrixRotationY(t) * XMMatrixRotationX(t * 0.5f);
    XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 0, -6, 0), XMVectorSet(0, 0, 0, 0), XMVectorSet(0, 1, 0, 0));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, (float)g_width / g_height, 0.1f, 100.0f);
    RootData rd; XMStoreFloat4x4(&rd.mvp, XMMatrixTranspose(world * view * proj)); rd.time = t; rd.tess = 10.0f;
    g_list->SetGraphicsRoot32BitConstants(0, 18, &rd, 0);

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

    g_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
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
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = "D3D12Tess";
    RegisterClass(&wc);
    RECT r = { 0, 0, (LONG)g_width, (LONG)g_height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindow("D3D12Tess", "D3D12 - Tessellation (cube to sphere)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, hInst, nullptr);
    if (!InitD3D(hwnd)) { MessageBox(hwnd, "D3D12 tessellation init failed", "Error", MB_OK); return 1; }
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
