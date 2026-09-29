// D3D11 Vertex Shader showcase.
// The vertex shader (vs_5_0) twists the cube about the Y axis by an angle that
// grows with height and time; the pixel shader outputs the interpolated color.
// ESC quits.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

using namespace DirectX;

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

struct Vertex { XMFLOAT3 pos; XMFLOAT4 color; };
struct CBuffer { XMMATRIX mvp; float time; float pad[3]; };

static ID3D11Device*           g_device = nullptr;
static ID3D11DeviceContext*    g_ctx = nullptr;
static IDXGISwapChain*         g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static ID3D11DepthStencilView* g_dsv = nullptr;
static ID3D11VertexShader*     g_vs = nullptr;
static ID3D11PixelShader*      g_ps = nullptr;
static ID3D11InputLayout*      g_layout = nullptr;
static ID3D11Buffer*           g_vb = nullptr;
static ID3D11Buffer*           g_ib = nullptr;
static ID3D11Buffer*           g_cb = nullptr;
static UINT g_width = 800, g_height = 600;

// [LEARN] Spotlight: the vertex shader (vs_5_0) transforming verts to clip space.
static const char* g_src =
"cbuffer CB : register(b0) { float4x4 mvp; float g_time; };\n"
"struct VSIn  { float3 pos : POSITION; float4 col : COLOR; };\n"
"struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
"VSOut VSMain(VSIn i) {\n"
"  float a = i.pos.y * 0.8 + g_time * 1.5;\n"
"  float s = sin(a), c = cos(a);\n"
"  float3 p = float3(i.pos.x * c - i.pos.z * s, i.pos.y, i.pos.x * s + i.pos.z * c);\n"
"  VSOut o; o.pos = mul(float4(p, 1.0), mvp); o.col = i.col; return o;\n"
"}\n"
"float4 PSMain(VSOut i) : SV_TARGET { return i.col; }\n";

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN: if (w == VK_ESCAPE) PostQuitMessage(0); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, w, l);
}

static ID3DBlob* Compile(const char* entry, const char* target) {
    ID3DBlob* b = nullptr; ID3DBlob* e = nullptr;
    if (FAILED(D3DCompile(g_src, strlen(g_src), nullptr, nullptr, nullptr, entry, target, 0, 0, &b, &e))) {
        if (e) e->Release(); return nullptr;
    }
    return b;
}

static bool InitD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = g_width; sd.BufferDesc.Height = g_height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60; sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &fl, &g_ctx)))
        return false;

    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();

    D3D11_TEXTURE2D_DESC dd = {};
    dd.Width = g_width; dd.Height = g_height; dd.MipLevels = 1; dd.ArraySize = 1;
    dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dd.SampleDesc.Count = 1;
    dd.Usage = D3D11_USAGE_DEFAULT; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D* depth = nullptr;
    g_device->CreateTexture2D(&dd, nullptr, &depth);
    g_device->CreateDepthStencilView(depth, nullptr, &g_dsv);
    depth->Release();

    g_ctx->OMSetRenderTargets(1, &g_rtv, g_dsv);
    D3D11_VIEWPORT vp = { 0, 0, (float)g_width, (float)g_height, 0.0f, 1.0f };
    g_ctx->RSSetViewports(1, &vp);

    ID3DBlob* vsb = Compile("VSMain", "vs_5_0");
    ID3DBlob* psb = Compile("PSMain", "ps_5_0");
    if (!vsb || !psb) return false;
    g_device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    g_device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);

    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    g_device->CreateInputLayout(il, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_layout);
    vsb->Release(); psb->Release();

    Vertex verts[] = {
        { {-1,-1,-1}, {0,0,0,1} }, { {-1, 1,-1}, {0,1,0,1} }, { { 1, 1,-1}, {1,1,0,1} }, { { 1,-1,-1}, {1,0,0,1} },
        { {-1,-1, 1}, {0,0,1,1} }, { {-1, 1, 1}, {0,1,1,1} }, { { 1, 1, 1}, {1,1,1,1} }, { { 1,-1, 1}, {1,0,1,1} },
    };
    unsigned short idx[] = {
        0,1,2, 0,2,3,  4,6,5, 4,7,6,  4,5,1, 4,1,0,
        3,2,6, 3,6,7,  1,5,6, 1,6,2,  4,0,3, 4,3,7,
    };
    D3D11_BUFFER_DESC bd = {}; D3D11_SUBRESOURCE_DATA srd = {};
    bd.Usage = D3D11_USAGE_DEFAULT; bd.ByteWidth = sizeof(verts);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; srd.pSysMem = verts;
    g_device->CreateBuffer(&bd, &srd, &g_vb);
    bd.ByteWidth = sizeof(idx); bd.BindFlags = D3D11_BIND_INDEX_BUFFER; srd.pSysMem = idx;
    g_device->CreateBuffer(&bd, &srd, &g_ib);
    bd.ByteWidth = sizeof(CBuffer); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_device->CreateBuffer(&bd, nullptr, &g_cb);
    return true;
}

static void Render(float t) {
    float clear[4] = { 0.1f, 0.1f, 0.2f, 1.0f };
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    g_ctx->ClearDepthStencilView(g_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);

    XMMATRIX world = XMMatrixRotationY(t) * XMMatrixRotationX(t * 0.5f);
    XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(0, 0, -6, 0), XMVectorSet(0, 0, 0, 0), XMVectorSet(0, 1, 0, 0));
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, (float)g_width / g_height, 0.1f, 100.0f);
    CBuffer cb; cb.mvp = XMMatrixTranspose(world * view * proj); cb.time = t;
    g_ctx->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);

    UINT stride = sizeof(Vertex), offset = 0;
    g_ctx->IASetInputLayout(g_layout);
    g_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
    g_ctx->IASetIndexBuffer(g_ib, DXGI_FORMAT_R16_UINT, 0);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->DrawIndexed(36, 0, 0);
    g_swap->Present(1, 0);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    WNDCLASS wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = "D3D11VS";
    RegisterClass(&wc);
    RECT r = { 0, 0, (LONG)g_width, (LONG)g_height };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindow("D3D11VS", "D3D11 - Vertex Shader (twist)",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, nullptr, nullptr, hInst, nullptr);
    if (!InitD3D(hwnd)) { MessageBox(hwnd, "D3D11 VS init failed", "Error", MB_OK); return 1; }
    ShowWindow(hwnd, SW_SHOW);

    DWORD start = GetTickCount();
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        else Render((GetTickCount() - start) / 1000.0f);
    }
    return (int)msg.wParam;
}
