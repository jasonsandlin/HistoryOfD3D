// Rotating cube using Direct3D 9 (fixed-function pipeline).
// Win32 window + windowed device, colored cube via FVF, spun with SetTransform.
// Matrices come from DirectXMath (row-major, matches D3D9's D3DMATRIX). ESC quits.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <DirectXMath.h>

using namespace DirectX;

#pragma comment(lib, "d3d9.lib")

struct Vertex { float x, y, z; DWORD color; };
// [LEARN] D3D9 fixed-function pipeline: geometry is described by an FVF (Flexible
// Vertex Format) and transformed by SetTransform() - there are NO shaders here.
// This is the classic pre-shader pipeline; contrast it with D3D9.VertexShader
// and D3D9.PixelShader, which replace these fixed stages with programmable HLSL.
static const DWORD FVF = D3DFVF_XYZ | D3DFVF_DIFFUSE;

static IDirect3D9*       g_d3d = nullptr;
static IDirect3DDevice9* g_dev = nullptr;
static IDirect3DVertexBuffer9* g_vb = nullptr;
static IDirect3DIndexBuffer9*  g_ib = nullptr;
static UINT g_width = 800, g_height = 600;

static void SetMatrix(D3DTRANSFORMSTATETYPE which, const XMMATRIX& m) {
    XMFLOAT4X4 f; XMStoreFloat4x4(&f, m);
    g_dev->SetTransform(which, reinterpret_cast<const D3DMATRIX*>(&f));
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN: if (w == VK_ESCAPE) PostQuitMessage(0); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, w, l);
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

    Vertex verts[] = {
        { -1,-1,-1, 0xff000000 }, { -1, 1,-1, 0xff00ff00 },
        {  1, 1,-1, 0xffffff00 }, {  1,-1,-1, 0xffff0000 },
        { -1,-1, 1, 0xff0000ff }, { -1, 1, 1, 0xff00ffff },
        {  1, 1, 1, 0xffffffff }, {  1,-1, 1, 0xffff00ff },
    };
    unsigned short idx[] = {
        0,1,2, 0,2,3,  4,6,5, 4,7,6,  4,5,1, 4,1,0,
        3,2,6, 3,6,7,  1,5,6, 1,6,2,  4,0,3, 4,3,7,
    };
    g_dev->CreateVertexBuffer(sizeof(verts), 0, FVF, D3DPOOL_MANAGED, &g_vb, nullptr);
    void* p = nullptr;
    g_vb->Lock(0, sizeof(verts), &p, 0); memcpy(p, verts, sizeof(verts)); g_vb->Unlock();
    g_dev->CreateIndexBuffer(sizeof(idx), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &g_ib, nullptr);
    g_ib->Lock(0, sizeof(idx), &p, 0); memcpy(p, idx, sizeof(idx)); g_ib->Unlock();

    g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    return true;
}

static void Render(float t) {
    g_dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
        D3DCOLOR_XRGB(25, 25, 51), 1.0f, 0);
    g_dev->BeginScene();

    XMMATRIX world = XMMatrixRotationY(t) * XMMatrixRotationX(t * 0.5f);
    XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 0, -6, 0), XMVectorSet(0, 0, 0, 0), XMVectorSet(0, 1, 0, 0));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, (float)g_width / g_height, 0.1f, 100.0f);
    SetMatrix(D3DTS_WORLD, world);
    SetMatrix(D3DTS_VIEW, view);
    SetMatrix(D3DTS_PROJECTION, proj);

    g_dev->SetFVF(FVF);
    g_dev->SetStreamSource(0, g_vb, 0, sizeof(Vertex));
    g_dev->SetIndices(g_ib);
    g_dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 8, 0, 12);

    g_dev->EndScene();
    g_dev->Present(nullptr, nullptr, nullptr, nullptr);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    WNDCLASS wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = "D3D9Cube";
    RegisterClass(&wc);
    RECT r = { 0, 0, (LONG)g_width, (LONG)g_height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindow("D3D9Cube", "Rotating Cube - Direct3D 9",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, hInst, nullptr);
    if (!InitD3D(hwnd)) { MessageBox(hwnd, "D3D9 init failed", "Error", MB_OK); return 1; }
    ShowWindow(hwnd, SW_SHOW);

    DWORD start = GetTickCount();
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        else Render((GetTickCount() - start) / 1000.0f);
    }
    return (int)msg.wParam;
}
