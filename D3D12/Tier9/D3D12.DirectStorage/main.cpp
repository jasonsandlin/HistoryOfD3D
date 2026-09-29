// D3D12.DirectStorage - stream texture bytes from disk into a D3D12 resource.
// DirectStorage (2022) batches file reads so fast NVMe storage can feed GPU
// resources with much less CPU overhead; the classic use is streaming large
// textures or scene chunks while rendering. Requires the external NuGet package
// Microsoft.Direct3D.DirectStorage because dstorage.h is not in the Windows SDK.
// This sample creates a tiny texture asset, loads it with factory -> queue ->
// request -> submit -> fence, and maps that texture onto a rotating cube.
// ESC quits.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winver.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>

#include "third_party/dstorage/include/dstorage.h"

using namespace DirectX;

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "third_party/dstorage/lib/x64/dstorage.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:WinMainCRTStartup")

static const UINT kFrames = 2;
static const UINT kTextureWidth = 64;
static const UINT kTextureHeight = 64;

struct Vertex { XMFLOAT3 pos; XMFLOAT2 uv; };
struct RootData { XMFLOAT4X4 mvp; };

static UINT g_width = 800, g_height = 600;
static ID3D12Device* g_dev = nullptr;
static ID3D12CommandQueue* g_queue = nullptr;
static IDXGISwapChain3* g_swap = nullptr;
static ID3D12DescriptorHeap* g_rtvHeap = nullptr;
static ID3D12DescriptorHeap* g_dsvHeap = nullptr;
static ID3D12DescriptorHeap* g_srvHeap = nullptr;
static UINT g_rtvSize = 0;
static ID3D12Resource* g_rtBuffers[kFrames] = {};
static ID3D12Resource* g_depth = nullptr;
static ID3D12CommandAllocator* g_alloc = nullptr;
static ID3D12GraphicsCommandList* g_list = nullptr;
static ID3D12RootSignature* g_rootSig = nullptr;
static ID3D12PipelineState* g_pso = nullptr;
static ID3D12Resource* g_vb = nullptr;
static ID3D12Resource* g_ib = nullptr;
static D3D12_VERTEX_BUFFER_VIEW g_vbv = {};
static D3D12_INDEX_BUFFER_VIEW g_ibv = {};
static ID3D12Resource* g_texture = nullptr;
static ID3D12Resource* g_textureBytes = nullptr;
static ID3D12Fence* g_fence = nullptr;
static UINT64 g_fenceValue = 0;
static HANDLE g_fenceEvent = nullptr;
static UINT g_frameIndex = 0;

static IDStorageFactory* g_dsFactory = nullptr;
static IDStorageQueue* g_dsQueue = nullptr;
static IDStorageFile* g_dsFile = nullptr;

using PFN_DStorageSetConfiguration1 = HRESULT(WINAPI*)(DSTORAGE_CONFIGURATION1 const*);
using PFN_DStorageGetFactory = HRESULT(WINAPI*)(REFIID, void**);
static PFN_DStorageSetConfiguration1 g_DStorageSetConfiguration1 = nullptr;
static PFN_DStorageGetFactory g_DStorageGetFactory = nullptr;

static void logf_line(const char* fmt, ...)
{
    FILE* f = nullptr;
    fopen_s(&f, "CubeD3D12.DirectStorage.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void ReleaseAll()
{
    if (g_dsFile) g_dsFile->Release();
    if (g_dsQueue) g_dsQueue->Release();
    if (g_dsFactory) g_dsFactory->Release();
    if (g_textureBytes) g_textureBytes->Release();
    if (g_texture) g_texture->Release();
    if (g_ib) g_ib->Release();
    if (g_vb) g_vb->Release();
    if (g_pso) g_pso->Release();
    if (g_rootSig) g_rootSig->Release();
    if (g_list) g_list->Release();
    if (g_alloc) g_alloc->Release();
    if (g_fence) g_fence->Release();
    if (g_fenceEvent) CloseHandle(g_fenceEvent);
    if (g_depth) g_depth->Release();
    for (UINT i = 0; i < kFrames; ++i) if (g_rtBuffers[i]) g_rtBuffers[i]->Release();
    if (g_srvHeap) g_srvHeap->Release();
    if (g_dsvHeap) g_dsvHeap->Release();
    if (g_rtvHeap) g_rtvHeap->Release();
    if (g_swap) g_swap->Release();
    if (g_queue) g_queue->Release();
    if (g_dev) g_dev->Release();
}

static std::wstring GetSampleDir()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    size_t last = dir.find_last_of(L"\\/");
    if (last != std::wstring::npos && _wcsicmp(dir.c_str() + last + 1, L"Out") == 0) dir.resize(last);
    return dir;
}

static std::wstring JoinPath(const std::wstring& a, const wchar_t* b)
{
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

static void LogDllVersion(const std::wstring& path)
{
    DWORD unused = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (!size) {
        logf_line("DirectStorage DLL version unavailable");
        return;
    }
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return;
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) && info && len) {
        logf_line("DirectStorage DLL version=%u.%u.%u.%u",
            HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
            HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    }
}

static bool LoadDirectStorageRuntime(const std::wstring& sampleDir)
{
    std::wstring dll = JoinPath(sampleDir, L"third_party\\dstorage\\bin\\x64\\dstorage.dll");
    HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        logf_line("LoadLibrary DirectStorage runtime failed gle=%lu", GetLastError());
        return false;
    }
    g_DStorageSetConfiguration1 = (PFN_DStorageSetConfiguration1)GetProcAddress(module, "DStorageSetConfiguration1");
    g_DStorageGetFactory = (PFN_DStorageGetFactory)GetProcAddress(module, "DStorageGetFactory");
    if (!g_DStorageSetConfiguration1 || !g_DStorageGetFactory) {
        logf_line("DirectStorage runtime exports missing");
        return false;
    }
    logf_line("DirectStorage SDK header version=%u", DSTORAGE_SDK_VERSION);
    LogDllVersion(dll);
    return true;
}

static ID3DBlob* CompileShader(const char* src, const char* entry, const char* target)
{
    ID3DBlob* shader = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, 0, 0, &shader, &errors);
    if (FAILED(hr)) {
        logf_line("D3DCompile %s/%s hr=0x%08X", entry, target, (unsigned)hr);
        if (errors) logf_line("shader error: %.*s", (int)errors->GetBufferSize(), (const char*)errors->GetBufferPointer());
        if (errors) errors->Release();
        return nullptr;
    }
    if (errors) errors->Release();
    return shader;
}

static void WaitForFenceValue(UINT64 value)
{
    if (g_fence->GetCompletedValue() < value) {
        g_fence->SetEventOnCompletion(value, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
}

static void WaitForGpu()
{
    const UINT64 value = ++g_fenceValue;
    g_queue->Signal(g_fence, value);
    WaitForFenceValue(value);
    g_frameIndex = g_swap->GetCurrentBackBufferIndex();
}

static HRESULT CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state, ID3D12Resource** resource)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heapType;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
        __uuidof(ID3D12Resource), (void**)resource);
}

static bool FillUploadBuffer(ID3D12Resource* res, const void* data, size_t size)
{
    void* mapped = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    if (FAILED(res->Map(0, &readRange, &mapped))) return false;
    memcpy(mapped, data, size);
    res->Unmap(0, nullptr);
    return true;
}

static bool WriteTextureAssetIfNeeded(const std::wstring& path, UINT rowPitch, UINT height)
{
    LARGE_INTEGER existingSize = {};
    HANDLE existing = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (existing != INVALID_HANDLE_VALUE) {
        GetFileSizeEx(existing, &existingSize);
        CloseHandle(existing);
        if (existingSize.QuadPart == (LONGLONG)rowPitch * height) return true;
    }

    std::vector<uint8_t> bytes((size_t)rowPitch * height);
    for (UINT y = 0; y < height; ++y) {
        uint8_t* row = bytes.data() + (size_t)y * rowPitch;
        for (UINT x = 0; x < kTextureWidth; ++x) {
            const bool checker = ((x / 8) + (y / 8)) % 2 == 0;
            const uint8_t r = checker ? 255 : 30;
            const uint8_t g = (uint8_t)(80 + x * 2);
            const uint8_t b = checker ? (uint8_t)(60 + y * 2) : 255;
            row[x * 4 + 0] = r;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = b;
            row[x * 4 + 3] = 255;
        }
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        logf_line("Create texture asset failed gle=%lu", GetLastError());
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(file, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    CloseHandle(file);
    if (!ok || written != bytes.size()) {
        logf_line("Write texture asset failed gle=%lu written=%lu", GetLastError(), written);
        return false;
    }
    logf_line("Created DirectStorageTexture.bin bytes=%u", written);
    return true;
}

static bool CopyStorageBufferToTexture(const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout)
{
    if (FAILED(g_alloc->Reset())) return false;
    if (FAILED(g_list->Reset(g_alloc, nullptr))) return false;

    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = g_textureBytes;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = g_texture;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_list->ResourceBarrier(1, barriers);

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = g_textureBytes;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = layout;
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = g_texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER ready[2] = {};
    ready[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    ready[0].Transition.pResource = g_texture;
    ready[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    ready[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    ready[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ready[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    ready[1].Transition.pResource = g_textureBytes;
    ready[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ready[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    ready[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_list->ResourceBarrier(2, ready);

    if (FAILED(g_list->Close())) return false;
    ID3D12CommandList* lists[] = { g_list };
    g_queue->ExecuteCommandLists(1, lists);
    WaitForGpu();
    return true;
}

static bool LoadTextureWithDirectStorage(const std::wstring& sampleDir)
{
    if (!LoadDirectStorageRuntime(sampleDir)) return false;

    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = kTextureWidth;
    textureDesc.Height = kTextureHeight;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    UINT numRows = 0;
    UINT64 rowSize = 0;
    UINT64 totalBytes = 0;
    g_dev->GetCopyableFootprints(&textureDesc, 0, 1, 0, &layout, &numRows, &rowSize, &totalBytes);
    if (totalBytes > UINT32_MAX) return false;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    HRESULT hr = g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&g_texture);
    if (FAILED(hr)) {
        logf_line("Create texture hr=0x%08X", (unsigned)hr);
        return false;
    }
    hr = CreateBuffer(totalBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, &g_textureBytes);
    if (FAILED(hr)) {
        logf_line("Create DirectStorage destination buffer hr=0x%08X", (unsigned)hr);
        return false;
    }

    std::wstring asset = JoinPath(sampleDir, L"DirectStorageTexture.bin");
    if (!WriteTextureAssetIfNeeded(asset, layout.Footprint.RowPitch, kTextureHeight)) return false;

    DSTORAGE_CONFIGURATION1 config = {};
    config.DisableBypassIO = TRUE;
    config.ForceFileBuffering = TRUE;
    config.DisableTelemetry = TRUE;
    hr = g_DStorageSetConfiguration1(&config);
    if (FAILED(hr)) logf_line("DStorageSetConfiguration1 hr=0x%08X", (unsigned)hr);

    // [LEARN] The factory is the app-wide DirectStorage entry point: create it once,
    // then use it to open files, status arrays, and queues.
    hr = g_DStorageGetFactory(__uuidof(IDStorageFactory), (void**)&g_dsFactory);
    if (FAILED(hr)) {
        logf_line("DStorageGetFactory hr=0x%08X", (unsigned)hr);
        return false;
    }
    g_dsFactory->SetDebugFlags(DSTORAGE_DEBUG_SHOW_ERRORS | DSTORAGE_DEBUG_RECORD_OBJECT_NAMES);

    // [LEARN] A file queue bound to the D3D12 device can target GPU resources;
    // memory-only queues do not need a device, but texture streaming does.
    DSTORAGE_QUEUE_DESC qd = {};
    qd.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    qd.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
    qd.Priority = DSTORAGE_PRIORITY_NORMAL;
    qd.Name = "Texture file queue";
    qd.Device = g_dev;
    hr = g_dsFactory->CreateQueue(&qd, __uuidof(IDStorageQueue), (void**)&g_dsQueue);
    if (FAILED(hr)) {
        logf_line("CreateQueue hr=0x%08X", (unsigned)hr);
        return false;
    }

    hr = g_dsFactory->OpenFile(asset.c_str(), __uuidof(IDStorageFile), (void**)&g_dsFile);
    if (FAILED(hr)) {
        logf_line("OpenFile hr=0x%08X", (unsigned)hr);
        return false;
    }

    IDStorageStatusArray* status = nullptr;
    hr = g_dsFactory->CreateStatusArray(1, "Texture status", __uuidof(IDStorageStatusArray), (void**)&status);
    if (FAILED(hr)) {
        logf_line("CreateStatusArray hr=0x%08X", (unsigned)hr);
        return false;
    }

    // [LEARN] A request describes the source, destination, and size; enqueueing
    // only records work. Submit is what lets DirectStorage begin processing.
    DSTORAGE_REQUEST req = {};
    req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    req.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_BUFFER;
    req.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
    req.Source.File.Source = g_dsFile;
    req.Source.File.Offset = 0;
    req.Source.File.Size = (UINT32)totalBytes;
    req.Destination.Buffer.Resource = g_textureBytes;
    req.Destination.Buffer.Offset = 0;
    req.Destination.Buffer.Size = (UINT32)totalBytes;
    req.Name = "Texture bytes";
    g_dsQueue->EnqueueRequest(&req);
    g_dsQueue->EnqueueStatus(status, 0);

    // [LEARN] Fence signaling gives a normal D3D12 synchronization point, so the
    // render queue can safely copy/use the data after the DirectStorage queue finishes.
    UINT64 signal = ++g_fenceValue;
    g_dsQueue->EnqueueSignal(g_fence, signal);
    g_dsQueue->Submit();
    WaitForFenceValue(signal);

    HRESULT dsResult = status->GetHResult(0);
    status->Release();
    if (FAILED(dsResult)) {
        logf_line("DirectStorage request failed hr=0x%08X", (unsigned)dsResult);
        if (WaitForSingleObject(g_dsQueue->GetErrorEvent(), 0) == WAIT_OBJECT_0) {
            DSTORAGE_ERROR_RECORD record = {};
            g_dsQueue->RetrieveErrorRecord(&record);
            logf_line("DirectStorage first failure hr=0x%08X command=%d failures=%u",
                (unsigned)record.FirstFailure.HResult, (int)record.FirstFailure.CommandType, record.FailureCount);
        }
        return false;
    }
    logf_line("DirectStorage streamed %llu bytes into a D3D12 buffer", totalBytes);

    return CopyStorageBufferToTexture(layout);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

static bool CreateSwapChainAndViews(HWND hwnd)
{
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory4), (void**)&factory);
    if (FAILED(hr)) return false;

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.BufferCount = kFrames;
    sd.Width = g_width;
    sd.Height = g_height;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc.Count = 1;
    IDXGISwapChain1* sc1 = nullptr;
    hr = factory->CreateSwapChainForHwnd(g_queue, hwnd, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr)) {
        factory->Release();
        return false;
    }
    sc1->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&g_swap);
    sc1->Release();
    factory->Release();
    g_frameIndex = g_swap->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rh = {};
    rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rh.NumDescriptors = kFrames;
    g_dev->CreateDescriptorHeap(&rh, __uuidof(ID3D12DescriptorHeap), (void**)&g_rtvHeap);
    g_rtvSize = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC dh = {};
    dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dh.NumDescriptors = 1;
    g_dev->CreateDescriptorHeap(&dh, __uuidof(ID3D12DescriptorHeap), (void**)&g_dsvHeap);

    D3D12_DESCRIPTOR_HEAP_DESC sh = {};
    sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.NumDescriptors = 1;
    sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_dev->CreateDescriptorHeap(&sh, __uuidof(ID3D12DescriptorHeap), (void**)&g_srvHeap);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrames; ++i) {
        g_swap->GetBuffer(i, __uuidof(ID3D12Resource), (void**)&g_rtBuffers[i]);
        g_dev->CreateRenderTargetView(g_rtBuffers[i], nullptr, rtv);
        rtv.ptr += g_rtvSize;
    }

    D3D12_HEAP_PROPERTIES dhp = {};
    dhp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC drd = {};
    drd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    drd.Width = g_width;
    drd.Height = g_height;
    drd.DepthOrArraySize = 1;
    drd.MipLevels = 1;
    drd.Format = DXGI_FORMAT_D32_FLOAT;
    drd.SampleDesc.Count = 1;
    drd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv = {};
    cv.Format = DXGI_FORMAT_D32_FLOAT;
    cv.DepthStencil.Depth = 1.0f;
    g_dev->CreateCommittedResource(&dhp, D3D12_HEAP_FLAG_NONE, &drd,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, __uuidof(ID3D12Resource), (void**)&g_depth);
    g_dev->CreateDepthStencilView(g_depth, nullptr, g_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    return g_swap && g_rtvHeap && g_dsvHeap && g_srvHeap && g_depth;
}

static bool CreatePipeline()
{
    static const char* src =
        "cbuffer CB : register(b0) { float4x4 mvp; };\n"
        "Texture2D tex0 : register(t0);\n"
        "SamplerState samp0 : register(s0);\n"
        "struct VSIn { float3 pos : POSITION; float2 uv : TEXCOORD0; };\n"
        "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
        "VSOut VSMain(VSIn i) { VSOut o; o.pos = mul(float4(i.pos, 1.0), mvp); o.uv = i.uv; return o; }\n"
        "float4 PSMain(VSOut i) : SV_TARGET { return tex0.Sample(samp0, i.uv); }\n";

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 16;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &range;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ShaderRegister = 0;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 2;
    rsd.pParameters = rp;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &sampler;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* sig = nullptr;
    ID3DBlob* sigErr = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr);
    if (FAILED(hr)) {
        logf_line("D3D12SerializeRootSignature hr=0x%08X", (unsigned)hr);
        if (sigErr) sigErr->Release();
        return false;
    }
    hr = g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
        __uuidof(ID3D12RootSignature), (void**)&g_rootSig);
    sig->Release();
    if (FAILED(hr)) return false;

    ID3DBlob* vs = CompileShader(src, "VSMain", "vs_5_0");
    ID3DBlob* ps = CompileShader(src, "PSMain", "ps_5_0");
    if (!vs || !ps) return false;

    D3D12_INPUT_ELEMENT_DESC il[2] = {};
    il[0].SemanticName = "POSITION";
    il[0].Format = DXGI_FORMAT_R32G32B32_FLOAT;
    il[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    il[1].SemanticName = "TEXCOORD";
    il[1].Format = DXGI_FORMAT_R32G32_FLOAT;
    il[1].AlignedByteOffset = 12;
    il[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_rootSig;
    pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.InputLayout = { il, 2 };
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    hr = g_dev->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&g_pso);
    vs->Release();
    ps->Release();
    if (FAILED(hr)) {
        logf_line("CreateGraphicsPipelineState hr=0x%08X", (unsigned)hr);
        return false;
    }
    return true;
}

static bool CreateCubeGeometry()
{
    static const Vertex verts[] = {
        { { -1,-1,-1 }, { 0,1 } }, { { -1, 1,-1 }, { 0,0 } }, { {  1, 1,-1 }, { 1,0 } }, { {  1,-1,-1 }, { 1,1 } },
        { {  1,-1, 1 }, { 0,1 } }, { {  1, 1, 1 }, { 0,0 } }, { { -1, 1, 1 }, { 1,0 } }, { { -1,-1, 1 }, { 1,1 } },
        { { -1,-1, 1 }, { 0,1 } }, { { -1, 1, 1 }, { 0,0 } }, { { -1, 1,-1 }, { 1,0 } }, { { -1,-1,-1 }, { 1,1 } },
        { {  1,-1,-1 }, { 0,1 } }, { {  1, 1,-1 }, { 0,0 } }, { {  1, 1, 1 }, { 1,0 } }, { {  1,-1, 1 }, { 1,1 } },
        { { -1, 1,-1 }, { 0,1 } }, { { -1, 1, 1 }, { 0,0 } }, { {  1, 1, 1 }, { 1,0 } }, { {  1, 1,-1 }, { 1,1 } },
        { { -1,-1, 1 }, { 0,1 } }, { { -1,-1,-1 }, { 0,0 } }, { {  1,-1,-1 }, { 1,0 } }, { {  1,-1, 1 }, { 1,1 } },
    };
    static const uint16_t idx[] = {
        0,1,2, 0,2,3, 4,5,6, 4,6,7, 8,9,10, 8,10,11,
        12,13,14, 12,14,15, 16,17,18, 16,18,19, 20,21,22, 20,22,23,
    };

    if (FAILED(CreateBuffer(sizeof(verts), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, &g_vb))) return false;
    if (FAILED(CreateBuffer(sizeof(idx), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, &g_ib))) return false;
    if (!FillUploadBuffer(g_vb, verts, sizeof(verts)) || !FillUploadBuffer(g_ib, idx, sizeof(idx))) return false;
    g_vbv.BufferLocation = g_vb->GetGPUVirtualAddress();
    g_vbv.StrideInBytes = sizeof(Vertex);
    g_vbv.SizeInBytes = sizeof(verts);
    g_ibv.BufferLocation = g_ib->GetGPUVirtualAddress();
    g_ibv.Format = DXGI_FORMAT_R16_UINT;
    g_ibv.SizeInBytes = sizeof(idx);
    return true;
}

static bool InitD3D(HWND hwnd)
{
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&g_dev);
    if (FAILED(hr)) {
        logf_line("D3D12CreateDevice hr=0x%08X", (unsigned)hr);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = g_dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&g_queue);
    if (FAILED(hr)) return false;

    if (!CreateSwapChainAndViews(hwnd)) return false;
    if (!CreatePipeline()) return false;
    if (!CreateCubeGeometry()) return false;

    hr = g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&g_alloc);
    if (FAILED(hr)) return false;
    hr = g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr,
        __uuidof(ID3D12GraphicsCommandList), (void**)&g_list);
    if (FAILED(hr)) return false;
    g_list->Close();
    hr = g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_fence);
    if (FAILED(hr)) return false;
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!g_fenceEvent) return false;

    // [LEARN] DirectStorage is used during asset upload, before the render loop,
    // because startup streaming has the same queue/request/fence pattern as runtime streaming.
    std::wstring sampleDir = GetSampleDir();
    if (!LoadTextureWithDirectStorage(sampleDir)) return false;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(g_texture, &srv, g_srvHeap->GetCPUDescriptorHandleForHeapStart());
    return true;
}

static void Render(float t)
{
    g_alloc->Reset();
    g_list->Reset(g_alloc, g_pso);

    g_list->SetGraphicsRootSignature(g_rootSig);
    ID3D12DescriptorHeap* heaps[] = { g_srvHeap };
    g_list->SetDescriptorHeaps(1, heaps);
    XMMATRIX world = XMMatrixRotationY(t) * XMMatrixRotationX(t * 0.5f);
    XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 0, -6, 0), XMVectorSet(0, 0, 0, 0), XMVectorSet(0, 1, 0, 0));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, (float)g_width / g_height, 0.1f, 100.0f);
    RootData rd;
    XMStoreFloat4x4(&rd.mvp, XMMatrixTranspose(world * view * proj));
    g_list->SetGraphicsRoot32BitConstants(0, 16, &rd, 0);
    g_list->SetGraphicsRootDescriptorTable(1, g_srvHeap->GetGPUDescriptorHandleForHeapStart());

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

    float clear[4] = { 0.07f, 0.08f, 0.12f, 1.0f };
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

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int)
{
    logf_line("Starting D3D12.DirectStorage sample");
    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "D3D12DirectStorage";
    RegisterClass(&wc);

    RECT r = { 0, 0, (LONG)g_width, (LONG)g_height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindow("D3D12DirectStorage", "D3D12 - DirectStorage texture streaming",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        logf_line("CreateWindow failed gle=%lu", GetLastError());
        return 1;
    }
    if (!InitD3D(hwnd)) {
        logf_line("D3D12.DirectStorage init failed");
        ReleaseAll();
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);

    DWORD start = GetTickCount();
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            Render((GetTickCount() - start) / 1000.0f);
        }
    }
    WaitForGpu();
    ReleaseAll();
    return (int)msg.wParam;
}
