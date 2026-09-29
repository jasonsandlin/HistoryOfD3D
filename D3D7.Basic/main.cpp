// Rotating cube using Direct3D 7 (DirectDraw7 immediate-mode Direct3D).
// Windowed: a DirectDraw primary surface with a clipper, an offscreen 3D-capable
// back buffer + attached Z-buffer, an IDirect3DDevice7 HAL device, and pre-lit
// D3DLVERTEX geometry drawn with DrawIndexedPrimitive. Each frame the back buffer
// is blitted to the window. ESC quits.
#define WIN32_LEAN_AND_MEAN
#define DIRECT3D_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <DirectXMath.h>

using namespace DirectX;

#pragma comment(lib, "ddraw.lib")
#pragma comment(lib, "dxguid.lib")

static UINT g_width = 800, g_height = 600;
static HWND g_hwnd = nullptr;
static LPDIRECTDRAW7        g_dd = nullptr;
static LPDIRECTDRAWSURFACE7 g_primary = nullptr;
static LPDIRECTDRAWSURFACE7 g_back = nullptr;
static LPDIRECTDRAWSURFACE7 g_zbuffer = nullptr;
static LPDIRECTDRAWCLIPPER  g_clipper = nullptr;
static LPDIRECT3D7          g_d3d = nullptr;
static LPDIRECT3DDEVICE7    g_device = nullptr;

static D3DLVERTEX g_verts[8];
static WORD g_idx[36] = {
    0,1,2, 0,2,3,  4,6,5, 4,7,6,  4,5,1, 4,1,0,
    3,2,6, 3,6,7,  1,5,6, 1,6,2,  4,0,3, 4,3,7,
};

static void MakeVertex(D3DLVERTEX& v, float x, float y, float z, D3DCOLOR c) {
    v.x = x; v.y = y; v.z = z; v.dwReserved = 0;
    v.color = c; v.specular = 0; v.tu = 0; v.tv = 0;
}

static void SetMatrix(D3DTRANSFORMSTATETYPE which, const XMMATRIX& m) {
    XMFLOAT4X4 f; XMStoreFloat4x4(&f, m);
    g_device->SetTransform(which, reinterpret_cast<D3DMATRIX*>(&f));
}

static HRESULT WINAPI EnumZBufferCallback(DDPIXELFORMAT* pddpf, VOID* ctx) {
    if (pddpf->dwFlags & DDPF_ZBUFFER) {
        *reinterpret_cast<DDPIXELFORMAT*>(ctx) = *pddpf;
        if (pddpf->dwZBufferBitDepth == 16) return D3DENUMRET_CANCEL; // prefer 16-bit
    }
    return D3DENUMRET_OK;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN: if (w == VK_ESCAPE) PostQuitMessage(0); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, w, l);
}

static bool InitD3D(HWND hwnd) {
    g_hwnd = hwnd;
    if (FAILED(DirectDrawCreateEx(nullptr, (void**)&g_dd, IID_IDirectDraw7, nullptr))) return false;
    if (FAILED(g_dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL))) return false;

    DDSURFACEDESC2 ddsd = {}; ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    if (FAILED(g_dd->CreateSurface(&ddsd, &g_primary, nullptr))) return false;

    if (FAILED(g_dd->CreateClipper(0, &g_clipper, nullptr))) return false;
    g_clipper->SetHWnd(0, hwnd);
    g_primary->SetClipper(g_clipper);

    // Back buffer (offscreen, 3D-capable), client sized.
    ZeroMemory(&ddsd, sizeof(ddsd)); ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    ddsd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE;
    ddsd.dwWidth = g_width; ddsd.dwHeight = g_height;
    if (FAILED(g_dd->CreateSurface(&ddsd, &g_back, nullptr))) return false;

    if (FAILED(g_dd->QueryInterface(IID_IDirect3D7, (void**)&g_d3d))) return false;

    // Enumerate a Z-buffer pixel format and create the Z-buffer.
    DDPIXELFORMAT zfmt = {};
    g_d3d->EnumZBufferFormats(IID_IDirect3DHALDevice, EnumZBufferCallback, &zfmt);
    if (zfmt.dwSize == 0) return false;
    ZeroMemory(&ddsd, sizeof(ddsd)); ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    ddsd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
    ddsd.dwWidth = g_width; ddsd.dwHeight = g_height;
    ddsd.ddpfPixelFormat = zfmt;
    if (FAILED(g_dd->CreateSurface(&ddsd, &g_zbuffer, nullptr))) return false;
    if (FAILED(g_back->AddAttachedSurface(g_zbuffer))) return false;

    // Create the 3D device on the back buffer (fall back to reference rasterizer).
    if (FAILED(g_d3d->CreateDevice(IID_IDirect3DHALDevice, g_back, &g_device)))
        if (FAILED(g_d3d->CreateDevice(IID_IDirect3DRGBDevice, g_back, &g_device)))
            return false;

    D3DVIEWPORT7 vp = { 0, 0, g_width, g_height, 0.0f, 1.0f };
    g_device->SetViewport(&vp);
    // [LEARN] D3D7 fixed-function render states: lighting, depth test, and cull
    // mode are toggled via SetRenderState - the whole pipeline is configured by
    // state, not shaders. This is the oldest (DirectDraw7 + IDirect3D7) style.
    g_device->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
    g_device->SetRenderState(D3DRENDERSTATE_ZENABLE, D3DZB_TRUE);
    g_device->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);

    MakeVertex(g_verts[0], -1, -1, -1, D3DRGB(0, 0, 0));
    MakeVertex(g_verts[1], -1,  1, -1, D3DRGB(0, 1, 0));
    MakeVertex(g_verts[2],  1,  1, -1, D3DRGB(1, 1, 0));
    MakeVertex(g_verts[3],  1, -1, -1, D3DRGB(1, 0, 0));
    MakeVertex(g_verts[4], -1, -1,  1, D3DRGB(0, 0, 1));
    MakeVertex(g_verts[5], -1,  1,  1, D3DRGB(0, 1, 1));
    MakeVertex(g_verts[6],  1,  1,  1, D3DRGB(1, 1, 1));
    MakeVertex(g_verts[7],  1, -1,  1, D3DRGB(1, 0, 1));
    return true;
}

static void Render(float t) {
    D3DVIEWPORT7 vp = { 0, 0, g_width, g_height, 0.0f, 1.0f };
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DRGB(0.1f, 0.1f, 0.2f), 1.0f, 0);

    XMMATRIX world = XMMatrixRotationY(t) * XMMatrixRotationX(t * 0.5f);
    XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 0, -6, 0), XMVectorSet(0, 0, 0, 0), XMVectorSet(0, 1, 0, 0));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, (float)g_width / g_height, 0.1f, 100.0f);

    if (SUCCEEDED(g_device->BeginScene())) {
        SetMatrix(D3DTRANSFORMSTATE_WORLD, world);
        SetMatrix(D3DTRANSFORMSTATE_VIEW, view);
        SetMatrix(D3DTRANSFORMSTATE_PROJECTION, proj);
        g_device->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, D3DFVF_LVERTEX,
            g_verts, 8, g_idx, 36, 0);
        g_device->EndScene();
    }

    // Blit back buffer to the window's client area (screen coords).
    RECT rc; GetClientRect(g_hwnd, &rc);
    POINT tl = { 0, 0 }; ClientToScreen(g_hwnd, &tl);
    RECT dest = { tl.x, tl.y, tl.x + rc.right, tl.y + rc.bottom };
    RECT src = { 0, 0, (LONG)g_width, (LONG)g_height };
    g_primary->Blt(&dest, g_back, &src, DDBLT_WAIT, nullptr);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    WNDCLASS wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = "D3D7Cube";
    RegisterClass(&wc);
    RECT r = { 0, 0, (LONG)g_width, (LONG)g_height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindow("D3D7Cube", "Rotating Cube - Direct3D 7",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, hInst, nullptr);
    if (!InitD3D(hwnd)) { MessageBox(hwnd, "D3D7 init failed", "Error", MB_OK); return 1; }
    ShowWindow(hwnd, SW_SHOW);

    DWORD start = GetTickCount();
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        else Render((GetTickCount() - start) / 1000.0f);
    }
    return (int)msg.wParam;
}
