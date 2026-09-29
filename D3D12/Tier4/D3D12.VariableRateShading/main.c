// =====================================================================
// D3D12.VariableRateShading - shade fewer pixels with VRS (Tier 1).
//
// Variable Rate Shading (VRS) lets you run the PIXEL SHADER once per BLOCK of
// pixels instead of once per pixel. A 2x2 rate runs the PS once per 2x2 block
// (a 4x saving); rasterization/coverage/depth stay full resolution, so edges
// remain crisp - only shading gets coarser. It is a cheap way to save GPU time
// where fine shading detail does not matter (periphery, motion blur, fog).
//
// This sample draws a rotating cube with a deliberately FINE procedural pattern
// so the effect is obvious: at 1x1 the pattern is sharp; at 4x4 it becomes
// visibly blocky because one PS invocation now colors a 4x4 pixel region.
//
// [LEARN] Key pieces:
//   1. Query D3D12_FEATURE_D3D12_OPTIONS6.VariableShadingRateTier. Tier 1 gives
//      per-draw RSSetShadingRate; Tier 2 adds screenspace image + per-primitive.
//   2. Get an ID3D12GraphicsCommandList5 (QueryInterface) - RSSetShadingRate
//      lives there.
//   3. Call RSSetShadingRate(rate, NULL) before the draw. Reset to 1X1 before
//      drawing UI so text stays sharp.
//
// [LEARN] VRS is capability-gated: if the GPU/driver reports Tier 0, the sample
// still runs but the rate buttons are inert (logged, shown in the UI).
//
// Built as C (Nuklear D3D12 backend uses C-style COM). SDK >= 10.0.22000.0.
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

// [DEBUG] Append diagnostics to a log file instead of popping dialogs.
static void logf_line(const char *fmt, ...)
{
    va_list ap; FILE *f = fopen("CubeD3D12.VariableRateShading.log", "a");
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
typedef struct { float m[16]; } Mat4;

static Mat4 mat_mul(Mat4 a, Mat4 b)
{
    Mat4 r; int i, j, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++) s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}
static Mat4 mat_rot_x(float a) { float c=cosf(a),s=sinf(a); Mat4 r={{1,0,0,0, 0,c,s,0, 0,-s,c,0, 0,0,0,1}}; return r; }
static Mat4 mat_rot_y(float a) { float c=cosf(a),s=sinf(a); Mat4 r={{c,0,-s,0, 0,1,0,0, s,0,c,0, 0,0,0,1}}; return r; }
static Mat4 mat_lookat_lh(float ex, float ey, float ez)
{
    float zx=-ex,zy=-ey,zz=-ez;
    float zl=sqrtf(zx*zx+zy*zy+zz*zz); zx/=zl; zy/=zl; zz/=zl;
    { float xx=1*zz-0*zy, xy=0*zx-0*zz, xz=0*zy-1*zx;
      float xl=sqrtf(xx*xx+xy*xy+xz*xz); xx/=xl; xy/=xl; xz/=xl;
      { float yx=zy*xz-zz*xy, yy=zz*xx-zx*xz, yz=zx*xy-zy*xx;
        float dx=-(xx*ex+xy*ey+xz*ez), dy=-(yx*ex+yy*ey+yz*ez), dz=-(zx*ex+zy*ey+zz*ez);
        Mat4 r={{ xx,yx,zx,0, xy,yy,zy,0, xz,yz,zz,0, dx,dy,dz,1 }}; return r; } }
}
static Mat4 mat_perspective_lh(float fovY, float aspect, float zn, float zf)
{
    float ys=1.0f/tanf(fovY*0.5f), xs=ys/aspect;
    Mat4 r={{ xs,0,0,0, 0,ys,0,0, 0,0,zf/(zf-zn),1, 0,0,-zn*zf/(zf-zn),0 }}; return r;
}

typedef struct { float px,py,pz; float r,g,b,a; } Vertex;
static const Vertex CUBE_VERTS[8] = {
    { -1,-1,-1, 0.90f,0.35f,0.35f,1 }, { -1, 1,-1, 0.90f,0.70f,0.30f,1 },
    {  1, 1,-1, 0.85f,0.85f,0.35f,1 }, {  1,-1,-1, 0.40f,0.80f,0.45f,1 },
    { -1,-1, 1, 0.35f,0.75f,0.85f,1 }, { -1, 1, 1, 0.40f,0.55f,0.90f,1 },
    {  1, 1, 1, 0.65f,0.45f,0.90f,1 }, {  1,-1, 1, 0.92f,0.92f,0.92f,1 },
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
static ID3D12GraphicsCommandList5 *command_list5;   // VRS lives here

static ID3D12DescriptorHeap *rtv_heap;
static ID3D12DescriptorHeap *dsv_heap;
static UINT rtv_increment;
static ID3D12Resource *rtv_buffers[2];
static D3D12_CPU_DESCRIPTOR_HANDLE rtv_handles[2];
static ID3D12Resource *depth_buffer;
static UINT rtv_index;

static ID3D12RootSignature *root_sig;
static ID3D12PipelineState *pso;
static ID3D12Resource *vbuffer;
static ID3D12Resource *ibuffer;
static D3D12_VERTEX_BUFFER_VIEW vbv;
static D3D12_INDEX_BUFFER_VIEW ibv;

static int g_width = WINDOW_WIDTH, g_height = WINDOW_HEIGHT;
static UINT g_vrs_tier = 0;

// The shading-rate options we expose.
static const D3D12_SHADING_RATE RATES[6] = {
    D3D12_SHADING_RATE_1X1, D3D12_SHADING_RATE_1X2, D3D12_SHADING_RATE_2X1,
    D3D12_SHADING_RATE_2X2, D3D12_SHADING_RATE_2X4, D3D12_SHADING_RATE_4X4,
};
static const char *RATE_NAMES[6] = { "1x1", "1x2", "2x1", "2x2", "2x4", "4x4" };
static int g_rate_index = 3;   // default 2x2

static const char *g_shader =
"cbuffer CB : register(b0) { row_major float4x4 g_mvp; }\n"
"struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
"VSOut VSMain(float3 pos : POSITION, float4 col : COLOR) {\n"
"  VSOut o; o.pos = mul(float4(pos, 1.0), g_mvp); o.col = col; return o;\n"
"}\n"
"float4 PSMain(VSOut i) : SV_TARGET {\n"
"  // [LEARN] a FINE screen-space checker. VRS runs this once per shading block,\n"
"  // so coarse rates turn the sharp checker into visible blocks.\n"
"  float2 s = floor(i.pos.xy / 3.0);\n"
"  float checker = fmod(s.x + s.y, 2.0);\n"
"  float3 tint = lerp(i.col.rgb * 0.45, i.col.rgb, checker);\n"
"  return float4(tint, 1);\n"
"}\n";

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
    D3D12_HEAP_PROPERTIES hp; D3D12_RESOURCE_DESC rd;
    ID3D12Resource *res = NULL; void *mapped = NULL; D3D12_RANGE none = { 0, 0 };
    memset(&hp, 0, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    memset(&rd, 0, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Device_CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&res);
    if (data) {
        ID3D12Resource_Map(res, 0, &none, &mapped);
        memcpy(mapped, data, size);
        ID3D12Resource_Unmap(res, 0, NULL);
    }
    return res;
}

// [LEARN] Query the Variable Shading Rate tier the device supports.
static void query_vrs_support(void)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6;
    memset(&o6, 0, sizeof(o6));
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(device,
        D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6))))
        g_vrs_tier = (UINT)o6.VariableShadingRateTier;
    else
        g_vrs_tier = 0;
    logf_line("VariableShadingRateTier=%u", g_vrs_tier);
}

static void create_depth_buffer(int width, int height)
{
    D3D12_HEAP_PROPERTIES hp; D3D12_RESOURCE_DESC rd; D3D12_CLEAR_VALUE cv;
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

static int create_cube_pipeline(void)
{
    D3D12_ROOT_PARAMETER rp;
    D3D12_ROOT_SIGNATURE_DESC rsd;
    D3D12_INPUT_ELEMENT_DESC il[2];
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    ID3DBlob *sig = NULL, *sigerr = NULL, *vs = NULL, *ps = NULL, *err = NULL;
    UINT i; HRESULT hr;

    memset(&rp, 0, sizeof(rp));
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.ShaderRegister = 0;
    rp.Constants.Num32BitValues = 16;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    memset(&rsd, 0, sizeof(rsd));
    rsd.NumParameters = 1; rsd.pParameters = &rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigerr);
    if (FAILED(hr)) { logf_line("rootsig hr=0x%08X", (unsigned)hr); log_blob("err", sigerr); if (sigerr) ID3D10Blob_Release(sigerr); return 0; }
    ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(sig),
        ID3D10Blob_GetBufferSize(sig), &IID_ID3D12RootSignature, (void **)&root_sig);
    ID3D10Blob_Release(sig);

    hr = D3DCompile(g_shader, strlen(g_shader), NULL, NULL, NULL, "VSMain", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr)) { logf_line("VS hr=0x%08X", (unsigned)hr); log_blob("err", err); if (err) ID3D10Blob_Release(err); return 0; }
    hr = D3DCompile(g_shader, strlen(g_shader), NULL, NULL, NULL, "PSMain", "ps_5_0", 0, 0, &ps, &err);
    if (FAILED(hr)) { logf_line("PS hr=0x%08X", (unsigned)hr); log_blob("err", err); if (err) ID3D10Blob_Release(err); return 0; }

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
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    for (i = 0; i < 8; i++)
        pd.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    ID3D12Device_CreateGraphicsPipelineState(device, &pd, &IID_ID3D12PipelineState, (void **)&pso);
    if (!pso) logf_line("PSO NULL");
    ID3D10Blob_Release(vs); ID3D10Blob_Release(ps);

    vbuffer = create_upload_buffer(CUBE_VERTS, sizeof(CUBE_VERTS));
    vbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vbuffer);
    vbv.StrideInBytes = sizeof(Vertex);
    vbv.SizeInBytes = sizeof(CUBE_VERTS);
    ibuffer = create_upload_buffer(CUBE_IDX, sizeof(CUBE_IDX));
    ibv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(ibuffer);
    ibv.Format = DXGI_FORMAT_R16_UINT;
    ibv.SizeInBytes = sizeof(CUBE_IDX);
    return (root_sig && pso && vbuffer && ibuffer);
}

static void set_swap_chain_size(int width, int height)
{
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
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_SIZE:
        if (swap_chain) {
            int width = LOWORD(lparam), height = HIWORD(lparam);
            if (width > 0 && height > 0) { set_swap_chain_size(width, height); nk_d3d12_resize(width, height); }
        }
        break;
    }
    if (nk_d3d12_handle_event(wnd, msg, wparam, lparam)) return 0;
    return DefWindowProcW(wnd, msg, wparam, lparam);
}

static void build_ui(struct nk_context *ctx, int *pause, float *speed)
{
    if (nk_begin(ctx, "D3D12 Variable Rate Shading", nk_rect(20, 20, 400, 560),
        NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_SCALABLE|NK_WINDOW_TITLE))
    {
        char buf[80];
        if (nk_tree_push(ctx, NK_TREE_TAB, "Support", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 16, 1);
            sprintf(buf, "VariableShadingRateTier: %u", g_vrs_tier);
            nk_label(ctx, buf, NK_TEXT_LEFT);
            if (g_vrs_tier == 0)
                nk_label(ctx, "VRS unsupported here - rates are inert.", NK_TEXT_LEFT);
            else
                nk_label(ctx, "Tier 1: per-draw RSSetShadingRate available.", NK_TEXT_LEFT);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Shading rate", NK_MAXIMIZED)) {
            int i;
            nk_layout_row_dynamic(ctx, 16, 1);
            nk_label(ctx, "Coarser rate = fewer PS runs = blockier pattern:", NK_TEXT_LEFT);
            nk_layout_row_static(ctx, 28, 60, 6);
            for (i = 0; i < 6; i++) {
                if (i == g_rate_index) nk_label(ctx, RATE_NAMES[i], NK_TEXT_CENTERED);
                else if (nk_button_label(ctx, RATE_NAMES[i])) g_rate_index = i;
            }
            nk_layout_row_dynamic(ctx, 16, 1);
            sprintf(buf, "Active rate: %s", RATE_NAMES[g_rate_index]);
            nk_label(ctx, buf, NK_TEXT_LEFT);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Controls", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_checkbox_label(ctx, "Pause rotation", pause);
            nk_property_float(ctx, "Speed", 0.0f, speed, 4.0f, 0.1f, 0.02f);
            nk_tree_pop(ctx);
        }
    }
    nk_end(ctx);
}

int main(void)
{
    struct nk_context *ctx;
    int pause = 0;
    float speed = 0.7f, angle = 0.0f;
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
    wc.lpszClassName = L"D3D12VRSClass";
    RegisterClassW(&wc);
    AdjustWindowRectEx(&rect, style, FALSE, WS_EX_APPWINDOW);
    wnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName,
        L"D3D12 Variable Rate Shading - shade blocks, not pixels",
        style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, wc.hInstance, NULL);

    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device);
    if (FAILED(hr)) { logf_line("D3D12CreateDevice hr=0x%08X", (unsigned)hr); return 1; }
    query_vrs_support();
    memset(&qd, 0, sizeof(qd));
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12Device_CreateCommandQueue(device, &qd, &IID_ID3D12CommandQueue, (void **)&command_queue);
    ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&queue_fence);
    ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void **)&command_allocator);
    ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        command_allocator, NULL, &IID_ID3D12GraphicsCommandList1, (void **)&command_list);
    // [LEARN] RSSetShadingRate needs the newer ID3D12GraphicsCommandList5.
    if (FAILED(ID3D12GraphicsCommandList_QueryInterface(command_list,
        &IID_ID3D12GraphicsCommandList5, (void **)&command_list5))) {
        command_list5 = NULL;
        logf_line("ID3D12GraphicsCommandList5 unavailable");
    }

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

    if (!create_cube_pipeline()) { logf_line("pipeline creation failed"); return 1; }

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
        Mat4 world, view, proj, mvp;
        float clear[4] = { 0.07f, 0.09f, 0.12f, 1.0f };

        nk_input_begin(ctx);
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        nk_input_end(ctx);

        build_ui(ctx, &pause, &speed);

        now = GetTickCount64();
        dt = (now - last_tick) / 1000.0f;
        last_tick = now;
        if (!pause) angle += dt * speed;

        world = mat_mul(mat_rot_y(angle), mat_rot_x(angle * 0.5f));
        view = mat_lookat_lh(0.0f, 0.0f, -4.5f);
        proj = mat_perspective_lh(3.14159265f / 4.0f, (float)g_width / (float)g_height, 0.1f, 100.0f);
        mvp = mat_mul(mat_mul(world, view), proj);

        memset(&barrier, 0, sizeof(barrier));
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = rtv_buffers[rtv_index];
        barrier.Transition.Subresource = 0;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barrier);

        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap, &dsv);
        ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &rtv_handles[rtv_index], FALSE, &dsv);
        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, rtv_handles[rtv_index], clear, 0, NULL);
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);

        vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (float)g_width; vp.Height = (float)g_height;
        vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
        scissor.left = 0; scissor.top = 0; scissor.right = g_width; scissor.bottom = g_height;
        ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &scissor);

        // [LEARN] Apply the chosen shading rate to the cube draw (Tier 1+).
        if (command_list5 && g_vrs_tier >= 1)
            ID3D12GraphicsCommandList5_RSSetShadingRate(command_list5, RATES[g_rate_index], NULL);

        ID3D12GraphicsCommandList_SetPipelineState(command_list, pso);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, root_sig);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 16, mvp.m, 0);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_IASetVertexBuffers(command_list, 0, 1, &vbv);
        ID3D12GraphicsCommandList_IASetIndexBuffer(command_list, &ibv);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, 36, 1, 0, 0, 0);

        // [LEARN] Reset to full rate so the UI text stays sharp.
        if (command_list5 && g_vrs_tier >= 1)
            ID3D12GraphicsCommandList5_RSSetShadingRate(command_list5, D3D12_SHADING_RATE_1X1, NULL);

        nk_d3d12_render(command_list, NK_ANTI_ALIASING_ON);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barrier);
        execute_commands();

        hr = IDXGISwapChain1_Present(swap_chain, 1, 0);
        rtv_index = (rtv_index + 1) % 2;
        if (hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_REMOVED) {
            logf_line("device lost on Present"); break;
        } else if (hr == DXGI_STATUS_OCCLUDED) { Sleep(10); }
    }

    nk_d3d12_shutdown();
    signal_and_wait();
    if (command_list5) ID3D12GraphicsCommandList5_Release(command_list5);
    ID3D12PipelineState_Release(pso);
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
    ID3D12GraphicsCommandList_Release(command_list);
    ID3D12CommandAllocator_Release(command_allocator);
    ID3D12CommandQueue_Release(command_queue);
    ID3D12Fence_Release(queue_fence);
    ID3D12Device_Release(device);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
