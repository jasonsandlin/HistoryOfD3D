// =====================================================================
// D3D12.Instancing - thousands of cubes with one indexed draw call.
//
// This sample teaches hardware instancing: slot 0 is ordinary PER-VERTEX
// cube POSITION+COLOR, while slot 1 is PER-INSTANCE data containing an offset
// and tint. The vertex shader receives both streams and applies the instance
// offset before the shared MVP matrix.
//
// [LEARN] Use instancing when many copies of one mesh differ only by cheap
// per-copy data such as transform, color, material index, or animation phase.
// It removes CPU draw-call overhead, but very high counts can still become
// GPU-bound; many different meshes usually need batching or indirect draws.
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
#define MAX_INSTANCES 4096

static void logf_line(const char *fmt, ...)
{
    va_list ap;
    FILE *f = fopen("CubeD3D12.Instancing.log", "a");
    if (!f) return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
static void log_blob(const char *label, ID3DBlob *b)
{
    if (b) logf_line("%s: %.*s", label, (int)ID3D10Blob_GetBufferSize(b),
        (const char *)ID3D10Blob_GetBufferPointer(b));
}

// ---------- tiny row-major / row-vector matrix helper (v' = v * M) ----------
typedef struct { float m[16]; } Mat4;

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
static Mat4 mat_lookat_lh(float ex, float ey, float ez)
{
    float zx = -ex, zy = -ey, zz = -ez;
    float zl = sqrtf(zx*zx + zy*zy + zz*zz); zx/=zl; zy/=zl; zz/=zl;
    float xx = 1*zz - 0*zy, xy = 0*zx - 0*zz, xz = 0*zy - 1*zx;
    float xl = sqrtf(xx*xx + xy*xy + xz*xz); xx/=xl; xy/=xl; xz/=xl;
    float yx = zy*xz - zz*xy, yy = zz*xx - zx*xz, yz = zx*xy - zy*xx;
    float dx = -(xx*ex + xy*ey + xz*ez);
    float dy = -(yx*ex + yy*ey + yz*ez);
    float dz = -(zx*ex + zy*ey + zz*ez);
    Mat4 r = {{ xx,yx,zx,0, xy,yy,zy,0, xz,yz,zz,0, dx,dy,dz,1 }};
    return r;
}
static Mat4 mat_perspective_lh(float fovY, float aspect, float zn, float zf)
{
    float ys = 1.0f / tanf(fovY * 0.5f);
    float xs = ys / aspect;
    Mat4 r = {{ xs,0,0,0, 0,ys,0,0, 0,0,zf/(zf-zn),1, 0,0,-zn*zf/(zf-zn),0 }};
    return r;
}

// ---------- geometry ----------
typedef struct { float px, py, pz; float r, g, b, a; } Vertex;
typedef struct { float ox, oy, oz; float tr, tg, tb; } InstanceData;
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
static InstanceData g_instances[MAX_INSTANCES];

// ---------- D3D12 objects ----------
static IDXGIFactory2 *dxgi_factory;
static IDXGISwapChain1 *swap_chain;
static ID3D12Device *device;
static ID3D12CommandQueue *command_queue;
static ID3D12Fence *queue_fence;
static UINT64 fence_value;
static ID3D12CommandAllocator *command_allocator;
static ID3D12GraphicsCommandList *command_list;

static ID3D12DescriptorHeap *rtv_heap;
static ID3D12DescriptorHeap *dsv_heap;
static UINT rtv_increment;
static ID3D12Resource *rtv_buffers[2];
static D3D12_CPU_DESCRIPTOR_HANDLE rtv_handles[2];
static ID3D12Resource *depth_buffer;
static UINT rtv_index;

static ID3D12RootSignature *root_sig;
static ID3D12PipelineState *pso_instanced;
static ID3D12Resource *vbuffer;
static ID3D12Resource *ibuffer;
static ID3D12Resource *instance_buffer;
static D3D12_VERTEX_BUFFER_VIEW vbv;
static D3D12_VERTEX_BUFFER_VIEW instance_vbv;
static D3D12_INDEX_BUFFER_VIEW ibv;

static int g_width = WINDOW_WIDTH, g_height = WINDOW_HEIGHT;
static UINT64 g_frame_count = 0;
static int g_instance_count = 1024;

static const char *g_shader =
"cbuffer CB : register(b0) { row_major float4x4 g_mvp; };\n"
"struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
"VSOut VSMain(float3 pos : POSITION, float4 col : COLOR, float3 off : INSTOFF, float3 tint : INSTTINT) {\n"
"  VSOut o;\n"
"  float3 worldPos = pos * 0.18f + off;\n"
"  o.pos = mul(float4(worldPos, 1.0f), g_mvp);\n"
"  o.col = float4(col.rgb * tint, 1.0f);\n"
"  return o;\n"
"}\n"
"float4 PSMain(VSOut i) : SV_TARGET { return i.col; }\n";

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
    if (res && data) {
        ID3D12Resource_Map(res, 0, &none, &mapped);
        memcpy(mapped, data, size);
        ID3D12Resource_Unmap(res, 0, NULL);
    }
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

static void fill_instances(void)
{
    int x, y, z, n = 0;
    float spacing = 0.62f;
    for (z = 0; z < 16; z++)
        for (y = 0; y < 16; y++)
            for (x = 0; x < 16; x++) {
                InstanceData *it = &g_instances[n++];
                it->ox = (x - 7.5f) * spacing;
                it->oy = (y - 7.5f) * spacing;
                it->oz = (z - 7.5f) * spacing;
                it->tr = 0.45f + 0.55f * (float)x / 15.0f;
                it->tg = 0.45f + 0.55f * (float)y / 15.0f;
                it->tb = 0.45f + 0.55f * (float)z / 15.0f;
            }
}

static ID3D12PipelineState *create_instancing_pso(ID3DBlob *vs, ID3DBlob *ps)
{
    D3D12_INPUT_ELEMENT_DESC il[4];
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
    il[2].SemanticName = "INSTOFF"; il[2].SemanticIndex = 0;
    il[2].Format = DXGI_FORMAT_R32G32B32_FLOAT; il[2].InputSlot = 1;
    il[2].AlignedByteOffset = 0;
    il[2].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
    il[2].InstanceDataStepRate = 1;
    il[3].SemanticName = "INSTTINT"; il[3].SemanticIndex = 0;
    il[3].Format = DXGI_FORMAT_R32G32B32_FLOAT; il[3].InputSlot = 1;
    il[3].AlignedByteOffset = 12;
    il[3].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
    il[3].InstanceDataStepRate = 1;

    memset(&pd, 0, sizeof(pd));
    pd.pRootSignature = root_sig;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
    pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps);
    pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
    pd.InputLayout.pInputElementDescs = il;
    pd.InputLayout.NumElements = 4;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    for (i = 0; i < 8; i++) pd.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    ID3D12Device_CreateGraphicsPipelineState(device, &pd, &IID_ID3D12PipelineState, (void **)&pso);
    if (!pso) logf_line("instancing PSO creation failed");
    return pso;
}

static int create_sample_pipeline(void)
{
    D3D12_ROOT_PARAMETER rp;
    D3D12_ROOT_SIGNATURE_DESC rsd;
    ID3DBlob *sig = NULL, *sigerr = NULL, *vs = NULL, *ps = NULL, *err = NULL;
    HRESULT hr;

    memset(&rp, 0, sizeof(rp));
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.Num32BitValues = 16;
    rp.Constants.ShaderRegister = 0;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    memset(&rsd, 0, sizeof(rsd));
    rsd.NumParameters = 1; rsd.pParameters = &rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigerr);
    if (FAILED(hr)) { logf_line("rootsig hr=0x%08X", (unsigned)hr); log_blob("rootsig", sigerr); if (sigerr) ID3D10Blob_Release(sigerr); return 0; }
    ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(sig),
        ID3D10Blob_GetBufferSize(sig), &IID_ID3D12RootSignature, (void **)&root_sig);
    ID3D10Blob_Release(sig);

    hr = D3DCompile(g_shader, strlen(g_shader), NULL, NULL, NULL, "VSMain", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr)) { logf_line("VS hr=0x%08X", (unsigned)hr); log_blob("err", err); if (err) ID3D10Blob_Release(err); return 0; }
    hr = D3DCompile(g_shader, strlen(g_shader), NULL, NULL, NULL, "PSMain", "ps_5_0", 0, 0, &ps, &err);
    if (FAILED(hr)) { logf_line("PS hr=0x%08X", (unsigned)hr); log_blob("err", err); if (err) ID3D10Blob_Release(err); ID3D10Blob_Release(vs); return 0; }

    pso_instanced = create_instancing_pso(vs, ps);
    ID3D10Blob_Release(vs); ID3D10Blob_Release(ps);

    vbuffer = create_upload_buffer(CUBE_VERTS, sizeof(CUBE_VERTS));
    vbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vbuffer);
    vbv.StrideInBytes = sizeof(Vertex);
    vbv.SizeInBytes = sizeof(CUBE_VERTS);
    ibuffer = create_upload_buffer(CUBE_IDX, sizeof(CUBE_IDX));
    ibv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(ibuffer);
    ibv.Format = DXGI_FORMAT_R16_UINT;
    ibv.SizeInBytes = sizeof(CUBE_IDX);
    fill_instances();
    instance_buffer = create_upload_buffer(g_instances, sizeof(g_instances));
    instance_vbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(instance_buffer);
    instance_vbv.StrideInBytes = sizeof(InstanceData);
    instance_vbv.SizeInBytes = sizeof(g_instances);
    return (root_sig && pso_instanced && vbuffer && ibuffer && instance_buffer);
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

// ---------- teaching UI ----------
static void build_ui(struct nk_context *ctx, struct nk_colorf *bg,
                     int *pause, float *speed, int *vsync, int *instance_count)
{
    if (nk_begin(ctx, "D3D12 Instancing", nk_rect(20, 20, 400, 610),
        NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_SCALABLE|NK_WINDOW_TITLE))
    {
        char buf[128];
        if (nk_tree_push(ctx, NK_TREE_TAB, "Instancing", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label_wrap(ctx, "One DrawIndexedInstanced call renders many copies of the same cube.");
            nk_label_wrap(ctx, "Slot 0 advances per vertex; slot 1 advances once per instance.");
            nk_property_int(ctx, "Instance count", 1, instance_count, MAX_INSTANCES, 16, 1.0f);
            sprintf(buf, "Draw call: DrawIndexedInstanced(36, %d, ...)", *instance_count);
            nk_label(ctx, buf, NK_TEXT_LEFT);
            sprintf(buf, "Frame #: %llu", (unsigned long long)g_frame_count);
            nk_label(ctx, buf, NK_TEXT_LEFT);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "When to use it", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 16, 1);
            nk_label_wrap(ctx, "[LEARN] Instancing helps when CPU draw-call cost dominates: trees, asteroids, crowds, bolts, particles, or decals sharing one mesh.");
            nk_label_wrap(ctx, "[LEARN] It is not magic: extreme counts can become GPU-bound, and different meshes/materials usually need batching or ExecuteIndirect.");
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Controls", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_checkbox_label(ctx, "Pause rotation", pause);
            nk_checkbox_label(ctx, "VSync", vsync);
            nk_property_float(ctx, "Speed", 0.0f, speed, 4.0f, 0.1f, 0.02f);
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "Clear color:", NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 90, 1);
            *bg = nk_color_picker(ctx, *bg, NK_RGB);
            nk_tree_pop(ctx);
        }
    }
    nk_end(ctx);
}

int main(void)
{
    struct nk_context *ctx;
    struct nk_colorf bg = { 0.08f, 0.10f, 0.14f, 1.0f };
    int pause = 0, vsync = 1;
    float speed = 0.8f, angle = 0.0f;
    ULONGLONG last_tick;
    WNDCLASSW wc;
    RECT rect = { 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT };
    DWORD style = WS_OVERLAPPEDWINDOW;
    HWND wnd; int running = 1; HRESULT hr;
    D3D12_COMMAND_QUEUE_DESC qd; DXGI_SWAP_CHAIN_DESC1 scd; D3D12_DESCRIPTOR_HEAP_DESC hd;

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(0);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"D3D12InstancingClass";
    RegisterClassW(&wc);
    AdjustWindowRectEx(&rect, style, FALSE, WS_EX_APPWINDOW);
    wnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName,
        L"D3D12 Instancing - many cubes, one draw call",
        style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, wc.hInstance, NULL);

    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device);
    if (FAILED(hr)) { logf_line("D3D12CreateDevice hr=0x%08X", (unsigned)hr); MessageBoxW(wnd, L"D3D12 device creation failed", L"Error", 0); return 1; }
    logf_line("sample start");
    memset(&qd, 0, sizeof(qd));
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12Device_CreateCommandQueue(device, &qd, &IID_ID3D12CommandQueue, (void **)&command_queue);
    ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&queue_fence);
    ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        command_allocator, NULL, &IID_ID3D12GraphicsCommandList1, (void **)&command_list);

    memset(&hd, 0, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 2;
    ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap);
    rtv_increment = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    memset(&hd, 0, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 1;
    ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&dsv_heap);

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

    if (!create_sample_pipeline()) { logf_line("pipeline creation failed"); return 1; }

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
        MSG msg; ULONGLONG now; float dt;
        D3D12_RESOURCE_BARRIER barrier;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv;
        D3D12_VIEWPORT vp; D3D12_RECT scissor;
        D3D12_VERTEX_BUFFER_VIEW views[2];
        Mat4 world, view, proj, mvp;
        float clear[4];

        nk_input_begin(ctx);
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        nk_input_end(ctx);

        build_ui(ctx, &bg, &pause, &speed, &vsync, &g_instance_count);

        now = GetTickCount64();
        dt = (now - last_tick) / 1000.0f;
        last_tick = now;
        if (!pause) angle += dt * speed;

        memset(&barrier, 0, sizeof(barrier));
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = rtv_buffers[rtv_index];
        barrier.Transition.Subresource = 0;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barrier);

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

        world = mat_mul(mat_rot_y(angle * 0.45f), mat_rot_x(angle * 0.25f));
        view = mat_lookat_lh(0.0f, 0.0f, -15.0f);
        proj = mat_perspective_lh(3.14159265f / 4.0f, (float)g_width / (float)g_height, 0.1f, 100.0f);
        mvp = mat_mul(mat_mul(world, view), proj);

        ID3D12GraphicsCommandList_SetPipelineState(command_list, pso_instanced);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, root_sig);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 16, mvp.m, 0);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        views[0] = vbv;
        views[1] = instance_vbv;
        // [LEARN] Two input slots: cube vertices repeat every vertex, instance data advances once per copy.
        ID3D12GraphicsCommandList_IASetVertexBuffers(command_list, 0, 2, views);
        ID3D12GraphicsCommandList_IASetIndexBuffer(command_list, &ibv);
        // [LEARN] The second argument is the copy count. The CPU submits one draw, not thousands.
        ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, 36, (UINT)g_instance_count, 0, 0, 0);

        nk_d3d12_render(command_list, NK_ANTI_ALIASING_ON);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barrier);
        execute_commands();

        hr = IDXGISwapChain1_Present(swap_chain, vsync ? 1 : 0, 0);
        rtv_index = (rtv_index + 1) % 2;
        g_frame_count++;
        if (hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_REMOVED) {
            logf_line("device lost on Present");
            break;
        } else if (hr == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
        }
    }

    nk_d3d12_shutdown();
    signal_and_wait();
    signal_and_wait();
    ID3D12PipelineState_Release(pso_instanced);
    ID3D12RootSignature_Release(root_sig);
    ID3D12Resource_Release(vbuffer);
    ID3D12Resource_Release(ibuffer);
    ID3D12Resource_Release(instance_buffer);
    ID3D12Resource_Release(depth_buffer);
    ID3D12Resource_Release(rtv_buffers[0]);
    ID3D12Resource_Release(rtv_buffers[1]);
    ID3D12DescriptorHeap_Release(rtv_heap);
    ID3D12DescriptorHeap_Release(dsv_heap);
    IDXGISwapChain1_Release(swap_chain);
    IDXGIFactory2_Release(dxgi_factory);
    ID3D12GraphicsCommandList_Release(command_list);
    ID3D12CommandAllocator_Release(command_allocator);
    ID3D12CommandQueue_Release(command_queue);
    ID3D12Fence_Release(queue_fence);
    ID3D12Device_Release(device);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
