#include "king_window.h"
#include "king/ecs/scene.h"
#include "king/ecs/components.h"
#include "king/systems/camera_system.h"
#include "king/systems/lighting_system.h"
#include "king/render/d3d11/render_device_d3d11.h"
#include "king/render/d3d11/render_system_d3d11.h"
#include "king/time/time.h"

#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <array>
#include <cmath>
#include <string>

struct InputState
{
    bool keys[256]{};
    bool hasFocus = true;

    bool rmbDown = false;
    bool haveMousePos = false;
    int32_t lastMouseX = 0;
    int32_t lastMouseY = 0;
    float mouseDeltaX = 0.0f;
    float mouseDeltaY = 0.0f;
};

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

    // No directional "sun" for this demo: use animated point lights so lighting is obvious.
    // (Note: our current shadow map path is directional-only, so shadows will be off.)
    std::array<king::Entity, 4> movingLights{};
    for (auto& e : movingLights)
        e = scene.reg.CreateEntity();

    // Four moving point lights (slightly dimmer than before).
    {
        auto& t = scene.reg.transforms.Emplace(movingLights[0]);
        t.position = { 0.0f, 3.0f, 0.0f };
        auto& l = scene.reg.lights.Emplace(movingLights[0]);
        l.type = king::LightType::Point;
        l.color = { 1.0f, 0.95f, 0.85f };
        l.intensity = 7.0f;
        l.range = 16.0f;
        l.groupMask = 0xFFFFFFFFu;
    }
    {
        auto& t = scene.reg.transforms.Emplace(movingLights[1]);
        t.position = { 0.0f, 3.0f, 0.0f };
        auto& l = scene.reg.lights.Emplace(movingLights[1]);
        l.type = king::LightType::Point;
        l.color = { 0.45f, 0.65f, 1.0f };
        l.intensity = 6.0f;
        l.range = 16.0f;
        l.groupMask = 0xFFFFFFFFu;
    }
    {
        auto& t = scene.reg.transforms.Emplace(movingLights[2]);
        t.position = { 0.0f, 3.0f, 0.0f };
        auto& l = scene.reg.lights.Emplace(movingLights[2]);
        l.type = king::LightType::Point;
        l.color = { 1.0f, 0.35f, 0.35f };
        l.intensity = 6.0f;
        l.range = 16.0f;
        l.groupMask = 0xFFFFFFFFu;
    }
    {
        auto& t = scene.reg.transforms.Emplace(movingLights[3]);
        t.position = { 0.0f, 3.0f, 0.0f };
        auto& l = scene.reg.lights.Emplace(movingLights[3]);
        l.type = king::LightType::Point;
        l.color = { 1.0f, 0.65f, 0.30f };
        l.intensity = 6.0f;
        l.range = 16.0f;
        l.groupMask = 0xFFFFFFFFu;
    }

    auto makeCubeMesh = [&](float halfExtents) -> king::Entity
    {
        const float h = halfExtents;
        king::Entity me = scene.reg.CreateEntity();
        auto& m = scene.reg.meshes.Emplace(me);

        // Indexed cube: 24 verts (4 per face), 36 indices.
        const king::VertexPN v[] = {
            // +X
            { h,-h,-h,  1,0,0 }, { h,-h, h,  1,0,0 }, { h, h, h,  1,0,0 }, { h, h,-h,  1,0,0 },
            // -X
            {-h,-h, h, -1,0,0 }, {-h,-h,-h, -1,0,0 }, {-h, h,-h, -1,0,0 }, {-h, h, h, -1,0,0 },
            // +Y
            {-h, h,-h,  0,1,0 }, { h, h,-h,  0,1,0 }, { h, h, h,  0,1,0 }, {-h, h, h,  0,1,0 },
            // -Y
            {-h,-h, h,  0,-1,0}, { h,-h, h,  0,-1,0}, { h,-h,-h,  0,-1,0}, {-h,-h,-h,  0,-1,0},
            // +Z
            { h,-h, h,  0,0,1 }, {-h,-h, h,  0,0,1 }, {-h, h, h,  0,0,1 }, { h, h, h,  0,0,1 },
            // -Z
            {-h,-h,-h,  0,0,-1}, { h,-h,-h,  0,0,-1}, { h, h,-h,  0,0,-1}, {-h, h,-h,  0,0,-1},
        };
        m.vertices.assign(std::begin(v), std::end(v));

        const uint16_t idx[] = {
            0,1,2, 0,2,3,
            4,5,6, 4,6,7,
            8,9,10, 8,10,11,
            12,13,14, 12,14,15,
            16,17,18, 16,18,19,
            20,21,22, 20,22,23
        };
        m.indices.assign(std::begin(idx), std::end(idx));

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
    // (Some boxes are assigned different light masks to demo light grouping.)
    auto addBox = [&](king::Float3 pos, king::Float3 scale, king::Float4 albedo, uint32_t lightMask = 0xFFFFFFFFu, bool receivesShadows = true)
    {
        king::Entity e = scene.reg.CreateEntity();
        auto& t = scene.reg.transforms.Emplace(e);
        t.position = pos;
        t.scale = scale;
        auto& r = scene.reg.renderers.Emplace(e);
        r.mesh = cubeMesh;
        r.material.albedo = albedo;
        r.material.shader = "pbr_test";
        r.lightMask = lightMask;
        r.receivesShadows = receivesShadows;
    };

    // Light groups
    constexpr uint32_t kAll = 0xFFFFFFFFu;
    constexpr uint32_t kRedGroup = 1u << 0;
    constexpr uint32_t kBlueGroup = 1u << 1;

    addBox({ -3.0f, 0.0f,  1.0f }, { 1.0f, 1.0f, 1.0f }, { 0.9f, 0.2f, 0.2f, 1.0f }, kAll);
    addBox({  0.0f, 0.0f,  2.0f }, { 1.0f, 2.0f, 1.0f }, { 0.2f, 0.6f, 0.9f, 1.0f }, kAll);
    addBox({  3.0f, 0.0f,  0.0f }, { 1.0f, 1.0f, 2.0f }, { 0.9f, 0.85f, 0.2f, 1.0f }, kAll);

    // A slightly denser cluster to make lighting/shadows obvious.
    for (int z = 0; z < 4; ++z)
    {
        for (int x = 0; x < 5; ++x)
        {
            const float fx = -4.0f + (float)x * 2.0f;
            const float fz =  4.0f + (float)z * 2.0f;
            const uint32_t mask = ((x + z) & 1) ? kRedGroup : kBlueGroup;
            const king::Float4 col = ((x + z) & 1) ? king::Float4{ 0.85f, 0.35f, 0.35f, 1.0f } : king::Float4{ 0.35f, 0.45f, 0.95f, 1.0f };
            addBox({ fx, 0.0f, fz }, { 0.9f, 0.9f, 0.9f }, col, mask);

            // Stack a few columns for depth/shadowing.
            if (((x * 13 + z * 7) % 5) == 0)
                addBox({ fx, 1.1f, fz }, { 0.9f, 1.6f, 0.9f }, { 0.85f, 0.85f, 0.85f, 1.0f }, kAll);
        }
    }

    // Group-specific boxes (only affected by their matching light)
    addBox({ -2.0f, 0.0f, -2.5f }, { 0.75f, 0.75f, 0.75f }, { 0.9f, 0.3f, 0.3f, 1.0f }, kRedGroup);
    addBox({  2.0f, 0.0f, -2.5f }, { 0.75f, 1.25f, 0.75f }, { 0.3f, 0.3f, 0.95f, 1.0f }, kBlueGroup);

    // Shadow reception demo: one box that doesn't receive shadows
    addBox({ 0.0f, 0.0f, -1.0f }, { 1.0f, 1.0f, 1.0f }, { 0.85f, 0.85f, 0.85f, 1.0f }, kAll, false);

    // (Old extra lights removed to keep exposure under control; the 4 moving point lights above cover the demo.)

    king::render::d3d11::RenderDeviceD3D11 device;
    HRESULT initHr = device.Initialize(window.Handle(), width, height);
    if (FAILED(initHr))
    {
        LogHResult("RenderDeviceD3D11::Initialize", initHr);
        std::printf("Failed to initialize D3D11 device.\n");
        MessageBoxW(window.Handle(), L"Failed to initialize D3D11. Check the debug output for details.", L"Error", MB_ICONERROR);
        return 1;
    }

    // Render system (D3D11)
    const std::wstring shaderPath = JoinPath(GetExeDirectory(), L"..\\..\\assets\\shaders\\pbr_test.hlsl");
    king::render::d3d11::RenderSystemD3D11 renderSystem;
    if (!renderSystem.Initialize(device, shaderPath))
    {
        std::printf("Failed to initialize render system.\n");
        MessageBoxW(window.Handle(), L"Failed to initialize render system.", L"Error", MB_ICONERROR);
        return 1;
    }

    device.QueueResize(width, height);

    king::Frustum frustum{};
    king::Mat4x4 viewProj{};

    InputState input;
    king::Time time;
    time.SetFixedDeltaSeconds(1.0 / 60.0);
    time.SetMaxDeltaSeconds(0.10);
    time.Reset();

    king::Window::Event ev{};
    while (window.PumpMessages())
    {
        time.Tick();
        if (time.FpsUpdated())
        {
            const double fps = time.Fps();
            const double ms = (fps > 1e-6) ? (1000.0 / fps) : 0.0;
            std::printf("FPS: %.1f (%.2f ms)\n", fps, ms);
        }

        input.mouseDeltaX = 0.0f;
        input.mouseDeltaY = 0.0f;

        while (window.PollEvent(ev))
        {
            if (ev.type == king::Window::EventType::Resize)
            {
                if (ev.width > 0 && ev.height > 0)
                    device.QueueResize((uint32_t)ev.width, (uint32_t)ev.height);
            }
            else if (ev.type == king::Window::EventType::CloseRequested)
            {
                std::printf("Close requested.\n");
            }
            else if (ev.type == king::Window::EventType::FocusGained)
            {
                input.hasFocus = true;
                input.haveMousePos = false;
            }
            else if (ev.type == king::Window::EventType::FocusLost)
            {
                input.hasFocus = false;
                input.rmbDown = false;
                input.haveMousePos = false;
                std::memset(input.keys, 0, sizeof(input.keys));
            }
            else if (ev.type == king::Window::EventType::KeyDown)
            {
                if (ev.key < 256)
                    input.keys[ev.key] = true;
            }
            else if (ev.type == king::Window::EventType::KeyUp)
            {
                if (ev.key < 256)
                    input.keys[ev.key] = false;
            }
            else if (ev.type == king::Window::EventType::MouseButtonDown)
            {
                if (ev.button == king::Window::MouseButton::Right)
                {
                    input.rmbDown = true;
                    input.haveMousePos = false;
                    SetCapture(window.Handle());
                }
            }
            else if (ev.type == king::Window::EventType::MouseButtonUp)
            {
                if (ev.button == king::Window::MouseButton::Right)
                {
                    input.rmbDown = false;
                    input.haveMousePos = false;
                    ReleaseCapture();
                }
            }
            else if (ev.type == king::Window::EventType::MouseMove)
            {
                if (!input.haveMousePos)
                {
                    input.lastMouseX = ev.mouseX;
                    input.lastMouseY = ev.mouseY;
                    input.haveMousePos = true;
                }
                else
                {
                    const int32_t dx = ev.mouseX - input.lastMouseX;
                    const int32_t dy = ev.mouseY - input.lastMouseY;
                    input.lastMouseX = ev.mouseX;
                    input.lastMouseY = ev.mouseY;

                    if (input.rmbDown)
                    {
                        input.mouseDeltaX += (float)dx;
                        input.mouseDeltaY += (float)dy;
                    }
                }
            }
        }

        // Skip rendering while minimized.
        if (IsIconic(window.Handle()))
        {
            Sleep(16);
            continue;
        }

        // Pass 0: apply renderer-owned resize and notify systems.
        uint32_t newW = 0, newH = 0;
        if (device.ApplyQueuedResize(&newW, &newH))
        {
            const float aspect = (newH > 0) ? ((float)newW / (float)newH) : (16.0f / 9.0f);
            king::systems::CameraSystem::OnResize(scene, aspect);
        }

        // Pass 1: update simulation systems.
        // Mouse look is per-frame (uses latest input), movement/animations are fixed-step.
        for (auto e : scene.reg.cameras.Entities())
        {
            auto* cc = scene.reg.cameras.TryGet(e);
            auto* t = scene.reg.transforms.TryGet(e);
            if (!cc || !t || !cc->primary)
                continue;

            if (input.hasFocus && input.rmbDown)
            {
                const float sens = 0.0025f;
                const float yaw = input.mouseDeltaX * sens;
                const float pitch = input.mouseDeltaY * sens;
                cc->camera.RotateYawPitchRoll(yaw, pitch, 0.0f);
            }
            break;
        }

        int fixedSteps = 0;
        constexpr int kMaxFixedStepsPerFrame = 6;
        while (time.ConsumeFixedStep() && fixedSteps++ < kMaxFixedStepsPerFrame)
        {
            const float stepDt = (float)time.FixedDeltaSeconds();
            const float tNow = (float)time.FixedTimeSeconds();

            // Animate the 4 point lights (fixed-step time base).
            {
                const king::Float3 center = { 0.0f, 0.5f, 6.0f };
                const float r0 = 8.0f;
                const float r1 = 6.0f;
                const float h0 = 3.0f;

                const king::Float3 pos[4] = {
                    { center.x + std::cos(tNow * 0.70f) * r0, h0 + std::sin(tNow * 1.10f) * 0.6f, center.z + std::sin(tNow * 0.70f) * r0 },
                    { center.x + std::cos(tNow * 0.85f + 1.6f) * r1, h0 + std::sin(tNow * 0.90f + 0.8f) * 0.5f, center.z + std::sin(tNow * 0.85f + 1.6f) * r1 },
                    { center.x + std::cos(tNow * 0.60f + 3.2f) * r0, h0 + std::sin(tNow * 1.30f + 2.2f) * 0.7f, center.z + std::sin(tNow * 0.60f + 3.2f) * r0 },
                    { center.x + std::cos(tNow * 0.95f + 4.7f) * r1, h0 + std::sin(tNow * 1.00f + 3.6f) * 0.6f, center.z + std::sin(tNow * 0.95f + 4.7f) * r1 },
                };

                for (int i = 0; i < 4; ++i)
                {
                    if (auto* lt = scene.reg.transforms.TryGet(movingLights[i]))
                        lt->position = pos[i];
                }
            }

            // Fixed-step camera movement (locomotion uses camera forward/right).
            for (auto e : scene.reg.cameras.Entities())
            {
                auto* cc = scene.reg.cameras.TryGet(e);
                auto* tr = scene.reg.transforms.TryGet(e);
                if (!cc || !tr || !cc->primary)
                    continue;

                const float speed = (input.keys[VK_SHIFT] ? 10.0f : 4.0f);

                const king::Float3 f0 = cc->camera.Forward();
                const king::Float3 r0 = cc->camera.Right();
                king::Float3 f = { f0.x, 0.0f, f0.z };
                king::Float3 r = { r0.x, 0.0f, r0.z };
                const float fl = std::sqrt(f.x * f.x + f.z * f.z);
                const float rl = std::sqrt(r.x * r.x + r.z * r.z);
                if (fl > 1e-5f) { f.x /= fl; f.z /= fl; }
                if (rl > 1e-5f) { r.x /= rl; r.z /= rl; }

                float forwardAxis = 0.0f;
                float rightAxis = 0.0f;
                if (input.keys['W']) forwardAxis += 1.0f;
                if (input.keys['S']) forwardAxis -= 1.0f;
                if (input.keys['D']) rightAxis += 1.0f;
                if (input.keys['A']) rightAxis -= 1.0f;

                king::Float3 worldDelta{
                    (f.x * forwardAxis + r.x * rightAxis) * speed * stepDt,
                    0.0f,
                    (f.z * forwardAxis + r.z * rightAxis) * speed * stepDt,
                };
                if (input.keys['E']) worldDelta.y += speed * stepDt;
                if (input.keys['Q']) worldDelta.y -= speed * stepDt;

                if (worldDelta.x != 0.0f || worldDelta.y != 0.0f || worldDelta.z != 0.0f)
                {
                    const king::Float3 p = cc->camera.Position();
                    cc->camera.SetPosition({ p.x + worldDelta.x, p.y + worldDelta.y, p.z + worldDelta.z });
                }

                tr->position = cc->camera.Position();
                break;
            }
        }

        king::Float3 primaryCamPos{ 0, 0, 0 };
        for (auto e : scene.reg.cameras.Entities())
        {
            auto* cc = scene.reg.cameras.TryGet(e);
            auto* tr = scene.reg.transforms.TryGet(e);
            if (cc && tr && cc->primary)
            {
                primaryCamPos = tr->position;
                break;
            }
        }

        (void)king::systems::CameraSystem::UpdatePrimaryCamera(scene, frustum, viewProj);

        // Pass 2: render passes.
        const float clearColor[4] = { 0.06f, 0.06f, 0.08f, 1.0f };
        device.BeginFrame(clearColor);
        // Slightly lower exposure to reduce overall brightness.
        renderSystem.RenderGeometryPass(device, scene, frustum, viewProj, primaryCamPos, 0.75f);

        // Uncapped: don't wait for v-sync.
        HRESULT phr = device.Present(0);
        if (FAILED(phr))
        {
            if (device.IsDeviceLost(phr))
            {
                LogHResult("Present(device lost)", phr);
                LogHResult("DeviceRemovedReason", device.GetDeviceRemovedReason());

                king::render::d3d11::RenderSystemD3D11::ReleaseSceneMeshBuffers(scene);
                if (!device.RecoverFromDeviceLost(window.Handle()) || !renderSystem.OnDeviceReset(device))
                {
                    std::printf("Failed to recover from device lost. Exiting.\n");
                    break;
                }

                // Re-seed resize after device reset.
                RECT r{};
                GetClientRect(window.Handle(), &r);
                device.QueueResize((uint32_t)(r.right - r.left), (uint32_t)(r.bottom - r.top));
            }
            else
            {
                LogHResult("Present", phr);
            }
        }
    }

    king::render::d3d11::RenderSystemD3D11::ReleaseSceneMeshBuffers(scene);
    renderSystem.Shutdown();
    device.Shutdown();
    return 0;
}
