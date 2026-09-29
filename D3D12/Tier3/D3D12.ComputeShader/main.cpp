// D3D12 Compute Shader showcase.
// A compute shader (cs_5_0) writes an animated procedural pattern into a texture
// through a UAV each frame; the rotating cube then samples that texture via an SRV
// descriptor table. Demonstrates a compute PSO feeding a graphics PSO. ESC quits.
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
static const UINT TEX = 256;
struct Vertex { XMFLOAT3 pos; XMFLOAT2 uv; };
struct RootData { XMFLOAT4X4 mvp; float time; };

static UINT g_width = 800, g_height = 600;
static ID3D12Device*              g_dev = nullptr;
static ID3D12CommandQueue*        g_queue = nullptr;
static IDXGISwapChain3*           g_swap = nullptr;
static ID3D12DescriptorHeap*      g_rtvHeap = nullptr;
static ID3D12DescriptorHeap*      g_dsvHeap = nullptr;
static ID3D12DescriptorHeap*      g_srvHeap = nullptr;  // shader-visible: UAV(0), SRV(1)
static UINT                       g_rtvSize = 0;
static UINT                       g_srvSize = 0;
static ID3D12Resource*            g_rtBuffers[kFrames] = {};
static ID3D12Resource*            g_depth = nullptr;
static ID3D12Resource*            g_tex = nullptr;
static ID3D12CommandAllocator*    g_alloc = nullptr;
static ID3D12GraphicsCommandList* g_list = nullptr;
static ID3D12RootSignature*       g_rootSig = nullptr;
static ID3D12PipelineState*       g_pso = nullptr;      // graphics
static ID3D12PipelineState*       g_csPso = nullptr;    // compute
static ID3D12Resource*            g_vb = nullptr;
static ID3D12Resource*            g_ib = nullptr;
static D3D12_VERTEX_BUFFER_VIEW   g_vbv = {};
static D3D12_INDEX_BUFFER_VIEW    g_ibv = {};
static ID3D12Fence*               g_fence = nullptr;
static UINT64                     g_fenceValue = 0;
static HANDLE                     g_fenceEvent = nullptr;
static UINT                       g_frameIndex = 0;

// [LEARN] Spotlight: the compute shader (cs_5_0) - GPU work dispatched off the
// draw pipeline whose results feed the render pass.
static const char* g_shaderSrc =
"cbuffer CB : register(b0) { float4x4 mvp; float g_time; };\n"
"// ---- compute ----\n"
"RWTexture2D<float4> Output : register(u0);\n"
"[numthreads(8,8,1)]\n"
"void CSMain(uint3 id : SV_DispatchThreadID) {\n"
"  float2 uv = id.xy / 256.0;\n"
"  float v = sin(uv.x*20.0 + g_time*2.0) * sin(uv.y*20.0 - g_time*1.7);\n"
"  float r = length(uv - 0.5);\n"
"  float3 c = 0.5 + 0.5*cos(float3(0.0,2.0,4.0) + v*3.14159 + r*12.0 - g_time*2.0);\n"
"  Output[id.xy] = float4(c, 1.0);\n"
"}\n"
"// ---- graphics ----\n"
"Texture2D Tex : register(t0);\n"
"SamplerState Samp : register(s0);\n"
"struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
"VSOut VSMain(float3 pos : POSITION, float2 uv : TEXCOORD0) {\n"
"  VSOut o; o.pos = mul(float4(pos,1.0f), mvp); o.uv = uv; return o; }\n"
"float4 PSMain(VSOut i) : SV_TARGET { return Tex.Sample(Samp, i.uv); }\n";

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

static void TexBarrier(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_tex;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_list->ResourceBarrier(1, &b);
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

    D3D12_DESCRIPTOR_HEAP_DESC sh = {};
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; sh.NumDescriptors = 2;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_dev->CreateDescriptorHeap(&sh, __uuidof(ID3D12DescriptorHeap), (void**)&g_srvHeap);
    g_srvSize = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

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

    // Compute-writable, sampleable texture. Created in PIXEL_SHADER_RESOURCE state.
    D3D12_RESOURCE_DESC trd = {};
    trd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    trd.Width = TEX; trd.Height = TEX; trd.DepthOrArraySize = 1; trd.MipLevels = 1;
    trd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; trd.SampleDesc.Count = 1;
    trd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    g_dev->CreateCommittedResource(&dhp, D3D12_HEAP_FLAG_NONE, &trd,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, __uuidof(ID3D12Resource), (void**)&g_tex);

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    // index 0 = UAV
    g_dev->CreateUnorderedAccessView(g_tex, nullptr, nullptr, srvCpu);
    // index 1 = SRV
    srvCpu.ptr += g_srvSize;
    g_dev->CreateShaderResourceView(g_tex, nullptr, srvCpu);

    // Root signature: b0 constants(17) | table SRV(t0) | table UAV(u0) | static sampler s0
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0; srvRange.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0; uavRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER rp[3] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 17; rp[0].Constants.ShaderRegister = 0;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &srvRange;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[2].DescriptorTable.NumDescriptorRanges = 1; rp[2].DescriptorTable.pDescriptorRanges = &uavRange;
    rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.ShaderRegister = 0; samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 3; rsd.pParameters = rp;
    rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &samp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* sig = nullptr; ID3DBlob* sigErr = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr))) {
        if (sigErr) sigErr->Release(); return false;
    }
    g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
        __uuidof(ID3D12RootSignature), (void**)&g_rootSig);
    sig->Release();

    ID3DBlob* vsb = nullptr; ID3DBlob* psb = nullptr; ID3DBlob* csb = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(g_shaderSrc, strlen(g_shaderSrc), nullptr, nullptr, nullptr,
        "VSMain", "vs_5_0", 0, 0, &vsb, &err))) { if (err) err->Release(); return false; }
    if (FAILED(D3DCompile(g_shaderSrc, strlen(g_shaderSrc), nullptr, nullptr, nullptr,
        "PSMain", "ps_5_0", 0, 0, &psb, &err))) { if (err) err->Release(); return false; }
    if (FAILED(D3DCompile(g_shaderSrc, strlen(g_shaderSrc), nullptr, nullptr, nullptr,
        "CSMain", "cs_5_0", 0, 0, &csb, &err))) { if (err) err->Release(); return false; }

    D3D12_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = g_rootSig;
    pso.VS = { vsb->GetBufferPointer(), vsb->GetBufferSize() };
    pso.PS = { psb->GetBufferPointer(), psb->GetBufferSize() };
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
    g_dev->CreateGraphicsPipelineState(&pso, __uuidof(ID3D12PipelineState), (void**)&g_pso);

    D3D12_COMPUTE_PIPELINE_STATE_DESC cpso = {};
    cpso.pRootSignature = g_rootSig;
    cpso.CS = { csb->GetBufferPointer(), csb->GetBufferSize() };
    g_dev->CreateComputePipelineState(&cpso, __uuidof(ID3D12PipelineState), (void**)&g_csPso);
    vsb->Release(); psb->Release(); csb->Release();

    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        __uuidof(ID3D12CommandAllocator), (void**)&g_alloc);
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, g_pso,
        __uuidof(ID3D12GraphicsCommandList), (void**)&g_list);
    g_list->Close();

    Vertex verts[] = {
        { {-1,-1,-1}, {0,1} }, { {-1, 1,-1}, {0,0} }, { { 1, 1,-1}, {1,0} }, { { 1,-1,-1}, {1,1} },
        { {-1,-1, 1}, {1,1} }, { {-1, 1, 1}, {1,0} }, { { 1, 1, 1}, {0,0} }, { { 1,-1, 1}, {0,1} },
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

    RootData rd; XMMATRIX world = XMMatrixRotationY(t) * XMMatrixRotationX(t * 0.5f);
    XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 0, -6, 0), XMVectorSet(0, 0, 0, 0), XMVectorSet(0, 1, 0, 0));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, (float)g_width / g_height, 0.1f, 100.0f);
    XMStoreFloat4x4(&rd.mvp, XMMatrixTranspose(world * view * proj)); rd.time = t;

    ID3D12DescriptorHeap* heaps[] = { g_srvHeap };
    g_list->SetDescriptorHeaps(1, heaps);
    D3D12_GPU_DESCRIPTOR_HANDLE uavGpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = uavGpu; srvGpu.ptr += g_srvSize;

    // Compute pass: write the texture through the UAV.
    TexBarrier(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_list->SetPipelineState(g_csPso);
    g_list->SetComputeRootSignature(g_rootSig);
    g_list->SetComputeRoot32BitConstants(0, 17, &rd, 0);
    g_list->SetComputeRootDescriptorTable(2, uavGpu);
    g_list->Dispatch(TEX / 8, TEX / 8, 1);
    TexBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Graphics pass: draw the cube sampling the computed texture.
    g_list->SetPipelineState(g_pso);
    g_list->SetGraphicsRootSignature(g_rootSig);
    g_list->SetGraphicsRoot32BitConstants(0, 17, &rd, 0);
    g_list->SetGraphicsRootDescriptorTable(1, srvGpu);

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
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = "D3D12CS";
    RegisterClass(&wc);
    RECT r = { 0, 0, (LONG)g_width, (LONG)g_height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindow("D3D12CS", "D3D12 - Compute Shader (animated texture)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, hInst, nullptr);
    if (!InitD3D(hwnd)) { MessageBox(hwnd, "D3D12 CS init failed", "Error", MB_OK); return 1; }
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
