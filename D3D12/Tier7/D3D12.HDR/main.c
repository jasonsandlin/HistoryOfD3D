// =====================================================================
// D3D12.HDR - negotiating HDR10 output for a Direct3D 12 swap chain.
//
// This sample starts from the Fundamentals cube and changes only the pieces
// needed for high-dynamic-range presentation:
//
// 1. A 10-bit DXGI_FORMAT_R10G10B10A2_UNORM swap chain and matching PSOs.
// 2. IDXGISwapChain3 color-space negotiation for HDR10 vs SDR output.
// 3. IDXGIOutput6 display capability logging: current color space and nits.
// 4. A pixel-shader paper-white/brightness control that makes HDR intent clear.
//
// [LEARN] HDR is worth the added complexity when content contains useful values
// above SDR white: specular highlights, emissive surfaces, skies, or filmed HDR
// media. It also requires capability checks because the same executable must run
// correctly on SDR monitors, HDR monitors with Windows HDR off, and remote GPUs.
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
#define HDR_SWAP_FORMAT DXGI_FORMAT_R10G10B10A2_UNORM

// [DEBUG] Append diagnostics to a log file instead of popping dialogs.
static void logf_line(const char *fmt, ...)
{
    va_list ap; FILE *f = fopen("CubeD3D12.HDR.log", "a");
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
static IDXGISwapChain3 *swap_chain3;
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
static ID3D12PipelineState *pso;
static ID3D12Resource *vbuffer;
static ID3D12Resource *ibuffer;
static D3D12_VERTEX_BUFFER_VIEW vbv;
static D3D12_INDEX_BUFFER_VIEW ibv;

static int g_width = WINDOW_WIDTH, g_height = WINDOW_HEIGHT;
static int g_hdr_requested = 1;
static int g_hdr_supported = 0;
static int g_windows_hdr_on = 0;
static int g_hdr_active = 0;
static int g_has_output6 = 0;
static float g_output_max_lum = 0.0f;
static float g_output_min_lum = 0.0f;
static DXGI_COLOR_SPACE_TYPE g_output_color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
static DXGI_COLOR_SPACE_TYPE g_current_color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

static const char *g_shader =
"cbuffer CB : register(b0) { row_major float4x4 g_mvp; }\n"
"cbuffer HDRCB : register(b1) { float g_brightness; float g_paper_white; float g_hdr_active; }\n"
"struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
"VSOut VSMain(float3 pos : POSITION, float4 col : COLOR) {\n"
"  VSOut o; o.pos = mul(float4(pos, 1.0), g_mvp); o.col = col; return o;\n"
"}\n"
"float3 pq_encode(float3 nits) {\n"
"  float3 L = saturate(nits / 10000.0);\n"
"  float m1 = 2610.0 / 16384.0;\n"
"  float m2 = 2523.0 / 32.0;\n"
"  float c1 = 3424.0 / 4096.0;\n"
"  float c2 = 2413.0 / 128.0;\n"
"  float c3 = 2392.0 / 128.0;\n"
"  float3 Lm = pow(L, m1);\n"
"  return pow((c1 + c2 * Lm) / (1.0 + c3 * Lm), m2);\n"
"}\n"
"float4 PSMain(VSOut i) : SV_TARGET {\n"
"  float3 sdr = saturate(i.col.rgb * g_brightness);\n"
"  // [LEARN] HDR10 stores PQ-encoded nits; brightness changes luminance before encode.\n"
"  float3 hdr = pq_encode(max(i.col.rgb, 0.0) * g_paper_white * g_brightness);\n"
"  return float4(lerp(sdr, hdr, g_hdr_active), 1);\n"
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

static const char *color_space_name(DXGI_COLOR_SPACE_TYPE cs)
{
    switch (cs) {
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return "HDR10 RGB_FULL_G2084_P2020";
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: return "SDR RGB_FULL_G22_P709";
    default: return "Other/unknown";
    }
}

static void query_output_hdr_info(void)
{
    IDXGIOutput *output = NULL;
    IDXGIOutput6 *output6 = NULL;
    DXGI_OUTPUT_DESC1 desc1;
    HRESULT hr;
    g_has_output6 = 0; g_windows_hdr_on = 0;
    g_output_max_lum = 0.0f; g_output_min_lum = 0.0f;
    g_output_color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    hr = IDXGISwapChain1_GetContainingOutput(swap_chain, &output);
    if (FAILED(hr) || !output) { logf_line("GetContainingOutput hr=0x%08X", (unsigned)hr); return; }
    // [LEARN] IDXGIOutput6 exposes HDR display metadata; older outputs run as SDR.
    hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput6, (void **)&output6);
    if (SUCCEEDED(hr) && output6) {
        memset(&desc1, 0, sizeof(desc1));
        hr = IDXGIOutput6_GetDesc1(output6, &desc1);
        if (SUCCEEDED(hr)) {
            g_has_output6 = 1;
            g_output_max_lum = desc1.MaxLuminance;
            g_output_min_lum = desc1.MinLuminance;
            g_output_color_space = desc1.ColorSpace;
            g_windows_hdr_on = (desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
            logf_line("Output6 ColorSpace=%s MaxLuminance=%.1f MinLuminance=%.4f WindowsHDR=%d",
                color_space_name(desc1.ColorSpace), desc1.MaxLuminance, desc1.MinLuminance, g_windows_hdr_on);
        } else logf_line("IDXGIOutput6_GetDesc1 hr=0x%08X", (unsigned)hr);
        IDXGIOutput6_Release(output6);
    } else logf_line("IDXGIOutput6 unavailable hr=0x%08X", (unsigned)hr);
    IDXGIOutput_Release(output);
}

static void apply_color_space(void)
{
    UINT support = 0;
    HRESULT hr;
    DXGI_COLOR_SPACE_TYPE desired = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    g_hdr_supported = 0;
    query_output_hdr_info();
    if (!swap_chain3) {
        logf_line("IDXGISwapChain3 unavailable; color-space negotiation forced to SDR");
        g_current_color_space = desired; g_hdr_active = 0; return;
    }
    hr = IDXGISwapChain3_CheckColorSpaceSupport(swap_chain3,
        DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &support);
    if (SUCCEEDED(hr))
        g_hdr_supported = !!(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT);
    logf_line("CheckColorSpaceSupport HDR10 hr=0x%08X support=0x%X present=%d",
        (unsigned)hr, support, g_hdr_supported);
    // [LEARN] HDR10 is used only when the swap chain, output, and OS HDR mode all agree.
    if (g_hdr_requested && g_hdr_supported && g_windows_hdr_on)
        desired = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    hr = IDXGISwapChain3_SetColorSpace1(swap_chain3, desired);
    if (FAILED(hr)) {
        logf_line("SetColorSpace1(%s) hr=0x%08X; falling back to SDR",
            color_space_name(desired), (unsigned)hr);
        desired = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        IDXGISwapChain3_SetColorSpace1(swap_chain3, desired);
    }
    g_current_color_space = desired;
    g_hdr_active = (desired == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
    logf_line("Current color space: %s", color_space_name(g_current_color_space));
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

static void recreate_nuklear_pso_for_format(DXGI_FORMAT format)
{
    D3D12_INPUT_ELEMENT_DESC layout[3];
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
    ID3D12PipelineState *new_pso = NULL;
    HRESULT hr;
    layout[0].SemanticName = "POSITION"; layout[0].SemanticIndex = 0; layout[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; layout[0].InputSlot = 0; layout[0].AlignedByteOffset = NK_OFFSETOF(struct nk_d3d12_vertex, position); layout[0].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA; layout[0].InstanceDataStepRate = 0;
    layout[1].SemanticName = "TEXCOORD"; layout[1].SemanticIndex = 0; layout[1].Format = DXGI_FORMAT_R32G32_FLOAT; layout[1].InputSlot = 0; layout[1].AlignedByteOffset = NK_OFFSETOF(struct nk_d3d12_vertex, uv); layout[1].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA; layout[1].InstanceDataStepRate = 0;
    layout[2].SemanticName = "COLOR"; layout[2].SemanticIndex = 0; layout[2].Format = DXGI_FORMAT_R8G8B8A8_UNORM; layout[2].InputSlot = 0; layout[2].AlignedByteOffset = NK_OFFSETOF(struct nk_d3d12_vertex, col); layout[2].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA; layout[2].InstanceDataStepRate = 0;
    memset(&desc, 0, sizeof(desc));
    desc.pRootSignature = d3d12.root_signature;
    desc.VS.pShaderBytecode = nk_d3d12_vertex_shader;
    desc.VS.BytecodeLength = sizeof(nk_d3d12_vertex_shader);
    desc.PS.pShaderBytecode = nk_d3d12_pixel_shader;
    desc.PS.BytecodeLength = sizeof(nk_d3d12_pixel_shader);
    desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.InputLayout.NumElements = 3;
    desc.InputLayout.pInputElementDescs = layout;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.SampleDesc.Count = 1;
    hr = ID3D12Device_CreateGraphicsPipelineState(device, &desc, &IID_ID3D12PipelineState, (void **)&new_pso);
    if (SUCCEEDED(hr) && new_pso) {
        ID3D12PipelineState_Release(d3d12.pipeline_state);
        d3d12.pipeline_state = new_pso;
        logf_line("Nuklear PSO RTV format updated to R10G10B10A2_UNORM");
    } else logf_line("Nuklear HDR PSO creation hr=0x%08X", (unsigned)hr);
}

static int create_cube_pipeline(void)
{
    D3D12_ROOT_PARAMETER rp[2];
    D3D12_ROOT_SIGNATURE_DESC rsd;
    D3D12_INPUT_ELEMENT_DESC il[2];
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    ID3DBlob *sig = NULL, *sigerr = NULL, *vs = NULL, *ps = NULL, *err = NULL;
    UINT i; HRESULT hr;

    memset(rp, 0, sizeof(rp));
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0;
    rp[0].Constants.Num32BitValues = 16;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.ShaderRegister = 1;
    rp[1].Constants.Num32BitValues = 3;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    memset(&rsd, 0, sizeof(rsd));
    rsd.NumParameters = 2; rsd.pParameters = rp;
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
    pd.RTVFormats[0] = HDR_SWAP_FORMAT;
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
    apply_color_space();
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

static void build_ui(struct nk_context *ctx, int *pause, float *speed, float *brightness, float *paper_white)
{
    if (nk_begin(ctx, "D3D12 HDR", nk_rect(20, 20, 430, 620),
        NK_WINDOW_BORDER|NK_WINDOW_MOVABLE|NK_WINDOW_SCALABLE|NK_WINDOW_TITLE))
    {
        char buf[160];
        int old_request = g_hdr_requested;
        if (nk_tree_push(ctx, NK_TREE_TAB, "HDR negotiation", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_checkbox_label(ctx, "Request HDR10 color space", &g_hdr_requested);
            if (old_request != g_hdr_requested) apply_color_space();
            nk_label(ctx, "Swap-chain format: R10G10B10A2_UNORM", NK_TEXT_LEFT);
            sprintf(buf, "Current color space: %s", color_space_name(g_current_color_space)); nk_label(ctx, buf, NK_TEXT_LEFT);
            sprintf(buf, "HDR10 supported by swap chain: %s", g_hdr_supported ? "yes" : "no"); nk_label(ctx, buf, NK_TEXT_LEFT);
            sprintf(buf, "Windows HDR enabled on output: %s", g_windows_hdr_on ? "yes" : "no"); nk_label(ctx, buf, NK_TEXT_LEFT);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Display info", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 18, 1);
            sprintf(buf, "IDXGIOutput6: %s", g_has_output6 ? "available" : "unavailable"); nk_label(ctx, buf, NK_TEXT_LEFT);
            sprintf(buf, "Output color space: %s", color_space_name(g_output_color_space)); nk_label(ctx, buf, NK_TEXT_LEFT);
            sprintf(buf, "Max luminance: %.1f nits", g_output_max_lum); nk_label(ctx, buf, NK_TEXT_LEFT);
            sprintf(buf, "Min luminance: %.4f nits", g_output_min_lum); nk_label(ctx, buf, NK_TEXT_LEFT);
            nk_tree_pop(ctx);
        }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Brightness", NK_MAXIMIZED)) {
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_property_float(ctx, "Brightness", 0.10f, brightness, 8.00f, 0.10f, 0.02f);
            nk_property_float(ctx, "Paper white nits", 80.0f, paper_white, 400.0f, 5.0f, 1.0f);
            nk_layout_row_dynamic(ctx, 16, 1);
            nk_label_wrap(ctx, "Paper white maps ordinary diffuse colors to a comfortable white level; brightness pushes highlights above SDR white on HDR displays.");
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
    float speed = 0.7f, angle = 0.0f, brightness = 1.5f, paper_white = 203.0f;
    ULONGLONG last_tick;
    WNDCLASSW wc;
    RECT rect = { 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT };
    DWORD style = WS_OVERLAPPEDWINDOW;
    HWND wnd; int running = 1; HRESULT hr;
    D3D12_COMMAND_QUEUE_DESC qd; DXGI_SWAP_CHAIN_DESC1 scd; D3D12_DESCRIPTOR_HEAP_DESC hd;

    logf_line("---- D3D12.HDR startup ----");
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(0);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"D3D12HDRClass";
    RegisterClassW(&wc);
    AdjustWindowRectEx(&rect, style, FALSE, WS_EX_APPWINDOW);
    wnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName,
        L"D3D12 HDR - HDR10 color-space negotiation",
        style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, wc.hInstance, NULL);

    hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device);
    if (FAILED(hr)) { logf_line("D3D12CreateDevice hr=0x%08X", (unsigned)hr); MessageBoxW(wnd, L"D3D12 device creation failed", L"Error", 0); return 1; }
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
    scd.Format = HDR_SWAP_FORMAT;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.Scaling = DXGI_SCALING_STRETCH;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    IDXGIFactory2_CreateSwapChainForHwnd(dxgi_factory, (IUnknown *)command_queue, wnd,
        &scd, NULL, NULL, &swap_chain);
    if (FAILED(IDXGISwapChain1_QueryInterface(swap_chain, &IID_IDXGISwapChain3, (void **)&swap_chain3))) {
        swap_chain3 = NULL;
        logf_line("IDXGISwapChain3 unavailable");
    }
    get_swap_chain_buffers();
    create_depth_buffer(WINDOW_WIDTH, WINDOW_HEIGHT);
    apply_color_space();

    if (!create_cube_pipeline()) { logf_line("pipeline creation failed"); return 1; }

    ctx = nk_d3d12_init(device, WINDOW_WIDTH, WINDOW_HEIGHT, MAX_VERTEX_BUFFER, MAX_INDEX_BUFFER, 1);
    recreate_nuklear_pso_for_format(HDR_SWAP_FORMAT);
    {
        struct nk_font_atlas *atlas;
        nk_d3d12_font_stash_begin(&atlas);
        nk_d3d12_font_stash_end(command_list);
    }
    execute_commands();
    nk_d3d12_font_stash_cleanup();

    last_tick = GetTickCount64();
    while (running) {
        MSG msg; ULONGLONG now; float dt; float hdr_consts[3];
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

        build_ui(ctx, &pause, &speed, &brightness, &paper_white);

        now = GetTickCount64();
        dt = (now - last_tick) / 1000.0f;
        last_tick = now;
        if (!pause) angle += dt * speed;

        world = mat_mul(mat_rot_y(angle), mat_rot_x(angle * 0.5f));
        view = mat_lookat_lh(0.0f, 0.0f, -4.5f);
        proj = mat_perspective_lh(3.14159265f / 4.0f, (float)g_width / (float)g_height, 0.1f, 100.0f);
        mvp = mat_mul(mat_mul(world, view), proj);
        hdr_consts[0] = brightness;
        hdr_consts[1] = paper_white;
        hdr_consts[2] = g_hdr_active ? 1.0f : 0.0f;

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

        ID3D12GraphicsCommandList_SetPipelineState(command_list, pso);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, root_sig);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 0, 16, mvp.m, 0);
        // [LEARN] Root constants are ideal for tiny per-draw HDR controls; no CBV heap is needed.
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(command_list, 1, 3, hdr_consts, 0);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_IASetVertexBuffers(command_list, 0, 1, &vbv);
        ID3D12GraphicsCommandList_IASetIndexBuffer(command_list, &ibv);
        ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, 36, 1, 0, 0, 0);

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
    ID3D12PipelineState_Release(pso);
    ID3D12RootSignature_Release(root_sig);
    ID3D12Resource_Release(vbuffer);
    ID3D12Resource_Release(ibuffer);
    ID3D12Resource_Release(depth_buffer);
    ID3D12Resource_Release(rtv_buffers[0]);
    ID3D12Resource_Release(rtv_buffers[1]);
    ID3D12DescriptorHeap_Release(rtv_heap);
    ID3D12DescriptorHeap_Release(dsv_heap);
    if (swap_chain3) IDXGISwapChain3_Release(swap_chain3);
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
