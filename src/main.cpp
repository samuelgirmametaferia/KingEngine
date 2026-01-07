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
#include <cstring>
#include <vector>

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

    // Clear once so the CLI starts clean (avoids leaving build logs in view).
    // Do NOT clear repeatedly (that would flicker).
    {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h && h != INVALID_HANDLE_VALUE)
        {
            CONSOLE_SCREEN_BUFFER_INFO csbi{};
            if (GetConsoleScreenBufferInfo(h, &csbi))
            {
                const DWORD cellCount = (DWORD)csbi.dwSize.X * (DWORD)csbi.dwSize.Y;
                DWORD written = 0;
                COORD home{ 0, 0 };
                FillConsoleOutputCharacterA(h, ' ', cellCount, home, &written);
                FillConsoleOutputAttribute(h, csbi.wAttributes, cellCount, home, &written);
                SetConsoleCursorPosition(h, home);
            }
        }
    }
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

static bool EnvFlag(const wchar_t* name)
{
    wchar_t buf[8]{};
    DWORD n = GetEnvironmentVariableW(name, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0)
        return false;
    return (buf[0] == L'1' || buf[0] == L't' || buf[0] == L'T' || buf[0] == L'y' || buf[0] == L'Y');
}

static uint32_t EnvUInt(const wchar_t* name, uint32_t defaultValue = 0)
{
    wchar_t buf[32]{};
    DWORD n = GetEnvironmentVariableW(name, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0)
        return defaultValue;
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(buf, &end, 10);
    (void)end;
    return (uint32_t)v;
}

static std::wstring EnvWString(const wchar_t* name)
{
    std::wstring out;
    wchar_t buf[1024]{};
    DWORD n = GetEnvironmentVariableW(name, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= (DWORD)(sizeof(buf) / sizeof(buf[0])))
        return out;
    out.assign(buf, buf + n);
    return out;
}

static double FindPerfMs(const std::vector<king::perf::PerfAnalyzer::Sample>& samples, const char* name, bool gpu)
{
    for (const auto& s : samples)
    {
        if (!s.name || !name)
            continue;
        if (s.name == name || std::strcmp(s.name, name) == 0)
            return gpu ? s.gpuMs : s.cpuMs;
    }
    return gpu ? -1.0 : 0.0;
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

    float cameraNearZ = 0.1f;
    float cameraFarZ = 2000.0f;

    const bool stressTest = EnvFlag(L"KING_STRESS_TEST");

    // Demo sphere "big sphere" layout params (also used for lighting + motion).
    const king::Float3 bigCenter{ 0.0f, 3.25f, 10.0f };
    const float bigRadius = 4.0f;

    // --- ECS sample scene ---
    king::Scene scene;

    // Camera entity
    king::Entity camEnt = scene.reg.CreateEntity();
    {
        auto& t = scene.reg.transforms.Emplace(camEnt);
        // Pull back a bit so the material grid is visible immediately.
        t.position = { 0.0f, 5.0f, -18.0f };

        auto& cc = scene.reg.cameras.Emplace(camEnt);
        cc.primary = true;

        king::PerspectiveParams persp;
        persp.aspect = (height > 0) ? ((float)width / (float)height) : (16.0f / 9.0f);
        cameraNearZ = persp.nearZ;
        cameraFarZ = persp.farZ;
        cc.camera.SetPerspective(persp);
        cc.camera.SetPosition(t.position);
    }

    // Lights: disabled in stress test (colors only).
    // Normal mode: exactly one point light at the center of the big sphere.
    if (!stressTest)
    {
        king::Entity e = scene.reg.CreateEntity();
        auto& t = scene.reg.transforms.Emplace(e);
        t.position = bigCenter;

        auto& l = scene.reg.lights.Emplace(e);
        l.type = king::LightType::Point;
        l.color = { 1.0f, 0.95f, 0.85f };
        // Night scene: keep range tight so the ground isn't lit too strongly.
        l.intensity = 8.0f;
        l.range = 14.0f;
        l.castsShadows = false;
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

    auto makeGroundPlaneMesh = [&](float halfExtents, int segments) -> king::Entity
    {
        const float h = halfExtents;
        king::Entity me = scene.reg.CreateEntity();
        auto& m = scene.reg.meshes.Emplace(me);

        // Subdivided grid plane (top face only), centered at origin on Y=0.
        // This makes the sandbox less "low poly" and helps diagnose any per-vertex artifacts.
        // Note: indices are uint16 in this demo mesh container.
        // Keep (segments + 1)^2 <= 65535.
        segments = std::max(1, std::min(segments, 254));
        const int vertsPerSide = segments + 1;
        m.vertices.reserve((size_t)vertsPerSide * (size_t)vertsPerSide);

        for (int z = 0; z <= segments; ++z)
        {
            const float tz = (float)z / (float)segments;
            const float pz = (-h) + (2.0f * h) * tz;
            for (int x = 0; x <= segments; ++x)
            {
                const float tx = (float)x / (float)segments;
                const float px = (-h) + (2.0f * h) * tx;
                m.vertices.push_back(king::VertexPN{ px, 0.0f, pz, 0, 1, 0 });
            }
        }

        m.indices.reserve((size_t)segments * (size_t)segments * 6u);
        for (int z = 0; z < segments; ++z)
        {
            for (int x = 0; x < segments; ++x)
            {
                const uint32_t i0 = (uint32_t)(z * vertsPerSide + x);
                const uint32_t i1 = i0 + 1;
                const uint32_t i2 = (uint32_t)((z + 1) * vertsPerSide + x + 1);
                const uint32_t i3 = (uint32_t)((z + 1) * vertsPerSide + x);

                // Indices are uint16 in this demo mesh container.
                m.indices.push_back((uint16_t)i0);
                m.indices.push_back((uint16_t)i1);
                m.indices.push_back((uint16_t)i2);
                m.indices.push_back((uint16_t)i0);
                m.indices.push_back((uint16_t)i2);
                m.indices.push_back((uint16_t)i3);
            }
        }

        m.boundsCenter = { 0, 0, 0 };
        // Sphere radius that covers the plane; scaled by transform later.
        m.boundsRadius = halfExtents * 1.45f;
        return me;
    };

    auto makeSphereMesh = [&](float radius, int slices, int stacks) -> king::Entity
    {
        // UV sphere (lat/long). Indices are uint16 in this demo mesh container.
        slices = std::max(3, std::min(slices, 128));
        stacks = std::max(2, std::min(stacks, 128));

        king::Entity me = scene.reg.CreateEntity();
        auto& m = scene.reg.meshes.Emplace(me);

        const int vertsPerRing = slices + 1;
        const int rings = stacks + 1;
        const int vertCount = vertsPerRing * rings;
        if (vertCount > 65000)
        {
            // Clamp to stay under uint16 indexing.
            stacks = std::min(stacks, 60);
            slices = std::min(slices, 60);
        }

        const float pi = 3.14159265358979323846f;
        const float twoPi = 6.28318530717958647692f;

        m.vertices.reserve((size_t)(slices + 1) * (size_t)(stacks + 1));
        for (int y = 0; y <= stacks; ++y)
        {
            const float v = (float)y / (float)stacks;
            const float phi = v * pi; // 0..pi
            const float sp = std::sinf(phi);
            const float cp = std::cosf(phi);

            for (int x = 0; x <= slices; ++x)
            {
                const float u = (float)x / (float)slices;
                const float theta = u * twoPi; // 0..2pi
                const float st = std::sinf(theta);
                const float ct = std::cosf(theta);

                const float nx = sp * ct;
                const float ny = cp;
                const float nz = sp * st;

                king::VertexPN vtx;
                vtx.x = nx * radius;
                vtx.y = ny * radius;
                vtx.z = nz * radius;
                vtx.nx = nx;
                vtx.ny = ny;
                vtx.nz = nz;
                m.vertices.push_back(vtx);
            }
        }

        m.indices.reserve((size_t)slices * (size_t)stacks * 6u);
        for (int y = 0; y < stacks; ++y)
        {
            for (int x = 0; x < slices; ++x)
            {
                const uint16_t i0 = (uint16_t)(y * (slices + 1) + x);
                const uint16_t i1 = (uint16_t)(y * (slices + 1) + x + 1);
                const uint16_t i2 = (uint16_t)((y + 1) * (slices + 1) + x);
                const uint16_t i3 = (uint16_t)((y + 1) * (slices + 1) + x + 1);

                // CCW winding when viewed from outside.
                m.indices.push_back(i0);
                m.indices.push_back(i2);
                m.indices.push_back(i1);

                m.indices.push_back(i1);
                m.indices.push_back(i2);
                m.indices.push_back(i3);
            }
        }

        m.boundsCenter = { 0, 0, 0 };
        m.boundsRadius = radius;
        return me;
    };

    // Shared sphere mesh for the material grid.
    king::Entity sphereMesh = makeSphereMesh(0.5f, 32, 16);

    // Sphere cluster helper.
    auto addSphere = [&](king::Float3 pos, king::Float3 scale, king::Float4 albedo, float roughness, float metallic, king::Float3 emissive) -> king::Entity
    {
        king::Entity e = scene.reg.CreateEntity();
        auto& t = scene.reg.transforms.Emplace(e);
        t.position = pos;
        t.scale = scale;

        auto& r = scene.reg.renderers.Emplace(e);
        r.mesh = sphereMesh;
        r.material.shader = stressTest ? "unlit" : "pbr";
        r.material.shadingModel = stressTest ? king::MaterialShadingModel::Unlit : king::MaterialShadingModel::Pbr;
        r.material.blendMode = king::MaterialBlendMode::Opaque;
        r.material.albedo = albedo;
        r.material.roughness = roughness;
        r.material.metallic = metallic;
        r.material.emissive = emissive;
        r.receivesShadows = !stressTest;
        r.castsShadows = !stressTest;
        r.lightMask = 0xFFFFFFFFu;

        return e;
    };

    // Keep track of the normal-mode sphere entities so we can animate them.
    std::vector<king::Entity> sphereEntities;

    // Normal-mode sphere count control.
    uint32_t sphereTargetCount = EnvUInt(L"KING_SPHERE_COUNT", 260u);
    sphereTargetCount = std::max(0u, std::min(sphereTargetCount, 2000u));

    auto clamp01 = [](float v) { return (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v); };

    // Default demo spheres: skip in stress test (we'll add incrementally).
    const float smallScale = 0.55f; // sphere mesh radius is 0.5 -> ~0.275 world radius
    const float pi = 3.14159265358979323846f;
    const float golden = 1.61803398874989484820f;
    auto EnsureSphereCount = [&](uint32_t desired)
    {
        if (stressTest)
            return;

        desired = std::max(0u, std::min(desired, 2000u));
        if (sphereEntities.size() > desired)
        {
            while (sphereEntities.size() > desired)
            {
                king::Entity e = sphereEntities.back();
                sphereEntities.pop_back();
                scene.reg.DestroyEntity(e);
            }
        }
        else if (sphereEntities.size() < desired)
        {
            const uint32_t start = (uint32_t)sphereEntities.size();
            sphereEntities.reserve(desired);
            for (uint32_t i = start; i < desired; ++i)
            {
                const uint32_t sphereCount = desired;

                // Fibonacci sphere distribution (uniform-ish over surface).
                const float t = (sphereCount > 1) ? ((float)i / (float)(sphereCount - 1)) : 0.0f;
                const float y = 1.0f - 2.0f * t;
                const float rr = std::sqrtf(std::max(0.0f, 1.0f - y * y));
                const float theta = 2.0f * pi * ((float)i / golden);
                const float x = std::cosf(theta) * rr;
                const float z = std::sinf(theta) * rr;

                float roughness = clamp01(0.05f + 0.95f * std::abs(y));
                float metallic = clamp01(0.5f + 0.5f * x);

                king::Float4 albedo;
                albedo.x = 0.65f + 0.25f * clamp01(0.5f + 0.5f * z);
                albedo.y = 0.65f + 0.20f * clamp01(0.5f + 0.5f * y);
                albedo.z = 0.65f + 0.25f * clamp01(0.5f + 0.5f * x);
                albedo.w = 1.0f;

                king::Float3 emissive{ 0.0f, 0.0f, 0.0f };
                if (std::abs(y) < 0.07f)
                    emissive = { 0.15f, 0.55f, 1.25f };

                const king::Float3 pos{ bigCenter.x + x * bigRadius, bigCenter.y + y * bigRadius, bigCenter.z + z * bigRadius };
                sphereEntities.push_back(addSphere(pos, { smallScale, smallScale, smallScale }, albedo, roughness, metallic, emissive));
            }
        }
    };

    if (!stressTest)
        EnsureSphereCount(sphereTargetCount);

    // (No extra lights.)

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
    king::Mat4x4 view{};
    king::Mat4x4 proj{};

    InputState input;
    king::Time time;
    time.SetFixedDeltaSeconds(1.0 / 60.0);
    time.SetMaxDeltaSeconds(0.10);
    time.Reset();

    // Console FPS (computed from time delta; printed once per ~1s).
    double fpsWindowSeconds = 0.0;
    uint32_t fpsWindowFrames = 0;

    // Runtime-tunable post settings (hotkeys below).
    bool postEnableTonemap = true;
    bool postEnableVignette = true;
    float postVignetteStrength = 0.35f;
    float postVignettePower = 2.2f;
    bool postEnableBloom = true;
    float postBloomIntensity = 0.70f;
    float postBloomThreshold = 1.15f;

    bool prevKeys[256]{};

    // Stress test config (colors only).
    const uint32_t stressMaxSpheres = EnvUInt(L"KING_STRESS_MAX_SPHERES", 5000u);
    const uint32_t stressStep = EnvUInt(L"KING_STRESS_STEP", 20u);
    const uint32_t stressWarmupFrames = EnvUInt(L"KING_STRESS_WARMUP_FRAMES", 60u);
    const uint32_t stressSampleFrames = EnvUInt(L"KING_STRESS_SAMPLE_FRAMES", 240u);

    uint32_t stressCurrentTarget = 0;
    bool stressPrintedHeader = false;
    uint32_t stressWarmupLeft = stressWarmupFrames;
    uint32_t stressSampleLeft = stressSampleFrames;
    double stressSumFps = 0.0;
    uint32_t stressFpsSamples = 0;
    double stressSumCpuMs = 0.0;
    double stressSumGpuMs = 0.0;
    uint32_t stressMsSamples = 0;

    if (stressTest)
    {
        // Enable perf tooling only for the stress test.
        renderSystem.SetPerfEnabled(true);
        renderSystem.SetGpuPerfEnabled(true);

        // Disable default overlay spam; we will print our own per-step summary.
        renderSystem.SetPerfPrintToStdout(false);
    }

    FILE* stressCsv = nullptr;
    std::wstring stressCsvPath;
    if (stressTest)
    {
        stressCsvPath = EnvWString(L"KING_STRESS_CSV");
        if (stressCsvPath.empty())
            stressCsvPath = JoinPath(GetExeDirectory(), L"stress_results.csv");

        _wfopen_s(&stressCsv, stressCsvPath.c_str(), L"wb");
        if (stressCsv)
        {
            std::fprintf(stressCsv, "spheres,avg_fps,avg_cpu_ms,avg_gpu_ms\n");
            std::fflush(stressCsv);
        }
    }

    king::Window::Event ev{};
    while (window.PumpMessages())
    {
        time.Tick();
        if (time.FpsUpdated())
            renderSystem.SetFps(time.Fps());

        // FPS via time delta (stable average over ~1 second).
        {
            const double dt = time.DeltaSeconds();
            if (dt > 0.0)
            {
                fpsWindowSeconds += dt;
                fpsWindowFrames += 1;
                if (fpsWindowSeconds >= 1.0)
                {
                    const double fps = (fpsWindowSeconds > 0.0) ? ((double)fpsWindowFrames / fpsWindowSeconds) : 0.0;
                    std::printf("FPS: %.1f\n", fps);
                    fpsWindowSeconds = 0.0;
                    fpsWindowFrames = 0;
                }
            }
        }

        // Normal-mode sphere motion (disabled in stress test).
        // Set KING_DISABLE_SPHERE_MOTION=1 to stop.
        if (!stressTest && !EnvFlag(L"KING_DISABLE_SPHERE_MOTION"))
        {
            const float tsec = (float)time.TotalSeconds();

            // Recompute the base Fibonacci directions and apply a gentle orbit + bob.
            const int sphereCount = (int)sphereEntities.size();
            if (sphereCount > 0)
            {
                const float orbitSpeed = 0.25f;
                const float bobSpeed = 1.35f;
                const float bobAmp = 0.25f;

                for (int i = 0; i < sphereCount; ++i)
                {
                    const float tt = (sphereCount > 1) ? ((float)i / (float)(sphereCount - 1)) : 0.0f;
                    const float y = 1.0f - 2.0f * tt;
                    const float rr = std::sqrtf(std::max(0.0f, 1.0f - y * y));
                    const float theta0 = 2.0f * pi * ((float)i / golden);
                    const float x0 = std::cosf(theta0) * rr;
                    const float z0 = std::sinf(theta0) * rr;

                    // Rotate around Y.
                    const float a = tsec * orbitSpeed;
                    const float ca = std::cosf(a);
                    const float sa = std::sinf(a);
                    const float x = x0 * ca - z0 * sa;
                    const float z = x0 * sa + z0 * ca;

                    const float bob = bobAmp * std::sinf(tsec * bobSpeed + (float)i * 0.13f);
                    const king::Float3 pos{ bigCenter.x + x * bigRadius,
                                            bigCenter.y + y * bigRadius + bob,
                                            bigCenter.z + z * bigRadius };

                    king::Entity e = sphereEntities[(size_t)i];
                    auto* tr = scene.reg.transforms.TryGet(e);
                    if (tr)
                        tr->position = pos;
                }
            }
        }

        if (stressTest)
        {
            // Ensure scene contains exactly `stressCurrentTarget` spheres.
            // We'll add spheres in batches as target increases.
            // Layout: simple grid in front of camera.
            const float spacing = 1.25f;
            const uint32_t currentCount = (uint32_t)scene.reg.renderers.Entities().size();
            if (currentCount < stressCurrentTarget)
            {
                const uint32_t toAdd = stressCurrentTarget - currentCount;
                for (uint32_t i = 0; i < toAdd; ++i)
                {
                    const uint32_t idx = currentCount + i;
                    const uint32_t cols = 100;
                    const uint32_t x = idx % cols;
                    const uint32_t z = idx / cols;

                    king::Float4 albedo{ (float)((idx * 97u) % 255u) / 255.0f,
                                        (float)((idx * 57u) % 255u) / 255.0f,
                                        (float)((idx * 17u) % 255u) / 255.0f,
                                        1.0f };
                    king::Float3 pos{ -((float)cols * 0.5f) * spacing + (float)x * spacing,
                                     0.75f,
                                     6.0f + (float)z * spacing };
                    addSphere(pos, { smallScale, smallScale, smallScale }, albedo, 1.0f, 0.0f, { 0, 0, 0 });
                }
            }
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

                    // Mouse-look is enabled when holding RMB, or when holding Alt.
                    if (input.rmbDown || input.keys[VK_MENU])
                    {
                        input.mouseDeltaX += (float)dx;
                        input.mouseDeltaY += (float)dy;
                    }
                }
            }
        }

        // Hotkeys for sphere count (normal mode only):
        // - = decrease by 20
        // + = increase by 20
        if (!stressTest)
        {
            auto KeyPressedOnce = [&](int vk) -> bool
            {
                const bool down = input.keys[vk];
                const bool pressed = down && !prevKeys[vk];
                return pressed;
            };

            if (KeyPressedOnce(VK_OEM_MINUS))
            {
                sphereTargetCount = (sphereTargetCount >= 20u) ? (sphereTargetCount - 20u) : 0u;
                EnsureSphereCount(sphereTargetCount);
                std::printf("[Spheres] count=%u\n", sphereTargetCount);
            }
            if (KeyPressedOnce(VK_OEM_PLUS) || KeyPressedOnce(VK_OEM_5))
            {
                // VK_OEM_PLUS is usually '=' key; VK_OEM_5 is a fallback on some layouts.
                sphereTargetCount = std::min(2000u, sphereTargetCount + 20u);
                EnsureSphereCount(sphereTargetCount);
                std::printf("[Spheres] count=%u\n", sphereTargetCount);
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

            if (input.hasFocus && (input.rmbDown || input.keys[VK_MENU]))
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

            (void)tNow;

            // Fixed-step camera movement (locomotion uses camera forward/right).
            for (auto e : scene.reg.cameras.Entities())
            {
                auto* cc = scene.reg.cameras.TryGet(e);
                auto* tr = scene.reg.transforms.TryGet(e);
                if (!cc || !tr || !cc->primary)
                    continue;

                // Keyboard camera rotation (works even without RMB/mouse).
                if (input.hasFocus)
                {
                    float yawAxis = 0.0f;
                    float pitchAxis = 0.0f;

                    if (input.keys[VK_LEFT] || input.keys['J']) yawAxis -= 1.0f;
                    if (input.keys[VK_RIGHT] || input.keys['L']) yawAxis += 1.0f;
                    if (input.keys[VK_UP] || input.keys['I']) pitchAxis -= 1.0f;
                    if (input.keys[VK_DOWN] || input.keys['K']) pitchAxis += 1.0f;

                    if (yawAxis != 0.0f || pitchAxis != 0.0f)
                    {
                        const float rotSpeed = input.keys[VK_SHIFT] ? 2.0f : 1.2f; // rad/sec
                        cc->camera.RotateYawPitchRoll(yawAxis * rotSpeed * stepDt, pitchAxis * rotSpeed * stepDt, 0.0f);
                    }
                }

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
                view = cc->camera.ViewMatrix();
                proj = cc->camera.ProjectionMatrix();
                break;
            }
        }

        (void)king::systems::CameraSystem::UpdatePrimaryCamera(scene, frustum, viewProj);

        // Post hotkeys (tap):
        // - B: toggle bloom
        // - V: toggle vignette
        // - T: toggle tonemap
        // - R: reset post defaults
        // - [ / ]: vignette strength -/+
        // - ; / ': vignette power -/+
        // - , / .: bloom intensity -/+
        // - O / P: bloom threshold -/+
        auto KeyPressed = [&](int vk) -> bool
        {
            if (vk < 0 || vk >= 256)
                return false;
            return input.keys[vk] && !prevKeys[vk];
        };

        auto Clamp = [](float v, float lo, float hi) -> float
        {
            return (v < lo) ? lo : (v > hi) ? hi : v;
        };

        const float stepSmall = input.keys[VK_SHIFT] ? 0.10f : 0.02f;
        const float stepPower = input.keys[VK_SHIFT] ? 0.50f : 0.10f;
        const float stepBloom = input.keys[VK_SHIFT] ? 0.20f : 0.05f;

        bool postChanged = false;
        if (KeyPressed('B')) { postEnableBloom = !postEnableBloom; postChanged = true; }
        if (KeyPressed('V')) { postEnableVignette = !postEnableVignette; postChanged = true; }
        if (KeyPressed('T')) { postEnableTonemap = !postEnableTonemap; postChanged = true; }

        if (KeyPressed('R'))
        {
            postEnableTonemap = true;
            postEnableVignette = true;
            postVignetteStrength = 0.35f;
            postVignettePower = 2.2f;
            postEnableBloom = true;
            postBloomIntensity = 0.70f;
            postBloomThreshold = 1.15f;
            postChanged = true;
        }

        if (KeyPressed(VK_OEM_4)) { postVignetteStrength = Clamp(postVignetteStrength - stepSmall, 0.0f, 1.0f); postChanged = true; } // [
        if (KeyPressed(VK_OEM_6)) { postVignetteStrength = Clamp(postVignetteStrength + stepSmall, 0.0f, 1.0f); postChanged = true; } // ]
        if (KeyPressed(VK_OEM_1)) { postVignettePower = Clamp(postVignettePower - stepPower, 0.25f, 8.0f); postChanged = true; }      // ;
        if (KeyPressed(VK_OEM_7)) { postVignettePower = Clamp(postVignettePower + stepPower, 0.25f, 8.0f); postChanged = true; }      // '

        if (KeyPressed(VK_OEM_COMMA))  { postBloomIntensity = Clamp(postBloomIntensity - stepBloom, 0.0f, 5.0f); postChanged = true; } // ,
        if (KeyPressed(VK_OEM_PERIOD)) { postBloomIntensity = Clamp(postBloomIntensity + stepBloom, 0.0f, 5.0f); postChanged = true; } // .
        if (KeyPressed('O')) { postBloomThreshold = Clamp(postBloomThreshold - stepBloom, 0.0f, 10.0f); postChanged = true; }
        if (KeyPressed('P')) { postBloomThreshold = Clamp(postBloomThreshold + stepBloom, 0.0f, 10.0f); postChanged = true; }

        if (postChanged)
        {
            std::printf(
                "[Post] Tonemap=%d | Vignette=%d (strength=%.2f power=%.2f) | Bloom=%d (intensity=%.2f threshold=%.2f)\n",
                postEnableTonemap ? 1 : 0,
                postEnableVignette ? 1 : 0,
                postVignetteStrength,
                postVignettePower,
                postEnableBloom ? 1 : 0,
                postBloomIntensity,
                postBloomThreshold);
        }

        // Pass 2: render passes.
        const float clearColor[4] = { 0.06f, 0.06f, 0.08f, 1.0f };
        device.BeginFrame(clearColor);
        // Renderer preferences (runtime-adjustable later via UI/console/config).
        king::render::d3d11::RenderSystemD3D11::RenderSettings renderSettings{};
        renderSettings.enableHdr = !stressTest;
        renderSettings.enableTonemap = (!stressTest) && postEnableTonemap;
        renderSettings.enableShadows = !stressTest;
        renderSettings.enableSsao = false;
        renderSettings.shadowMapSize = 2048;
        renderSettings.cascadeCount = 3;
        renderSettings.shadowStrength = 1.0f;
        renderSettings.shadowBias = 0.00035f;
        // Keep shadows from going pitch black.
        renderSettings.shadowMinVisibility = 0.25f;
        // Clamp shadow distance to keep texel density high (improves perceived resolution).
        // Set to 0.0f to use the full camera far plane.
        renderSettings.shadowMaxDistance = 300.0f;
        // Controls remaining "zig-zag" aliasing on shadow edges.
        // 0 = hard, 1 = default, >1 = softer.
        renderSettings.shadowSoftness = 1.35f;
        // 1=PCF 3x3 (default), 2=PCF 5x5, 3=Poisson 9-tap
        renderSettings.shadowFilterQuality = 1;
        // Night exposure: darker overall.
        renderSettings.exposure = stressTest ? 0.65f : 0.22f;
        renderSettings.enableShadowPoissonPcf = false;
        renderSettings.enableShadowNormalOffsetBias = true;
        renderSettings.enableShadowReceiverPlaneBias = true;

        // Post processing (runtime-tunable via hotkeys)
        renderSettings.enableVignette = (!stressTest) && postEnableVignette;
        renderSettings.vignetteStrength = postVignetteStrength;
        renderSettings.vignettePower = postVignettePower;

        renderSettings.enableBloom = (!stressTest) && postEnableBloom;
        renderSettings.bloomIntensity = postBloomIntensity;
        renderSettings.bloomThreshold = postBloomThreshold;
        // Debug toggles (off by default):
        // - KING_SHADOW_READBACK=1: prints min/max depth and written pixel count from cascade 0.
        // - KING_SHADOW_DEBUG=1: raw shadow factor (white=lit, black=shadow)
        // - KING_SHADOW_DEBUG=2: receivesShadows flag
        // - KING_SHADOW_DEBUG=3: castsShadows flag
        renderSettings.debugShadowReadbackOnce = EnvFlag(L"KING_SHADOW_READBACK");
        renderSettings.shadowDebugView = EnvUInt(L"KING_SHADOW_DEBUG", 0u);
        renderSystem.RenderGeometryPass(device, scene, frustum, viewProj, view, proj, primaryCamPos, cameraNearZ, cameraFarZ, renderSettings);

        // VSync: set KING_VSYNC=1 to enable waiting for v-sync.
        const uint32_t vsync = EnvUInt(L"KING_VSYNC", 0u);
        HRESULT phr = device.Present(vsync ? 1u : 0u);
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

        // Update previous key state after processing.
        std::memcpy(prevKeys, input.keys, sizeof(prevKeys));

        if (stressTest)
        {
            // Sample per-frame performance for the current step.
            const auto& samples = renderSystem.PerfSamples();
            const double frameCpuMs = FindPerfMs(samples, "Frame", false);
            const double frameGpuMs = FindPerfMs(samples, "Frame", true);

            if (stressWarmupLeft > 0)
            {
                stressWarmupLeft--;
            }
            else
            {
                // FPS samples arrive once per ~1 second.
                if (time.FpsUpdated())
                {
                    stressSumFps += time.Fps();
                    stressFpsSamples++;
                }
                stressSumCpuMs += frameCpuMs;
                if (frameGpuMs >= 0.0)
                    stressSumGpuMs += frameGpuMs;
                stressMsSamples++;

                if (stressSampleLeft > 0)
                    stressSampleLeft--;

                if (stressSampleLeft == 0)
                {
                    if (!stressPrintedHeader)
                    {
                        std::printf("[StressTest] max=%u step=%u warmup=%u sample=%u\n", stressMaxSpheres, stressStep, stressWarmupFrames, stressSampleFrames);
                        std::printf("[StressTest] spheres, avgFPS, avgCPUms, avgGPUms\n");
                        stressPrintedHeader = true;
                    }

                    const uint32_t sphereCountNow = (uint32_t)scene.reg.renderers.Entities().size();
                    const double avgFps = (stressFpsSamples > 0) ? (stressSumFps / (double)stressFpsSamples) : 0.0;
                    const double avgCpu = (stressMsSamples > 0) ? (stressSumCpuMs / (double)stressMsSamples) : 0.0;
                    const double avgGpu = (stressMsSamples > 0) ? (stressSumGpuMs / (double)stressMsSamples) : -1.0;

                    std::printf("[StressTest] %u, %.1f, %.3f, %.3f\n", sphereCountNow, avgFps, avgCpu, avgGpu);
                    if (stressCsv)
                    {
                        std::fprintf(stressCsv, "%u,%.3f,%.6f,%.6f\n", sphereCountNow, avgFps, avgCpu, avgGpu);
                        std::fflush(stressCsv);
                    }

                    // Next step.
                    if (stressCurrentTarget >= stressMaxSpheres)
                    {
                        std::printf("[StressTest] done\n");
                        break;
                    }

                    stressCurrentTarget = std::min(stressMaxSpheres, stressCurrentTarget + stressStep);
                    stressWarmupLeft = stressWarmupFrames;
                    stressSampleLeft = stressSampleFrames;
                    stressSumFps = 0.0;
                    stressFpsSamples = 0;
                    stressSumCpuMs = 0.0;
                    stressSumGpuMs = 0.0;
                    stressMsSamples = 0;
                }
            }
        }
    }

    if (stressCsv)
    {
        std::fclose(stressCsv);
        stressCsv = nullptr;
        std::wprintf(L"[StressTest] CSV written: %s\n", stressCsvPath.c_str());
    }

    king::render::d3d11::RenderSystemD3D11::ReleaseSceneMeshBuffers(scene);
    renderSystem.Shutdown();
    device.Shutdown();
    return 0;
}
