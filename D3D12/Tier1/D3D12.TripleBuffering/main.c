// =====================================================================
// D3D12.TripleBuffering - frames in flight, per-frame resources, and how
// deep the CPU is allowed to run ahead of the GPU.
//
// D3D12.Fundamentals waits for the GPU after EVERY frame (signal_and_wait),
// so the CPU and GPU never work at the same time. That is the simplest model
// but it wastes both processors. This sample instead pipelines frames:
//
//   - The swap chain has 3 back buffers (classic "triple buffering") so
//     Present rarely has to block waiting for a free buffer.
//   - Every per-frame GPU resource is duplicated FRAME_COUNT times: one
//     command allocator and one constant-buffer slot PER in-flight frame.
//     The CPU writes frame N's data into slot N while the GPU is still
//     reading slot N-1 - they never touch the same memory at once.
//   - A single monotonically increasing fence tracks completion. Each frame
//     records the fence value it was submitted with; before REUSING a slot
//     the CPU waits only if that slot's previous frame has not finished yet.
//
// [LEARN] The core idea: DYNAMIC per-frame data must be N-buffered. If you
// reused one constant buffer and didn't wait, you would overwrite data the
// GPU is still reading. The "Frames CPU-ahead" slider lets you dial how far
// the CPU may lead the GPU and watch the stall counter react.
//
// Caveat worth knowing: the Nuklear overlay's D3D12 backend single-buffers
// its OWN vertex/index upload buffer, so it is only safe with 1 frame of
// CPU-ahead latency. That is exactly why we default the slider to 1 - and it
// is a perfect illustration of why real engines N-buffer every dynamic
// resource. Push it to 2 to see the overlay start to shimmer.
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

// [LEARN] FRAME_COUNT = number of frames that may be "in flight" at once. We
// duplicate the back buffers, command allocators, and constant-buffer slots
// this many times so the CPU and GPU never fight over the same memory.
#define FRAME_COUNT 3
#define CB_SLOT_SIZE 256   // constant buffers must be 256-byte aligned

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
static IDXGISwapChain3 *swap_chain;
static ID3D12Device *device;
static ID3D12CommandQueue *command_queue;
static ID3D12Fence *queue_fence;
static UINT64 fence_value;                        // monotonic, ++ per submit
static HANDLE fence_event;
static ID3D12CommandAllocator *command_allocator[FRAME_COUNT];
static UINT64 frame_fence_value[FRAME_COUNT];     // fence value each slot was submitted with
static ID3D12GraphicsCommandList *command_list;

static ID3D12DescriptorHeap *rtv_heap;
static ID3D12DescriptorHeap *dsv_heap;
static UINT rtv_increment;
static ID3D12Resource *rtv_buffers[FRAME_COUNT];
static D3D12_CPU_DESCRIPTOR_HANDLE rtv_handles[FRAME_COUNT];
static ID3D12Resource *depth_buffer;

static ID3D12RootSignature *root_sig;
static ID3D12PipelineState *pso;
static ID3D12Resource *vbuffer;
static ID3D12Resource *ibuffer;
static D3D12_VERTEX_BUFFER_VIEW vbv;
static D3D12_INDEX_BUFFER_VIEW ibv;

// [LEARN] One upload buffer holding FRAME_COUNT constant-buffer slots. Kept
// mapped for the whole run (persistent mapping is fine for UPLOAD heaps). Each
// frame writes its MVP into a different slot, addressed via a root CBV.
static ID3D12Resource *cbuffer;
static unsigned char *cbuffer_cpu;                // persistently mapped
static D3D12_GPU_VIRTUAL_ADDRESS cbuffer_gpu;

static int g_width = WINDOW_WIDTH, g_height = WINDOW_HEIGHT;
static UINT64 g_frame_count = 0;
static UINT g_stalls = 0;                          // times the CPU had to wait for a slot
static UINT g_last_wait_slot = 0;

static const char *g_shader_src =
"cbuffer CB : register(b0) { row_major float4x4 g_mvp; };\n"
"struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
"VSOut VSMain(float3 pos : POSITION, float4 col : COLOR) {\n"
"  VSOut o; o.pos = mul(float4(pos,1.0f), g_mvp); o.col = col; return o; }\n"
"float4 PSMain(VSOut i) : SV_TARGET { return i.col; }\n";

// [LEARN] Block the CPU until the GPU has reached a given fence value. Using an
// event (instead of spinning) lets the CPU sleep. Returns 1 if it actually had
// to wait (the GPU was behind), 0 if the value was already reached.
static int wait_for_fence(UINT64 value)
{
    if (value == 0) return 0;
    if (ID3D12Fence_GetCompletedValue(queue_fence) >= value) return 0;
    ID3D12Fence_SetEventOnCompletion(queue_fence, value, fence_event);
    WaitForSingleObject(fence_event, INFINITE);
    return 1;
}

static void wait_gpu_idle(void)
{
    UINT64 v = ++fence_value;
    ID3D12CommandQueue_Signal(command_queue, queue_fence, v);
    wait_for_fence(v);
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
    D3D12_CPU_DESCRIPTOR_HANDLE h; UINT i;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap, &h);
    for (i = 0; i < FRAME_COUNT; i++) {
        IDXGISwapChain3_GetBuffer(swap_chain, i, &IID_ID3D12Resource, (void **)&rtv_buffers[i]);
        ID3D12Device_CreateRenderTargetView(device, rtv_buffers[i], NULL, h);
        rtv_handles[i] = h;
        h.ptr += rtv_increment;
    }
}

static int create_cube_pipeline(void)
{
    D3D12_ROOT_PARAMETER rp; D3D12_ROOT_SIGNATURE_DESC rsd;
    D3D12_INPUT_ELEMENT_DESC il[2];
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    ID3DBlob *sig = NULL, *sigerr = NULL, *vs = NULL, *ps = NULL, *err = NULL;
    UINT i;

    // [LEARN] Root signature uses a ROOT CBV (a root descriptor) at b0. Each
    // frame we point it at a different 256-byte slot of cbuffer, so the shader
    // reads that frame's MVP. Contrast with Fundamentals' 32-bit root constants.
    memset(&rp, 0, sizeof(rp));
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp.Descriptor.ShaderRegister = 0;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    memset(&rsd, 0, sizeof(rsd));
    rsd.NumParameters = 1; rsd.pParameters = &rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigerr))) {
        if (sigerr) ID3D10Blob_Release(sigerr); return 0;
    }
    ID3D12Device_CreateRootSignature(device, 0,
        ID3D10Blob_GetBufferPointer(sig), ID3D10Blob_GetBufferSize(sig),
        &IID_ID3D12RootSignature, (void **)&root_sig);
    ID3D10Blob_Release(sig);

    if (FAILED(D3DCompile(g_shader_src, strlen(g_shader_src), NULL, NULL, NULL,
        "VSMain", "vs_5_0", 0, 0, &vs, &err))) { if (err) ID3D10Blob_Release(err); return 0; }
    if (FAILED(D3DCompile(g_shader_src, strlen(g_shader_src), NULL, NULL, NULL,
        "PSMain", "ps_5_0", 0, 0, &ps, &err))) { if (err) ID3D10Blob_Release(err); return 0; }

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
    ID3D10Blob_Release(vs); ID3D10Blob_Release(ps);

    vbuffer = create_upload_buffer(CUBE_VERTS, sizeof(CUBE_VERTS));
    vbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vbuffer);
    vbv.StrideInBytes = sizeof(Vertex);
    vbv.SizeInBytes = sizeof(CUBE_VERTS);
    ibuffer = create_upload_buffer(CUBE_IDX, sizeof(CUBE_IDX));
    ibv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(ibuffer);
    ibv.Format = DXGI_FORMAT_R16_UINT;
    ibv.SizeInBytes = sizeof(CUBE_IDX);

    // The N-buffered constant buffer (FRAME_COUNT slots), mapped for the run.
    {
        D3D12_RANGE none = { 0, 0 };
        cbuffer = create_upload_buffer(NULL, FRAME_COUNT * CB_SLOT_SIZE);
        cbuffer_gpu = ID3D12Resource_GetGPUVirtualAddress(cbuffer);
        ID3D12Resource_Map(cbuffer, 0, &none, (void **)&cbuffer_cpu);
    }
    return (root_sig && pso && vbuffer && ibuffer && cbuffer);
}

static void set_swap_chain_size(int width, int height)
{
    UINT i;
    wait_gpu_idle();
    for (i = 0; i < FRAME_COUNT; i++) ID3D12Resource_Release(rtv_buffers[i]);
    ID3D12Resource_Release(depth_buffer);
    IDXGISwapChain3_ResizeBuffers(swap_chain, FRAME_COUNT, (UINT)width, (UINT)height,
        DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
    get_swap_chain_buffers();
    create_depth_buffer(width, height);
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

// ---------- teaching UI ----------
static void build_ui(struct nk_context *ctx, struct nk_colorf *bg,
                     int *pause, float *speed, int *vsync, int *frames_ahead)
{
    if (nk_begin(ctx, "D3D12 Triple Buffering", nk_rect(20, 20, 360, 660),
        NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_SCALABLE|NK_WINDOW_TITLE))
    {
        char buf[160]; UINT i;
        if (nk_tree_push(ctx, NK_TREE_TAB, "Live pipeline state", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 18, 1);
            sprintf(buf, "Frame #: %llu", (unsigned long long)g_frame_count);
            nk_label(ctx, buf, NK_TEXT_LEFT);
            sprintf(buf, "Back buffer index: %u", IDXGISwapChain3_GetCurrentBackBufferIndex(swap_chain));
            nk_label(ctx, buf, NK_TEXT_LEFT);
            sprintf(buf, "Fence submitted (CPU): %llu", (unsigned long long)fence_value);
            nk_label(ctx, buf, NK_TEXT_LEFT);
            sprintf(buf, "Fence completed (GPU): %llu",
                (unsigned long long)ID3D12Fence_GetCompletedValue(queue_fence));
            nk_label(ctx, buf, NK_TEXT_LEFT);
            sprintf(buf, "CPU-ahead of GPU: %llu frames",
                (unsigned long long)(fence_value - ID3D12Fence_GetCompletedValue(queue_fence)));
            nk_label(ctx, buf, NK_TEXT_LEFT);
            sprintf(buf, "Slot waits (stalls): %u", g_stalls);
            nk_label(ctx, buf, NK_TEXT_LEFT);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Per-frame slots", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 16, 1);
            for (i = 0; i < FRAME_COUNT; i++) {
                sprintf(buf, "slot %u: allocator + CB region, last fence %llu%s",
                    i, (unsigned long long)frame_fence_value[i],
                    (i == g_last_wait_slot) ? "  <- waited" : "");
                nk_label(ctx, buf, NK_TEXT_LEFT);
            }
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Controls", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_checkbox_label(ctx, "Pause rotation", pause);
            nk_checkbox_label(ctx, "VSync", vsync);
            nk_property_float(ctx, "Speed", 0.0f, speed, 4.0f, 0.1f, 0.02f);
            // [LEARN] How many frames the CPU may run ahead. 1 = safe for the
            // single-buffered Nuklear overlay. 2 lets the cube pipeline deeper
            // but the overlay's dynamic buffer may be overwritten mid-read.
            nk_property_int(ctx, "Frames CPU-ahead", 1, frames_ahead, FRAME_COUNT - 1, 1, 0.2f);
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "Clear color:", NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 90, 1);
            *bg = nk_color_picker(ctx, *bg, NK_RGB);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "What am I looking at?", NK_MINIMIZED)) {
            nk_layout_row_dynamic(ctx, 15, 1);
            nk_label_wrap(ctx, "3 swap-chain back buffers + 3 command allocators + a 3-slot");
            nk_label_wrap(ctx, "constant buffer. The CPU writes frame N into slot N while the");
            nk_label_wrap(ctx, "GPU still reads slot N-1, so they never collide.");
            nk_label_wrap(ctx, " ");
            nk_label_wrap(ctx, "'CPU-ahead' shows how far ahead the CPU is running. A stall");
            nk_label_wrap(ctx, "happens only when the CPU wants a slot the GPU hasn't freed.");
            nk_tree_pop(ctx);
        }
    }
    nk_end(ctx);
}

int main(void)
{
    struct nk_context *ctx;
    struct nk_colorf bg = { 0.10f, 0.18f, 0.24f, 1.0f };
    int pause = 0, vsync = 1, frames_ahead = 1;
    float speed = 0.8f, angle = 0.0f;
    ULONGLONG last_tick;
    WNDCLASSW wc;
    RECT rect = { 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT };
    DWORD style = WS_OVERLAPPEDWINDOW;
    HWND wnd; int running = 1; HRESULT hr; UINT i;
    D3D12_COMMAND_QUEUE_DESC qd; DXGI_SWAP_CHAIN_DESC1 scd; D3D12_DESCRIPTOR_HEAP_DESC hd;

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(0);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"D3D12TripleBufferingClass";
    RegisterClassW(&wc);
    AdjustWindowRectEx(&rect, style, FALSE, WS_EX_APPWINDOW);
    wnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName,
        L"D3D12 Triple Buffering - frames in flight",
        style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, wc.hInstance, NULL);

    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device);
    if (FAILED(hr)) { MessageBoxW(wnd, L"D3D12 device creation failed", L"Error", 0); return 1; }
    memset(&qd, 0, sizeof(qd));
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12Device_CreateCommandQueue(device, &qd, &IID_ID3D12CommandQueue, (void **)&command_queue);
    ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&queue_fence);
    fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    for (i = 0; i < FRAME_COUNT; i++)
        ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&command_allocator[i]);
    ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        command_allocator[0], NULL, &IID_ID3D12GraphicsCommandList1, (void **)&command_list);
    ID3D12GraphicsCommandList_Close(command_list);

    memset(&hd, 0, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = FRAME_COUNT;
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
    scd.BufferCount = FRAME_COUNT;                 // [LEARN] 3 = triple buffering
    scd.Scaling = DXGI_SCALING_STRETCH;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    {
        IDXGISwapChain1 *sc1 = NULL;
        IDXGIFactory2_CreateSwapChainForHwnd(dxgi_factory, (IUnknown *)command_queue, wnd,
            &scd, NULL, NULL, &sc1);
        IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&swap_chain);
        IDXGISwapChain1_Release(sc1);
    }
    get_swap_chain_buffers();
    create_depth_buffer(WINDOW_WIDTH, WINDOW_HEIGHT);

    if (!create_cube_pipeline()) { MessageBoxW(wnd, L"Cube pipeline creation failed", L"Error", 0); return 1; }

    // Nuklear GUI (font upload uses slot 0's allocator + the command list once).
    ctx = nk_d3d12_init(device, WINDOW_WIDTH, WINDOW_HEIGHT, MAX_VERTEX_BUFFER, MAX_INDEX_BUFFER, 1);
    ID3D12CommandAllocator_Reset(command_allocator[0]);
    ID3D12GraphicsCommandList_Reset(command_list, command_allocator[0], NULL);
    {
        struct nk_font_atlas *atlas;
        nk_d3d12_font_stash_begin(&atlas);
        nk_d3d12_font_stash_end(command_list);
    }
    ID3D12GraphicsCommandList_Close(command_list);
    { ID3D12CommandList *lists[1]; lists[0] = (ID3D12CommandList *)command_list;
      ID3D12CommandQueue_ExecuteCommandLists(command_queue, 1, lists); }
    wait_gpu_idle();
    nk_d3d12_font_stash_cleanup();

    last_tick = GetTickCount64();
    while (running) {
        MSG msg; ULONGLONG now; float dt;
        UINT slot, back;
        D3D12_RESOURCE_BARRIER barrier;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv;
        D3D12_VIEWPORT vp; D3D12_RECT scissor;
        Mat4 world, view, proj, mvp;
        float clear[4];
        UINT64 wait_target;

        nk_input_begin(ctx);
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        nk_input_end(ctx);

        build_ui(ctx, &bg, &pause, &speed, &vsync, &frames_ahead);

        now = GetTickCount64();
        dt = (now - last_tick) / 1000.0f;
        last_tick = now;
        if (!pause) angle += dt * speed;

        world = mat_mul(mat_rot_y(angle), mat_rot_x(angle * 0.5f));
        view = mat_lookat_lh(0.0f, 0.0f, -6.0f);
        proj = mat_perspective_lh(3.14159265f / 4.0f, (float)g_width / (float)g_height, 0.1f, 100.0f);
        mvp = mat_mul(mat_mul(world, view), proj);

        slot = (UINT)(g_frame_count % FRAME_COUNT);

        // [LEARN] Throttle the CPU so it stays at most 'frames_ahead' frames in
        // front of the GPU. We wait for the fence value submitted that many
        // frames ago; if the GPU already passed it there is no stall. This is
        // the heart of frame pipelining: wait as LATE and as LITTLE as possible.
        wait_target = (fence_value >= (UINT64)frames_ahead) ? (fence_value - (UINT64)frames_ahead + 1) : 0;
        g_last_wait_slot = slot;
        if (wait_for_fence(wait_target)) g_stalls++;

        // The slot's allocator is now guaranteed free -> reset and record.
        ID3D12CommandAllocator_Reset(command_allocator[slot]);
        ID3D12GraphicsCommandList_Reset(command_list, command_allocator[slot], NULL);

        // Write this frame's MVP into this slot's constant-buffer region.
        memcpy(cbuffer_cpu + (SIZE_T)slot * CB_SLOT_SIZE, mvp.m, sizeof(mvp.m));

        back = IDXGISwapChain3_GetCurrentBackBufferIndex(swap_chain);
        memset(&barrier, 0, sizeof(barrier));
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = rtv_buffers[back];
        barrier.Transition.Subresource = 0;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barrier);

        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap, &dsv);
        ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &rtv_handles[back], FALSE, &dsv);
        clear[0] = bg.r; clear[1] = bg.g; clear[2] = bg.b; clear[3] = bg.a;
        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, rtv_handles[back], clear, 0, NULL);
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);

        vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (float)g_width; vp.Height = (float)g_height;
        vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
        scissor.left = 0; scissor.top = 0; scissor.right = g_width; scissor.bottom = g_height;
        ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &scissor);

        ID3D12GraphicsCommandList_SetPipelineState(command_list, pso);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, root_sig);
        // [LEARN] Bind THIS frame's constant-buffer slot as a root CBV.
        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(command_list, 0,
            cbuffer_gpu + (UINT64)slot * CB_SLOT_SIZE);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_IASetVertexBuffers(command_list, 0, 1, &vbv);
        ID3D12GraphicsCommandList_IASetIndexBuffer(command_list, &ibv);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, 36, 1, 0, 0, 0);

        nk_d3d12_render(command_list, NK_ANTI_ALIASING_ON);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barrier);

        ID3D12GraphicsCommandList_Close(command_list);
        { ID3D12CommandList *lists[1]; lists[0] = (ID3D12CommandList *)command_list;
          ID3D12CommandQueue_ExecuteCommandLists(command_queue, 1, lists); }

        // [LEARN] Signal AFTER submit and record which fence value freed this
        // slot. No wait here - the throttle at the top of the loop handles it.
        frame_fence_value[slot] = ++fence_value;
        ID3D12CommandQueue_Signal(command_queue, queue_fence, fence_value);

        hr = IDXGISwapChain3_Present(swap_chain, vsync ? 1 : 0, 0);
        g_frame_count++;
        if (hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_REMOVED) {
            MessageBoxW(NULL, L"D3D12 device lost", L"Error", 0); break;
        } else if (hr == DXGI_STATUS_OCCLUDED) { Sleep(10); }
    }

    nk_d3d12_shutdown();
    wait_gpu_idle();
    ID3D12Resource_Unmap(cbuffer, 0, NULL);
    ID3D12PipelineState_Release(pso);
    ID3D12RootSignature_Release(root_sig);
    ID3D12Resource_Release(vbuffer);
    ID3D12Resource_Release(ibuffer);
    ID3D12Resource_Release(cbuffer);
    ID3D12Resource_Release(depth_buffer);
    for (i = 0; i < FRAME_COUNT; i++) ID3D12Resource_Release(rtv_buffers[i]);
    ID3D12DescriptorHeap_Release(rtv_heap);
    ID3D12DescriptorHeap_Release(dsv_heap);
    IDXGISwapChain3_Release(swap_chain);
    IDXGIFactory2_Release(dxgi_factory);
    ID3D12GraphicsCommandList_Release(command_list);
    for (i = 0; i < FRAME_COUNT; i++) ID3D12CommandAllocator_Release(command_allocator[i]);
    ID3D12CommandQueue_Release(command_queue);
    ID3D12Fence_Release(queue_fence);
    CloseHandle(fence_event);
    ID3D12Device_Release(device);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
