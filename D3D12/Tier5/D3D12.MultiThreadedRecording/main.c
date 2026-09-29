// =====================================================================
// D3D12.MultiThreadedRecording - record command lists in parallel.
//
// This sample draws a grid of cubes by splitting the cube draws across Win32
// worker threads. Each worker owns its own DIRECT command allocator and command
// list; the main thread submits [begin, worker0..workerN, end] in one ordered
// ExecuteCommandLists call on one queue.
//
// [LEARN] D3D12's queue/allocator/list split lets CPU threads record in
// parallel while GPU execution remains explicitly ordered. This sample keeps the
// simple serialized fence-per-frame model; real engines keep persistent workers
// busy and pipeline frames for performance.
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
#define MAX_WORKERS 4
#define MAX_CUBES 256

static void logf_line(const char *fmt, ...)
{
    va_list ap; FILE *f = fopen("CubeD3D12.MultiThreadedRecording.log", "a");
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
static Mat4 mat_scale(float x, float y, float z)
{
    Mat4 r = {{x,0,0,0, 0,y,0,0, 0,0,z,0, 0,0,0,1}};
    return r;
}
static Mat4 mat_translate(float x, float y, float z)
{
    Mat4 r = {{1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1}};
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

typedef struct WorkerState {
    ID3D12CommandAllocator *allocator;
    ID3D12GraphicsCommandList *list;
    HANDLE start_event;
    HANDLE done_event;
    HANDLE thread;
    int index;
} WorkerState;

static WorkerState g_workers[MAX_WORKERS];
static ID3D12CommandAllocator *end_allocator;
static ID3D12GraphicsCommandList *end_list;
static volatile LONG g_worker_quit;
static int g_worker_count = 4;
static int g_cube_count = 160;
static int g_cubes_recorded[MAX_WORKERS];
static float g_record_angle;

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
        log_blob("RootSignature", sigerr);
        if (sigerr) ID3D10Blob_Release(sigerr);
        return 0;
    }
    ID3D12Device_CreateRootSignature(device, 0,
        ID3D10Blob_GetBufferPointer(sig), ID3D10Blob_GetBufferSize(sig),
        &IID_ID3D12RootSignature, (void **)&root_sig);
    ID3D10Blob_Release(sig);

    // Compile shaders at runtime (SM5 via D3DCompile / fxc).
    if (FAILED(D3DCompile(g_shader_src, strlen(g_shader_src), NULL, NULL, NULL,
        "VSMain", "vs_5_0", 0, 0, &vs, &err))) { log_blob("VS", err); if (err) ID3D10Blob_Release(err); return 0; }
    if (FAILED(D3DCompile(g_shader_src, strlen(g_shader_src), NULL, NULL, NULL,
        "PSMain", "ps_5_0", 0, 0, &ps, &err))) { log_blob("PS", err); if (err) ID3D10Blob_Release(err); return 0; }

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



static Mat4 cube_mvp(int cube_index, float angle)
{
    int cols = 16;
    int row = cube_index / cols;
    int col = cube_index % cols;
    float x = ((float)col - (cols - 1) * 0.5f) * 1.85f;
    float y = ((float)row - 4.5f) * 1.85f;
    Mat4 scale = mat_scale(0.38f, 0.38f, 0.38f);
    Mat4 rot = mat_mul(mat_rot_y(angle + cube_index * 0.07f), mat_rot_x(angle * 0.5f));
    Mat4 trans = mat_translate(x, y, 0.0f);
    Mat4 view = mat_lookat_lh(0.0f, 0.0f, -26.0f);
    Mat4 proj = mat_perspective_lh(3.14159265f / 4.0f, (float)g_width / (float)g_height, 0.1f, 100.0f);
    return mat_mul(mat_mul(mat_mul(mat_mul(scale, rot), trans), view), proj);
}

static void record_worker_commands(int worker_id)
{
    WorkerState *w = &g_workers[worker_id];
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    D3D12_VIEWPORT vp;
    D3D12_RECT scissor;
    int active_workers = g_worker_count;
    int begin, end, i;
    if (active_workers < 1) active_workers = 1;
    if (active_workers > MAX_WORKERS) active_workers = MAX_WORKERS;
    begin = (g_cube_count * worker_id) / active_workers;
    end = (g_cube_count * (worker_id + 1)) / active_workers;
    g_cubes_recorded[worker_id] = end - begin;
    ID3D12CommandAllocator_Reset(w->allocator);
    ID3D12GraphicsCommandList_Reset(w->list, w->allocator, pso_solid);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap, &dsv);
    vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (float)g_width; vp.Height = (float)g_height;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    scissor.left = 0; scissor.top = 0; scissor.right = g_width; scissor.bottom = g_height;
    ID3D12GraphicsCommandList_OMSetRenderTargets(w->list, 1, &rtv_handles[rtv_index], FALSE, &dsv);
    ID3D12GraphicsCommandList_RSSetViewports(w->list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(w->list, 1, &scissor);
    ID3D12GraphicsCommandList_SetPipelineState(w->list, pso_solid);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(w->list, root_sig);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(w->list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_IASetVertexBuffers(w->list, 0, 1, &vbv);
    ID3D12GraphicsCommandList_IASetIndexBuffer(w->list, &ibv);
    for (i = begin; i < end; i++) {
        Mat4 mvp = cube_mvp(i, g_record_angle);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(w->list, 0, 16, mvp.m, 0);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(w->list, 36, 1, 0, 0, 0);
    }
    ID3D12GraphicsCommandList_Close(w->list);
}

static DWORD WINAPI worker_proc(LPVOID param)
{
    WorkerState *w = (WorkerState *)param;
    for (;;) {
        WaitForSingleObject(w->start_event, INFINITE);
        if (InterlockedCompareExchange(&g_worker_quit, 0, 0)) break;
        record_worker_commands(w->index);
        SetEvent(w->done_event);
    }
    return 0;
}

static void create_worker_lists(void)
{
    int i;
    ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void **)&end_allocator);
    ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        end_allocator, NULL, &IID_ID3D12GraphicsCommandList1, (void **)&end_list);
    ID3D12GraphicsCommandList_Close(end_list);
    for (i = 0; i < MAX_WORKERS; i++) {
        g_workers[i].index = i;
        g_workers[i].start_event = CreateEventW(NULL, FALSE, FALSE, NULL);
        g_workers[i].done_event = CreateEventW(NULL, TRUE, FALSE, NULL);
        ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&g_workers[i].allocator);
        ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            g_workers[i].allocator, NULL, &IID_ID3D12GraphicsCommandList1, (void **)&g_workers[i].list);
        ID3D12GraphicsCommandList_Close(g_workers[i].list);
        g_workers[i].thread = CreateThread(NULL, 0, worker_proc, &g_workers[i], 0, NULL);
    }
}

static void record_begin_list(struct nk_colorf bg)
{
    D3D12_RESOURCE_BARRIER barrier;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    float clear[4];
    ID3D12CommandAllocator_Reset(command_allocator);
    ID3D12GraphicsCommandList_Reset(command_list, command_allocator, NULL);
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
    ID3D12GraphicsCommandList_Close(command_list);
}

static void record_end_list(void)
{
    D3D12_RESOURCE_BARRIER barrier;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    ID3D12CommandAllocator_Reset(end_allocator);
    ID3D12GraphicsCommandList_Reset(end_list, end_allocator, NULL);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap, &dsv);
    ID3D12GraphicsCommandList_OMSetRenderTargets(end_list, 1, &rtv_handles[rtv_index], FALSE, &dsv);
    nk_d3d12_render(end_list, NK_ANTI_ALIASING_ON);
    memset(&barrier, 0, sizeof(barrier));
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = rtv_buffers[rtv_index];
    barrier.Transition.Subresource = 0;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    ID3D12GraphicsCommandList_ResourceBarrier(end_list, 1, &barrier);
    g_barriers_last_frame++;
    ID3D12GraphicsCommandList_Close(end_list);
}

static void execute_threaded_frame(void)
{
    ID3D12CommandList *lists[MAX_WORKERS + 2];
    int i, n = 0;
    lists[n++] = (ID3D12CommandList *)command_list;
    for (i = 0; i < g_worker_count; i++) lists[n++] = (ID3D12CommandList *)g_workers[i].list;
    lists[n++] = (ID3D12CommandList *)end_list;
    // [LEARN] One queue submission preserves the explicit order: begin, workers, GUI/end.
    ID3D12CommandQueue_ExecuteCommandLists(command_queue, (UINT)n, lists);
    signal_and_wait();
}

static void shutdown_workers(void)
{
    int i;
    InterlockedExchange(&g_worker_quit, 1);
    for (i = 0; i < MAX_WORKERS; i++) SetEvent(g_workers[i].start_event);
    for (i = 0; i < MAX_WORKERS; i++) WaitForSingleObject(g_workers[i].thread, INFINITE);
    for (i = 0; i < MAX_WORKERS; i++) {
        CloseHandle(g_workers[i].thread);
        CloseHandle(g_workers[i].start_event);
        CloseHandle(g_workers[i].done_event);
        ID3D12GraphicsCommandList_Release(g_workers[i].list);
        ID3D12CommandAllocator_Release(g_workers[i].allocator);
    }
    ID3D12GraphicsCommandList_Release(end_list);
    ID3D12CommandAllocator_Release(end_allocator);
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
                     int *pause, float *speed, int *wire, int *vsync)
{
    if (nk_begin(ctx, "D3D12 MultiThreadedRecording", nk_rect(20, 20, 410, 600),
        NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_SCALABLE|NK_WINDOW_TITLE))
    {
        char buf[128];
        int cubes_per_thread = (g_cube_count + g_worker_count - 1) / g_worker_count;
        nk_layout_row_dynamic(ctx, 18, 1);
        sprintf(buf, "Frame #: %llu", (unsigned long long)g_frame_count); nk_label(ctx, buf, NK_TEXT_LEFT);
        sprintf(buf, "Worker-thread count: %d", g_worker_count); nk_label(ctx, buf, NK_TEXT_LEFT);
        sprintf(buf, "Cubes per thread: about %d", cubes_per_thread); nk_label(ctx, buf, NK_TEXT_LEFT);
        sprintf(buf, "Total cubes: %d", g_cube_count); nk_label(ctx, buf, NK_TEXT_LEFT);
        sprintf(buf, "Submitted lists: begin + %d workers + end", g_worker_count); nk_label(ctx, buf, NK_TEXT_LEFT);
        nk_property_int(ctx, "Workers", 1, &g_worker_count, MAX_WORKERS, 1, 1.0f);
        nk_property_int(ctx, "Cubes", 16, &g_cube_count, MAX_CUBES, 8, 1.0f);
        nk_checkbox_label(ctx, "Pause rotation", pause);
        nk_checkbox_label(ctx, "VSync", vsync);
        nk_property_float(ctx, "Speed", 0.0f, speed, 4.0f, 0.1f, 0.02f);
        nk_layout_row_dynamic(ctx, 80, 1);
        *bg = nk_color_picker(ctx, *bg, NK_RGB);
        nk_layout_row_dynamic(ctx, 16, 1);
        nk_label_wrap(ctx, "[LEARN] Each worker resets and records its own allocator/list for a cube subset, with no shared command-list writes.");
        nk_label_wrap(ctx, "The main thread records a begin list for barriers/clears and an end list for Nuklear plus the PRESENT barrier.");
        nk_label_wrap(ctx, "A real engine keeps persistent workers and pipelines frames; this sample waits every frame for correctness and clarity.");
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
    wc.lpszClassName = L"D3D12FundamentalsClass";
    RegisterClassW(&wc);
    AdjustWindowRectEx(&rect, style, FALSE, WS_EX_APPWINDOW);
    wnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName,
        L"D3D12 Fundamentals - core objects tour",
        style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, wc.hInstance, NULL);

    // Device, queue, fence, allocator, command list.
    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device);
    if (FAILED(hr)) { MessageBoxW(wnd, L"D3D12 device creation failed", L"Error", 0); return 1; }
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
    create_worker_lists();

    // Nuklear GUI.
    ctx = nk_d3d12_init(device, WINDOW_WIDTH, WINDOW_HEIGHT, MAX_VERTEX_BUFFER, MAX_INDEX_BUFFER, 1);
    {
        struct nk_font_atlas *atlas;
        nk_d3d12_font_stash_begin(&atlas);
        nk_d3d12_font_stash_end(command_list);
    }
    execute_commands();
    ID3D12GraphicsCommandList_Close(command_list);
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
        int i;
        HANDLE done_events[MAX_WORKERS];

        // --- input ---
        nk_input_begin(ctx);
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        nk_input_end(ctx);

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
        if (g_worker_count < 1) g_worker_count = 1;
        if (g_worker_count > MAX_WORKERS) g_worker_count = MAX_WORKERS;
        if (g_cube_count < 1) g_cube_count = 1;
        if (g_cube_count > MAX_CUBES) g_cube_count = MAX_CUBES;
        g_record_angle = angle;
        record_begin_list(bg);
        for (i = 0; i < g_worker_count; i++) {
            ResetEvent(g_workers[i].done_event);
            SetEvent(g_workers[i].start_event);
            done_events[i] = g_workers[i].done_event;
        }
        WaitForMultipleObjects((DWORD)g_worker_count, done_events, TRUE, INFINITE);
        record_end_list();
        execute_threaded_frame();

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

    // Shutdown.
    shutdown_workers();
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
    ID3D12GraphicsCommandList_Release(command_list);
    ID3D12CommandAllocator_Release(command_allocator);
    ID3D12CommandQueue_Release(command_queue);
    ID3D12Fence_Release(queue_fence);
    ID3D12Device_Release(device);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
