#include "king_window.h"
#include "king/ecs/scene.h"
#include "king/ecs/components.h"
#include "king/scene/camera.h"
#include "king/scene/frustum.h"
#include "king/render/material.h"
#include "king/render/shader.h"
#include "king/math/dxmath.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

static ID3D11Device* gDevice = nullptr;
static ID3D11DeviceContext* gContext = nullptr;
static IDXGISwapChain* gSwapChain = nullptr;
static ID3D11RenderTargetView* gRTV = nullptr;
static ID3D11VertexShader* gVS = nullptr;
static ID3D11PixelShader* gPS = nullptr;
static ID3D11InputLayout* gInputLayout = nullptr;
static ID3D11Buffer* gCameraCB = nullptr;
static ID3D11Buffer* gLightCB = nullptr;
static ID3D11Buffer* gObjectCB = nullptr;
static ID3D11DepthStencilView* gDSV = nullptr;
static ID3D11DepthStencilState* gDSS = nullptr;
static ID3D11RasterizerState* gRS = nullptr;
static D3D11_VIEWPORT gViewport{};

// Renderer-owned backbuffer state and resize queue.
static UINT gBackBufferW = 0;
static UINT gBackBufferH = 0;
static bool gResizeQueued = false;
static UINT gQueuedW = 0;
static UINT gQueuedH = 0;

static void SafeRelease(IUnknown* p)
{
    if (p) p->Release();
}

static void SetupDebugConsole()
{
    // Prefer attaching to an existing terminal (e.g., VS Code integrated terminal)
    // so we don't spawn a separate console window.
    // If launched from Explorer (no parent console), we'll log via OutputDebugString.
    bool haveConsole = false;
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        haveConsole = true;
    }
    else
    {
        // Optional: set KING_ALLOC_CONSOLE=1 to force a dedicated console window.
        wchar_t buf[8]{};
        DWORD n = GetEnvironmentVariableW(L"KING_ALLOC_CONSOLE", buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
        if (n > 0 && (buf[0] == L'1' || buf[0] == L't' || buf[0] == L'T' || buf[0] == L'y' || buf[0] == L'Y'))
        {
            haveConsole = AllocConsole() != 0;
            if (haveConsole)
            SetConsoleTitleW(L"King Debug Console");
        }
    }

    if (!haveConsole)
        return;

    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    freopen_s(&f, "CONIN$", "r", stdin);

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

static std::wstring HResultMessage(HRESULT hr)
{
    wchar_t* buf = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD len = FormatMessageW(flags, nullptr, (DWORD)hr, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring msg;
    if (len && buf)
    {
        msg.assign(buf, buf + len);
        LocalFree(buf);
    }
    else
    {
        msg = L"(no system message)";
    }
    return msg;
}

static std::wstring GetExeDirectory()
{
    wchar_t path[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return L".";

    // Strip filename
    for (DWORD i = n; i > 0; --i)
    {
        if (path[i - 1] == L'\\' || path[i - 1] == L'/')
        {
            path[i - 1] = 0;
            break;
        }
    }
    return path;
}

static std::wstring JoinPath(const std::wstring& a, const std::wstring& b)
{
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

static void LogHResult(const char* what, HRESULT hr)
{
    std::wstring msg = HResultMessage(hr);
    char tmp[512];
    std::snprintf(tmp, sizeof(tmp), "%s failed: hr=0x%08X\n", what, (unsigned)hr);
    OutputDebugStringA(tmp);
    std::printf("%s", tmp);
    std::wstring w = L"  " + msg + L"\n";
    OutputDebugStringW(w.c_str());
    std::wprintf(L"%s", w.c_str());
}

static void CleanupD3D()
{
    SafeRelease(gCameraCB);
    SafeRelease(gLightCB);
    SafeRelease(gObjectCB);
    SafeRelease(gDSV);
    SafeRelease(gDSS);
    SafeRelease(gRS);
    SafeRelease(gInputLayout);
    SafeRelease(gVS);
    SafeRelease(gPS);
    SafeRelease(gRTV);
    SafeRelease(gSwapChain);
    if (gContext)
    {
        gContext->ClearState();
        gContext->Flush();
    }
    SafeRelease(gContext);
    SafeRelease(gDevice);
}

static HRESULT CreateRenderTarget()
{
    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = gSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (FAILED(hr)) return hr;

    // Prefer sRGB RTV for correct gamma output (fallback to default if unsupported).
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice = 0;
    hr = gDevice->CreateRenderTargetView(backBuffer, &rtvDesc, &gRTV);
    if (FAILED(hr))
        hr = gDevice->CreateRenderTargetView(backBuffer, nullptr, &gRTV);
    backBuffer->Release();
    return hr;
}

static HRESULT CreateDepthTarget(UINT width, UINT height)
{
    SafeRelease(gDSV);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    ID3D11Texture2D* depthTex = nullptr;
    HRESULT hr = gDevice->CreateTexture2D(&td, nullptr, &depthTex);
    if (FAILED(hr))
    {
        SafeRelease(depthTex);
        return hr;
    }

    hr = gDevice->CreateDepthStencilView(depthTex, nullptr, &gDSV);
    depthTex->Release();
    return hr;
}

static HRESULT Resize(UINT width, UINT height)
{
    if (!gSwapChain) return S_OK;

    gBackBufferW = width;
    gBackBufferH = height;

    if (gContext)
    {
        gContext->OMSetRenderTargets(0, nullptr, nullptr);
    }

    SafeRelease(gRTV);

    SafeRelease(gDSV);

    HRESULT hr = gSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) return hr;

    hr = CreateRenderTarget();
    if (FAILED(hr)) return hr;

    hr = CreateDepthTarget(width, height);
    if (FAILED(hr)) return hr;

    gViewport.TopLeftX = 0.0f;
    gViewport.TopLeftY = 0.0f;
    gViewport.Width = (float)width;
    gViewport.Height = (float)height;
    gViewport.MinDepth = 0.0f;
    gViewport.MaxDepth = 1.0f;

    return S_OK;
}

static void QueueResize(UINT width, UINT height)
{
    gResizeQueued = true;
    gQueuedW = width;
    gQueuedH = height;
}

static void ApplyQueuedResizeIfAny(king::Scene& scene)
{
    if (!gResizeQueued)
        return;

    gResizeQueued = false;
    if (gQueuedW == 0 || gQueuedH == 0)
        return;

    HRESULT hr = Resize(gQueuedW, gQueuedH);
    if (FAILED(hr))
    {
        LogHResult("Resize(from queued)", hr);
        return;
    }

    // Update primary camera aspect inside the render-owned resize.
    for (auto e : scene.reg.cameras.Entities())
    {
        auto* cc = scene.reg.cameras.TryGet(e);
        if (cc && cc->primary)
        {
            king::PerspectiveParams p;
            p.aspect = (gQueuedH > 0) ? ((float)gQueuedW / (float)gQueuedH) : (16.0f / 9.0f);
            cc->camera.SetPerspective(p);
            break;
        }
    }
}

static void ReleaseSceneMeshBuffers(king::Scene& scene)
{
    for (auto me : scene.reg.meshes.Entities())
    {
        auto* m = scene.reg.meshes.TryGet(me);
        if (m && m->vb)
        {
            m->vb->Release();
            m->vb = nullptr;
        }
    }
}

static bool TryRecoverFromDeviceLost(king::Scene& scene, HWND hwnd)
{
    // Drop scene GPU buffers before device teardown.
    ReleaseSceneMeshBuffers(scene);
    CleanupD3D();

    // Recreate D3D with last known size.
    UINT w = gBackBufferW;
    UINT h = gBackBufferH;
    if (w == 0 || h == 0)
    {
        RECT r{};
        GetClientRect(hwnd, &r);
        w = (UINT)((r.right > r.left) ? (r.right - r.left) : 1280);
        h = (UINT)((r.bottom > r.top) ? (r.bottom - r.top) : 720);
    }

    HRESULT hr = InitD3D(hwnd, w, h);
    if (FAILED(hr))
        return false;

    // Ensure viewport/aspect match the current size.
    QueueResize(w, h);
    ApplyQueuedResizeIfAny(scene);
    return true;
}

static void EnableDpiAwareness()
{
    // Avoid extra SDK/lib dependencies: dynamically call user32!SetProcessDpiAwarenessContext if present.
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32)
        return;

    using Fn = BOOL(WINAPI*)(HANDLE);
    auto fn = (Fn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (!fn)
        return;

    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
    fn((HANDLE)-4);
}

struct CameraCBData
{
    king::Mat4x4 viewProj;
};

struct LightCBData
{
    float dir[3];
    float intensity;
    float color[3];
    float _pad0;
};

struct ObjectCBData
{
    king::Mat4x4 world;
    float albedo[4];
};

static HRESULT InitD3D(HWND hwnd, UINT width, UINT height)
{
    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = width;
    scd.BufferDesc.Height = height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createFlags = 0;
#if defined(_DEBUG)
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // Accept 11.0..10.0 to work on slightly older hardware.
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL featureLevelOut{};

    auto tryCreate = [&](D3D_DRIVER_TYPE driverType, UINT flags) -> HRESULT
    {
        return D3D11CreateDeviceAndSwapChain(
            nullptr,
            driverType,
            nullptr,
            flags,
            featureLevels,
            (UINT)(sizeof(featureLevels) / sizeof(featureLevels[0])),
            D3D11_SDK_VERSION,
            &scd,
            &gSwapChain,
            &gDevice,
            &featureLevelOut,
            &gContext
        );
    };

    std::printf("InitD3D: creating device/swapchain (%ux%u)\n", width, height);

    HRESULT hr = tryCreate(D3D_DRIVER_TYPE_HARDWARE, createFlags);
    if (FAILED(hr))
    {
        LogHResult("D3D11CreateDeviceAndSwapChain(HARDWARE)", hr);

#if defined(_DEBUG)
        // Common failure: debug layer not installed. Retry without debug.
        if (createFlags & D3D11_CREATE_DEVICE_DEBUG)
        {
            std::printf("Retrying without D3D11 debug layer...\n");
            hr = tryCreate(D3D_DRIVER_TYPE_HARDWARE, createFlags & ~D3D11_CREATE_DEVICE_DEBUG);
            if (FAILED(hr))
            {
                LogHResult("D3D11CreateDeviceAndSwapChain(HARDWARE, no-debug)", hr);
            }
        }
#endif

        if (FAILED(hr))
        {
            std::printf("Retrying with WARP software device...\n");
            hr = tryCreate(D3D_DRIVER_TYPE_WARP, 0);
            if (FAILED(hr))
            {
                LogHResult("D3D11CreateDeviceAndSwapChain(WARP)", hr);
                return hr;
            }
        }
    }

    std::printf("D3D device created. Feature level: 0x%X\n", (unsigned)featureLevelOut);

    hr = CreateRenderTarget();
    if (FAILED(hr))
    {
        LogHResult("CreateRenderTarget", hr);
        return hr;
    }

    hr = Resize(width, height);
    if (FAILED(hr))
    {
        LogHResult("Resize", hr);
        return hr;
    }

    // --- King shader system: compile + cache + reflection ---
    king::ShaderCache shaderCache(gDevice);
    king::CompiledShader vs;
    king::CompiledShader ps;
    std::string shaderErr;

    // Resolve shader path relative to the exe: build/Debug/King.exe -> ../../assets/...
    std::wstring shaderPath = JoinPath(GetExeDirectory(), L"..\\..\\assets\\shaders\\pbr_test.hlsl");
    if (!shaderCache.CompileVSFromFile(shaderPath.c_str(), "VSMain", {}, vs, &shaderErr))
    {
        std::printf("Shader VS compile error:\n%s\n", shaderErr.c_str());
        return E_FAIL;
    }

    if (!shaderCache.CompilePSFromFile(shaderPath.c_str(), "PSMain", {}, ps, &shaderErr))
    {
        std::printf("Shader PS compile error:\n%s\n", shaderErr.c_str());
        return E_FAIL;
    }

    std::printf("VS reflection: %zu resources, %zu cbuffers\n", vs.reflection.resources.size(), vs.reflection.cbuffers.size());
    for (const auto& r : vs.reflection.resources)
        std::printf("  res: %s type=%u slot=%u count=%u\n", r.name.c_str(), (unsigned)r.type, r.bindPoint, r.bindCount);
    for (const auto& cb : vs.reflection.cbuffers)
        std::printf("  cbuf: %s slot=%u size=%u\n", cb.name.c_str(), cb.bindPoint, cb.sizeBytes);

    std::printf("PS reflection: %zu resources, %zu cbuffers\n", ps.reflection.resources.size(), ps.reflection.cbuffers.size());
    for (const auto& r : ps.reflection.resources)
        std::printf("  res: %s type=%u slot=%u count=%u\n", r.name.c_str(), (unsigned)r.type, r.bindPoint, r.bindCount);
    for (const auto& cb : ps.reflection.cbuffers)
        std::printf("  cbuf: %s slot=%u size=%u\n", cb.name.c_str(), cb.bindPoint, cb.sizeBytes);

    hr = gDevice->CreateVertexShader(vs.bytecode->GetBufferPointer(), vs.bytecode->GetBufferSize(), nullptr, &gVS);
    if (FAILED(hr))
    {
        LogHResult("CreateVertexShader", hr);
        return hr;
    }

    hr = gDevice->CreatePixelShader(ps.bytecode->GetBufferPointer(), ps.bytecode->GetBufferSize(), nullptr, &gPS);
    if (FAILED(hr))
    {
        LogHResult("CreatePixelShader", hr);
        return hr;
    }

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    hr = gDevice->CreateInputLayout(
        layout,
        (UINT)(sizeof(layout) / sizeof(layout[0])),
        vs.bytecode->GetBufferPointer(),
        vs.bytecode->GetBufferSize(),
        &gInputLayout
    );
    if (FAILED(hr))
    {
        LogHResult("CreateInputLayout", hr);
        return hr;
    }

    // Camera constant buffer (b0)
    D3D11_BUFFER_DESC cbd{};
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.ByteWidth = 64; // float4x4
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = gDevice->CreateBuffer(&cbd, nullptr, &gCameraCB);
    if (FAILED(hr))
    {
        LogHResult("CreateBuffer(CameraCB)", hr);
        return hr;
    }

    // Light constant buffer (b1)
    cbd.ByteWidth = sizeof(LightCBData);
    hr = gDevice->CreateBuffer(&cbd, nullptr, &gLightCB);
    if (FAILED(hr))
    {
        LogHResult("CreateBuffer(LightCB)", hr);
        return hr;
    }

    // Object constant buffer (b2)
    cbd.ByteWidth = sizeof(ObjectCBData);
    hr = gDevice->CreateBuffer(&cbd, nullptr, &gObjectCB);
    if (FAILED(hr))
    {
        LogHResult("CreateBuffer(ObjectCB)", hr);
        return hr;
    }

    // Depth testing state
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    dsd.StencilEnable = FALSE;
    hr = gDevice->CreateDepthStencilState(&dsd, &gDSS);
    if (FAILED(hr))
    {
        LogHResult("CreateDepthStencilState", hr);
        return hr;
    }

    // Rasterizer state: disable culling for now (avoids winding issues while iterating quickly).
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    hr = gDevice->CreateRasterizerState(&rs, &gRS);
    if (FAILED(hr))
    {
        LogHResult("CreateRasterizerState", hr);
        return hr;
    }

    return S_OK;
}

static void UpdateCameraCB(const king::Mat4x4& viewProj)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(gContext->Map(gCameraCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        std::memcpy(mapped.pData, viewProj.m, 64);
        gContext->Unmap(gCameraCB, 0);
    }
}

static void UpdateLightCB(const king::Light& light)
{
    LightCBData data{};
    data.dir[0] = light.direction.x;
    data.dir[1] = light.direction.y;
    data.dir[2] = light.direction.z;
    data.intensity = light.intensity;
    data.color[0] = light.color.x;
    data.color[1] = light.color.y;
    data.color[2] = light.color.z;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(gContext->Map(gLightCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        std::memcpy(mapped.pData, &data, sizeof(data));
        gContext->Unmap(gLightCB, 0);
    }
}

static void UpdateObjectCB(const king::Mat4x4& world, const king::PbrMaterial& mat)
{
    ObjectCBData data{};
    data.world = world;
    data.albedo[0] = mat.albedo.x;
    data.albedo[1] = mat.albedo.y;
    data.albedo[2] = mat.albedo.z;
    data.albedo[3] = mat.albedo.w;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(gContext->Map(gObjectCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        std::memcpy(mapped.pData, &data, sizeof(data));
        gContext->Unmap(gObjectCB, 0);
    }
}

static king::Mat4x4 TransformToWorld(const king::Transform& t)
{
    using namespace DirectX;
    const XMVECTOR q = XMVectorSet(t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w);
    XMMATRIX S = XMMatrixScaling(t.scale.x, t.scale.y, t.scale.z);
    XMMATRIX R = XMMatrixRotationQuaternion(q);
    XMMATRIX T = XMMatrixTranslation(t.position.x, t.position.y, t.position.z);
    XMMATRIX W = S * R * T;
    return king::dx::StoreMat4x4(W);
}

static HRESULT CreateVertexBuffer(const std::vector<king::VertexPN>& verts, ID3D11Buffer** outVB)
{
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = (UINT)(verts.size() * sizeof(king::VertexPN));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = verts.data();
    return gDevice->CreateBuffer(&bd, &init, outVB);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    EnableDpiAwareness();
    SetupDebugConsole();
    std::printf("King starting...\n");

    king::Window window;
    king::Window::Desc winDesc;
    winDesc.width = 1280;
    winDesc.height = 720;
    winDesc.title = L"King - Sandbox (D3D11)";
    winDesc.resizable = true;

    // Exercise: change the icon. This uses a built-in system icon (no .ico file needed).
    // If you later add a real .ico resource, you can pass those handles here instead.
    winDesc.iconBig = LoadIconW(nullptr, IDI_APPLICATION);
    winDesc.iconSmall = LoadIconW(nullptr, IDI_APPLICATION);

    if (!window.Create(hInstance, winDesc))
        return 1;

    window.Show(nCmdShow);

    UINT width = window.ClientWidth();
    UINT height = window.ClientHeight();

    // --- ECS sample scene ---
    king::Scene scene;

    // Camera entity
    king::Entity camEnt = scene.reg.CreateEntity();
    {
        auto& t = scene.reg.transforms.Emplace(camEnt);
        t.position = { 0.0f, 1.5f, -5.0f };

        auto& cc = scene.reg.cameras.Emplace(camEnt);
        cc.primary = true;

        king::PerspectiveParams persp;
        persp.aspect = (height > 0) ? ((float)width / (float)height) : (16.0f / 9.0f);
        cc.camera.SetPerspective(persp);
        cc.camera.SetPosition(t.position);
    }

    // Directional light entity (stub for now)
    king::Entity lightEnt = scene.reg.CreateEntity();
    {
        scene.reg.transforms.Emplace(lightEnt);
        auto& l = scene.reg.lights.Emplace(lightEnt);
        l.type = king::LightType::Directional;
        l.color = { 1, 1, 1 };
        l.intensity = 2.0f;
        l.direction = { 0.35f, -1.0f, 0.25f };
    }

    auto makeCubeMesh = [&](float halfExtents) -> king::Entity
    {
        const float h = halfExtents;
        king::Entity me = scene.reg.CreateEntity();
        auto& m = scene.reg.meshes.Emplace(me);

        // 36 verts (6 faces * 2 tris * 3 verts), per-face normals.
        auto pushFace = [&](king::Float3 n, king::Float3 a, king::Float3 b, king::Float3 c, king::Float3 d)
        {
            m.vertices.push_back({ a.x, a.y, a.z, n.x, n.y, n.z });
            m.vertices.push_back({ b.x, b.y, b.z, n.x, n.y, n.z });
            m.vertices.push_back({ c.x, c.y, c.z, n.x, n.y, n.z });
            m.vertices.push_back({ a.x, a.y, a.z, n.x, n.y, n.z });
            m.vertices.push_back({ c.x, c.y, c.z, n.x, n.y, n.z });
            m.vertices.push_back({ d.x, d.y, d.z, n.x, n.y, n.z });
        };

        // +X
        pushFace({ 1,0,0 }, { h,-h,-h }, { h,-h, h }, { h, h, h }, { h, h,-h });
        // -X
        pushFace({-1,0,0 }, {-h,-h, h }, {-h,-h,-h }, {-h, h,-h }, {-h, h, h });
        // +Y
        pushFace({ 0,1,0 }, {-h, h,-h }, { h, h,-h }, { h, h, h }, {-h, h, h });
        // -Y
        pushFace({ 0,-1,0}, {-h,-h, h }, { h,-h, h }, { h,-h,-h }, {-h,-h,-h });
        // +Z
        pushFace({ 0,0,1 }, { h,-h, h }, {-h,-h, h }, {-h, h, h }, { h, h, h });
        // -Z
        pushFace({ 0,0,-1}, {-h,-h,-h }, { h,-h,-h }, { h, h,-h }, {-h, h,-h });

        m.boundsCenter = { 0, 0, 0 };
        m.boundsRadius = halfExtents * 1.8f;
        return me;
    };

    king::Entity cubeMesh = makeCubeMesh(0.5f);

    // Ground box
    king::Entity groundEnt = scene.reg.CreateEntity();
    {
        auto& t = scene.reg.transforms.Emplace(groundEnt);
        t.position = { 0, -1.0f, 0 };
        t.scale = { 20.0f, 1.0f, 20.0f };
        auto& r = scene.reg.renderers.Emplace(groundEnt);
        r.mesh = cubeMesh;
        r.material.albedo = { 0.25f, 0.75f, 0.25f, 1.0f };
        r.material.shader = "pbr_test";
    }

    // Boxes
    auto addBox = [&](king::Float3 pos, king::Float3 scale, king::Float4 albedo)
    {
        king::Entity e = scene.reg.CreateEntity();
        auto& t = scene.reg.transforms.Emplace(e);
        t.position = pos;
        t.scale = scale;
        auto& r = scene.reg.renderers.Emplace(e);
        r.mesh = cubeMesh;
        r.material.albedo = albedo;
        r.material.shader = "pbr_test";
    };

    addBox({ -2.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, { 0.9f, 0.2f, 0.2f, 1.0f });
    addBox({  0.0f, 0.0f, 2.0f }, { 1.0f, 2.0f, 1.0f }, { 0.2f, 0.6f, 0.9f, 1.0f });
    addBox({  2.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 2.0f }, { 0.9f, 0.85f, 0.2f, 1.0f });

    // Camera/frustum for culling foundation
    king::Frustum frustum{};

    if (FAILED(InitD3D(window.Handle(), width, height)))
    {
        CleanupD3D();
        std::printf("Failed to initialize D3D11. See messages above.\n");
        MessageBoxW(window.Handle(), L"Failed to initialize D3D11. Check the debug output for details.", L"Error", MB_ICONERROR);
        std::printf("Press Enter to exit...\n");
        (void)getchar();
        return 1;
    }

    // Seed render-owned resize path.
    QueueResize(width, height);

    king::Window::Event ev{};
    while (window.PumpMessages())
    {
        while (window.PollEvent(ev))
        {
            if (ev.type == king::Window::EventType::Resize)
            {
                if (ev.width > 0 && ev.height > 0)
                    QueueResize((UINT)ev.width, (UINT)ev.height);
            }
            else if (ev.type == king::Window::EventType::CloseRequested)
            {
                std::printf("Close requested.\n");
            }
        }

        // Skip rendering while minimized.
        if (IsIconic(window.Handle()))
        {
            Sleep(16);
            continue;
        }

        // Apply queued resize at a stable point owned by the renderer.
        ApplyQueuedResizeIfAny(scene);

        // CameraSystem (very small): sync camera position from Transform.
        king::Mat4x4 viewProj{};
        for (auto e : scene.reg.cameras.Entities())
        {
            auto* cc = scene.reg.cameras.TryGet(e);
            auto* t = scene.reg.transforms.TryGet(e);
            if (!cc || !t || !cc->primary) continue;
            cc->camera.SetPosition(t->position);
            viewProj = cc->camera.ViewProjectionMatrix();
            frustum = king::Frustum::FromViewProjection(viewProj);
            break;
        }
        UpdateCameraCB(viewProj);

        // LightingSystem (minimal): take first directional light.
        king::Light light{};
        bool haveLight = false;
        for (auto e : scene.reg.lights.Entities())
        {
            auto* l = scene.reg.lights.TryGet(e);
            if (l && l->type == king::LightType::Directional)
            {
                light = *l;
                haveLight = true;
                break;
            }
        }
        if (haveLight)
            UpdateLightCB(light);

        // RenderSystem: render ECS MeshRenderer entities.
        const float clearColor[4] = { 0.06f, 0.06f, 0.08f, 1.0f };
        gContext->ClearRenderTargetView(gRTV, clearColor);
        gContext->ClearDepthStencilView(gDSV, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        gContext->OMSetRenderTargets(1, &gRTV, gDSV);
        gContext->OMSetDepthStencilState(gDSS, 0);
        gContext->RSSetState(gRS);
        gContext->RSSetViewports(1, &gViewport);

        gContext->IASetInputLayout(gInputLayout);
        gContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        gContext->VSSetShader(gVS, nullptr, 0);
        gContext->PSSetShader(gPS, nullptr, 0);

        gContext->VSSetConstantBuffers(0, 1, &gCameraCB);
        gContext->PSSetConstantBuffers(1, 1, &gLightCB);
        gContext->VSSetConstantBuffers(2, 1, &gObjectCB);
        gContext->PSSetConstantBuffers(2, 1, &gObjectCB);

        // Ensure mesh VBs exist
        for (auto me : scene.reg.meshes.Entities())
        {
            auto* m = scene.reg.meshes.TryGet(me);
            if (!m || m->vb) continue;
            HRESULT hr = CreateVertexBuffer(m->vertices, &m->vb);
            if (FAILED(hr))
                LogHResult("CreateVertexBuffer(mesh)", hr);
        }

        // Draw renderables
        for (auto e : scene.reg.renderers.Entities())
        {
            auto* r = scene.reg.renderers.TryGet(e);
            auto* t = scene.reg.transforms.TryGet(e);
            if (!r || !t) continue;

            auto* m = scene.reg.meshes.TryGet(r->mesh);
            if (!m || !m->vb) continue;

            // Frustum culling (sphere)
            king::Sphere s{};
            s.center = { t->position.x + m->boundsCenter.x, t->position.y + m->boundsCenter.y, t->position.z + m->boundsCenter.z };
            const float sx = t->scale.x;
            const float sy = t->scale.y;
            const float sz = t->scale.z;
            const float maxScale = (sx > sy) ? ((sx > sz) ? sx : sz) : ((sy > sz) ? sy : sz);
            s.radius = m->boundsRadius * maxScale;
            if (!frustum.Intersects(s))
                continue;

            king::Mat4x4 world = TransformToWorld(*t);
            UpdateObjectCB(world, r->material);

            UINT stride = sizeof(king::VertexPN);
            UINT offset = 0;
            gContext->IASetVertexBuffers(0, 1, &m->vb, &stride, &offset);
            gContext->Draw((UINT)m->vertices.size(), 0);
        }

        HRESULT phr = gSwapChain->Present(1, 0);
        if (FAILED(phr))
        {
            if (phr == DXGI_ERROR_DEVICE_REMOVED || phr == DXGI_ERROR_DEVICE_RESET)
            {
                HRESULT reason = gDevice ? gDevice->GetDeviceRemovedReason() : phr;
                LogHResult("Present(device lost)", phr);
                LogHResult("DeviceRemovedReason", reason);

                if (!TryRecoverFromDeviceLost(scene, window.Handle()))
                {
                    std::printf("Failed to recover from device lost. Exiting.\n");
                    break;
                }
            }
            else
            {
                LogHResult("Present", phr);
            }
        }
    }

    // Release mesh vertex buffers
    ReleaseSceneMeshBuffers(scene);

    CleanupD3D();
    return 0;
}
