// D3D12 Ray Tracing (DXR) showcase.
// A single ray-tracing pipeline with three shader types working together:
//   * ray generation - shoots a camera ray per pixel into the scene,
//   * miss           - shades the background gradient,
//   * closest hit    - shades the cube using barycentrics + a diffuse term.
// The cube is a bottom-level acceleration structure (BLAS); a top-level AS (TLAS)
// holds one instance whose transform is rotated every frame, so the cube spins.
// The ray-tracing shaders live in one DXIL library compiled at runtime with DXC.
// Requires a DXR-capable GPU/WARP and dxcompiler.dll/dxil.dll. ESC quits.
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

static UINT g_width = 800, g_height = 600;
static ID3D12Device5*             g_dev = nullptr;
static ID3D12CommandQueue*        g_queue = nullptr;
static IDXGISwapChain3*           g_swap = nullptr;
static ID3D12Resource*            g_rtBuffers[kFrames] = {};
static ID3D12CommandAllocator*    g_alloc = nullptr;
static ID3D12GraphicsCommandList4* g_list = nullptr;
static ID3D12RootSignature*       g_globalRS = nullptr;
static ID3D12StateObject*         g_rtpso = nullptr;
static ID3D12Fence*               g_fence = nullptr;
static UINT64                     g_fenceValue = 0;
static HANDLE                     g_fenceEvent = nullptr;
static UINT                       g_frameIndex = 0;

static ID3D12Resource*            g_positions = nullptr;
static ID3D12Resource*            g_indices = nullptr;
static ID3D12Resource*            g_colors = nullptr;
static ID3D12Resource*            g_blas = nullptr;
static ID3D12Resource*            g_tlas = nullptr;
static ID3D12Resource*            g_scratch = nullptr;
static ID3D12Resource*            g_instanceBuf = nullptr;
static void*                      g_instanceMapped = nullptr;
static ID3D12Resource*            g_shaderTable = nullptr;
static UINT64                     g_stBase = 0;
static ID3D12Resource*            g_output = nullptr;
static ID3D12DescriptorHeap*      g_uavHeap = nullptr;
static ID3D12Resource*            g_cb = nullptr;
static void*                      g_cbMapped = nullptr;

static const UINT STRIDE = 64; // shader-table record stride (>= identifier size, aligned)

struct CBData { XMFLOAT4X4 invViewProj; XMFLOAT3 camPos; float time; };

// [LEARN] Spotlight: the DXR shader library (lib_6_3) holding the raygen, miss,
// and closest-hit shaders that drive hardware ray tracing.
static const char* g_src =
"RaytracingAccelerationStructure Scene : register(t0);\n"
"StructuredBuffer<float3> Positions : register(t1);\n"
"StructuredBuffer<uint>   Indices   : register(t2);\n"
"StructuredBuffer<float4> Colors    : register(t3);\n"
"RWTexture2D<float4> Output : register(u0);\n"
"cbuffer CB : register(b0) { float4x4 invViewProj; float3 camPos; float g_time; };\n"
"struct Payload { float3 color; };\n"
"[shader(\"raygeneration\")]\n"
"void RayGen() {\n"
"  uint2 px = DispatchRaysIndex().xy;\n"
"  float2 d = (float2(px) + 0.5) / float2(DispatchRaysDimensions().xy) * 2.0 - 1.0;\n"
"  d.y = -d.y;\n"
"  float4 world = mul(float4(d, 0, 1), invViewProj); world /= world.w;\n"
"  RayDesc ray; ray.Origin = camPos; ray.Direction = normalize(world.xyz - camPos);\n"
"  ray.TMin = 0.001; ray.TMax = 1000.0;\n"
"  Payload p; p.color = float3(0,0,0);\n"
"  TraceRay(Scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, p);\n"
"  Output[px] = float4(p.color, 1.0);\n"
"}\n"
"[shader(\"miss\")]\n"
"void Miss(inout Payload p) {\n"
"  float t = (float)DispatchRaysIndex().y / (float)DispatchRaysDimensions().y;\n"
"  p.color = lerp(float3(0.10,0.10,0.20), float3(0.02,0.02,0.05), t);\n"
"}\n"
"[shader(\"closesthit\")]\n"
"void ClosestHit(inout Payload p, BuiltInTriangleIntersectionAttributes attr) {\n"
"  float3 bary = float3(1.0 - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);\n"
"  uint prim = PrimitiveIndex();\n"
"  uint i0 = Indices[prim*3+0], i1 = Indices[prim*3+1], i2 = Indices[prim*3+2];\n"
"  float3 col = bary.x*Colors[i0].rgb + bary.y*Colors[i1].rgb + bary.z*Colors[i2].rgb;\n"
"  float3 wp0 = mul(ObjectToWorld3x4(), float4(Positions[i0], 1.0));\n"
"  float3 wp1 = mul(ObjectToWorld3x4(), float4(Positions[i1], 1.0));\n"
"  float3 wp2 = mul(ObjectToWorld3x4(), float4(Positions[i2], 1.0));\n"
"  float3 n = normalize(cross(wp1 - wp0, wp2 - wp0));\n"
"  float3 L = normalize(float3(0.5, 0.8, -0.6));\n"
"  float diff = saturate(dot(n, L)) * 0.7 + 0.3;\n"
"  p.color = col * diff;\n"
"}\n";

// ---- runtime DXC ----
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

static IDxcBlob* CompileLib(const char* src) {
    static HMODULE dll = LoadDxc();
    if (!dll) return nullptr;
    auto create = (DxcCreateInstanceProc)GetProcAddress(dll, "DxcCreateInstance");
    if (!create) return nullptr;
    IDxcCompiler3* comp = nullptr;
    if (FAILED(create(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), (void**)&comp))) return nullptr;
    DxcBuffer buf = { src, strlen(src), DXC_CP_UTF8 };
    const wchar_t* args[] = { L"-T", L"lib_6_3", L"-O3" };
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

static ID3D12Resource* CreateUpload(const void* data, UINT64 size) {
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* res = nullptr;
    g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), (void**)&res);
    if (data) {
        void* m = nullptr; D3D12_RANGE none = { 0, 0 };
        res->Map(0, &none, &m); memcpy(m, data, size); res->Unmap(0, nullptr);
    }
    return res;
}

static ID3D12Resource* CreateUAVBuffer(UINT64 size, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* res = nullptr;
    g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        state, nullptr, __uuidof(ID3D12Resource), (void**)&res);
    return res;
}

static void UAVBarrier(ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = r;
    g_list->ResourceBarrier(1, &b);
}

static bool InitD3D(HWND hwnd) {
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&factory))) return false;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
        __uuidof(ID3D12Device5), (void**)&g_dev))) { factory->Release(); return false; }

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5 = {};
    if (FAILED(g_dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5))) ||
        opt5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED) {
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
    for (UINT i = 0; i < kFrames; i++)
        g_swap->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&g_rtBuffers[i]);

    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        __uuidof(ID3D12CommandAllocator), (void**)&g_alloc);
    ID3D12GraphicsCommandList* list0 = nullptr;
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr,
        __uuidof(ID3D12GraphicsCommandList), (void**)&list0);
    list0->QueryInterface(__uuidof(ID3D12GraphicsCommandList4), (void**)&g_list);
    list0->Release();

    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_fence);
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // ---- cube geometry ----
    XMFLOAT3 positions[8] = {
        {-1,-1,-1}, {-1,1,-1}, {1,1,-1}, {1,-1,-1},
        {-1,-1, 1}, {-1,1, 1}, {1,1, 1}, {1,-1, 1},
    };
    XMFLOAT4 colors[8] = {
        {0,0,0,1}, {0,1,0,1}, {1,1,0,1}, {1,0,0,1},
        {0,0,1,1}, {0,1,1,1}, {1,1,1,1}, {1,0,1,1},
    };
    UINT32 indices[36] = {
        0,1,2, 0,2,3, 4,6,5, 4,7,6, 4,5,1, 4,1,0,
        3,2,6, 3,6,7, 1,5,6, 1,6,2, 4,0,3, 4,3,7,
    };
    g_positions = CreateUpload(positions, sizeof(positions));
    g_colors = CreateUpload(colors, sizeof(colors));
    g_indices = CreateUpload(indices, sizeof(indices));

    // ---- BLAS ----
    D3D12_RAYTRACING_GEOMETRY_DESC geom = {};
    geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.VertexBuffer.StartAddress = g_positions->GetGPUVirtualAddress();
    geom.Triangles.VertexBuffer.StrideInBytes = sizeof(XMFLOAT3);
    geom.Triangles.VertexCount = 8;
    geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.IndexBuffer = g_indices->GetGPUVirtualAddress();
    geom.Triangles.IndexCount = 36;
    geom.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blIn = {};
    blIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    blIn.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    blIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    blIn.NumDescs = 1; blIn.pGeometryDescs = &geom;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blPre = {};
    g_dev->GetRaytracingAccelerationStructurePrebuildInfo(&blIn, &blPre);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlIn = {};
    tlIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlIn.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tlIn.NumDescs = 1;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlPre = {};
    g_dev->GetRaytracingAccelerationStructurePrebuildInfo(&tlIn, &tlPre);

    UINT64 scratchSize = max(blPre.ScratchDataSizeInBytes, tlPre.ScratchDataSizeInBytes);
    g_scratch = CreateUAVBuffer(scratchSize, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_blas = CreateUAVBuffer(blPre.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    g_tlas = CreateUAVBuffer(tlPre.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    g_instanceBuf = CreateUpload(nullptr, sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    g_instanceBuf->Map(0, nullptr, &g_instanceMapped);

    // Build BLAS once.
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blBuild = {};
    blBuild.Inputs = blIn;
    blBuild.ScratchAccelerationStructureData = g_scratch->GetGPUVirtualAddress();
    blBuild.DestAccelerationStructureData = g_blas->GetGPUVirtualAddress();
    g_list->BuildRaytracingAccelerationStructure(&blBuild, 0, nullptr);
    UAVBarrier(g_blas);
    g_list->Close();
    ID3D12CommandList* initLists[] = { g_list };
    g_queue->ExecuteCommandLists(1, initLists);
    WaitForGpu();

    // ---- global root signature ----
    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0; uavRange.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER rp[6] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1; rp[0].DescriptorTable.pDescriptorRanges = &uavRange;
    for (int i = 1; i <= 4; i++) {
        rp[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rp[i].Descriptor.ShaderRegister = i - 1; // t0..t3
    }
    rp[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[5].Descriptor.ShaderRegister = 0; // b0
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 6; rsd.pParameters = rp;
    ID3DBlob* sig = nullptr; ID3DBlob* sigErr = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr))) {
        if (sigErr) sigErr->Release(); return false;
    }
    g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
        __uuidof(ID3D12RootSignature), (void**)&g_globalRS);
    sig->Release();

    // ---- ray-tracing pipeline (state object) ----
    IDxcBlob* lib = CompileLib(g_src);
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
    sub[1].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP; sub[1].pDesc = &hg;

    D3D12_RAYTRACING_SHADER_CONFIG shaderCfg = {};
    shaderCfg.MaxPayloadSizeInBytes = sizeof(float) * 3;
    shaderCfg.MaxAttributeSizeInBytes = sizeof(float) * 2;
    sub[2].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG; sub[2].pDesc = &shaderCfg;

    D3D12_GLOBAL_ROOT_SIGNATURE grs = {}; grs.pGlobalRootSignature = g_globalRS;
    sub[3].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE; sub[3].pDesc = &grs;

    D3D12_RAYTRACING_PIPELINE_CONFIG pipeCfg = {}; pipeCfg.MaxTraceRecursionDepth = 1;
    sub[4].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG; sub[4].pDesc = &pipeCfg;

    D3D12_STATE_OBJECT_DESC soDesc = {};
    soDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    soDesc.NumSubobjects = _countof(sub); soDesc.pSubobjects = sub;
    if (FAILED(g_dev->CreateStateObject(&soDesc, __uuidof(ID3D12StateObject), (void**)&g_rtpso)))
        return false;

    // ---- shader table (RayGen @0, Miss @STRIDE, HitGroup @2*STRIDE) ----
    ID3D12StateObjectProperties* props = nullptr;
    g_rtpso->QueryInterface(__uuidof(ID3D12StateObjectProperties), (void**)&props);
    const UINT idSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    BYTE table[STRIDE * 3] = {};
    memcpy(table + 0 * STRIDE, props->GetShaderIdentifier(L"RayGen"), idSize);
    memcpy(table + 1 * STRIDE, props->GetShaderIdentifier(L"Miss"), idSize);
    memcpy(table + 2 * STRIDE, props->GetShaderIdentifier(L"HitGroup"), idSize);
    props->Release();
    g_shaderTable = CreateUpload(table, sizeof(table));
    g_stBase = g_shaderTable->GetGPUVirtualAddress();

    // ---- output UAV texture + descriptor heap ----
    D3D12_HEAP_PROPERTIES dhp = {}; dhp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC od = {};
    od.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    od.Width = g_width; od.Height = g_height; od.DepthOrArraySize = 1; od.MipLevels = 1;
    od.Format = DXGI_FORMAT_R8G8B8A8_UNORM; od.SampleDesc.Count = 1;
    od.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    g_dev->CreateCommittedResource(&dhp, D3D12_HEAP_FLAG_NONE, &od,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource), (void**)&g_output);

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&g_uavHeap);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    g_dev->CreateUnorderedAccessView(g_output, nullptr, &uav,
        g_uavHeap->GetCPUDescriptorHandleForHeapStart());

    // ---- constant buffer ----
    g_cb = CreateUpload(nullptr, 256);
    g_cb->Map(0, nullptr, &g_cbMapped);
    return true;
}

static void Render(float t) {
    // Update rotating instance transform (object -> world), row-major 3x4.
    XMMATRIX world = XMMatrixRotationY(t) * XMMatrixRotationX(t * 0.5f);
    XMFLOAT4X4 wt; XMStoreFloat4x4(&wt, XMMatrixTranspose(world));
    D3D12_RAYTRACING_INSTANCE_DESC inst = {};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++)
            inst.Transform[r][c] = wt.m[r][c];
    inst.InstanceMask = 1;
    inst.AccelerationStructure = g_blas->GetGPUVirtualAddress();
    memcpy(g_instanceMapped, &inst, sizeof(inst));

    // Update camera constants.
    XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 0, -6, 0), XMVectorSet(0, 0, 0, 0), XMVectorSet(0, 1, 0, 0));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, (float)g_width / g_height, 0.1f, 100.0f);
    XMMATRIX invVP = XMMatrixInverse(nullptr, view * proj);
    CBData cb; XMStoreFloat4x4(&cb.invViewProj, XMMatrixTranspose(invVP));
    cb.camPos = XMFLOAT3(0, 0, -6); cb.time = t;
    memcpy(g_cbMapped, &cb, sizeof(cb));

    g_alloc->Reset();
    g_list->Reset(g_alloc, nullptr);

    // Rebuild TLAS with the new transform.
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

    // Dispatch rays.
    ID3D12DescriptorHeap* heaps[] = { g_uavHeap };
    g_list->SetDescriptorHeaps(1, heaps);
    g_list->SetComputeRootSignature(g_globalRS);
    g_list->SetComputeRootDescriptorTable(0, g_uavHeap->GetGPUDescriptorHandleForHeapStart());
    g_list->SetComputeRootShaderResourceView(1, g_tlas->GetGPUVirtualAddress());
    g_list->SetComputeRootShaderResourceView(2, g_positions->GetGPUVirtualAddress());
    g_list->SetComputeRootShaderResourceView(3, g_indices->GetGPUVirtualAddress());
    g_list->SetComputeRootShaderResourceView(4, g_colors->GetGPUVirtualAddress());
    g_list->SetComputeRootConstantBufferView(5, g_cb->GetGPUVirtualAddress());
    g_list->SetPipelineState1(g_rtpso);

    D3D12_DISPATCH_RAYS_DESC drd = {};
    drd.RayGenerationShaderRecord.StartAddress = g_stBase + 0 * STRIDE;
    drd.RayGenerationShaderRecord.SizeInBytes = STRIDE;
    drd.MissShaderTable.StartAddress = g_stBase + 1 * STRIDE;
    drd.MissShaderTable.SizeInBytes = STRIDE;
    drd.MissShaderTable.StrideInBytes = STRIDE;
    drd.HitGroupTable.StartAddress = g_stBase + 2 * STRIDE;
    drd.HitGroupTable.SizeInBytes = STRIDE;
    drd.HitGroupTable.StrideInBytes = STRIDE;
    drd.Width = g_width; drd.Height = g_height; drd.Depth = 1;
    g_list->DispatchRays(&drd);

    // Copy the ray-traced image to the back buffer.
    D3D12_RESOURCE_BARRIER b[2] = {};
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource = g_output;
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[1].Transition.pResource = g_rtBuffers[g_frameIndex];
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_list->ResourceBarrier(2, b);

    g_list->CopyResource(g_rtBuffers[g_frameIndex], g_output);

    D3D12_RESOURCE_BARRIER b2[2] = {};
    b2[0] = b[0]; b2[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b2[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b2[1] = b[1]; b2[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b2[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_list->ResourceBarrier(2, b2);

    g_list->Close();
    ID3D12CommandList* lists[] = { g_list };
    g_queue->ExecuteCommandLists(1, lists);
    g_swap->Present(1, 0);
    WaitForGpu();
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    WNDCLASS wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = "D3D12RT";
    RegisterClass(&wc);
    RECT r = { 0, 0, (LONG)g_width, (LONG)g_height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindow("D3D12RT", "D3D12 - Ray Tracing (DXR: raygen + miss + closest-hit)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, hInst, nullptr);
    if (!InitD3D(hwnd)) {
        MessageBox(hwnd, "D3D12 ray tracing init failed (needs a DXR-capable GPU/WARP "
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
