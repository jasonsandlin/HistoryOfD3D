// =====================================================================
// D3D12.ReservedResources - sparse/tiled textures with virtual allocation.
//
// Reserved resources separate virtual texture address space from physical GPU
// memory. Engines use them for terrain, megatextures, and streaming worlds:
// reserve a large texture, map only the physical tiles that are needed, and
// change those mappings as the camera moves.
//
// This sample creates a 512x512 texture. On TiledResourcesTier 1+ it uses
// CreateReservedResource, creates a small DEFAULT heap, maps a 2x2 tile region
// with UpdateTileMappings, uploads only that mapped rectangle, and samples it on
// the cube. If unsupported or creation fails, it logs the reason and falls back
// to a normal committed texture so the sample still renders.
//
// [LEARN] Sparse resources are capability-gated. Always query the tier, log the
// path taken, and keep a committed-resource fallback for old GPUs, WARP, or
// driver combinations that cannot run the sparse path.
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
#define TEX_SIZE 512
#define MAPPED_TILE_WIDTH 2
#define MAPPED_TILE_HEIGHT 2
#define TILE_SIZE_BYTES 65536u


// [DEBUG] Append diagnostics to a log file instead of popping dialogs.
static void logf_line(const char *fmt, ...)
{
    va_list ap; FILE *f = fopen("CubeD3D12.ReservedResources.log", "a");
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

// ---------- geometry: 24 verts (per-face UVs), 36 indices ----------
typedef struct { float px, py, pz; float u, v; } Vertex;
static const Vertex CUBE_VERTS[] = {
    {-1,-1,-1, 0,1},{ 1,-1,-1, 1,1},{ 1, 1,-1, 1,0},{-1, 1,-1, 0,0},
 // front  (z=-1)
    { 1,-1, 1, 0,1},{-1,-1, 1, 1,1},{-1, 1, 1, 1,0},{ 1, 1, 1, 0,0},
 // back   (z=+1)
    {-1,-1, 1, 0,1},{-1,-1,-1, 1,1},{-1, 1,-1, 1,0},{-1, 1, 1, 0,0},
 // left   (x=-1)
    { 1,-1,-1, 0,1},{ 1,-1, 1, 1,1},{ 1, 1, 1, 1,0},{ 1, 1,-1, 0,0},
 // right  (x=+1)
    {-1, 1,-1, 0,1},{ 1, 1,-1, 1,1},{ 1, 1, 1, 1,0},{-1, 1, 1, 0,0},
 // top    (y=+1)
    {-1,-1, 1, 0,1},{ 1,-1, 1, 1,1},{ 1,-1,-1, 1,0},{-1,-1,-1, 0,0},
 // bottom (y=-1)
};
static const unsigned short CUBE_IDX[] = {
     0, 1, 2,  0, 2, 3,   4, 5, 6,  4, 6, 7,   8, 9,10,  8,10,11,
    12,13,14, 12,14,15,  16,17,18, 16,18,19,  20,21,22, 20,22,23,
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

static ID3D12DescriptorHeap *rtv_heap;
static ID3D12DescriptorHeap *dsv_heap;
static ID3D12DescriptorHeap *srv_heap;   // shader-visible CBV_SRV_UAV heap
static UINT rtv_increment;
static ID3D12Resource *rtv_buffers[2];
static D3D12_CPU_DESCRIPTOR_HANDLE rtv_handles[2];
static ID3D12Resource *depth_buffer;
static UINT rtv_index;

static ID3D12RootSignature *root_sig;
static ID3D12PipelineState *pso;
static ID3D12Resource *vbuffer;
static ID3D12Resource *ibuffer;
static ID3D12Resource *texture;          // DEFAULT heap or reserved texture
static ID3D12Heap *tile_heap;
static D3D12_VERTEX_BUFFER_VIEW vbv;
static D3D12_INDEX_BUFFER_VIEW ibv;
static D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu;

static int g_width = WINDOW_WIDTH, g_height = WINDOW_HEIGHT;
static UINT g_tiled_tier = 0;
static UINT g_standard_tile_size = TILE_SIZE_BYTES;
static UINT g_mapped_tiles = 0;
static int g_using_reserved = 0;
static int g_tiles_mapped = 0;
static char g_status[192] = "not initialized";

static const char *g_shader_src =
"cbuffer CB : register(b0) { row_major float4x4 g_mvp; uint g_useLinear; uint g_showUV; };\n"
"Texture2D    gTex        : register(t0);\n"
"SamplerState gSampPoint  : register(s0);\n"
"SamplerState gSampLinear : register(s1);\n"
"struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };\n"
"VSOut VSMain(float3 pos : POSITION, float2 uv : TEXCOORD) {\n"
"  VSOut o; o.pos = mul(float4(pos,1.0f), g_mvp); o.uv = uv; return o; }\n"
"float4 PSMain(VSOut i) : SV_TARGET {\n"
"  if (g_showUV) return float4(i.uv, 0, 1);\n"
"  return g_useLinear ? gTex.Sample(gSampLinear, i.uv) : gTex.Sample(gSampPoint, i.uv);\n"
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

static ID3D12Resource *create_buffer(D3D12_HEAP_TYPE heap, UINT size, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp; D3D12_RESOURCE_DESC rd; ID3D12Resource *res = NULL;
    memset(&hp, 0, sizeof(hp)); hp.Type = heap;
    memset(&rd, 0, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Device_CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
        state, NULL, &IID_ID3D12Resource, (void **)&res);
    return res;
}

static ID3D12Resource *create_upload_buffer(const void *data, UINT size)
{
    ID3D12Resource *res = create_buffer(D3D12_HEAP_TYPE_UPLOAD, size, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *mapped = NULL; D3D12_RANGE none = { 0, 0 };
    ID3D12Resource_Map(res, 0, &none, &mapped);
    memcpy(mapped, data, size);
    ID3D12Resource_Unmap(res, 0, NULL);
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
    memset(&cv, 0, sizeof(cv)); cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 1.0f;
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

static void query_tiled_support(void)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS opts; HRESULT hr;
    memset(&opts, 0, sizeof(opts));
    hr = ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts));
    if (SUCCEEDED(hr)) g_tiled_tier = (UINT)opts.TiledResourcesTier;
    else g_tiled_tier = 0;
    logf_line("TiledResourcesTier=%u", g_tiled_tier);
}

static void set_tile_mapping(int mapped)
{
    D3D12_TILED_RESOURCE_COORDINATE coord;
    D3D12_TILE_REGION_SIZE region;
    D3D12_TILE_RANGE_FLAGS range_flag;
    UINT heap_offset = 0;
    UINT tile_count = MAPPED_TILE_WIDTH * MAPPED_TILE_HEIGHT;
    if (!g_using_reserved || !texture) return;
    memset(&coord, 0, sizeof(coord));
    memset(&region, 0, sizeof(region));
    region.NumTiles = tile_count;
    region.UseBox = TRUE;
    region.Width = MAPPED_TILE_WIDTH;
    region.Height = MAPPED_TILE_HEIGHT;
    region.Depth = 1;
    range_flag = mapped ? D3D12_TILE_RANGE_FLAG_NONE : D3D12_TILE_RANGE_FLAG_NULL;
    // [LEARN] UpdateTileMappings is the sparse-resource page table update: the
    // texture's virtual tiles are mapped to, or unmapped from, a real heap.
    ID3D12CommandQueue_UpdateTileMappings(command_queue, texture, 1, &coord, &region,
        mapped ? tile_heap : NULL, 1, &range_flag, &heap_offset, &tile_count,
        D3D12_TILE_MAPPING_FLAG_NONE);
    g_tiles_mapped = mapped;
    sprintf(g_status, "reserved texture tiles %s", mapped ? "mapped" : "unmapped");
    logf_line("UpdateTileMappings mapped=%d tiles=%u", mapped, tile_count);
    signal_and_wait();
}

// Generate a 512x512 RGBA checkerboard. The reserved path copies only the
// top-left mapped rectangle; the committed fallback copies the full image.
static void generate_texture(unsigned char *px)
{
    int x, y;
    for (y = 0; y < TEX_SIZE; y++)
        for (x = 0; x < TEX_SIZE; x++) {
            int cell = ((x >> 5) + (y >> 5)) & 1;
            unsigned char *p = px + ((SIZE_T)y * TEX_SIZE + x) * 4;
            if (cell) {
                p[0] = (unsigned char)(x / 2);
                p[1] = (unsigned char)(y / 2);
                p[2] = 220; p[3] = 255;
            } else {
                p[0] = 20; p[1] = 25; p[2] = 40; p[3] = 255;
            }
        }
}

static ID3D12Resource *upload_texture_region(UINT copy_width, UINT copy_height)
{
    D3D12_RESOURCE_DESC td;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT num_rows = 0; UINT64 row_bytes = 0, total_bytes = 0;
    ID3D12Resource *upload;
    unsigned char *pixels;
    void *mapped = NULL; D3D12_RANGE none = { 0, 0 };
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    D3D12_BOX box;
    D3D12_RESOURCE_BARRIER b;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd;
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu;
    UINT y;

    ID3D12Resource_GetDesc(texture, &td);
    ID3D12Device_GetCopyableFootprints(device, &td, 0, 1, 0, &fp, &num_rows, &row_bytes, &total_bytes);
    upload = create_buffer(D3D12_HEAP_TYPE_UPLOAD, (UINT)total_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    pixels = (unsigned char *)malloc((SIZE_T)TEX_SIZE * TEX_SIZE * 4);
    generate_texture(pixels);
    ID3D12Resource_Map(upload, 0, &none, &mapped);
    for (y = 0; y < copy_height; y++)
        memcpy((unsigned char *)mapped + fp.Offset + (SIZE_T)y * fp.Footprint.RowPitch,
               pixels + (SIZE_T)y * TEX_SIZE * 4, (SIZE_T)copy_width * 4);
    ID3D12Resource_Unmap(upload, 0, NULL);
    free(pixels);

    memset(&dst, 0, sizeof(dst));
    dst.pResource = texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    memset(&src, 0, sizeof(src));
    src.pResource = upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    box.left = 0; box.top = 0; box.front = 0; box.right = copy_width; box.bottom = copy_height; box.back = 1;
    ID3D12GraphicsCommandList_CopyTextureRegion(command_list, &dst, 0, 0, 0, &src, &box);

    memset(&b, 0, sizeof(b));
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = texture;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &b);

    memset(&sd, 0, sizeof(sd));
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(srv_heap, &srv_cpu);
    ID3D12Device_CreateShaderResourceView(device, texture, &sd, srv_cpu);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(srv_heap, &srv_gpu);
    return upload;
}

static int try_create_reserved_texture(ID3D12Resource **upload)
{
    D3D12_RESOURCE_DESC td;
    D3D12_HEAP_DESC hd;
    HRESULT hr;
    memset(&td, 0, sizeof(td));
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = TEX_SIZE; td.Height = TEX_SIZE;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    hr = ID3D12Device_CreateReservedResource(device, &td, D3D12_RESOURCE_STATE_COPY_DEST,
        NULL, &IID_ID3D12Resource, (void **)&texture);
    if (FAILED(hr)) { logf_line("CreateReservedResource hr=0x%08X", (unsigned)hr); return 0; }

    memset(&hd, 0, sizeof(hd));
    hd.SizeInBytes = (UINT64)MAPPED_TILE_WIDTH * MAPPED_TILE_HEIGHT * TILE_SIZE_BYTES;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    hr = ID3D12Device_CreateHeap(device, &hd, &IID_ID3D12Heap, (void **)&tile_heap);
    if (FAILED(hr)) { logf_line("CreateHeap(tile heap) hr=0x%08X", (unsigned)hr); ID3D12Resource_Release(texture); texture = NULL; return 0; }

    g_using_reserved = 1;
    g_mapped_tiles = MAPPED_TILE_WIDTH * MAPPED_TILE_HEIGHT;
    set_tile_mapping(1);
    // [LEARN] Copy only the mapped 2x2 tile region. Copying into unmapped
    // virtual tiles would be invalid; streamers upload as they map pages.
    *upload = upload_texture_region(256, 256);
    sprintf(g_status, "reserved resource: mapped and uploaded %u tiles", g_mapped_tiles);
    return 1;
}

static ID3D12Resource *create_texture(void)
{
    D3D12_HEAP_PROPERTIES hp;
    D3D12_RESOURCE_DESC td;
    ID3D12Resource *upload = NULL;

    query_tiled_support();
    if (g_tiled_tier != 0 && try_create_reserved_texture(&upload))
        return upload;

    if (texture) { ID3D12Resource_Release(texture); texture = NULL; }
    if (tile_heap) { ID3D12Heap_Release(tile_heap); tile_heap = NULL; }
    memset(&hp, 0, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    memset(&td, 0, sizeof(td));
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = TEX_SIZE; td.Height = TEX_SIZE;
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ID3D12Device_CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&texture);
    g_using_reserved = 0;
    g_tiles_mapped = 1;
    g_mapped_tiles = (TEX_SIZE / 128) * (TEX_SIZE / 128);
    strcpy(g_status, g_tiled_tier ? "reserved path failed; committed fallback" : "Tier 0; committed fallback");
    logf_line("using committed fallback texture: %s", g_status);
    return upload_texture_region(TEX_SIZE, TEX_SIZE);
}

static int create_cube_pipeline(void)
{
    D3D12_DESCRIPTOR_RANGE range;
    D3D12_ROOT_PARAMETER rp[2];
    D3D12_STATIC_SAMPLER_DESC samp[2];
    D3D12_ROOT_SIGNATURE_DESC rsd;
    D3D12_INPUT_ELEMENT_DESC il[2];
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    ID3DBlob *sig = NULL, *sigerr = NULL, *vs = NULL, *ps = NULL, *err = NULL;
    UINT i;

    // Root param 0: 18 root constants (mvp[16] + useLinear + showUV).
    memset(&rp, 0, sizeof(rp));
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 18;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // Root param 1: descriptor table with one SRV (t0).
    memset(&range, 0, sizeof(range));
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &range;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Two static samplers: s0 point, s1 linear.
    memset(&samp, 0, sizeof(samp));
    for (i = 0; i < 2; i++) {
        samp[i].AddressU = samp[i].AddressV = samp[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samp[i].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samp[i].MaxLOD = D3D12_FLOAT32_MAX;
        samp[i].ShaderRegister = i;
        samp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    samp[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;

    memset(&rsd, 0, sizeof(rsd));
    rsd.NumParameters = 2; rsd.pParameters = rp;
    rsd.NumStaticSamplers = 2; rsd.pStaticSamplers = samp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigerr))) {
        if (sigerr) ID3D10Blob_Release(sigerr);
        return 0;
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
    il[1].SemanticName = "TEXCOORD"; il[1].SemanticIndex = 0;
    il[1].Format = DXGI_FORMAT_R32G32_FLOAT; il[1].InputSlot = 0;
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
    return (root_sig && pso && vbuffer && ibuffer);
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

static void build_ui(struct nk_context *ctx, struct nk_colorf *bg,
                     int *pause, float *speed, int *vsync, int *use_linear, int *show_uv)
{
    if (nk_begin(ctx, "D3D12 Reserved Resources", nk_rect(20, 20, 430, 660),
        NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_SCALABLE|NK_WINDOW_TITLE))
    {
        char buf[160];
        if (nk_tree_push(ctx, NK_TREE_TAB, "Sparse texture", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 18, 1);
            sprintf(buf, "TiledResourcesTier: %u", g_tiled_tier); nk_label(ctx, buf, NK_TEXT_LEFT);
            sprintf(buf, "Standard tile size: %u bytes", g_standard_tile_size); nk_label(ctx, buf, NK_TEXT_LEFT);
            sprintf(buf, "Path: %s", g_using_reserved ? "reserved resource" : "committed fallback"); nk_label(ctx, buf, NK_TEXT_LEFT);
            sprintf(buf, "Mapped tiles: %u", g_using_reserved && g_tiles_mapped ? g_mapped_tiles : 0); nk_label(ctx, buf, NK_TEXT_LEFT);
            nk_label_wrap(ctx, g_status);
            if (g_using_reserved) {
                int mapped = g_tiles_mapped;
                if (nk_checkbox_label(ctx, "Map physical tiles", &mapped) && mapped != g_tiles_mapped)
                    set_tile_mapping(mapped);
            }
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Sampling", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 24, 2);
            if (nk_option_label(ctx, "Point (s0)", *use_linear == 0)) *use_linear = 0;
            if (nk_option_label(ctx, "Linear (s1)", *use_linear == 1)) *use_linear = 1;
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_checkbox_label(ctx, "Show UVs (debug)", show_uv);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Controls", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_checkbox_label(ctx, "Pause rotation", pause);
            nk_checkbox_label(ctx, "VSync", vsync);
            nk_property_float(ctx, "Speed", 0.0f, speed, 4.0f, 0.1f, 0.02f);
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, "Clear color:", NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 100, 1);
            *bg = nk_color_picker(ctx, *bg, NK_RGB);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "What this teaches", NK_MINIMIZED)) {
            nk_layout_row_dynamic(ctx, 15, 1);
            nk_label_wrap(ctx, "CreateReservedResource reserves virtual texture space; UpdateTileMappings binds virtual tiles to a real heap.");
            nk_label_wrap(ctx, "This teaching sample maps a tiny 2x2 region. Tier 2+ reads unmapped tiles as zero; Tier 1 has weaker guarantees.");
            nk_label_wrap(ctx, "If the tier is 0, or the reserved path fails, the same shader samples a committed fallback texture.");
            nk_tree_pop(ctx);
        }
    }
    nk_end(ctx);
}

int main(void)
{
    struct nk_context *ctx;
    struct nk_colorf bg = { 0.10f, 0.18f, 0.24f, 1.0f };
    int pause = 0, vsync = 1, use_linear = 1, show_uv = 0;
    float speed = 0.8f, angle = 0.0f;
    ULONGLONG last_tick;
    ID3D12Resource *tex_upload;

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
    wc.lpszClassName = L"D3D12ReservedResourcesClass";
    RegisterClassW(&wc);
    AdjustWindowRectEx(&rect, style, FALSE, WS_EX_APPWINDOW);
    wnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName,
        L"D3D12 Reserved Resources - sparse texture tiles",
        style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, wc.hInstance, NULL);

    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device);
    if (FAILED(hr)) { MessageBoxW(wnd, L"D3D12 device creation failed", L"Error", 0); logf_line("D3D12CreateDevice hr=0x%08X", (unsigned)hr); return 1; }
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
    // Shader-visible heap that holds the texture's SRV.
    memset(&hd, 0, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&srv_heap);

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

    // Record the texture upload, then Nuklear font upload, then execute once.
    tex_upload = create_texture();
    ctx = nk_d3d12_init(device, WINDOW_WIDTH, WINDOW_HEIGHT, MAX_VERTEX_BUFFER, MAX_INDEX_BUFFER, 1);
    {
        struct nk_font_atlas *atlas;
        nk_d3d12_font_stash_begin(&atlas);
        nk_d3d12_font_stash_end(command_list);
    }
    execute_commands();
    nk_d3d12_font_stash_cleanup();
    ID3D12Resource_Release(tex_upload);   // copy finished; staging no longer needed

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

        nk_input_begin(ctx);
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        nk_input_end(ctx);

        build_ui(ctx, &bg, &pause, &speed, &vsync, &use_linear, &show_uv);

        now = GetTickCount64();
        dt = (now - last_tick) / 1000.0f;
        last_tick = now;
        if (!pause) angle += dt * speed;

        world = mat_mul(mat_rot_y(angle), mat_rot_x(angle * 0.5f));
        view = mat_lookat_lh(0.0f, 0.0f, -6.0f);
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
        clear[0] = bg.r; clear[1] = bg.g; clear[2] = bg.b; clear[3] = bg.a;
        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, rtv_handles[rtv_index], clear, 0, NULL);
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);

        vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (float)g_width; vp.Height = (float)g_height;
        vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
        scissor.left = 0; scissor.top = 0; scissor.right = g_width; scissor.bottom = g_height;
        ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &scissor);

        ID3D12GraphicsCommandList_SetPipelineState(command_list, pso);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, root_sig);
        ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &srv_heap);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 16, mvp.m, 0);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(command_list, 0, (UINT)use_linear, 16);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(command_list, 0, (UINT)show_uv, 17);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 1, srv_gpu);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_IASetVertexBuffers(command_list, 0, 1, &vbv);
        ID3D12GraphicsCommandList_IASetIndexBuffer(command_list, &ibv);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, 36, 1, 0, 0, 0);

        nk_d3d12_render(command_list, NK_ANTI_ALIASING_ON);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &barrier);

        execute_commands();

        hr = IDXGISwapChain1_Present(swap_chain, vsync ? 1 : 0, 0);
        rtv_index = (rtv_index + 1) % 2;
        if (hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_REMOVED) {
            logf_line("D3D12 device lost");
            break;
        } else if (hr == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
        }
    }

    nk_d3d12_shutdown();
    signal_and_wait();
    signal_and_wait();
    ID3D12PipelineState_Release(pso);
    ID3D12RootSignature_Release(root_sig);
    ID3D12Resource_Release(texture);
    if (tile_heap) ID3D12Heap_Release(tile_heap);
    ID3D12Resource_Release(vbuffer);
    ID3D12Resource_Release(ibuffer);
    ID3D12Resource_Release(depth_buffer);
    ID3D12Resource_Release(rtv_buffers[0]);
    ID3D12Resource_Release(rtv_buffers[1]);
    ID3D12DescriptorHeap_Release(rtv_heap);
    ID3D12DescriptorHeap_Release(dsv_heap);
    ID3D12DescriptorHeap_Release(srv_heap);
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
