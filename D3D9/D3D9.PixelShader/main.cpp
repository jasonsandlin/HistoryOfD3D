// D3D9 Pixel Shader showcase.
// The vertex shader just transforms the cube and forwards its object-space
// position. The pixel shader (ps_3_0, compiled from HLSL at runtime) then
// computes an animated procedural color per pixel from that position and time,
// producing moving color bands across the cube faces. ESC quits.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

using namespace DirectX;

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dcompiler.lib")

struct Vertex { float x, y, z; float r, g, b; };

static IDirect3D9*       g_d3d = nullptr;
static IDirect3DDevice9* g_dev = nullptr;
static IDirect3DVertexBuffer9* g_vb = nullptr;
static IDirect3DIndexBuffer9*  g_ib = nullptr;
static IDirect3DVertexDeclaration9* g_decl = nullptr;
static IDirect3DVertexShader9* g_vs = nullptr;
static IDirect3DPixelShader9*  g_ps = nullptr;
static UINT g_width = 800, g_height = 600;

static const char* g_vsSrc =
"float4x4 g_mvp : register(c0);\n"
"struct VSIn  { float3 pos : POSITION; float3 col : COLOR; };\n"
"struct VSOut { float4 pos : POSITION; float3 opos : TEXCOORD0; };\n"
"VSOut main(VSIn i) {\n"
"  VSOut o; o.pos = mul(float4(i.pos, 1.0), g_mvp); o.opos = i.pos; return o;\n"
"}\n";

// [LEARN] Spotlight: this ps_3_0 pixel shader runs once per rasterized pixel and
// computes its color in code - the programmable replacement for D3D9's
// fixed-function texture/color blender.
static const char* g_psSrc =
"float g_time : register(c0);\n"
"struct VSOut { float4 pos : POSITION; float3 opos : TEXCOORD0; };\n"
"float4 main(VSOut i) : COLOR {\n"
"  float v = sin(i.opos.x * 6.0 + g_time * 3.0)\n"
"          * sin(i.opos.y * 6.0 - g_time * 2.0)\n"
"          * sin(i.opos.z * 6.0 + g_time);\n"
"  float3 c = 0.5 + 0.5 * cos(float3(0.0, 2.0, 4.0) + v * 3.14159 + g_time);\n"
"  return float4(c, 1.0);\n"
"}\n";

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN: if (w == VK_ESCAPE) PostQuitMessage(0); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, w, l);
}

static IDirect3DVertexShader9* CompileVS(const char* src) {
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "main", "vs_3_0", 0, 0, &code, &err))) {
        if (err) err->Release(); return nullptr;
    }
    IDirect3DVertexShader9* s = nullptr;
    g_dev->CreateVertexShader((const DWORD*)code->GetBufferPointer(), &s);
    code->Release();
    return s;
}

static IDirect3DPixelShader9* CompilePS(const char* src) {
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "main", "ps_3_0", 0, 0, &code, &err))) {
        if (err) err->Release(); return nullptr;
    }
    IDirect3DPixelShader9* s = nullptr;
    g_dev->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &s);
    code->Release();
    return s;
}

static bool InitD3D(HWND hwnd) {
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) return false;
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D16;
    if (FAILED(g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_dev)))
        return false;

    D3DVERTEXELEMENT9 decl[] = {
        { 0,  0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
        D3DDECL_END()
    };
    g_dev->CreateVertexDeclaration(decl, &g_decl);

    g_vs = CompileVS(g_vsSrc);
    g_ps = CompilePS(g_psSrc);
    if (!g_vs || !g_ps) return false;

    Vertex verts[] = {
        { -1,-1,-1, 0,0,0 }, { -1, 1,-1, 0,1,0 }, { 1, 1,-1, 1,1,0 }, { 1,-1,-1, 1,0,0 },
        { -1,-1, 1, 0,0,1 }, { -1, 1, 1, 0,1,1 }, { 1, 1, 1, 1,1,1 }, { 1,-1, 1, 1,0,1 },
    };
    unsigned short idx[] = {
        0,1,2, 0,2,3,  4,6,5, 4,7,6,  4,5,1, 4,1,0,
        3,2,6, 3,6,7,  1,5,6, 1,6,2,  4,0,3, 4,3,7,
    };
    void* p = nullptr;
    g_dev->CreateVertexBuffer(sizeof(verts), 0, 0, D3DPOOL_MANAGED, &g_vb, nullptr);
    g_vb->Lock(0, sizeof(verts), &p, 0); memcpy(p, verts, sizeof(verts)); g_vb->Unlock();
    g_dev->CreateIndexBuffer(sizeof(idx), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &g_ib, nullptr);
    g_ib->Lock(0, sizeof(idx), &p, 0); memcpy(p, idx, sizeof(idx)); g_ib->Unlock();

    g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    return true;
}

static void Render(float t) {
    g_dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(25, 25, 51), 1.0f, 0);
    g_dev->BeginScene();

    XMMATRIX world = XMMatrixRotationY(t) * XMMatrixRotationX(t * 0.5f);
    XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 0, -6, 0), XMVectorSet(0, 0, 0, 0), XMVectorSet(0, 1, 0, 0));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, (float)g_width / g_height, 0.1f, 100.0f);
    XMFLOAT4X4 mvp; XMStoreFloat4x4(&mvp, XMMatrixTranspose(world * view * proj));
    float timeC[4] = { t, 0, 0, 0 };

    g_dev->SetVertexDeclaration(g_decl);
    g_dev->SetVertexShader(g_vs);
    g_dev->SetPixelShader(g_ps);
    g_dev->SetVertexShaderConstantF(0, reinterpret_cast<const float*>(&mvp), 4);
    g_dev->SetPixelShaderConstantF(0, timeC, 1);
    g_dev->SetStreamSource(0, g_vb, 0, sizeof(Vertex));
    g_dev->SetIndices(g_ib);
    g_dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 8, 0, 12);

    g_dev->EndScene();
    g_dev->Present(nullptr, nullptr, nullptr, nullptr);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    WNDCLASS wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = "D3D9PS";
    RegisterClass(&wc);
    RECT r = { 0, 0, (LONG)g_width, (LONG)g_height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindow("D3D9PS", "D3D9 - Pixel Shader (procedural color)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, hInst, nullptr);
    if (!InitD3D(hwnd)) { MessageBox(hwnd, "D3D9 PS init failed", "Error", MB_OK); return 1; }
    ShowWindow(hwnd, SW_SHOW);

    DWORD start = GetTickCount();
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        else Render((GetTickCount() - start) / 1000.0f);
    }
    return (int)msg.wParam;
}
