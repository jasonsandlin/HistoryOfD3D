// =====================================================================
// D3D12.Debugging - the validation tools for explicit graphics APIs.
//
// Direct3D 12 removes much of the driver's hidden safety net: the application is
// responsible for resource states, synchronization, descriptor lifetime, and GPU
// work submission. The replacement is explicit validation tooling that you turn
// on in development builds.
//
// This sample enables the debug layer before device creation, requests
// GPU-based validation, enables DRED auto-breadcrumb/page-fault capture, drains
// ID3D12InfoQueue messages every frame, and wraps the cube draw in raw
// BeginEvent/SetMarker/EndEvent PIX-style markers without including pix3.h.
// It also probes for DirectX Dump Files (.dxdmp), the preview successor
// direction to DRED, and explains in the panel why retail runtimes say "no".
//
// [LEARN] Use these tools while developing and testing. They are intentionally
// noisy and can be expensive, so shipping builds usually disable them after the
// content and engine have been validated.
// ===================================================================

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <combaseapi.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "version.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#define NK_D3D12_IMPLEMENTATION
#include "third_party/nuklear/nuklear.h"
#include "third_party/nuklear/nuklear_d3d12.h"

#define WINDOW_WIDTH 1180
#define WINDOW_HEIGHT 720
#define MAX_VERTEX_BUFFER (512 * 1024)
#define MAX_INDEX_BUFFER (128 * 1024)


// [DEBUG] Append diagnostics to a log file instead of popping dialogs.
static void logf_line(const char *fmt, ...)
{
    va_list ap; FILE *f = fopen("CubeD3D12.Debugging.log", "a");
    if (!f) return;
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}
static void log_blob(const char *label, ID3DBlob *b)
{
    if (b) logf_line("%s: %.*s", label, (int)ID3D10Blob_GetBufferSize(b),
        (const char *)ID3D10Blob_GetBufferPointer(b));
}

// ---------- tiny row-major / row-vector matrix helper (v' = v * M) ----------
typedef struct { float m[16]; } Mat4; // m[row*4 + col]

static Mat4 mat_mul(Mat4 a, Mat4 b)
{
    Mat4 r;
    int i, j, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++) s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}
static Mat4 mat_rot_x(float a)
{
    float c = cosf(a), s = sinf(a);
    Mat4 r = {{1,0,0,0, 0,c,s,0, 0,-s,c,0, 0,0,0,1}};
    return r;
}
static Mat4 mat_rot_y(float a)
{
    float c = cosf(a), s = sinf(a);
    Mat4 r = {{c,0,-s,0, 0,1,0,0, s,0,c,0, 0,0,0,1}};
    return r;
}
// Left-handed look-at (row-vector convention, matches XMMatrixLookAtLH),
// eye=(ex,ey,ez), target=origin, up=+Y.
static Mat4 mat_lookat_lh(float ex, float ey, float ez)
{
    float zx = -ex, zy = -ey, zz = -ez;
    float zl = sqrtf(zx*zx + zy*zy + zz*zz); zx/=zl; zy/=zl; zz/=zl;
    // x = normalize(cross(up=+Y, z))
    float xx = 1*zz - 0*zy, xy = 0*zx - 0*zz, xz = 0*zy - 1*zx;
    float xl = sqrtf(xx*xx + xy*xy + xz*xz); xx/=xl; xy/=xl; xz/=xl;
    // y = cross(z, x)
    float yx = zy*xz - zz*xy, yy = zz*xx - zx*xz, yz = zx*xy - zy*xx;
    {
        float dx = -(xx*ex + xy*ey + xz*ez);
        float dy = -(yx*ex + yy*ey + yz*ez);
        float dz = -(zx*ex + zy*ey + zz*ez);
        Mat4 r = {{ xx, yx, zx, 0,
                    xy, yy, zy, 0,
                    xz, yz, zz, 0,
                    dx, dy, dz, 1 }};
        return r;
    }
}
// Left-handed perspective, z in [0,1] (matches XMMatrixPerspectiveFovLH).
static Mat4 mat_perspective_lh(float fovY, float aspect, float zn, float zf)
{
    float ys = 1.0f / tanf(fovY * 0.5f);
    float xs = ys / aspect;
    Mat4 r = {{ xs,0,0,0, 0,ys,0,0, 0,0,zf/(zf-zn),1, 0,0,-zn*zf/(zf-zn),0 }};
    return r;
}

// ---------- geometry ----------
typedef struct { float px, py, pz; float r, g, b, a; } Vertex;
static const Vertex CUBE_VERTS[] = {
    { -1,-1,-1, 0,0,0,1 }, { -1, 1,-1, 0,1,0,1 },
    {  1, 1,-1, 1,1,0,1 }, {  1,-1,-1, 1,0,0,1 },
    { -1,-1, 1, 0,0,1,1 }, { -1, 1, 1, 0,1,1,1 },
    {  1, 1, 1, 1,1,1,1 }, {  1,-1, 1, 1,0,1,1 },
};
static const unsigned short CUBE_IDX[] = {
    0,1,2, 0,2,3,  4,6,5, 4,7,6,  4,5,1, 4,1,0,
    3,2,6, 3,6,7,  1,5,6, 1,6,2,  4,0,3, 4,3,7,
};

// ---------- D3D12 objects ----------
static IDXGIFactory2 *dxgi_factory;
static IDXGISwapChain1 *swap_chain;
static ID3D12Device *device;
static ID3D12CommandQueue *command_queue;
static ID3D12Fence *queue_fence;
static UINT64 fence_value;
static ID3D12CommandAllocator *command_allocator;
static ID3D12GraphicsCommandList *command_list;
static ID3D12InfoQueue *info_queue;

static ID3D12DescriptorHeap *rtv_heap;
static ID3D12DescriptorHeap *dsv_heap;
static UINT rtv_increment;
static ID3D12Resource *rtv_buffers[2];
static D3D12_CPU_DESCRIPTOR_HANDLE rtv_handles[2];
static ID3D12Resource *depth_buffer;
static UINT rtv_index;

static ID3D12RootSignature *root_sig;
static ID3D12PipelineState *pso_solid;
static ID3D12PipelineState *pso_wire;
static ID3D12Resource *vbuffer;
static ID3D12Resource *ibuffer;
static D3D12_VERTEX_BUFFER_VIEW vbv;
static D3D12_INDEX_BUFFER_VIEW ibv;

static int g_width = WINDOW_WIDTH, g_height = WINDOW_HEIGHT;
static UINT64 g_frame_count = 0;
static UINT g_barriers_last_frame = 0;
static int g_debug_layer_enabled = 0;
static int g_gbv_enabled = 0;
static int g_dred_enabled = 0;
static int g_trigger_warning = 0;
#define RECENT_DEBUG_MESSAGES 8
static char g_debug_messages[RECENT_DEBUG_MESSAGES][256];
static int g_debug_message_count = 0;
static int g_debug_message_next = 0;

// ---------- DirectX Dump Files (preview) capability probe ----------
// [LEARN] This sample deliberately builds against the *retail* Windows SDK
// headers, which stop at D3D12_FEATURE value 61. The dump-file API only exists in
// the Agility SDK 1.721-preview d3d12.h (D3D12_FEATURE_DUMP_FILE = 71,
// D3D12_FEATURE_DATA_DUMP_FILE, ID3D12DevicePreview). So we declare the value and
// a struct with the same layout locally and just ask the runtime. On a retail
// runtime the answer is E_INVALIDARG ("unknown feature") - which is the lesson:
// feature probes must be written to fail gracefully.
#define SAMPLE_D3D12_FEATURE_DUMP_FILE ((D3D12_FEATURE)71)
typedef struct SAMPLE_FEATURE_DATA_DUMP_FILE {
    BOOL SupportedByOS;              // OS/runtime can write .dxdmp files at all
    UINT DumpFileDriverTier;         // D3D12_DUMP_FILE_DRIVER_TIER: 0 none, 1 TIER_1, 2 TIER_2
    UINT DumpFileDriverOptionsMask;  // D3D12_DUMP_FILE_DRIVER_OPTIONS bits the driver supports
} SAMPLE_FEATURE_DATA_DUMP_FILE;
// IID_ID3D12DevicePreview {8f0856ad-e37c-4d75-be18-fbb3d8c04ced} (preview d3d12.h).
static const IID SAMPLE_IID_ID3D12DevicePreview =
    { 0x8f0856ad, 0xe37c, 0x4d75, { 0xbe, 0x18, 0xfb, 0xb3, 0xd8, 0xc0, 0x4c, 0xed } };
static const struct { UINT bit; const char *name; } g_dump_option_names[] = {
    { 0x01, "NO_OVERHEAD" }, { 0x02, "MEDIUM_OVERHEAD" }, { 0x04, "HIGH_OVERHEAD" },
    { 0x08, "NO_DATA" }, { 0x10, "SHADER_REGISTERS" }, { 0x20, "RESOURCES" },
    { 0x40, "EVENT_MARKERS" },
};
static HRESULT g_dump_hr = E_FAIL;
static HRESULT g_dump_preview_hr = E_FAIL;
static SAMPLE_FEATURE_DATA_DUMP_FILE g_dump_data;
static char g_dump_status[192] = "Not probed yet";
static char g_dump_options[192] = "";
static char g_dump_preview_status[128] = "";
static char g_runtime_status[160] = "";

// ---------- PIX marker encoding (no pix3.h) ----------
// [LEARN] BeginEvent/SetMarker take (metadata, bytes, size). Metadata 2
// (WINPIX_EVENT_PIX3BLOB_VERSION) is the blob WinPixEventRuntime's PIXBeginEvent
// writes: qword0 = event type << 10 (timestamp 0), qword1 = ARGB color,
// qword2 = string header (8-byte copy chunks, ANSI), then the NUL-terminated
// string packed 8 chars per qword. PIX and the debug layer both parse this.
#define PIX_EVENT_METADATA_PIX3BLOB 2
#define PIX_EVENT_BEGIN_NOARGS      0x002
#define PIX_EVENT_SETMARKER_NOARGS  0x008
static UINT encode_pix_marker(UINT64 *buf, UINT qwords, UINT type, UINT64 color, const char *text)
{
    UINT n = 0;
    size_t len = strlen(text), i;
    size_t chunks = (len + 1 + 7) / 8;          // string + NUL, rounded up to qwords
    if (3 + chunks + 1 > qwords) { chunks = qwords - 4; len = chunks * 8 - 1; }
    buf[n++] = ((UINT64)type & 0x3FF) << 10;
    buf[n++] = color;
    buf[n++] = (8ull << 55) | (1ull << 54);     // copy chunk 8, isANSI
    memset(&buf[n], 0, chunks * sizeof(UINT64));
    for (i = 0; i < len; i++) ((unsigned char *)&buf[n])[i] = (unsigned char)text[i];
    n += (UINT)chunks;
    buf[n] = 0;                                 // terminator (not counted in size)
    return n * (UINT)sizeof(UINT64);
}

static const char *g_shader_src =
"cbuffer CB : register(b0) { row_major float4x4 g_mvp; };\n"
"struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
"VSOut VSMain(float3 pos : POSITION, float4 col : COLOR) {\n"
"  VSOut o; o.pos = mul(float4(pos,1.0f), g_mvp); o.col = col; return o; }\n"
"float4 PSMain(VSOut i) : SV_TARGET { return i.col; }\n";

// [LEARN] GPU/CPU synchronization via a fence. The GPU runs asynchronously;
// this signals the fence to (fence_value) once the queue drains, then spins the
// CPU until GetCompletedValue() reaches it. Calling this every frame is the
// SIMPLEST (fully serialized) model - real engines instead let several frames
// overlap. Watch the "CPU fence" vs "GPU completed" values in the UI to see it.
static void signal_and_wait(void)
{
    HRESULT hr = ID3D12CommandQueue_Signal(command_queue, queue_fence, ++fence_value);
    assert(SUCCEEDED(hr)); (void)hr;
    while (ID3D12Fence_GetCompletedValue(queue_fence) != fence_value)
        SwitchToThread();
}

static void execute_commands(void)
{
    ID3D12CommandList *lists[1];
    ID3D12GraphicsCommandList_Close(command_list);
    lists[0] = (ID3D12CommandList *)command_list;
    ID3D12CommandQueue_ExecuteCommandLists(command_queue, 1, lists);
    signal_and_wait();
    ID3D12CommandAllocator_Reset(command_allocator);
    ID3D12GraphicsCommandList_Reset(command_list, command_allocator, NULL);
}

static ID3D12Resource *create_upload_buffer(const void *data, UINT size)
{
    D3D12_HEAP_PROPERTIES hp;
    D3D12_RESOURCE_DESC rd;
    ID3D12Resource *res = NULL;
    void *mapped = NULL;
    D3D12_RANGE none = { 0, 0 };

    memset(&hp, 0, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    memset(&rd, 0, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Device_CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&res);
    ID3D12Resource_Map(res, 0, &none, &mapped);
    memcpy(mapped, data, size);
    ID3D12Resource_Unmap(res, 0, NULL);
    return res;
}

static void create_depth_buffer(int width, int height)
{
    D3D12_HEAP_PROPERTIES hp;
    D3D12_RESOURCE_DESC rd;
    D3D12_CLEAR_VALUE cv;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;

    memset(&hp, 0, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    memset(&rd, 0, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT)width; rd.Height = (UINT)height;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_D32_FLOAT; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    memset(&cv, 0, sizeof(cv));
    cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 1.0f;
    ID3D12Device_CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, &IID_ID3D12Resource, (void **)&depth_buffer);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap, &dsv);
    ID3D12Device_CreateDepthStencilView(device, depth_buffer, NULL, dsv);
}

static void get_swap_chain_buffers(void)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap, &h);
    IDXGISwapChain1_GetBuffer(swap_chain, 0, &IID_ID3D12Resource, (void **)&rtv_buffers[0]);
    IDXGISwapChain1_GetBuffer(swap_chain, 1, &IID_ID3D12Resource, (void **)&rtv_buffers[1]);
    ID3D12Device_CreateRenderTargetView(device, rtv_buffers[0], NULL, h);
    rtv_handles[0] = h;
    h.ptr += rtv_increment;
    ID3D12Device_CreateRenderTargetView(device, rtv_buffers[1], NULL, h);
    rtv_handles[1] = h;
}

static ID3D12PipelineState *create_pso(ID3DBlob *vs, ID3DBlob *ps, D3D12_FILL_MODE fill)
{
    D3D12_INPUT_ELEMENT_DESC il[2];
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    ID3D12PipelineState *pso = NULL;
    UINT i;

    il[0].SemanticName = "POSITION"; il[0].SemanticIndex = 0;
    il[0].Format = DXGI_FORMAT_R32G32B32_FLOAT; il[0].InputSlot = 0;
    il[0].AlignedByteOffset = 0;
    il[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    il[0].InstanceDataStepRate = 0;
    il[1].SemanticName = "COLOR"; il[1].SemanticIndex = 0;
    il[1].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; il[1].InputSlot = 0;
    il[1].AlignedByteOffset = 12;
    il[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    il[1].InstanceDataStepRate = 0;

    memset(&pd, 0, sizeof(pd));
    pd.pRootSignature = root_sig;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
    pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps);
    pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
    pd.InputLayout.pInputElementDescs = il;
    pd.InputLayout.NumElements = 2;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = fill;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    for (i = 0; i < 8; i++)
        pd.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    ID3D12Device_CreateGraphicsPipelineState(device, &pd, &IID_ID3D12PipelineState, (void **)&pso);
    return pso;
}

static int create_cube_pipeline(void)
{
    D3D12_ROOT_PARAMETER rp;
    D3D12_ROOT_SIGNATURE_DESC rsd;
    ID3DBlob *sig = NULL, *sigerr = NULL;
    ID3DBlob *vs = NULL, *ps = NULL, *err = NULL;

    // Root signature: 16 32-bit root constants (the MVP matrix) at b0.
    memset(&rp, 0, sizeof(rp));
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.Num32BitValues = 16;
    rp.Constants.ShaderRegister = 0;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    memset(&rsd, 0, sizeof(rsd));
    rsd.NumParameters = 1; rsd.pParameters = &rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigerr))) {
        if (sigerr) ID3D10Blob_Release(sigerr);
        return 0;
    }
    ID3D12Device_CreateRootSignature(device, 0,
        ID3D10Blob_GetBufferPointer(sig), ID3D10Blob_GetBufferSize(sig),
        &IID_ID3D12RootSignature, (void **)&root_sig);
    ID3D10Blob_Release(sig);

    // Compile shaders at runtime (SM5 via D3DCompile / fxc).
    if (FAILED(D3DCompile(g_shader_src, strlen(g_shader_src), NULL, NULL, NULL,
        "VSMain", "vs_5_0", 0, 0, &vs, &err))) { if (err) ID3D10Blob_Release(err); return 0; }
    if (FAILED(D3DCompile(g_shader_src, strlen(g_shader_src), NULL, NULL, NULL,
        "PSMain", "ps_5_0", 0, 0, &ps, &err))) { if (err) ID3D10Blob_Release(err); return 0; }

    pso_solid = create_pso(vs, ps, D3D12_FILL_MODE_SOLID);
    pso_wire  = create_pso(vs, ps, D3D12_FILL_MODE_WIREFRAME);
    ID3D10Blob_Release(vs); ID3D10Blob_Release(ps);

    // Upload-heap vertex + index buffers.
    vbuffer = create_upload_buffer(CUBE_VERTS, sizeof(CUBE_VERTS));
    vbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vbuffer);
    vbv.StrideInBytes = sizeof(Vertex);
    vbv.SizeInBytes = sizeof(CUBE_VERTS);
    ibuffer = create_upload_buffer(CUBE_IDX, sizeof(CUBE_IDX));
    ibv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(ibuffer);
    ibv.Format = DXGI_FORMAT_R16_UINT;
    ibv.SizeInBytes = sizeof(CUBE_IDX);
    return (root_sig && pso_solid && pso_wire && vbuffer && ibuffer);
}

static void set_swap_chain_size(int width, int height)
{
    signal_and_wait();
    signal_and_wait();
    ID3D12Resource_Release(rtv_buffers[0]);
    ID3D12Resource_Release(rtv_buffers[1]);
    ID3D12Resource_Release(depth_buffer);
    IDXGISwapChain1_ResizeBuffers(swap_chain, 2, (UINT)width, (UINT)height,
        DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
    get_swap_chain_buffers();
    create_depth_buffer(width, height);
    rtv_index = 0;
    g_width = width; g_height = height;
}

static LRESULT CALLBACK WindowProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (swap_chain) {
            int width = LOWORD(lparam), height = HIWORD(lparam);
            if (width > 0 && height > 0) {
                set_swap_chain_size(width, height);
                nk_d3d12_resize(width, height);
            }
        }
        break;
    }
    if (nk_d3d12_handle_event(wnd, msg, wparam, lparam))
        return 0;
    return DefWindowProcW(wnd, msg, wparam, lparam);
}


static void push_debug_message(const char *text)
{
    strncpy(g_debug_messages[g_debug_message_next], text, sizeof(g_debug_messages[0]) - 1);
    g_debug_messages[g_debug_message_next][sizeof(g_debug_messages[0]) - 1] = 0;
    g_debug_message_next = (g_debug_message_next + 1) % RECENT_DEBUG_MESSAGES;
    if (g_debug_message_count < RECENT_DEBUG_MESSAGES) g_debug_message_count++;
}

static void enable_debug_tools(void)
{
    ID3D12Debug *debug = NULL;
    ID3D12Debug1 *debug1 = NULL;
    ID3D12DeviceRemovedExtendedDataSettings *dred = NULL;
    HRESULT hr;
    // [LEARN] The debug layer must be enabled before D3D12CreateDevice so the
    // device and every child object are wrapped by validation.
    hr = D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&debug);
    if (SUCCEEDED(hr) && debug) {
        ID3D12Debug_EnableDebugLayer(debug);
        g_debug_layer_enabled = 1;
        logf_line("Debug layer enabled");
        if (SUCCEEDED(ID3D12Debug_QueryInterface(debug, &IID_ID3D12Debug1, (void **)&debug1))) {
            ID3D12Debug1_SetEnableGPUBasedValidation(debug1, TRUE);
            g_gbv_enabled = 1;
            logf_line("GPU-based validation requested");
            ID3D12Debug1_Release(debug1);
        }
        ID3D12Debug_Release(debug);
    } else logf_line("D3D12GetDebugInterface(ID3D12Debug) hr=0x%08X", (unsigned)hr);

    hr = D3D12GetDebugInterface(&IID_ID3D12DeviceRemovedExtendedDataSettings, (void **)&dred);
    if (SUCCEEDED(hr) && dred) {
        ID3D12DeviceRemovedExtendedDataSettings_SetAutoBreadcrumbsEnablement(dred, D3D12_DRED_ENABLEMENT_FORCED_ON);
        ID3D12DeviceRemovedExtendedDataSettings_SetPageFaultEnablement(dred, D3D12_DRED_ENABLEMENT_FORCED_ON);
        g_dred_enabled = 1;
        logf_line("DRED breadcrumbs and page faults enabled");
        ID3D12DeviceRemovedExtendedDataSettings_Release(dred);
    } else logf_line("D3D12GetDebugInterface(DRED settings) hr=0x%08X", (unsigned)hr);
}

static void drain_info_queue(void)
{
    UINT64 n, i;
    if (!info_queue) return;
    n = ID3D12InfoQueue_GetNumStoredMessages(info_queue);
    for (i = 0; i < n; i++) {
        SIZE_T len = 0;
        D3D12_MESSAGE *msg;
        char line[512];
        if (FAILED(ID3D12InfoQueue_GetMessage(info_queue, i, NULL, &len)) || len == 0) continue;
        msg = (D3D12_MESSAGE *)malloc(len);
        if (!msg) continue;
        if (SUCCEEDED(ID3D12InfoQueue_GetMessage(info_queue, i, msg, &len))) {
            sprintf(line, "[%d/%d id=%d] %.*s", (int)msg->Severity, (int)msg->Category,
                (int)msg->ID, (int)msg->DescriptionByteLength, msg->pDescription);
            logf_line("InfoQueue %s", line);
            push_debug_message(line);
        }
        free(msg);
    }
    if (n) ID3D12InfoQueue_ClearStoredMessages(info_queue);
}

static void trigger_teaching_warning(void)
{
    const char *text = "Intentional teaching warning: this app-defined InfoQueue message proves the debug-message drain and UI path are working without risking device removal.";
    if (info_queue)
        ID3D12InfoQueue_AddMessage(info_queue, D3D12_MESSAGE_CATEGORY_APPLICATION_DEFINED,
            D3D12_MESSAGE_SEVERITY_WARNING, D3D12_MESSAGE_ID_STRING_FROM_APPLICATION, text);
    else {
        logf_line("InfoQueue unavailable; warning button pressed");
        push_debug_message("InfoQueue unavailable; warning button pressed");
    }
}

// ---------- DirectX Dump Files probe ----------
// Logs which D3D12Core.dll is actually running: the OS copy (retail) here,
// versus a .\D3D12\D3D12Core.dll an Agility SDK app would ship next to its exe.
static void describe_runtime(void)
{
    HMODULE core = GetModuleHandleW(L"D3D12Core.dll");
    wchar_t path[MAX_PATH] = L"(D3D12Core.dll not loaded)";
    DWORD handle = 0, size;
    unsigned v[4] = { 0, 0, 0, 0 };
    wchar_t lower[MAX_PATH];
    int in_system32;
    if (core) GetModuleFileNameW(core, path, MAX_PATH);
    wcscpy_s(lower, MAX_PATH, path);
    _wcslwr_s(lower, MAX_PATH);
    in_system32 = wcsstr(lower, L"\\system32\\") != NULL;
    size = core ? GetFileVersionInfoSizeW(path, &handle) : 0;
    if (size) {
        void *data = malloc(size);
        VS_FIXEDFILEINFO *ffi = NULL; UINT len = 0;
        if (data && GetFileVersionInfoW(path, 0, size, data) &&
            VerQueryValueW(data, L"\\", (void **)&ffi, &len) && ffi) {
            v[0] = HIWORD(ffi->dwFileVersionMS); v[1] = LOWORD(ffi->dwFileVersionMS);
            v[2] = HIWORD(ffi->dwFileVersionLS); v[3] = LOWORD(ffi->dwFileVersionLS);
        }
        free(data);
    }
    snprintf(g_runtime_status, sizeof(g_runtime_status), "D3D12 runtime: D3D12Core %u.%u.%u.%u (%s)",
        v[0], v[1], v[2], v[3], in_system32 ? "retail OS copy" : "Agility SDK copy");
    logf_line("D3D12Core.dll: %ls version=%u.%u.%u.%u", path, v[0], v[1], v[2], v[3]);
}

// [LEARN] DirectX Dump Files are the successor direction to DRED. DRED gives you
// breadcrumbs + page-fault data *in-process* after device removal; a dump file is a
// .dxdmp written by the runtime and driver - like a console GPU crash dump - that
// also carries DRED data, adapter/OS info, snapshots of live D3D objects and
// IHV-specific KMD/UMD GPU state blobs, so it can be analyzed offline later.
static void probe_dump_files(void)
{
    IUnknown *preview = NULL;
    describe_runtime();
    memset(&g_dump_data, 0, sizeof(g_dump_data));
    g_dump_hr = ID3D12Device_CheckFeatureSupport(device, SAMPLE_D3D12_FEATURE_DUMP_FILE,
        &g_dump_data, sizeof(g_dump_data));
    if (SUCCEEDED(g_dump_hr)) {
        size_t i, used = 0;
        snprintf(g_dump_status, sizeof(g_dump_status), "SupportedByOS=%s, driver tier %u%s",
            g_dump_data.SupportedByOS ? "yes" : "no", g_dump_data.DumpFileDriverTier,
            g_dump_data.DumpFileDriverTier ? "" : " (NOT_SUPPORTED)");
        g_dump_options[0] = 0;
        for (i = 0; i < sizeof(g_dump_option_names) / sizeof(g_dump_option_names[0]); i++) {
            if (g_dump_data.DumpFileDriverOptionsMask & g_dump_option_names[i].bit) {
                int n = snprintf(g_dump_options + used, sizeof(g_dump_options) - used, "%s%s",
                    used ? " | " : "", g_dump_option_names[i].name);
                if (n > 0 && used + (size_t)n < sizeof(g_dump_options)) used += (size_t)n;
            }
        }
        if (!used) snprintf(g_dump_options, sizeof(g_dump_options), "none");
    } else {
        snprintf(g_dump_status, sizeof(g_dump_status),
            "Not available on this runtime (hr=0x%08X) - requires Agility SDK 1.721-preview",
            (unsigned)g_dump_hr);
        snprintf(g_dump_options, sizeof(g_dump_options), "n/a");
    }
    logf_line("CheckFeatureSupport(71 = D3D12_FEATURE_DUMP_FILE) hr=0x%08X SupportedByOS=%d Tier=%u OptionsMask=0x%X -> %s",
        (unsigned)g_dump_hr, (int)g_dump_data.SupportedByOS, g_dump_data.DumpFileDriverTier,
        g_dump_data.DumpFileDriverOptionsMask, g_dump_status);

    // [LEARN] ConfigureDumpFile / SetDumpFileCallbacks / AddBlobToDumpFile /
    // RetainDumpFile live on ID3D12DevicePreview, so its absence is a second signal.
    g_dump_preview_hr = ID3D12Device_QueryInterface(device, &SAMPLE_IID_ID3D12DevicePreview, (void **)&preview);
    if (preview) IUnknown_Release(preview);
    snprintf(g_dump_preview_status, sizeof(g_dump_preview_status), "ID3D12DevicePreview: %s (hr=0x%08X)",
        SUCCEEDED(g_dump_preview_hr) ? "exposed" : "not exposed", (unsigned)g_dump_preview_hr);
    logf_line("QueryInterface(ID3D12DevicePreview) hr=0x%08X", (unsigned)g_dump_preview_hr);
}

// [LEARN] nk_label_wrap only draws as many lines as its row is tall, so a paragraph
// in an 18px row silently shows one line. This measures the wrapped text with the
// UI font (greedy word wrap, like Nuklear's own) and sizes the row to fit.
static void paragraph_row(struct nk_context *ctx, const char *text)
{
    const struct nk_user_font *font = ctx->style.font;
    float avail = ctx->current->layout->bounds.w - 2.0f * ctx->style.window.padding.x - 12.0f;
    float line_w = 0.0f, space_w = font->width(font->userdata, font->height, " ", 1);
    int lines = 1;
    const char *p = text;
    while (*p) {
        const char *word = p;
        float w;
        while (*p && *p != ' ') p++;
        w = font->width(font->userdata, font->height, word, (int)(p - word));
        if (line_w > 0.0f && line_w + w > avail) { lines++; line_w = 0.0f; }
        line_w += w + space_w;
        while (*p == ' ') p++;
    }
    nk_layout_row_dynamic(ctx, lines * font->height + 6.0f, 1);
}
static void label_paragraph(struct nk_context *ctx, const char *text)
{
    paragraph_row(ctx, text);
    nk_label_wrap(ctx, text);
}

// ---------- teaching UI ----------
static void build_ui(struct nk_context *ctx, struct nk_colorf *bg,
                     int *pause, float *speed, int *wire, int *vsync)
{
    if (nk_begin(ctx, "D3D12 Debugging", nk_rect(20, 20, 500, 700),
        NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_SCALABLE|NK_WINDOW_TITLE))
    {
        char buf[256];
        int i;

        if (nk_tree_push(ctx, NK_TREE_TAB, "Validation status", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 18, 1);
            sprintf(buf, "Debug layer: %s", g_debug_layer_enabled ? "enabled" : "unavailable"); nk_label(ctx, buf, NK_TEXT_LEFT);
            sprintf(buf, "GPU-based validation: %s", g_gbv_enabled ? "requested" : "unavailable"); nk_label(ctx, buf, NK_TEXT_LEFT);
            sprintf(buf, "DRED breadcrumbs/page faults: %s", g_dred_enabled ? "enabled" : "unavailable"); nk_label(ctx, buf, NK_TEXT_LEFT);
            sprintf(buf, "InfoQueue: %s", info_queue ? "active" : "unavailable"); nk_label(ctx, buf, NK_TEXT_LEFT);
            if (nk_button_label(ctx, "Trigger validation warning")) g_trigger_warning = 1;
            label_paragraph(ctx, "The button injects a benign app-defined warning into the same InfoQueue used by the debug layer, so you can see logging and UI plumbing without destabilizing the device.");
            nk_tree_pop(ctx);
        }

        if (nk_tree_push(ctx, NK_TREE_TAB, "DirectX Dump Files (preview)", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label(ctx, g_runtime_status, NK_TEXT_LEFT);
            nk_label(ctx, "CheckFeatureSupport(71 = D3D12_FEATURE_DUMP_FILE):", NK_TEXT_LEFT);
            paragraph_row(ctx, g_dump_status);
            nk_label_colored_wrap(ctx, g_dump_status, SUCCEEDED(g_dump_hr) ? nk_rgb(120, 220, 140) : nk_rgb(240, 190, 90));
            nk_layout_row_dynamic(ctx, 18, 1);
            snprintf(buf, sizeof(buf), "Driver options: %s", g_dump_options); nk_label(ctx, buf, NK_TEXT_LEFT);
            nk_label(ctx, g_dump_preview_status, NK_TEXT_LEFT);
            label_paragraph(ctx, "What: a .dxdmp GPU-crash dump - like a console GPU crash dump - that the runtime and driver write when the device is removed. It is the successor direction to DRED: breadcrumbs and page-fault data become sections of the dump, next to adapter/OS info, snapshots of live D3D objects and IHV GPU-state blobs, readable offline.");
            label_paragraph(ctx, "Driver tier (NOT_SUPPORTED, TIER_1, TIER_2) plus options chosen with ConfigureDumpFile: NO / MEDIUM / HIGH overhead, NO_DATA, SHADER_REGISTERS, RESOURCES, EVENT_MARKERS.");
            label_paragraph(ctx, "Apps can add their own blobs (AddBlobToDumpFile), get begin/end callbacks (SetDumpFileCallbacks) and keep the file (RetainDumpFile). The API is ID3D12DevicePreview in Agility SDK 1.721-preview; format in DXDumpFileFormat.h.");
            label_paragraph(ctx, "Microsoft: there is \"no way to use it just yet\" in 1.721 - so this is an honest capability probe, not a working dump.");
            nk_tree_pop(ctx);
        }

        if (nk_tree_push(ctx, NK_TREE_TAB, "Recent debug messages", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 16, 1);
            if (!g_debug_message_count) nk_label(ctx, "No messages yet.", NK_TEXT_LEFT);
            for (i = 0; i < g_debug_message_count; i++) {
                int idx = (g_debug_message_next + RECENT_DEBUG_MESSAGES - 1 - i) % RECENT_DEBUG_MESSAGES;
                nk_label_wrap(ctx, g_debug_messages[idx]);
            }
            nk_tree_pop(ctx);
        }

        if (nk_tree_push(ctx, NK_TREE_TAB, "Controls", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_checkbox_label(ctx, "Pause rotation", pause);
            nk_checkbox_label(ctx, "Wireframe PSO", wire);
            nk_checkbox_label(ctx, "VSync", vsync);
            nk_property_float(ctx, "Speed", 0.0f, speed, 4.0f, 0.1f, 0.02f);
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "Clear color:", NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 100, 1);
            *bg = nk_color_picker(ctx, *bg, NK_RGB);
            nk_tree_pop(ctx);
        }

        if (nk_tree_push(ctx, NK_TREE_TAB, "Why it matters", NK_MINIMIZED)) {
            label_paragraph(ctx, "D3D12 makes barriers, descriptors, and synchronization explicit. Validation tooling catches mistakes while they are still cheap to diagnose.");
            label_paragraph(ctx, "Enable the debug layer and GBV in development and CI smoke tests; turn them off in shipping builds because they add CPU/GPU overhead.");
            label_paragraph(ctx, "DRED is post-mortem data for device removal: breadcrumbs show recent GPU work, page-fault data points at bad memory access.");
            nk_tree_pop(ctx);
        }
    }
    nk_end(ctx);
}

int main(void)
{
    struct nk_context *ctx;
    struct nk_colorf bg = { 0.10f, 0.18f, 0.24f, 1.0f };
    int pause = 0, wire = 0, vsync = 1;
    float speed = 0.8f;
    float angle = 0.0f;
    ULONGLONG last_tick;

    WNDCLASSW wc;
    RECT rect = { 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT };
    DWORD style = WS_OVERLAPPEDWINDOW;
    HWND wnd;
    int running = 1;
    HRESULT hr;
    D3D12_COMMAND_QUEUE_DESC qd;
    DXGI_SWAP_CHAIN_DESC1 scd;
    D3D12_DESCRIPTOR_HEAP_DESC hd;

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(0);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"D3D12DebuggingClass";
    RegisterClassW(&wc);
    AdjustWindowRectEx(&rect, style, FALSE, WS_EX_APPWINDOW);
    wnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName,
        L"D3D12 Debugging - debug layer, DRED, InfoQueue",
        style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, wc.hInstance, NULL);

    enable_debug_tools();

    // Device, queue, fence, allocator, command list.
    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device);
    if (FAILED(hr)) { MessageBoxW(wnd, L"D3D12 device creation failed", L"Error", 0); logf_line("D3D12CreateDevice hr=0x%08X", (unsigned)hr); return 1; }
    if (SUCCEEDED(ID3D12Device_QueryInterface(device, &IID_ID3D12InfoQueue, (void **)&info_queue)))
        logf_line("InfoQueue acquired");
    else
        logf_line("InfoQueue unavailable");
    probe_dump_files();

    memset(&qd, 0, sizeof(qd));
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12Device_CreateCommandQueue(device, &qd, &IID_ID3D12CommandQueue, (void **)&command_queue);
    ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&queue_fence);
    ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        command_allocator, NULL, &IID_ID3D12GraphicsCommandList1, (void **)&command_list);

    // RTV + DSV descriptor heaps.
    memset(&hd, 0, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 2;
    ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap);
    rtv_increment = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    memset(&hd, 0, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 1;
    ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&dsv_heap);

    // Swap chain.
    CreateDXGIFactory1(&IID_IDXGIFactory2, (void **)&dxgi_factory);
    memset(&scd, 0, sizeof(scd));
    scd.Width = WINDOW_WIDTH; scd.Height = WINDOW_HEIGHT;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.Scaling = DXGI_SCALING_STRETCH;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    IDXGIFactory2_CreateSwapChainForHwnd(dxgi_factory, (IUnknown *)command_queue, wnd,
        &scd, NULL, NULL, &swap_chain);
    get_swap_chain_buffers();
    create_depth_buffer(WINDOW_WIDTH, WINDOW_HEIGHT);

    if (!create_cube_pipeline()) {
        logf_line("Cube pipeline creation failed");
        return 1;
    }

    // Nuklear GUI.
    ctx = nk_d3d12_init(device, WINDOW_WIDTH, WINDOW_HEIGHT, MAX_VERTEX_BUFFER, MAX_INDEX_BUFFER, 1);
    {
        struct nk_font_atlas *atlas;
        nk_d3d12_font_stash_begin(&atlas);
        nk_d3d12_font_stash_end(command_list);
    }
    execute_commands();
    nk_d3d12_font_stash_cleanup();

    last_tick = GetTickCount64();
    while (running) {
        MSG msg;
        ULONGLONG now;
        float dt;
        D3D12_RESOURCE_BARRIER barrier;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv;
        D3D12_VIEWPORT vp;
        D3D12_RECT scissor;
        Mat4 world, view, proj, mvp;
        float clear[4];

        // --- input ---
        nk_input_begin(ctx);
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        nk_input_end(ctx);

        drain_info_queue();
        if (g_trigger_warning) { trigger_teaching_warning(); g_trigger_warning = 0; drain_info_queue(); }

        // --- gui ---
        build_ui(ctx, &bg, &pause, &speed, &wire, &vsync);

        // --- update ---
        now = GetTickCount64();
        dt = (now - last_tick) / 1000.0f;
        last_tick = now;
        if (!pause) angle += dt * speed;

        world = mat_mul(mat_rot_y(angle), mat_rot_x(angle * 0.5f));
        view = mat_lookat_lh(0.0f, 0.0f, -6.0f);
        proj = mat_perspective_lh(3.14159265f / 4.0f, (float)g_width / (float)g_height, 0.1f, 100.0f);
        mvp = mat_mul(mat_mul(world, view), proj);

        // --- record ---
        // [LEARN] Resource barrier: the back buffer must be transitioned from
        // PRESENT to RENDER_TARGET before you draw to it (and back to PRESENT
        // before Present). D3D12 makes YOU manage these state transitions - the
        // driver no longer does it for you. The UI counts them per frame.
        g_barriers_last_frame = 0;
        memset(&barrier, 0, sizeof(barrier));
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = rtv_buffers[rtv_index];
        barrier.Transition.Subresource = 0;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barrier);
        g_barriers_last_frame++;

        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap, &dsv);
        ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &rtv_handles[rtv_index], FALSE, &dsv);
        clear[0] = bg.r; clear[1] = bg.g; clear[2] = bg.b; clear[3] = bg.a;
        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, rtv_handles[rtv_index], clear, 0, NULL);
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);

        vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (float)g_width; vp.Height = (float)g_height;
        vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
        scissor.left = 0; scissor.top = 0; scissor.right = g_width; scissor.bottom = g_height;
        ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &scissor);

        {
            UINT64 draw_blob[16], marker_blob[16];
            UINT draw_size = encode_pix_marker(draw_blob, 16, PIX_EVENT_BEGIN_NOARGS, 0xFF4080FFull, "Draw rotating cube");
            UINT marker_size = encode_pix_marker(marker_blob, 16, PIX_EVENT_SETMARKER_NOARGS, 0xFFFFC040ull, "Bind cube pipeline");
            // [LEARN] PIX-style markers are just raw command-list events. Tools
            // display this label around the GPU work without requiring pix3.h.
            // The metadata value says how to parse the payload; the debug layer
            // only accepts the WinPixEventRuntime blob format (see encode_pix_marker),
            // and flags a raw string as CORRUPTION + an unmatched EndEvent ERROR.
            ID3D12GraphicsCommandList_BeginEvent(command_list, PIX_EVENT_METADATA_PIX3BLOB, draw_blob, draw_size);
            ID3D12GraphicsCommandList_SetMarker(command_list, PIX_EVENT_METADATA_PIX3BLOB, marker_blob, marker_size);
        }

        ID3D12GraphicsCommandList_SetPipelineState(command_list, wire ? pso_wire : pso_solid);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, root_sig);
        // [LEARN] Root constants: the MVP matrix (16 floats) is pushed straight
        // into the root signature - no constant buffer, no descriptor, no upload
        // heap needed. This is the cheapest way to feed a small amount of
        // per-draw data to a shader.
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 16, mvp.m, 0);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_IASetVertexBuffers(command_list, 0, 1, &vbv);
        ID3D12GraphicsCommandList_IASetIndexBuffer(command_list, &ibv);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, 36, 1, 0, 0, 0);
        ID3D12GraphicsCommandList_EndEvent(command_list);

        // GUI overlay (sets its own PSO/root sig/heap/topology/viewport).
        nk_d3d12_render(command_list, NK_ANTI_ALIASING_ON);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barrier);
        g_barriers_last_frame++;

        execute_commands();

        hr = IDXGISwapChain1_Present(swap_chain, vsync ? 1 : 0, 0);
        rtv_index = (rtv_index + 1) % 2;
        g_frame_count++;
        if (hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_REMOVED) {
            logf_line("D3D12 device lost");
            break;
        } else if (hr == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
        }
    }

    // Shutdown.
    nk_d3d12_shutdown();
    signal_and_wait();
    signal_and_wait();
    ID3D12PipelineState_Release(pso_solid);
    ID3D12PipelineState_Release(pso_wire);
    ID3D12RootSignature_Release(root_sig);
    ID3D12Resource_Release(vbuffer);
    ID3D12Resource_Release(ibuffer);
    ID3D12Resource_Release(depth_buffer);
    ID3D12Resource_Release(rtv_buffers[0]);
    ID3D12Resource_Release(rtv_buffers[1]);
    ID3D12DescriptorHeap_Release(rtv_heap);
    ID3D12DescriptorHeap_Release(dsv_heap);
    IDXGISwapChain1_Release(swap_chain);
    IDXGIFactory2_Release(dxgi_factory);
    if (info_queue) ID3D12InfoQueue_Release(info_queue);
    ID3D12GraphicsCommandList_Release(command_list);
    ID3D12CommandAllocator_Release(command_allocator);
    ID3D12CommandQueue_Release(command_queue);
    ID3D12Fence_Release(queue_fence);
    ID3D12Device_Release(device);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
