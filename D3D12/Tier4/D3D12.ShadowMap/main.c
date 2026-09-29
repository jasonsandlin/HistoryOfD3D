// =====================================================================
// D3D12.ShadowMap - real-time shadows via shadow mapping (2 passes).
//
// Shadow mapping renders the scene twice:
//   PASS 1 (shadow): render the scene's DEPTH from the LIGHT's point of view
//     into a depth texture (the "shadow map"). No color - just how far the
//     nearest surface is from the light in each direction.
//   PASS 2 (main): render the scene from the camera. For each pixel, transform
//     its position into the light's clip space and compare its light-space depth
//     against the shadow map. If the pixel is farther than what the light saw,
//     something is between it and the light, so it is in shadow.
//
// [LEARN] Key pieces:
//   1. The shadow map is a DEPTH texture (R32_TYPELESS) with BOTH a DSV (to be
//      rendered into) and an SRV (to be sampled in the main pass).
//   2. The shadow PSO is DEPTH-ONLY: NumRenderTargets = 0 and no pixel shader.
//   3. A SamplerComparisonState + SampleCmpLevelZero does the depth compare in
//      hardware (this is percentage-closer filtering when linear).
//   4. A small depth BIAS avoids "shadow acne" (self-shadowing artifacts).
//
// A rotating cube casts a shadow onto a ground plane so the effect is visible.
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
#define SHADOW_SIZE 1024

// [DEBUG] Append diagnostics to a log file instead of popping dialogs.
static void logf_line(const char *fmt, ...)
{
    va_list ap; FILE *f = fopen("CubeD3D12.ShadowMap.log", "a");
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
static Mat4 mat_identity(void) { Mat4 r={{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}}; return r; }
static Mat4 mat_rot_x(float a) { float c=cosf(a),s=sinf(a); Mat4 r={{1,0,0,0, 0,c,s,0, 0,-s,c,0, 0,0,0,1}}; return r; }
static Mat4 mat_rot_y(float a) { float c=cosf(a),s=sinf(a); Mat4 r={{c,0,-s,0, 0,1,0,0, s,0,c,0, 0,0,0,1}}; return r; }
static Mat4 mat_translate(float x, float y, float z) { Mat4 r={{1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1}}; return r; }
// look-at from an arbitrary eye toward the origin (target = 0,0,0).
static Mat4 mat_lookat_origin_lh(float ex, float ey, float ez)
{
    float zx=-ex,zy=-ey,zz=-ez;
    float zl=sqrtf(zx*zx+zy*zy+zz*zz); zx/=zl; zy/=zl; zz/=zl;
    { // up = (0,1,0)
      float xx= 1.0f*zz - 0.0f*zy, xy= 0.0f*zx - 0.0f*zz, xz= 0.0f*zy - 1.0f*zx;
      float xl=sqrtf(xx*xx+xy*xy+xz*xz); if (xl<1e-5f){xx=1;xy=0;xz=0;xl=1;} xx/=xl; xy/=xl; xz/=xl;
      { float yx=zy*xz-zz*xy, yy=zz*xx-zx*xz, yz=zx*xy-zy*xx;
        float dx=-(xx*ex+xy*ey+xz*ez), dy=-(yx*ex+yy*ey+yz*ez), dz=-(zx*ex+zy*ey+zz*ez);
        Mat4 r={{ xx,yx,zx,0, xy,yy,zy,0, xz,yz,zz,0, dx,dy,dz,1 }}; return r; } }
}
static Mat4 mat_perspective_lh(float fovY, float aspect, float zn, float zf)
{
    float ys=1.0f/tanf(fovY*0.5f), xs=ys/aspect;
    Mat4 r={{ xs,0,0,0, 0,ys,0,0, 0,0,zf/(zf-zn),1, 0,0,-zn*zf/(zf-zn),0 }}; return r;
}
static Mat4 mat_ortho_lh(float w, float h, float zn, float zf)
{
    Mat4 r={{ 2.0f/w,0,0,0, 0,2.0f/h,0,0, 0,0,1.0f/(zf-zn),0, 0,0,-zn/(zf-zn),1 }}; return r;
}

typedef struct { float px,py,pz; float nx,ny,nz; float r,g,b; } Vertex;
// 8 cube corners (verts 0..7) + 4 ground-plane corners (verts 8..11).
static const Vertex VERTS[12] = {
    { -1,-1,-1, -0.577f,-0.577f,-0.577f, 0.85f,0.35f,0.35f },
    { -1, 1,-1, -0.577f, 0.577f,-0.577f, 0.85f,0.55f,0.30f },
    {  1, 1,-1,  0.577f, 0.577f,-0.577f, 0.80f,0.80f,0.35f },
    {  1,-1,-1,  0.577f,-0.577f,-0.577f, 0.40f,0.80f,0.45f },
    { -1,-1, 1, -0.577f,-0.577f, 0.577f, 0.35f,0.75f,0.85f },
    { -1, 1, 1, -0.577f, 0.577f, 0.577f, 0.40f,0.55f,0.90f },
    {  1, 1, 1,  0.577f, 0.577f, 0.577f, 0.65f,0.45f,0.90f },
    {  1,-1, 1,  0.577f,-0.577f, 0.577f, 0.90f,0.90f,0.90f },
    { -5,-1.6f,-5, 0,1,0, 0.55f,0.55f,0.60f },
    {  5,-1.6f,-5, 0,1,0, 0.55f,0.55f,0.60f },
    {  5,-1.6f, 5, 0,1,0, 0.55f,0.55f,0.60f },
    { -5,-1.6f, 5, 0,1,0, 0.55f,0.55f,0.60f },
};
static const unsigned short IDX[] = {
    // cube (36)
    0,1,2, 0,2,3,  4,6,5, 4,7,6,  4,5,1, 4,1,0,
    3,2,6, 3,6,7,  1,5,6, 1,6,2,  4,0,3, 4,3,7,
    // plane (6)
    8,9,10, 8,10,11,
};
#define CUBE_INDEX_COUNT 36
#define PLANE_INDEX_COUNT 6

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
static ID3D12DescriptorHeap *dsv_heap;   // slot0 scene depth, slot1 shadow map
static ID3D12DescriptorHeap *srv_heap;   // shader-visible: slot0 = shadow map
static UINT rtv_increment;
static UINT dsv_increment;
static ID3D12Resource *rtv_buffers[2];
static D3D12_CPU_DESCRIPTOR_HANDLE rtv_handles[2];
static ID3D12Resource *depth_buffer;
static UINT rtv_index;

static ID3D12Resource *shadow_map;
static D3D12_CPU_DESCRIPTOR_HANDLE shadow_dsv;
static D3D12_GPU_DESCRIPTOR_HANDLE shadow_srv;

static ID3D12RootSignature *shadow_root_sig;
static ID3D12PipelineState *shadow_pso;
static ID3D12RootSignature *main_root_sig;
static ID3D12PipelineState *main_pso;
static ID3D12Resource *vbuffer;
static ID3D12Resource *ibuffer;
static D3D12_VERTEX_BUFFER_VIEW vbv;
static D3D12_INDEX_BUFFER_VIEW ibv;

static int g_width = WINDOW_WIDTH, g_height = WINDOW_HEIGHT;

// Shadow pass: depth only, one matrix (world * lightViewProj).
static const char *g_shadow_shader =
"cbuffer CB : register(b0) { row_major float4x4 g_lightMVP; }\n"
"float4 VSMain(float3 pos : POSITION) : SV_POSITION {\n"
"  return mul(float4(pos, 1.0), g_lightMVP);\n"
"}\n";

// Main pass: shade + sample the shadow map.
static const char *g_main_shader =
"cbuffer CB : register(b0) {\n"
"  row_major float4x4 g_camMVP;\n"
"  row_major float4x4 g_lightMVP;\n"
"  row_major float4x4 g_world;\n"
"}\n"
"cbuffer L : register(b1) { float3 g_lightDir; float g_bias; }\n"
"Texture2D g_shadow : register(t0);\n"
"SamplerComparisonState g_shadowSamp : register(s0);\n"
"struct VSOut {\n"
"  float4 pos : SV_POSITION; float3 nrm : NORMAL; float3 col : COLOR; float4 lpos : TEXCOORD0;\n"
"};\n"
"VSOut VSMain(float3 pos : POSITION, float3 nrm : NORMAL, float3 col : COLOR) {\n"
"  VSOut o;\n"
"  o.pos = mul(float4(pos, 1.0), g_camMVP);\n"
"  o.lpos = mul(float4(pos, 1.0), g_lightMVP);\n"   // position in light clip space
"  o.nrm = normalize(mul(float4(nrm, 0.0), g_world).xyz);\n"
"  o.col = col; return o;\n"
"}\n"
"float4 PSMain(VSOut i) : SV_TARGET {\n"
"  // [LEARN] project light-space position to shadow-map UV + depth.\n"
"  float3 proj = i.lpos.xyz / i.lpos.w;\n"
"  float2 uv = proj.xy * float2(0.5, -0.5) + 0.5;\n"
"  float depth = proj.z - g_bias;\n"
"  // SampleCmpLevelZero returns the fraction of texels nearer than 'depth'.\n"
"  float lit = g_shadow.SampleCmpLevelZero(g_shadowSamp, uv, depth);\n"
"  float ndl = saturate(dot(normalize(i.nrm), normalize(g_lightDir)));\n"
"  float3 c = i.col * (0.22 + 0.78 * ndl * lit);\n"
"  return float4(c, 1);\n"
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

// [LEARN] The shadow map: R32_TYPELESS resource with a D32 DSV and an R32 SRV.
static void create_shadow_map(void)
{
    D3D12_HEAP_PROPERTIES hp; D3D12_RESOURCE_DESC rd; D3D12_CLEAR_VALUE cv;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvd;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_cpu, srv_cpu;
    memset(&hp, 0, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    memset(&rd, 0, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = SHADOW_SIZE; rd.Height = SHADOW_SIZE;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32_TYPELESS; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    memset(&cv, 0, sizeof(cv));
    cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 1.0f;
    // create in PIXEL_SHADER_RESOURCE so the per-frame PSR->DEPTH_WRITE->PSR loop is uniform.
    ID3D12Device_CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv, &IID_ID3D12Resource, (void **)&shadow_map);

    memset(&dsvd, 0, sizeof(dsvd));
    dsvd.Format = DXGI_FORMAT_D32_FLOAT;
    dsvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap, &dsv_cpu);
    dsv_cpu.ptr += (SIZE_T)1 * dsv_increment;   // slot1
    ID3D12Device_CreateDepthStencilView(device, shadow_map, &dsvd, dsv_cpu);
    shadow_dsv = dsv_cpu;

    memset(&srvd, 0, sizeof(srvd));
    srvd.Format = DXGI_FORMAT_R32_FLOAT;
    srvd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Texture2D.MipLevels = 1;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(srv_heap, &srv_cpu);
    ID3D12Device_CreateShaderResourceView(device, shadow_map, &srvd, srv_cpu);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(srv_heap, &shadow_srv);
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

static int create_shadow_pipeline(void)
{
    D3D12_ROOT_PARAMETER rp;
    D3D12_ROOT_SIGNATURE_DESC rsd;
    D3D12_INPUT_ELEMENT_DESC il[1];
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    ID3DBlob *sig = NULL, *sigerr = NULL, *vs = NULL, *err = NULL;
    HRESULT hr;

    memset(&rp, 0, sizeof(rp));
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.ShaderRegister = 0;
    rp.Constants.Num32BitValues = 16;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    memset(&rsd, 0, sizeof(rsd));
    rsd.NumParameters = 1; rsd.pParameters = &rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigerr);
    if (FAILED(hr)) { logf_line("shadow rootsig hr=0x%08X", (unsigned)hr); log_blob("err", sigerr); if (sigerr) ID3D10Blob_Release(sigerr); return 0; }
    ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(sig),
        ID3D10Blob_GetBufferSize(sig), &IID_ID3D12RootSignature, (void **)&shadow_root_sig);
    ID3D10Blob_Release(sig);

    hr = D3DCompile(g_shadow_shader, strlen(g_shadow_shader), NULL, NULL, NULL, "VSMain", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr)) { logf_line("shadow VS hr=0x%08X", (unsigned)hr); log_blob("err", err); if (err) ID3D10Blob_Release(err); return 0; }

    il[0].SemanticName = "POSITION"; il[0].SemanticIndex = 0;
    il[0].Format = DXGI_FORMAT_R32G32B32_FLOAT; il[0].InputSlot = 0;
    il[0].AlignedByteOffset = 0;
    il[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    il[0].InstanceDataStepRate = 0;

    memset(&pd, 0, sizeof(pd));
    pd.pRootSignature = shadow_root_sig;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
    pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
    // [LEARN] depth-only: no pixel shader, no render targets.
    pd.InputLayout.pInputElementDescs = il;
    pd.InputLayout.NumElements = 1;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 0;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pd.RasterizerState.DepthClipEnable = TRUE;
    // [LEARN] slope-scaled depth bias in rasterizer also reduces shadow acne.
    pd.RasterizerState.DepthBias = 1000;
    pd.RasterizerState.SlopeScaledDepthBias = 1.5f;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    ID3D12Device_CreateGraphicsPipelineState(device, &pd, &IID_ID3D12PipelineState, (void **)&shadow_pso);
    if (!shadow_pso) logf_line("shadow PSO NULL");
    ID3D10Blob_Release(vs);
    return (shadow_root_sig && shadow_pso);
}

static int create_main_pipeline(void)
{
    D3D12_ROOT_PARAMETER rp[3];
    D3D12_DESCRIPTOR_RANGE range;
    D3D12_STATIC_SAMPLER_DESC samp;
    D3D12_ROOT_SIGNATURE_DESC rsd;
    D3D12_INPUT_ELEMENT_DESC il[3];
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    ID3DBlob *sig = NULL, *sigerr = NULL, *vs = NULL, *ps = NULL, *err = NULL;
    UINT i; HRESULT hr;

    memset(rp, 0, sizeof(rp));
    // b0: cam MVP + light MVP + world (48 dwords), VS.
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.Num32BitValues = 48;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    // b1: light dir + bias (PS).
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.ShaderRegister = 1;
    rp[1].Constants.Num32BitValues = 4;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // t0: shadow map SRV (PS).
    memset(&range, 0, sizeof(range));
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[2].DescriptorTable.NumDescriptorRanges = 1;
    rp[2].DescriptorTable.pDescriptorRanges = &range;
    rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // [LEARN] a COMPARISON sampler drives SampleCmp; border = 1 keeps areas
    // outside the shadow map fully lit.
    memset(&samp, 0, sizeof(samp));
    samp.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samp.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    memset(&rsd, 0, sizeof(rsd));
    rsd.NumParameters = 3; rsd.pParameters = rp;
    rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &samp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigerr);
    if (FAILED(hr)) { logf_line("main rootsig hr=0x%08X", (unsigned)hr); log_blob("err", sigerr); if (sigerr) ID3D10Blob_Release(sigerr); return 0; }
    ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(sig),
        ID3D10Blob_GetBufferSize(sig), &IID_ID3D12RootSignature, (void **)&main_root_sig);
    ID3D10Blob_Release(sig);

    hr = D3DCompile(g_main_shader, strlen(g_main_shader), NULL, NULL, NULL, "VSMain", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr)) { logf_line("main VS hr=0x%08X", (unsigned)hr); log_blob("err", err); if (err) ID3D10Blob_Release(err); return 0; }
    hr = D3DCompile(g_main_shader, strlen(g_main_shader), NULL, NULL, NULL, "PSMain", "ps_5_0", 0, 0, &ps, &err);
    if (FAILED(hr)) { logf_line("main PS hr=0x%08X", (unsigned)hr); log_blob("err", err); if (err) ID3D10Blob_Release(err); return 0; }

    il[0].SemanticName = "POSITION"; il[0].SemanticIndex = 0;
    il[0].Format = DXGI_FORMAT_R32G32B32_FLOAT; il[0].InputSlot = 0;
    il[0].AlignedByteOffset = 0;
    il[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    il[0].InstanceDataStepRate = 0;
    il[1].SemanticName = "NORMAL"; il[1].SemanticIndex = 0;
    il[1].Format = DXGI_FORMAT_R32G32B32_FLOAT; il[1].InputSlot = 0;
    il[1].AlignedByteOffset = 12;
    il[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    il[1].InstanceDataStepRate = 0;
    il[2].SemanticName = "COLOR"; il[2].SemanticIndex = 0;
    il[2].Format = DXGI_FORMAT_R32G32B32_FLOAT; il[2].InputSlot = 0;
    il[2].AlignedByteOffset = 24;
    il[2].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    il[2].InstanceDataStepRate = 0;

    memset(&pd, 0, sizeof(pd));
    pd.pRootSignature = main_root_sig;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
    pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps);
    pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
    pd.InputLayout.pInputElementDescs = il;
    pd.InputLayout.NumElements = 3;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pd.RasterizerState.DepthClipEnable = TRUE;
    for (i = 0; i < 8; i++)
        pd.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    ID3D12Device_CreateGraphicsPipelineState(device, &pd, &IID_ID3D12PipelineState, (void **)&main_pso);
    if (!main_pso) logf_line("main PSO NULL");
    ID3D10Blob_Release(vs); ID3D10Blob_Release(ps);
    return (main_root_sig && main_pso);
}

static void create_geometry(void)
{
    vbuffer = create_upload_buffer(VERTS, sizeof(VERTS));
    vbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vbuffer);
    vbv.StrideInBytes = sizeof(Vertex);
    vbv.SizeInBytes = sizeof(VERTS);
    ibuffer = create_upload_buffer(IDX, sizeof(IDX));
    ibv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(ibuffer);
    ibv.Format = DXGI_FORMAT_R16_UINT;
    ibv.SizeInBytes = sizeof(IDX);
}

static void transition(ID3D12Resource *res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof(b));
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = 0;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(command_list, 1, &b);
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

static void build_ui(struct nk_context *ctx, int *pause, float *speed, float *light_ang, float *bias)
{
    if (nk_begin(ctx, "D3D12 Shadow Mapping", nk_rect(20, 20, 380, 560),
        NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_SCALABLE|NK_WINDOW_TITLE))
    {
        if (nk_tree_push(ctx, NK_TREE_TAB, "Two passes", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 16, 1);
            nk_label(ctx, "Pass 1: depth from the light -> shadow map.", NK_TEXT_LEFT);
            nk_label(ctx, "Pass 2: compare each pixel's light-depth.", NK_TEXT_LEFT);
            nk_label(ctx, "Cube casts a shadow onto the ground plane.", NK_TEXT_LEFT);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Controls", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_checkbox_label(ctx, "Pause rotation", pause);
            nk_property_float(ctx, "Cube speed", 0.0f, speed, 4.0f, 0.1f, 0.02f);
            nk_property_float(ctx, "Light angle", 0.0f, light_ang, 6.28f, 0.05f, 0.01f);
            nk_property_float(ctx, "Depth bias", 0.0f, bias, 0.02f, 0.0005f, 0.0001f);
            nk_label(ctx, "Raise bias if you see shadow acne.", NK_TEXT_LEFT);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Shadow map", NK_MINIMIZED)) {
            char buf[64];
            nk_layout_row_dynamic(ctx, 15, 1);
            sprintf(buf, "Resolution: %d x %d (D32)", SHADOW_SIZE, SHADOW_SIZE);
            nk_label(ctx, buf, NK_TEXT_LEFT);
            nk_label_wrap(ctx, "R32_TYPELESS: D32 DSV to write, R32 SRV to read.");
            nk_tree_pop(ctx);
        }
    }
    nk_end(ctx);
}

int main(void)
{
    struct nk_context *ctx;
    int pause = 0;
    float speed = 0.7f, angle = 0.0f, light_ang = 0.9f, bias = 0.002f;
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
    wc.lpszClassName = L"D3D12ShadowMapClass";
    RegisterClassW(&wc);
    AdjustWindowRectEx(&rect, style, FALSE, WS_EX_APPWINDOW);
    wnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName,
        L"D3D12 Shadow Mapping - depth from the light drives shadows",
        style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, wc.hInstance, NULL);

    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device);
    if (FAILED(hr)) { logf_line("D3D12CreateDevice hr=0x%08X", (unsigned)hr); return 1; }
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
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 2; // scene + shadow
    ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&dsv_heap);
    dsv_increment = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    memset(&hd, 0, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
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
    create_shadow_map();

    if (!create_shadow_pipeline() || !create_main_pipeline()) {
        logf_line("pipeline creation failed"); return 1;
    }
    create_geometry();

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
        D3D12_CPU_DESCRIPTOR_HANDLE dsv;
        D3D12_VIEWPORT vp; D3D12_RECT scissor;
        Mat4 cube_world, plane_world, cam_view, cam_proj, cam_vp;
        Mat4 light_view, light_proj, light_vp;
        float lx, ly, lz, light_len;
        float clear[4] = { 0.10f, 0.12f, 0.16f, 1.0f };
        float light_consts[4];
        Mat4 cam_mvp, light_mvp;

        nk_input_begin(ctx);
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        nk_input_end(ctx);

        build_ui(ctx, &pause, &speed, &light_ang, &bias);

        now = GetTickCount64();
        dt = (now - last_tick) / 1000.0f;
        last_tick = now;
        if (!pause) angle += dt * speed;

        // Cube floats above the plane and rotates.
        cube_world = mat_mul(mat_mul(mat_rot_y(angle), mat_rot_x(angle * 0.5f)), mat_translate(0.0f, 0.4f, 0.0f));
        plane_world = mat_identity();

        cam_view = mat_lookat_origin_lh(3.5f, 4.0f, -7.0f);
        cam_proj = mat_perspective_lh(3.14159265f / 4.0f, (float)g_width / (float)g_height, 0.1f, 100.0f);
        cam_vp = mat_mul(cam_view, cam_proj);

        // Directional light orbiting overhead.
        lx = cosf(light_ang) * 6.0f; ly = 7.0f; lz = sinf(light_ang) * 6.0f;
        light_view = mat_lookat_origin_lh(lx, ly, lz);
        light_proj = mat_ortho_lh(16.0f, 16.0f, 0.1f, 40.0f);
        light_vp = mat_mul(light_view, light_proj);
        light_len = sqrtf(lx*lx + ly*ly + lz*lz);
        light_consts[0] = lx / light_len; light_consts[1] = ly / light_len; light_consts[2] = lz / light_len;
        light_consts[3] = bias;

        // ---- PASS 1: shadow map (depth from light) ----
        transition(shadow_map, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 0, NULL, FALSE, &shadow_dsv);
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, shadow_dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
        vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = SHADOW_SIZE; vp.Height = SHADOW_SIZE;
        vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
        scissor.left = 0; scissor.top = 0; scissor.right = SHADOW_SIZE; scissor.bottom = SHADOW_SIZE;
        ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &scissor);
        ID3D12GraphicsCommandList_SetPipelineState(command_list, shadow_pso);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, shadow_root_sig);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_IASetVertexBuffers(command_list, 0, 1, &vbv);
        ID3D12GraphicsCommandList_IASetIndexBuffer(command_list, &ibv);
        light_mvp = mat_mul(cube_world, light_vp);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 16, light_mvp.m, 0);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, CUBE_INDEX_COUNT, 1, 0, 0, 0);
        light_mvp = mat_mul(plane_world, light_vp);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 16, light_mvp.m, 0);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, PLANE_INDEX_COUNT, 1, CUBE_INDEX_COUNT, 0, 0);
        transition(shadow_map, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // ---- PASS 2: main scene, sampling the shadow map ----
        transition(rtv_buffers[rtv_index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap, &dsv);
        ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &rtv_handles[rtv_index], FALSE, &dsv);
        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, rtv_handles[rtv_index], clear, 0, NULL);
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
        vp.Width = (float)g_width; vp.Height = (float)g_height;
        scissor.right = g_width; scissor.bottom = g_height;
        ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &scissor);
        ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &srv_heap);
        ID3D12GraphicsCommandList_SetPipelineState(command_list, main_pso);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, main_root_sig);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 2, shadow_srv);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 1, 4, light_consts, 0);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_IASetVertexBuffers(command_list, 0, 1, &vbv);
        ID3D12GraphicsCommandList_IASetIndexBuffer(command_list, &ibv);
        // cube
        cam_mvp = mat_mul(cube_world, cam_vp);
        light_mvp = mat_mul(cube_world, light_vp);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 16, cam_mvp.m, 0);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 16, light_mvp.m, 16);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 16, cube_world.m, 32);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, CUBE_INDEX_COUNT, 1, 0, 0, 0);
        // plane
        cam_mvp = mat_mul(plane_world, cam_vp);
        light_mvp = mat_mul(plane_world, light_vp);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 16, cam_mvp.m, 0);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 16, light_mvp.m, 16);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 16, plane_world.m, 32);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, PLANE_INDEX_COUNT, 1, CUBE_INDEX_COUNT, 0, 0);

        nk_d3d12_render(command_list, NK_ANTI_ALIASING_ON);

        transition(rtv_buffers[rtv_index], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        execute_commands();

        hr = IDXGISwapChain1_Present(swap_chain, 1, 0);
        rtv_index = (rtv_index + 1) % 2;
        if (hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_REMOVED) {
            logf_line("device lost on Present"); break;
        } else if (hr == DXGI_STATUS_OCCLUDED) { Sleep(10); }
    }

    nk_d3d12_shutdown();
    signal_and_wait();
    ID3D12PipelineState_Release(shadow_pso);
    ID3D12RootSignature_Release(shadow_root_sig);
    ID3D12PipelineState_Release(main_pso);
    ID3D12RootSignature_Release(main_root_sig);
    ID3D12Resource_Release(shadow_map);
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
