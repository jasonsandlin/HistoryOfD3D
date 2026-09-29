// D3D12 Dynamic Resources showcase.
// Shader Model 6.6 lets HLSL index directly into ResourceDescriptorHeap[] and
// SamplerDescriptorHeap[] using integer descriptor indices passed at draw time.
// The root signature has no SRV/CBV/UAV/sampler descriptor tables, only root
// constants plus the HEAP_DIRECTLY_INDEXED flags. Unsupported systems log and
// render the same rotating cube with a plain color shader. ESC quits.
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3d12shader.h>
#include <dxgi1_6.h>
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
static const UINT kTexSize = 64;

struct Vertex { XMFLOAT3 pos; XMFLOAT4 color; XMFLOAT2 uv; };
struct RootData { XMFLOAT4X4 mvp; float time; UINT textureIndex; UINT cbvIndex; UINT samplerIndex; };
struct MaterialData { XMFLOAT4 tint; };

template<typename T, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type>
struct alignas(void*) Sub {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE _t = Type;
    T _v{};
    Sub() = default;
    Sub(const T& v) : _v(v) {}
    Sub& operator=(const T& v) { _v = v; return *this; }
};

static UINT g_width = 800, g_height = 600;
static ID3D12Device*              g_dev = nullptr;
static ID3D12CommandQueue*        g_queue = nullptr;
static IDXGISwapChain3*           g_swap = nullptr;
static ID3D12DescriptorHeap*      g_rtvHeap = nullptr;
static ID3D12DescriptorHeap*      g_dsvHeap = nullptr;
static ID3D12DescriptorHeap*      g_srvHeap = nullptr;
static ID3D12DescriptorHeap*      g_samplerHeap = nullptr;
static UINT                       g_rtvSize = 0;
static ID3D12Resource*            g_rtBuffers[kFrames] = {};
static ID3D12Resource*            g_depth = nullptr;
static ID3D12CommandAllocator*    g_alloc = nullptr;
static ID3D12GraphicsCommandList* g_list = nullptr;
static ID3D12RootSignature*       g_rootSig = nullptr;
static ID3D12PipelineState*       g_pso = nullptr;
static ID3D12Resource*            g_vb = nullptr;
static ID3D12Resource*            g_ib = nullptr;
static ID3D12Resource*            g_texture = nullptr;
static ID3D12Resource*            g_material = nullptr;
static D3D12_VERTEX_BUFFER_VIEW   g_vbv = {};
static D3D12_INDEX_BUFFER_VIEW    g_ibv = {};
static ID3D12Fence*               g_fence = nullptr;
static UINT64                     g_fenceValue = 0;
static HANDLE                     g_fenceEvent = nullptr;
static UINT                       g_frameIndex = 0;
static D3D12_RESOURCE_BINDING_TIER g_bindingTier = D3D12_RESOURCE_BINDING_TIER_1;
static D3D_SHADER_MODEL           g_highestShaderModel = D3D_SHADER_MODEL_5_1;
static bool                       g_useDynamicResources = false;

static void logf_line(const char* fmt, ...) {
    FILE* f = fopen("CubeD3D12.DynamicResources.log", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

// [LEARN] ResourceDescriptorHeap[] is the SM6.6 bindless path: no descriptor table root parameter is bound for these resources.
static const char* g_shaderDynamic =
"cbuffer Draw : register(b0) { float4x4 mvp; float g_time; uint g_textureIndex; uint g_cbvIndex; uint g_samplerIndex; };\n"
"struct Material { float4 tint; };\n"
"struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; float2 uv : TEXCOORD0; };\n"
"VSOut VSMain(float3 pos : POSITION, float4 col : COLOR, float2 uv : TEXCOORD0) {\n"
"  VSOut o; o.pos = mul(float4(pos, 1.0), mvp); o.col = col; o.uv = uv; return o;\n"
"}\n"
"float4 PSMain(VSOut i) : SV_TARGET {\n"
"  Texture2D<float4> tex = ResourceDescriptorHeap[g_textureIndex];\n"
"  ConstantBuffer<Material> mat = ResourceDescriptorHeap[g_cbvIndex];\n"
"  SamplerState samp = SamplerDescriptorHeap[g_samplerIndex];\n"
"  // [LEARN] The shader chooses descriptors by integer index, eliminating per-draw descriptor-table rebinding.\n"
"  float2 uv = frac(i.uv * 2.0 + float2(g_time * 0.05, 0.0));\n"
"  return tex.Sample(samp, uv) * mat.tint * lerp(0.60, 1.0, i.col);\n"
"}\n";

static const char* g_shaderFallback =
"cbuffer Draw : register(b0) { float4x4 mvp; float g_time; uint g_textureIndex; uint g_cbvIndex; uint g_samplerIndex; };\n"
"struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; float2 uv : TEXCOORD0; };\n"
"VSOut VSMain(float3 pos : POSITION, float4 col : COLOR, float2 uv : TEXCOORD0) {\n"
"  VSOut o; o.pos = mul(float4(pos, 1.0), mvp); o.col = col; o.uv = uv; return o;\n"
"}\n"
"float4 PSMain(VSOut i) : SV_TARGET {\n"
"  float pulse = 0.80 + 0.20 * sin(g_time * 2.0);\n"
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
    return nullptr;
}

static IDxcBlob* CompileDXC(const char* src, const wchar_t* entry, const wchar_t* target) {
    static HMODULE dll = LoadDxc();
    if (!dll) { logf_line("dxcompiler.dll not found"); return nullptr; }
    auto create = (DxcCreateInstanceProc)GetProcAddress(dll, "DxcCreateInstance");
    if (!create) return nullptr;
    IDxcCompiler3* comp = nullptr;
    if (FAILED(create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), (void**)&comp))) return nullptr;
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

static void WaitForGpu() {
    const UINT64 v = ++g_fenceValue;
    g_queue->Signal(g_fence, v);
    if (g_fence->GetCompletedValue() < v) {
        g_fence->SetEventOnCompletion(v, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
    g_frameIndex = g_swap->GetCurrentBackBufferIndex();
}

static void ExecuteAndReset() {
    g_list->Close();
    ID3D12CommandList* lists[] = { g_list };
    g_queue->ExecuteCommandLists(1, lists);
    WaitForGpu();
    g_alloc->Reset();
    g_list->Reset(g_alloc, nullptr);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN: if (w == VK_ESCAPE) PostQuitMessage(0); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, w, l);
}

static ID3D12Resource* CreateUploadBuffer(UINT64 size, const void* data) {
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* res = nullptr;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&res))) return nullptr;
    if (data) {
        void* mapped = nullptr; D3D12_RANGE none = { 0, 0 };
        res->Map(0, &none, &mapped); memcpy(mapped, data, (size_t)size); res->Unmap(0, nullptr);
    }
    return res;
}

static bool CreateTextureDescriptors() {
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kTexSize; td.Height = kTexSize; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&g_texture))) return false;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    UINT rows = 0; UINT64 rowBytes = 0, totalBytes = 0;
    g_dev->GetCopyableFootprints(&td, 0, 1, 0, &layout, &rows, &rowBytes, &totalBytes);
    ID3D12Resource* upload = CreateUploadBuffer(totalBytes, nullptr);
    if (!upload) return false;
    BYTE* base = nullptr; D3D12_RANGE none = { 0, 0 };
    upload->Map(0, &none, (void**)&base);
    for (UINT y = 0; y < kTexSize; ++y) {
        UINT* row = (UINT*)(base + layout.Offset + y * layout.Footprint.RowPitch);
        for (UINT x = 0; x < kTexSize; ++x) {
            UINT checker = ((x >> 3) ^ (y >> 3)) & 1;
            BYTE r = checker ? 240 : 60;
            BYTE g = checker ? 200 : 120;
            BYTE b = checker ? 80 : 230;
            row[x] = 0xff000000u | ((UINT)b << 16) | ((UINT)g << 8) | r;
        }
    }
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = g_texture; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = layout;
    g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_texture;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_list->ResourceBarrier(1, &b);
    ExecuteAndReset();
    upload->Release();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = td.Format; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(g_texture, &srv, g_srvHeap->GetCPUDescriptorHandleForHeapStart());

    MaterialData mat = { XMFLOAT4(0.90f, 1.00f, 0.82f, 1.0f) };
    g_material = CreateUploadBuffer(256, nullptr);
    if (!g_material) return false;
    void* mapped = nullptr;
    g_material->Map(0, &none, &mapped); memcpy(mapped, &mat, sizeof(mat)); g_material->Unmap(0, nullptr);
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {};
    cbv.BufferLocation = g_material->GetGPUVirtualAddress();
    cbv.SizeInBytes = 256;
    D3D12_CPU_DESCRIPTOR_HANDLE cbvHandle = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    cbvHandle.ptr += g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_dev->CreateConstantBufferView(&cbv, cbvHandle);

    D3D12_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    g_dev->CreateSampler(&samp, g_samplerHeap->GetCPUDescriptorHandleForHeapStart());
    return true;
}

static void QuerySupport() {
    D3D12_FEATURE_DATA_D3D12_OPTIONS opts = {};
    if (SUCCEEDED(g_dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts))))
        g_bindingTier = opts.ResourceBindingTier;

    D3D12_FEATURE_DATA_SHADER_MODEL sm = {};
    sm.HighestShaderModel = D3D_SHADER_MODEL_6_6;
    if (SUCCEEDED(g_dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
        g_highestShaderModel = sm.HighestShaderModel;

    g_useDynamicResources = g_highestShaderModel >= D3D_SHADER_MODEL_6_6;
    // [LEARN] Dynamic resources are shader-model gated; ResourceBindingTier is still logged because descriptor count limits matter in real engines.
    logf_line("ResourceBindingTier=%u", (unsigned)g_bindingTier);
    logf_line("HighestShaderModel=0x%04X", (unsigned)g_highestShaderModel);
    if (!g_useDynamicResources) logf_line("SM6.6 unsupported: rendering plain colored fallback");
}

static bool CreateRootSignature(bool dynamicResources) {
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.Num32BitValues = sizeof(RootData) / 4;
    rp.Constants.ShaderRegister = 0;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 1; rsd.pParameters = &rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (dynamicResources) {
        // [LEARN] These flags make ResourceDescriptorHeap[] and SamplerDescriptorHeap[] legal without descriptor-table root bindings.
        rsd.Flags = (D3D12_ROOT_SIGNATURE_FLAGS)(rsd.Flags |
            D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
            D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);
    }
    ID3DBlob* sig = nullptr; ID3DBlob* sigErr = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr);
    if (FAILED(hr)) { logf_line("rootsig dynamic=%u hr=0x%08X", dynamicResources ? 1u : 0u, (unsigned)hr); if (sigErr) sigErr->Release(); return false; }
    hr = g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&g_rootSig);
    sig->Release();
    if (FAILED(hr)) logf_line("CreateRootSignature dynamic=%u hr=0x%08X", dynamicResources ? 1u : 0u, (unsigned)hr);
    return SUCCEEDED(hr);
}

static bool CreatePipeline() {
    if (!CreateRootSignature(g_useDynamicResources)) {
        if (g_useDynamicResources) {
            logf_line("directly-indexed root signature unavailable; falling back");
            g_useDynamicResources = false;
            if (!CreateRootSignature(false)) return false;
        } else return false;
    }

    const char* shader = g_useDynamicResources ? g_shaderDynamic : g_shaderFallback;
    const wchar_t* vsTarget = g_useDynamicResources ? L"vs_6_6" : L"vs_6_0";
    const wchar_t* psTarget = g_useDynamicResources ? L"ps_6_6" : L"ps_6_0";
    IDxcBlob* vs = CompileDXC(shader, L"VSMain", vsTarget);
    IDxcBlob* ps = CompileDXC(shader, L"PSMain", psTarget);
    if ((!vs || !ps) && g_useDynamicResources) {
        if (vs) vs->Release(); if (ps) ps->Release();
        if (g_rootSig) { g_rootSig->Release(); g_rootSig = nullptr; }
        logf_line("SM6.6 dynamic shader compile failed; compiling fallback shader");
        g_useDynamicResources = false;
        if (!CreateRootSignature(false)) return false;
        vs = CompileDXC(g_shaderFallback, L"VSMain", L"vs_6_0");
        ps = CompileDXC(g_shaderFallback, L"PSMain", L"ps_6_0");
    }
    if (!vs || !ps) return false;

    D3D12_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = g_rootSig;
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout = { il, 3 };
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
    HRESULT hr = g_dev->CreateGraphicsPipelineState(&pso, __uuidof(ID3D12PipelineState), (void**)&g_pso);
    vs->Release(); ps->Release();
    if (FAILED(hr)) logf_line("CreateGraphicsPipelineState hr=0x%08X", (unsigned)hr);
    return SUCCEEDED(hr);
}

static bool InitD3D(HWND hwnd) {
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&factory))) return false;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&g_dev))) { factory->Release(); return false; }
    QuerySupport();

    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&g_queue))) return false;

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.BufferCount = kFrames; sd.Width = g_width; sd.Height = g_height;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc.Count = 1;
    IDXGISwapChain1* sc1 = nullptr;
    if (FAILED(factory->CreateSwapChainForHwnd(g_queue, hwnd, &sd, nullptr, nullptr, &sc1))) { factory->Release(); return false; }
    sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&g_swap);
    sc1->Release(); factory->Release();
    g_frameIndex = g_swap->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rh = {}; rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rh.NumDescriptors = kFrames;
    g_dev->CreateDescriptorHeap(&rh, __uuidof(ID3D12DescriptorHeap), (void**)&g_rtvHeap);
    g_rtvSize = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_DESCRIPTOR_HEAP_DESC dh = {}; dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; dh.NumDescriptors = 1;
    g_dev->CreateDescriptorHeap(&dh, __uuidof(ID3D12DescriptorHeap), (void**)&g_dsvHeap);
    D3D12_DESCRIPTOR_HEAP_DESC sh = {}; sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; sh.NumDescriptors = 2; sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_dev->CreateDescriptorHeap(&sh, __uuidof(ID3D12DescriptorHeap), (void**)&g_srvHeap);
    D3D12_DESCRIPTOR_HEAP_DESC sampHeap = {}; sampHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER; sampHeap.NumDescriptors = 1; sampHeap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_dev->CreateDescriptorHeap(&sampHeap, __uuidof(ID3D12DescriptorHeap), (void**)&g_samplerHeap);

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
    drd.Format = DXGI_FORMAT_D32_FLOAT; drd.SampleDesc.Count = 1; drd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 1.0f;
    g_dev->CreateCommittedResource(&dhp, D3D12_HEAP_FLAG_NONE, &drd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, __uuidof(ID3D12Resource), (void**)&g_depth);
    g_dev->CreateDepthStencilView(g_depth, nullptr, g_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&g_alloc);
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&g_list);
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_fence);
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    if (!CreateTextureDescriptors()) return false;
    if (!CreatePipeline()) return false;

    Vertex verts[] = {
        { {-1,-1,-1}, {0.70f,0.70f,0.70f,1}, {0,1} }, { {-1, 1,-1}, {0.80f,1.00f,0.80f,1}, {0,0} },
        { { 1, 1,-1}, {1.00f,1.00f,0.75f,1}, {1,0} }, { { 1,-1,-1}, {1.00f,0.80f,0.80f,1}, {1,1} },
        { {-1,-1, 1}, {0.70f,0.85f,1.00f,1}, {1,1} }, { {-1, 1, 1}, {0.75f,0.75f,1.00f,1}, {1,0} },
        { { 1, 1, 1}, {1.00f,0.80f,1.00f,1}, {0,0} }, { { 1,-1, 1}, {1.00f,1.00f,1.00f,1}, {0,1} },
    };
    unsigned short idx[] = { 0,1,2, 0,2,3,  4,6,5, 4,7,6,  4,5,1, 4,1,0,  3,2,6, 3,6,7,  1,5,6, 1,6,2,  4,0,3, 4,3,7 };
    g_vb = CreateUploadBuffer(sizeof(verts), verts);
    g_vbv.BufferLocation = g_vb->GetGPUVirtualAddress(); g_vbv.StrideInBytes = sizeof(Vertex); g_vbv.SizeInBytes = sizeof(verts);
    g_ib = CreateUploadBuffer(sizeof(idx), idx);
    g_ibv.BufferLocation = g_ib->GetGPUVirtualAddress(); g_ibv.Format = DXGI_FORMAT_R16_UINT; g_ibv.SizeInBytes = sizeof(idx);
    g_list->Close();
    return true;
}

static void Render(float t) {
    g_alloc->Reset();
    g_list->Reset(g_alloc, g_pso);

    g_list->SetGraphicsRootSignature(g_rootSig);
    ID3D12DescriptorHeap* heaps[] = { g_srvHeap, g_samplerHeap };
    g_list->SetDescriptorHeaps(2, heaps);

    XMMATRIX world = XMMatrixRotationY(t) * XMMatrixRotationX(t * 0.5f);
    XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 0, -6, 0), XMVectorSet(0, 0, 0, 0), XMVectorSet(0, 1, 0, 0));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, (float)g_width / g_height, 0.1f, 100.0f);
    RootData rd = {}; XMStoreFloat4x4(&rd.mvp, XMMatrixTranspose(world * view * proj));
    rd.time = t;
    rd.textureIndex = 0;
    rd.cbvIndex = 1;
    rd.samplerIndex = 0;
    // [LEARN] These root constants are just integers; the shader uses them to fetch descriptors directly from the heaps.
    g_list->SetGraphicsRoot32BitConstants(0, sizeof(RootData) / 4, &rd, 0);

    D3D12_VIEWPORT vp = { 0, 0, (float)g_width, (float)g_height, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)g_width, (LONG)g_height };
    g_list->RSSetViewports(1, &vp); g_list->RSSetScissorRects(1, &sc);

    D3D12_RESOURCE_BARRIER toRT = {};
    toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRT.Transition.pResource = g_rtBuffers[g_frameIndex];
    toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_list->ResourceBarrier(1, &toRT);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart(); rtv.ptr += g_frameIndex * g_rtvSize;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    g_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    float clear[4] = { 0.08f, 0.07f, 0.11f, 1.0f };
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
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = "D3D12DynamicResources";
    RegisterClass(&wc);
    RECT r = { 0, 0, (LONG)g_width, (LONG)g_height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindow("D3D12DynamicResources", "D3D12 - Dynamic Resources", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, hInst, nullptr);
    if (!InitD3D(hwnd)) { logf_line("D3D12 Dynamic Resources init failed"); return 1; }
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
