// =====================================================================
// D3D12.DeferredMRT - a deferred G-buffer with Multiple Render Targets.
//
// Deferred shading splits rendering into two passes:
//   PASS 1 (geometry): draw the cube ONCE into several render targets at the
//     same time - a "G-buffer". Here RT0 stores albedo (surface color) and RT1
//     stores the world-space normal. This is what "MRT" means: a pixel shader
//     writes SV_Target0, SV_Target1, ... simultaneously.
//   PASS 2 (lighting): draw a single fullscreen triangle that READS the G-buffer
//     textures as SRVs and computes lighting per screen pixel. Geometry cost is
//     paid once; any number of lights would reuse the same G-buffer.
//
// [LEARN] Key pieces:
//   1. G-buffer textures are created with ALLOW_RENDER_TARGET and have BOTH an
//      RTV (to be drawn into) and an SRV (to be read in the lighting pass).
//   2. The geometry PSO sets NumRenderTargets = 2 and a PSOut struct writes
//      SV_Target0 (albedo) and SV_Target1 (normal).
//   3. Between passes the G-buffer transitions RENDER_TARGET -> PIXEL_SHADER_
//      RESOURCE so the lighting pass can sample it.
//   4. The fullscreen triangle uses SV_VertexID - no vertex buffer needed.
//
// [LEARN] The UI "View" control shows the raw albedo/normal buffers or the
// final lit result, so you can SEE what each G-buffer target holds.
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
    va_list ap; FILE *f = fopen("CubeD3D12.DeferredMRT.log", "a");
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

typedef struct { float px, py, pz; float r, g, b, a; } Vertex;
// 8 cube corners, each a distinct albedo color.
static const Vertex CUBE_VERTS[8] = {
    { -1,-1,-1, 0.90f,0.30f,0.30f,1 }, { -1, 1,-1, 0.90f,0.75f,0.25f,1 },
    {  1, 1,-1, 0.85f,0.85f,0.30f,1 }, {  1,-1,-1, 0.35f,0.80f,0.40f,1 },
    { -1,-1, 1, 0.30f,0.75f,0.85f,1 }, { -1, 1, 1, 0.35f,0.50f,0.90f,1 },
    {  1, 1, 1, 0.65f,0.40f,0.90f,1 }, {  1,-1, 1, 0.90f,0.90f,0.90f,1 },
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

// rtv_heap: slots 0,1 = back buffers; slots 2,3 = G-buffer albedo/normal.
static ID3D12DescriptorHeap *rtv_heap;
static ID3D12DescriptorHeap *dsv_heap;
static ID3D12DescriptorHeap *srv_heap;   // shader-visible: slot0 albedo, slot1 normal
static UINT rtv_increment;
static UINT srv_increment;
static ID3D12Resource *rtv_buffers[2];
static D3D12_CPU_DESCRIPTOR_HANDLE rtv_handles[2];
static ID3D12Resource *depth_buffer;
static UINT rtv_index;

// G-buffer targets.
static ID3D12Resource *gbuf_albedo;
static ID3D12Resource *gbuf_normal;
static D3D12_CPU_DESCRIPTOR_HANDLE gbuf_rtv[2];   // albedo, normal
static D3D12_GPU_DESCRIPTOR_HANDLE gbuf_srv_start; // table start (albedo at t0)

static ID3D12RootSignature *geo_root_sig;
static ID3D12PipelineState *geo_pso;
static ID3D12RootSignature *light_root_sig;
static ID3D12PipelineState *light_pso;
static ID3D12Resource *vbuffer;
static ID3D12Resource *ibuffer;
static D3D12_VERTEX_BUFFER_VIEW vbv;
static D3D12_INDEX_BUFFER_VIEW ibv;

static int g_width = WINDOW_WIDTH, g_height = WINDOW_HEIGHT;

// Geometry pass: writes albedo (SV_Target0) and world normal (SV_Target1).
static const char *g_geo_shader =
"cbuffer CB : register(b0) { row_major float4x4 g_mvp; row_major float4x4 g_world; }\n"
"struct VSOut { float4 pos : SV_POSITION; float3 nrm : NORMAL; float3 col : COLOR; };\n"
"VSOut VSMain(float3 pos : POSITION, float4 col : COLOR) {\n"
"  VSOut o; o.pos = mul(float4(pos, 1.0), g_mvp);\n"
"  o.nrm = normalize(mul(float4(normalize(pos), 0.0), g_world).xyz);\n"
"  o.col = col.rgb; return o;\n"
"}\n"
"struct PSOut { float4 albedo : SV_Target0; float4 normal : SV_Target1; };\n"
"PSOut PSMain(VSOut i) {\n"
"  PSOut o;\n"
"  o.albedo = float4(i.col, 1.0);\n"
"  // [LEARN] pack a [-1,1] normal into an [0,1] RGBA8 target.\n"
"  o.normal = float4(normalize(i.nrm) * 0.5 + 0.5, 1.0);\n"
"  return o;\n"
"}\n";

// Lighting pass: fullscreen triangle reads the two G-buffer SRVs.
static const char *g_light_shader =
"Texture2D g_albedo : register(t0);\n"
"Texture2D g_normal : register(t1);\n"
"SamplerState g_samp : register(s0);\n"
"cbuffer L : register(b0) { float3 g_lightDir; uint g_mode; }\n"
"struct VOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };\n"
"VOut VSMain(uint vid : SV_VertexID) {\n"
"  VOut o; o.uv = float2((vid << 1) & 2, vid & 2);\n"     // (0,0)(2,0)(0,2)
"  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
"  return o;\n"
"}\n"
"float4 PSMain(VOut i) : SV_TARGET {\n"
"  float3 alb = g_albedo.Sample(g_samp, i.uv).rgb;\n"
"  float3 nrm = g_normal.Sample(g_samp, i.uv).rgb * 2.0 - 1.0;\n"
"  if (g_mode == 1) return float4(alb, 1);\n"             // view albedo buffer
"  if (g_mode == 2) return float4(nrm * 0.5 + 0.5, 1);\n" // view normal buffer
"  float ndl = saturate(dot(normalize(nrm), normalize(g_lightDir)));\n"
"  float3 lit = alb * (0.15 + 0.85 * ndl);\n"
"  return float4(lit, 1);\n"
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

// [LEARN] A G-buffer target: RENDER_TARGET-capable, with an RTV and an SRV.
static ID3D12Resource *create_gbuffer(int width, int height, UINT rtv_slot, UINT srv_slot)
{
    D3D12_HEAP_PROPERTIES hp; D3D12_RESOURCE_DESC rd; D3D12_CLEAR_VALUE cv;
    ID3D12Resource *tex = NULL;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv, srv;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&hp, 0, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    memset(&rd, 0, sizeof(rd));
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = (UINT)width; rd.Height = (UINT)height;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    memset(&cv, 0, sizeof(cv));
    cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    // create in PIXEL_SHADER_RESOURCE so the per-frame PSR->RT->PSR loop is uniform.
    ID3D12Device_CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv, &IID_ID3D12Resource, (void **)&tex);

    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap, &rtv);
    rtv.ptr += (SIZE_T)rtv_slot * rtv_increment;
    ID3D12Device_CreateRenderTargetView(device, tex, NULL, rtv);
    gbuf_rtv[rtv_slot - 2] = rtv;

    memset(&sd, 0, sizeof(sd));
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(srv_heap, &srv);
    srv.ptr += (SIZE_T)srv_slot * srv_increment;
    ID3D12Device_CreateShaderResourceView(device, tex, &sd, srv);
    return tex;
}

static void create_gbuffers(int width, int height)
{
    gbuf_albedo = create_gbuffer(width, height, 2, 0);
    gbuf_normal = create_gbuffer(width, height, 3, 1);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(srv_heap, &gbuf_srv_start);
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

static int create_geo_pipeline(void)
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
    rp.Constants.Num32BitValues = 32;   // mvp(16) + world(16)
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    memset(&rsd, 0, sizeof(rsd));
    rsd.NumParameters = 1; rsd.pParameters = &rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigerr);
    if (FAILED(hr)) { logf_line("geo rootsig hr=0x%08X", (unsigned)hr); log_blob("geo rs err", sigerr); if (sigerr) ID3D10Blob_Release(sigerr); return 0; }
    ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(sig),
        ID3D10Blob_GetBufferSize(sig), &IID_ID3D12RootSignature, (void **)&geo_root_sig);
    ID3D10Blob_Release(sig);

    hr = D3DCompile(g_geo_shader, strlen(g_geo_shader), NULL, NULL, NULL, "VSMain", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr)) { logf_line("geo VS hr=0x%08X", (unsigned)hr); log_blob("geo VS err", err); if (err) ID3D10Blob_Release(err); return 0; }
    hr = D3DCompile(g_geo_shader, strlen(g_geo_shader), NULL, NULL, NULL, "PSMain", "ps_5_0", 0, 0, &ps, &err);
    if (FAILED(hr)) { logf_line("geo PS hr=0x%08X", (unsigned)hr); log_blob("geo PS err", err); if (err) ID3D10Blob_Release(err); return 0; }

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
    pd.pRootSignature = geo_root_sig;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
    pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps);
    pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
    pd.InputLayout.pInputElementDescs = il;
    pd.InputLayout.NumElements = 2;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    // [LEARN] MRT: two render targets written in one draw.
    pd.NumRenderTargets = 2;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
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
    ID3D12Device_CreateGraphicsPipelineState(device, &pd, &IID_ID3D12PipelineState, (void **)&geo_pso);
    if (!geo_pso) logf_line("geo CreateGraphicsPipelineState NULL");
    ID3D10Blob_Release(vs); ID3D10Blob_Release(ps);
    return (geo_root_sig && geo_pso);
}

static int create_light_pipeline(void)
{
    D3D12_ROOT_PARAMETER rp[2];
    D3D12_DESCRIPTOR_RANGE range;
    D3D12_STATIC_SAMPLER_DESC samp;
    D3D12_ROOT_SIGNATURE_DESC rsd;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    ID3DBlob *sig = NULL, *sigerr = NULL, *vs = NULL, *ps = NULL, *err = NULL;
    UINT i; HRESULT hr;

    memset(rp, 0, sizeof(rp));
    // t0-t1: the two G-buffer SRVs.
    memset(&range, 0, sizeof(range));
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1;
    rp[0].DescriptorTable.pDescriptorRanges = &range;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    // b0: light dir + view mode.
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.ShaderRegister = 0;
    rp[1].Constants.Num32BitValues = 4;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    memset(&samp, 0, sizeof(samp));
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    memset(&rsd, 0, sizeof(rsd));
    rsd.NumParameters = 2; rsd.pParameters = rp;
    rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &samp;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigerr);
    if (FAILED(hr)) { logf_line("light rootsig hr=0x%08X", (unsigned)hr); log_blob("light rs err", sigerr); if (sigerr) ID3D10Blob_Release(sigerr); return 0; }
    ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(sig),
        ID3D10Blob_GetBufferSize(sig), &IID_ID3D12RootSignature, (void **)&light_root_sig);
    ID3D10Blob_Release(sig);

    hr = D3DCompile(g_light_shader, strlen(g_light_shader), NULL, NULL, NULL, "VSMain", "vs_5_0", 0, 0, &vs, &err);
    if (FAILED(hr)) { logf_line("light VS hr=0x%08X", (unsigned)hr); log_blob("light VS err", err); if (err) ID3D10Blob_Release(err); return 0; }
    hr = D3DCompile(g_light_shader, strlen(g_light_shader), NULL, NULL, NULL, "PSMain", "ps_5_0", 0, 0, &ps, &err);
    if (FAILED(hr)) { logf_line("light PS hr=0x%08X", (unsigned)hr); log_blob("light PS err", err); if (err) ID3D10Blob_Release(err); return 0; }

    memset(&pd, 0, sizeof(pd));
    pd.pRootSignature = light_root_sig;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
    pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps);
    pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
    // [LEARN] no input layout - the VS builds the fullscreen triangle from SV_VertexID.
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    for (i = 0; i < 8; i++)
        pd.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = FALSE;
    ID3D12Device_CreateGraphicsPipelineState(device, &pd, &IID_ID3D12PipelineState, (void **)&light_pso);
    if (!light_pso) logf_line("light CreateGraphicsPipelineState NULL");
    ID3D10Blob_Release(vs); ID3D10Blob_Release(ps);
    return (light_root_sig && light_pso);
}

static void create_geometry(void)
{
    vbuffer = create_upload_buffer(CUBE_VERTS, sizeof(CUBE_VERTS));
    vbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vbuffer);
    vbv.StrideInBytes = sizeof(Vertex);
    vbv.SizeInBytes = sizeof(CUBE_VERTS);
    ibuffer = create_upload_buffer(CUBE_IDX, sizeof(CUBE_IDX));
    ibv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(ibuffer);
    ibv.Format = DXGI_FORMAT_R16_UINT;
    ibv.SizeInBytes = sizeof(CUBE_IDX);
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
    ID3D12Resource_Release(gbuf_albedo);
    ID3D12Resource_Release(gbuf_normal);
    IDXGISwapChain1_ResizeBuffers(swap_chain, 2, (UINT)width, (UINT)height,
        DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
    get_swap_chain_buffers();
    create_depth_buffer(width, height);
    create_gbuffers(width, height);
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

static void build_ui(struct nk_context *ctx, int *pause, float *speed, int *mode, float *light_ang)
{
    static const char *modes[] = { "Lit (deferred)", "Albedo buffer", "Normal buffer" };
    if (nk_begin(ctx, "D3D12 Deferred MRT", nk_rect(20, 20, 380, 560),
        NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_SCALABLE|NK_WINDOW_TITLE))
    {
        if (nk_tree_push(ctx, NK_TREE_TAB, "Two passes", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 16, 1);
            nk_label(ctx, "Pass 1: draw cube -> albedo + normal (MRT).", NK_TEXT_LEFT);
            nk_label(ctx, "Pass 2: fullscreen triangle lights from G-buffer.", NK_TEXT_LEFT);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "View", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 24, 1);
            *mode = nk_combo(ctx, modes, 3, *mode, 24, nk_vec2(320, 120));
            nk_label(ctx, "See what each G-buffer target stores.", NK_TEXT_LEFT);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Controls", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_checkbox_label(ctx, "Pause rotation", pause);
            nk_property_float(ctx, "Cube speed", 0.0f, speed, 4.0f, 0.1f, 0.02f);
            nk_property_float(ctx, "Light angle", 0.0f, light_ang, 6.28f, 0.05f, 0.01f);
            nk_tree_pop(ctx);
        }
    }
    nk_end(ctx);
}

int main(void)
{
    struct nk_context *ctx;
    int pause = 0, mode = 0;
    float speed = 0.7f, angle = 0.0f, light_ang = 0.8f;
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
    wc.lpszClassName = L"D3D12DeferredMRTClass";
    RegisterClassW(&wc);
    AdjustWindowRectEx(&rect, style, FALSE, WS_EX_APPWINDOW);
    wnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName,
        L"D3D12 Deferred MRT - G-buffer + fullscreen lighting",
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
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 4; // 2 back + 2 gbuffer
    ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap);
    rtv_increment = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    memset(&hd, 0, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 1;
    ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&dsv_heap);
    memset(&hd, 0, sizeof(hd));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&srv_heap);
    srv_increment = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

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
    create_gbuffers(WINDOW_WIDTH, WINDOW_HEIGHT);

    if (!create_geo_pipeline() || !create_light_pipeline()) {
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
        D3D12_CPU_DESCRIPTOR_HANDLE gbuf_targets[2];
        D3D12_VIEWPORT vp; D3D12_RECT scissor;
        Mat4 world, view, proj, mvp;
        float black[4] = { 0, 0, 0, 1 };
        float light_consts[4];

        nk_input_begin(ctx);
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        nk_input_end(ctx);

        build_ui(ctx, &pause, &speed, &mode, &light_ang);

        now = GetTickCount64();
        dt = (now - last_tick) / 1000.0f;
        last_tick = now;
        if (!pause) angle += dt * speed;

        world = mat_mul(mat_rot_y(angle), mat_rot_x(angle * 0.5f));
        view = mat_lookat_lh(0.0f, 0.0f, -6.0f);
        proj = mat_perspective_lh(3.14159265f / 4.0f, (float)g_width / (float)g_height, 0.1f, 100.0f);
        mvp = mat_mul(mat_mul(world, view), proj);

        vp.TopLeftX = 0; vp.TopLeftY = 0; vp.Width = (float)g_width; vp.Height = (float)g_height;
        vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
        scissor.left = 0; scissor.top = 0; scissor.right = g_width; scissor.bottom = g_height;
        ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &scissor);
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap, &dsv);

        // ---- PASS 1: geometry into the G-buffer (MRT) ----
        transition(gbuf_albedo, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        transition(gbuf_normal, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        gbuf_targets[0] = gbuf_rtv[0]; gbuf_targets[1] = gbuf_rtv[1];
        ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 2, gbuf_targets, FALSE, &dsv);
        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, gbuf_rtv[0], black, 0, NULL);
        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, gbuf_rtv[1], black, 0, NULL);
        ID3D12GraphicsCommandList_ClearDepthStencilView(command_list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
        ID3D12GraphicsCommandList_SetPipelineState(command_list, geo_pso);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, geo_root_sig);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 16, mvp.m, 0);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 16, world.m, 16);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_IASetVertexBuffers(command_list, 0, 1, &vbv);
        ID3D12GraphicsCommandList_IASetIndexBuffer(command_list, &ibv);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, 36, 1, 0, 0, 0);

        // ---- PASS 2: fullscreen lighting reading the G-buffer ----
        transition(gbuf_albedo, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(gbuf_normal, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(rtv_buffers[rtv_index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12GraphicsCommandList_OMSetRenderTargets(command_list, 1, &rtv_handles[rtv_index], FALSE, NULL);
        ID3D12GraphicsCommandList_ClearRenderTargetView(command_list, rtv_handles[rtv_index], black, 0, NULL);

        ID3D12GraphicsCommandList_SetDescriptorHeaps(command_list, 1, &srv_heap);
        ID3D12GraphicsCommandList_SetPipelineState(command_list, light_pso);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, light_root_sig);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(command_list, 0, gbuf_srv_start);
        light_consts[0] = cosf(light_ang);
        light_consts[1] = 0.6f;
        light_consts[2] = sinf(light_ang);
        *(UINT *)&light_consts[3] = (UINT)mode;
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 1, 4, light_consts, 0);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

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
    ID3D12PipelineState_Release(geo_pso);
    ID3D12RootSignature_Release(geo_root_sig);
    ID3D12PipelineState_Release(light_pso);
    ID3D12RootSignature_Release(light_root_sig);
    ID3D12Resource_Release(gbuf_albedo);
    ID3D12Resource_Release(gbuf_normal);
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
