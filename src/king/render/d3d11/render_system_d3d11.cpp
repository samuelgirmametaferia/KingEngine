#include "render_system_d3d11.h"

#include "shadows.h"
#include "shader_program_d3d11.h"
#include "texture_manager_d3d11.h"

#include "../../thread_config.h"

#include "../../math/dxmath.h"
#include "../shader.h"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <cfloat>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <unordered_map>

namespace
{
static std::wstring ToWide(const std::string& s)
{
    if (s.empty())
        return {};
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (wlen <= 0)
        return {};
    std::wstring w;
    w.resize((size_t)wlen);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), wlen);
    return w;
}

static std::wstring GetDirOfPath(const std::wstring& p)
{
    if (p.empty())
        return {};
    size_t pos = p.find_last_of(L"/\\");
    if (pos == std::wstring::npos)
        return {};
    return p.substr(0, pos);
}

static bool EnvFlagA(const char* name)
{
    if (!name || !*name)
        return false;

#if defined(_WIN32)
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, name) != 0 || !buf)
        return false;
    const char c = buf[0];
    free(buf);
    return (c == '1' || c == 't' || c == 'T' || c == 'y' || c == 'Y');
#else
    const char* v = std::getenv(name);
    if (!v || !*v)
        return false;
    const char c = v[0];
    return (c == '1' || c == 't' || c == 'T' || c == 'y' || c == 'Y');
#endif
}

static bool EndsWithI(const std::string& s, const char* suffix)
{
    if (!suffix)
        return false;
    const size_t n = s.size();
    const size_t m = std::strlen(suffix);
    if (m > n)
        return false;
    for (size_t i = 0; i < m; ++i)
    {
        char a = s[n - m + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

static uint64_t Fnv1a64(const void* data, size_t len, uint64_t seed = 1469598103934665603ull)
{
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i)
    {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static uint64_t HashU32(uint64_t h, uint32_t v)
{
    return Fnv1a64(&v, sizeof(v), h);
}

static uint64_t HashF32(uint64_t h, float v)
{
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(v), "float must be 32-bit");
    std::memcpy(&bits, &v, sizeof(bits));
    return HashU32(h, bits);
}

static uint64_t HashString(uint64_t h, const std::string& s)
{
    // Prefix with length to avoid boundary ambiguity.
    h = HashU32(h, (uint32_t)s.size());
    if (!s.empty())
        h = Fnv1a64(s.data(), s.size(), h);
    return h;
}

static uint64_t MakeMaterialKeyHash(const king::PbrMaterial& m)
{
    // Fast, stable key for caching/material batching (avoid per-frame string building).
    uint64_t h = 1469598103934665603ull;

    // Only include explicit shader path when it's a dev override.
    if (EndsWithI(m.shader, ".hlsl") || EndsWithI(m.shader, ".hlsli"))
        h = HashString(h, m.shader);

    h = HashU32(h, (uint32_t)m.blendMode);
    h = HashU32(h, (uint32_t)m.shadingModel);

    h = HashF32(h, m.albedo.x);
    h = HashF32(h, m.albedo.y);
    h = HashF32(h, m.albedo.z);
    h = HashF32(h, m.albedo.w);
    h = HashF32(h, m.roughness);
    h = HashF32(h, m.metallic);
    h = HashF32(h, m.emissive.x);
    h = HashF32(h, m.emissive.y);
    h = HashF32(h, m.emissive.z);

    h = HashString(h, m.textures.albedo);
    h = HashString(h, m.textures.normal);
    h = HashString(h, m.textures.metallicRoughness);
    h = HashString(h, m.textures.emissive);

    if (!m.scalars.empty())
    {
        std::vector<std::pair<std::string, float>> scalars;
        scalars.reserve(m.scalars.size());
        for (const auto& kv : m.scalars)
            scalars.push_back(kv);
        std::sort(scalars.begin(), scalars.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

        for (const auto& kv : scalars)
        {
            h = HashString(h, kv.first);
            h = HashF32(h, kv.second);
        }
    }

    return h;
}

static std::string MakeDefinesKey(std::vector<king::ShaderDefine> defines)
{
    // Stable cache key for program variants.
    std::sort(defines.begin(), defines.end(), [](const king::ShaderDefine& a, const king::ShaderDefine& b)
    {
        if (a.name != b.name)
            return a.name < b.name;
        return a.value < b.value;
    });

    std::string k;
    k.reserve(128);
    for (const auto& d : defines)
    {
        k += d.name;
        k += '=';
        k += d.value;
        k += ';';
    }
    return k;
}
}

namespace king::render::d3d11
{

static void SafeRelease(IUnknown*& p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

static Mat4x4 TransformToWorld(const Transform& t)
{
    using namespace DirectX;
    const XMVECTOR q = XMVectorSet(t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w);
    XMMATRIX S = XMMatrixScaling(t.scale.x, t.scale.y, t.scale.z);
    XMMATRIX R = XMMatrixRotationQuaternion(q);
    XMMATRIX T = XMMatrixTranslation(t.position.x, t.position.y, t.position.z);
    XMMATRIX W = S * R * T;
    return dx::StoreMat4x4(W);
}

static Mat4x4 WorldToNormalMatrix(const Mat4x4& world)
{
    using namespace DirectX;
    const XMMATRIX W = dx::LoadMat4x4(world);
    XMVECTOR det{};
    const XMMATRIX invW = XMMatrixInverse(&det, W);
    // If determinant is near-zero (singular), fall back to identity.
    const float detLen = std::fabs(XMVectorGetX(det));
    if (detLen < 1e-8f)
        return dx::StoreMat4x4(XMMatrixIdentity());
    const XMMATRIX N = XMMatrixTranspose(invW);
    return dx::StoreMat4x4(N);
}

static Float3 TransformPoint(const Mat4x4& m, const Float3& p)
{
    using namespace DirectX;
    const XMMATRIX M = dx::LoadMat4x4(m);
    const XMVECTOR v = XMVectorSet(p.x, p.y, p.z, 1.0f);
    const XMVECTOR r = XMVector4Transform(v, M);
    Float3 out{};
    out.x = XMVectorGetX(r);
    out.y = XMVectorGetY(r);
    out.z = XMVectorGetZ(r);
    return out;
}

static Mat4x4 MakePointShadowViewProj(const Float3& lightPos, int face, float nearZ, float farZ)
{
    using namespace DirectX;
    const XMVECTOR eye = XMVectorSet(lightPos.x, lightPos.y, lightPos.z, 1.0f);

    XMVECTOR at;
    XMVECTOR up;
    switch (face)
    {
    case 0: // +X
        at = XMVectorSet(lightPos.x + 1.0f, lightPos.y, lightPos.z, 1.0f);
        up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        break;
    case 1: // -X
        at = XMVectorSet(lightPos.x - 1.0f, lightPos.y, lightPos.z, 1.0f);
        up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        break;
    case 2: // +Y
        at = XMVectorSet(lightPos.x, lightPos.y + 1.0f, lightPos.z, 1.0f);
        up = XMVectorSet(0.0f, 0.0f, -1.0f, 0.0f);
        break;
    case 3: // -Y
        at = XMVectorSet(lightPos.x, lightPos.y - 1.0f, lightPos.z, 1.0f);
        up = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
        break;
    case 4: // +Z
        at = XMVectorSet(lightPos.x, lightPos.y, lightPos.z + 1.0f, 1.0f);
        up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        break;
    default: // 5: -Z
        at = XMVectorSet(lightPos.x, lightPos.y, lightPos.z - 1.0f, 1.0f);
        up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        break;
    }

    const XMMATRIX V = XMMatrixLookAtLH(eye, at, up);
    const XMMATRIX P = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, nearZ, farZ);
    return dx::StoreMat4x4(V * P);
}

static bool EnsurePointShadowResources(
    RenderDeviceD3D11& device,
    uint32_t size,
    ID3D11Texture2D*& tex,
    ID3D11ShaderResourceView*& srv,
    ID3D11RenderTargetView* rtv[6],
    ID3D11Texture2D*& depthTex,
    ID3D11DepthStencilView* dsv[6])
{
    ID3D11Device* d = device.Device();
    if (!d)
        return false;

    if (tex && srv && depthTex && rtv[0] && dsv[0])
        return true;

    // Color cubemap: R32_FLOAT
    if (!tex)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = size;
        td.Height = size;
        td.MipLevels = 1;
        td.ArraySize = 6;
        td.Format = DXGI_FORMAT_R32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

        HRESULT hr = d->CreateTexture2D(&td, nullptr, &tex);
        if (FAILED(hr))
            return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = td.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        sd.TextureCube.MipLevels = 1;
        hr = d->CreateShaderResourceView(tex, &sd, &srv);
        if (FAILED(hr))
            return false;

        for (int face = 0; face < 6; ++face)
        {
            D3D11_RENDER_TARGET_VIEW_DESC rd{};
            rd.Format = td.Format;
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            rd.Texture2DArray.MipSlice = 0;
            rd.Texture2DArray.FirstArraySlice = (UINT)face;
            rd.Texture2DArray.ArraySize = 1;
            hr = d->CreateRenderTargetView(tex, &rd, &rtv[face]);
            if (FAILED(hr))
                return false;
        }
    }

    // Depth cubemap: D32
    if (!depthTex)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = size;
        td.Height = size;
        td.MipLevels = 1;
        td.ArraySize = 6;
        td.Format = DXGI_FORMAT_D32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

        HRESULT hr = d->CreateTexture2D(&td, nullptr, &depthTex);
        if (FAILED(hr))
            return false;

        for (int face = 0; face < 6; ++face)
        {
            D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
            dd.Format = td.Format;
            dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            dd.Texture2DArray.MipSlice = 0;
            dd.Texture2DArray.FirstArraySlice = (UINT)face;
            dd.Texture2DArray.ArraySize = 1;
            hr = d->CreateDepthStencilView(depthTex, &dd, &dsv[face]);
            if (FAILED(hr))
                return false;
        }
    }

    return (tex && srv && depthTex && rtv[0] && dsv[0]);
}

RenderSystemD3D11::RenderSystemD3D11()
{
    // Perf tooling is opt-in. This avoids query allocation and console work
    // unless explicitly requested (stress test enables it explicitly).
    const bool enablePerf = EnvFlagA("KING_PERF");
    const bool enableGpuPerf = EnvFlagA("KING_GPU_PERF") || enablePerf;
    mPerf.SetEnabled(enablePerf);
    mGpuPerf.SetEnabled(enableGpuPerf);
}

RenderSystemD3D11::~RenderSystemD3D11()
{
    Shutdown();
}

void RenderSystemD3D11::EnsureDeferredContexts(RenderDeviceD3D11& device)
{
    if (!mDeferredContexts.empty())
        return;

    ID3D11Device* d = device.Device();
    if (!d)
        return;

    // Keep it modest: D3D11 command list recording helps, but too many contexts
    // can increase overhead. Cap at 4.
    unsigned desired = 0;
    {
        const king::ThreadConfig& tc = king::GetThreadConfig();
        if (tc.renderDeferredContexts > 0)
            desired = (unsigned)tc.renderDeferredContexts;
    }
    if (desired == 0)
    {
        unsigned hc = std::thread::hardware_concurrency();
        desired = (hc > 1) ? (hc - 1) : 0;
    }
    if (desired > 4) desired = 4;
    if (desired < 1) desired = 1;

    mDeferredContexts.reserve(desired);
    for (unsigned i = 0; i < desired; ++i)
    {
        ID3D11DeviceContext* dc = nullptr;
        if (SUCCEEDED(d->CreateDeferredContext(0, &dc)) && dc)
            mDeferredContexts.push_back(dc);
    }

    if (mDeferredContexts.empty())
    {
        // If deferred contexts aren't available for some reason, just fall back to
        // immediate submission.
        std::printf("Warning: failed to create deferred contexts; using immediate submission.\n");
    }
}

void RenderSystemD3D11::ReleaseDeferredContexts()
{
    for (auto* dc : mDeferredContexts)
    {
        if (dc)
            dc->Release();
    }
    mDeferredContexts.clear();
}

void RenderSystemD3D11::StartWorker()
{
    StopWorker();

    mWorkerExit = false;
    mPendingValid = false;
    mReadyValid = false;

    mWorker = std::thread([this]()
    {
        // Reuse storage across frames to avoid allocation churn.
        std::vector<SnapshotItem> items;
        Frustum fr{};

        for (;;)
        {
            {
                std::unique_lock<std::mutex> lock(mWorkMutex);
                mWorkCv.wait(lock, [this]() { return mWorkerExit || mPendingValid; });
                if (mWorkerExit)
                    return;

                // Swap so the main thread retains a buffer with capacity.
                items.swap(mPendingItems);
                fr = mPendingFrustum;
                mPendingValid = false;
            }

            PreparedFrame frame;
            BuildPreparedFrame(items, fr, frame);

            {
                std::lock_guard<std::mutex> lock(mWorkMutex);
                mReadyFrame = std::move(frame);
                mReadyValid = true;
            }
        }
    });
}

void RenderSystemD3D11::StopWorker()
{
    {
        std::lock_guard<std::mutex> lock(mWorkMutex);
        mWorkerExit = true;
        mPendingValid = false;
    }
    mWorkCv.notify_all();

    if (mWorker.joinable())
        mWorker.join();
}

bool RenderSystemD3D11::Initialize(RenderDeviceD3D11& device, const std::wstring& shaderPath)
{
    Shutdown();

    mShaderPath = shaderPath;
    mShaderDir = GetDirOfPath(shaderPath);

    ID3D11Device* d = device.Device();
    if (!d)
        return false;

    mShaderCache = std::make_unique<king::ShaderCache>(d);

    // Basic texture manager (WIC) so materials can bind textures for custom shaders.
    if (!mTextures.Initialize(d))
        return false;

    king::CompiledShader vs;
    king::CompiledShader vsDepth;
    king::CompiledShader vsPointShadow;
    king::CompiledShader ps;
    king::CompiledShader psMrt;
    king::CompiledShader psPointShadow;
    std::string shaderErr;

    if (!mShaderCache->CompileVSFromFile(shaderPath.c_str(), "VSMain", {}, vs, &shaderErr))
    {
        std::printf("Shader VS compile error:\n%s\n", shaderErr.c_str());
        return false;
    }

    if (!mShaderCache->CompileVSFromFile(shaderPath.c_str(), "VSDepthMain", {}, vsDepth, &shaderErr))
    {
        std::printf("Shader VSDepthMain compile error:\n%s\n", shaderErr.c_str());
        return false;
    }

    if (!mShaderCache->CompilePSFromFile(shaderPath.c_str(), "PSMain", {}, ps, &shaderErr))
    {
        std::printf("Shader PS compile error:\n%s\n", shaderErr.c_str());
        return false;
    }

    if (!mShaderCache->CompilePSFromFile(shaderPath.c_str(), "PSMainMRT", {}, psMrt, &shaderErr))
    {
        std::printf("Shader PSMainMRT compile error:\n%s\n", shaderErr.c_str());
        return false;
    }

    if (!mShaderCache->CompileVSFromFile(shaderPath.c_str(), "VSPointShadowMain", {}, vsPointShadow, &shaderErr))
    {
        std::printf("Shader VSPointShadowMain compile error:\n%s\n", shaderErr.c_str());
        return false;
    }

    if (!mShaderCache->CompilePSFromFile(shaderPath.c_str(), "PSPointShadowMain", {}, psPointShadow, &shaderErr))
    {
        std::printf("Shader PSPointShadowMain compile error:\n%s\n", shaderErr.c_str());
        return false;
    }

    // Print reflection once to keep debugging approachable.
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

    HRESULT hr = d->CreateVertexShader(vs.bytecode->GetBufferPointer(), vs.bytecode->GetBufferSize(), nullptr, &mVS);
    if (FAILED(hr))
    {
        std::printf("CreateVertexShader(VSMain) failed hr=0x%08X\n", (unsigned)hr);
        return false;
    }

    hr = d->CreateVertexShader(vsDepth.bytecode->GetBufferPointer(), vsDepth.bytecode->GetBufferSize(), nullptr, &mVSDepth);
    if (FAILED(hr))
    {
        std::printf("CreateVertexShader(VSDepthMain) failed hr=0x%08X\n", (unsigned)hr);
        return false;
    }

    hr = d->CreatePixelShader(ps.bytecode->GetBufferPointer(), ps.bytecode->GetBufferSize(), nullptr, &mPS);
    if (FAILED(hr))
    {
        std::printf("CreatePixelShader(PSMain) failed hr=0x%08X\n", (unsigned)hr);
        return false;
    }

    hr = d->CreatePixelShader(psMrt.bytecode->GetBufferPointer(), psMrt.bytecode->GetBufferSize(), nullptr, &mPSMrt);
    if (FAILED(hr))
    {
        std::printf("CreatePixelShader(PSMainMRT) failed hr=0x%08X\n", (unsigned)hr);
        return false;
    }

    hr = d->CreateVertexShader(vsPointShadow.bytecode->GetBufferPointer(), vsPointShadow.bytecode->GetBufferSize(), nullptr, &mVSPointShadow);
    if (FAILED(hr))
    {
        std::printf("CreateVertexShader(VSPointShadowMain) failed hr=0x%08X\n", (unsigned)hr);
        return false;
    }

    hr = d->CreatePixelShader(psPointShadow.bytecode->GetBufferPointer(), psPointShadow.bytecode->GetBufferSize(), nullptr, &mPSPointShadow);
    if (FAILED(hr))
    {
        std::printf("CreatePixelShader(PSPointShadowMain) failed hr=0x%08X\n", (unsigned)hr);
        return false;
    }

    // Post-processing system (full-screen shaders + post chain)
    if (!mPost.Initialize(device, *mShaderCache, shaderPath))
    {
        std::printf("RenderSystemD3D11: PostProcessD3D11::Initialize failed.\n");
        return false;
    }

    // Shadows module (compiles VSShadow and owns shadow resources).
    mShadows = std::make_unique<ShadowsD3D11>();
    if (!mShadows->Initialize(device, *mShaderCache, shaderPath))
    {
        std::printf("RenderSystemD3D11: ShadowsD3D11::Initialize failed.\n");
        return false;
    }

    // Point shadow constant buffer (updated per cubemap face).
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (UINT)sizeof(Mat4x4) + 16u; // viewProj + (float3 + float)
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = d->CreateBuffer(&bd, nullptr, &mPointShadowCB);
        if (FAILED(hr))
        {
            std::printf("CreateBuffer(PointShadowCB) failed hr=0x%08X\n", (unsigned)hr);
            return false;
        }
    }

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },

        // Per-instance data (slot 1)
        { "TEXCOORD",  4, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0,  D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD",  5, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD",  6, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD",  7, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        // Normal matrix rows (inverse-transpose of world)
        { "TEXCOORD", 11, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 64,  D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD", 12, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 80,  D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD", 13, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 96,  D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD", 14, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 112, D3D11_INPUT_PER_INSTANCE_DATA, 1 },

        { "COLOR",     0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 128, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD",  8, DXGI_FORMAT_R32G32_FLOAT,       1, 144, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD",  9, DXGI_FORMAT_R32_UINT,           1, 152, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD", 10, DXGI_FORMAT_R32_UINT,           1, 156, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };

    hr = d->CreateInputLayout(
        layout,
        (UINT)(sizeof(layout) / sizeof(layout[0])),
        vs.bytecode->GetBufferPointer(),
        vs.bytecode->GetBufferSize(),
        &mInputLayout);
    if (FAILED(hr))
        return false;

    // Constant buffers
    D3D11_BUFFER_DESC cbd{};
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    static_assert(sizeof(CameraCBData) % 16 == 0, "CameraCBData must be 16-byte aligned");
    cbd.ByteWidth = (UINT)sizeof(CameraCBData);
    hr = d->CreateBuffer(&cbd, nullptr, &mCameraCB);
    if (FAILED(hr))
        return false;

    static_assert(sizeof(LightCBData) % 16 == 0, "LightCBData must be 16-byte aligned");
    cbd.ByteWidth = (UINT)sizeof(LightCBData);
    hr = d->CreateBuffer(&cbd, nullptr, &mLightCB);
    if (FAILED(hr))
        return false;

    static_assert(sizeof(SsaoCBData) % 16 == 0, "SsaoCBData must be 16-byte aligned");
    cbd.ByteWidth = (UINT)sizeof(SsaoCBData);
    hr = d->CreateBuffer(&cbd, nullptr, &mSsaoCB);
    if (FAILED(hr))
        return false;

    // Samplers
    {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = d->CreateSamplerState(&sd, &mLinearClamp);
        if (FAILED(hr))
            return false;

        D3D11_SAMPLER_DESC sdp{};
        sdp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sdp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sdp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sdp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sdp.MaxLOD = D3D11_FLOAT32_MAX;
        hr = d->CreateSamplerState(&sdp, &mPointClamp);
        if (FAILED(hr))
            return false;

    }

    // Derived render states (from high-level material intent)
    {
        // Opaque: default (no blending)
        D3D11_BLEND_DESC bd{};
        bd.AlphaToCoverageEnable = FALSE;
        bd.IndependentBlendEnable = FALSE;
        auto& rt0 = bd.RenderTarget[0];
        rt0.BlendEnable = FALSE;
        rt0.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        (void)d->CreateBlendState(&bd, &mBlendOpaque);

        // Alpha blend
        D3D11_BLEND_DESC bad{};
        bad.AlphaToCoverageEnable = FALSE;
        bad.IndependentBlendEnable = FALSE;
        auto& rta = bad.RenderTarget[0];
        rta.BlendEnable = TRUE;
        rta.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        rta.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        rta.BlendOp = D3D11_BLEND_OP_ADD;
        rta.SrcBlendAlpha = D3D11_BLEND_ONE;
        rta.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rta.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rta.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        (void)d->CreateBlendState(&bad, &mBlendAlpha);

        // Depth read-only (depth test on, no depth writes) for transparent materials.
        D3D11_DEPTH_STENCIL_DESC dsd{};
        dsd.DepthEnable = TRUE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        dsd.StencilEnable = FALSE;
        (void)d->CreateDepthStencilState(&dsd, &mDepthReadOnly);
    }

    // Instance buffer: created lazily (capacity grows with scene).
    mInstanceCapacity = 0;

    // 1x1 white AO texture (used when SSAO is disabled so the tonemap shader
    // can always sample gSsao safely).
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = 1;
        td.Height = 1;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        const uint8_t white = 255;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = &white;
        init.SysMemPitch = 1;

        (void)d->CreateTexture2D(&td, &init, &mAoWhiteTex);
        if (mAoWhiteTex)
            (void)d->CreateShaderResourceView(mAoWhiteTex, nullptr, &mAoWhiteSRV);
    }

    {
        const king::ThreadConfig& tc = king::GetThreadConfig();
        mUsePrepareWorker = (tc.renderPrepareWorkerThreads > 0);
    }

    // Cache init-time feature switches to avoid per-frame env checks.
    mAllowDeferredContexts = EnvFlagA("KING_USE_DEFERRED_CONTEXTS");
    mAllowPostProcessing = !EnvFlagA("KING_DISABLE_POST") && !EnvFlagA("KING_DISABLE_POST_PROCESSING");

    if (mAllowDeferredContexts)
        EnsureDeferredContexts(device);

    // Perf tooling (GPU queries) is opt-in. When disabled, we avoid allocating queries.
    if (mGpuPerf.Enabled())
        mGpuPerf.Initialize(d);

    if (mUsePrepareWorker)
        StartWorker();

    return true;
}

void RenderSystemD3D11::Shutdown()
{
    StopWorker();

    if (mGpuPerf.Enabled())
        mGpuPerf.Shutdown();

    ReleaseDeferredContexts();

    IUnknown* tmp = nullptr;

    tmp = (IUnknown*)mCameraCB;
    SafeRelease(tmp);
    mCameraCB = nullptr;

    tmp = (IUnknown*)mVSDepth;
    SafeRelease(tmp);
    mVSDepth = nullptr;

    tmp = (IUnknown*)mLightCB;
    SafeRelease(tmp);
    mLightCB = nullptr;

    tmp = (IUnknown*)mSsaoCB;
    SafeRelease(tmp);
    mSsaoCB = nullptr;

    tmp = (IUnknown*)mLinearClamp;
    SafeRelease(tmp);
    mLinearClamp = nullptr;

    tmp = (IUnknown*)mPointClamp;
    SafeRelease(tmp);
    mPointClamp = nullptr;

    tmp = (IUnknown*)mBlendOpaque;
    SafeRelease(tmp);
    mBlendOpaque = nullptr;

    tmp = (IUnknown*)mBlendAlpha;
    SafeRelease(tmp);
    mBlendAlpha = nullptr;

    tmp = (IUnknown*)mDepthReadOnly;
    SafeRelease(tmp);
    mDepthReadOnly = nullptr;

    if (mShadows)
    {
        mShadows->Shutdown();
        mShadows.reset();
    }

    // Point shadow resources
    SafeRelease((IUnknown*&)mVSPointShadow);
    SafeRelease((IUnknown*&)mPSPointShadow);
    SafeRelease((IUnknown*&)mPointShadowCB);
    SafeRelease((IUnknown*&)mPointShadowSRV);
    SafeRelease((IUnknown*&)mPointShadowTex);
    for (int i = 0; i < 6; ++i)
    {
        SafeRelease((IUnknown*&)mPointShadowRTV[i]);
        SafeRelease((IUnknown*&)mPointShadowDSV[i]);
    }
    SafeRelease((IUnknown*&)mPointShadowDepthTex);
    mPointShadowSize = 0;

    for (auto& kv : mMaterialCache)
    {
        IUnknown* tmp2 = (IUnknown*)kv.second.materialCB;
        SafeRelease(tmp2);
        kv.second.materialCB = nullptr;
        kv.second.program = nullptr;
        kv.second.albedoSRV = nullptr;
        kv.second.normalSRV = nullptr;
        kv.second.mrSRV = nullptr;
        kv.second.emissiveSRV = nullptr;
    }
    mMaterialCache.clear();
    mProgramCache.clear();
    mTextures.Shutdown();
    mShaderCache.reset();

    tmp = (IUnknown*)mHdrSRV;
    SafeRelease(tmp);
    mHdrSRV = nullptr;

    tmp = (IUnknown*)mHdrRTV;
    SafeRelease(tmp);
    mHdrRTV = nullptr;

    tmp = (IUnknown*)mHdrTex;
    SafeRelease(tmp);
    mHdrTex = nullptr;
    mHdrW = 0;
    mHdrH = 0;

    tmp = (IUnknown*)mNormalSRV;
    SafeRelease(tmp);
    mNormalSRV = nullptr;

    tmp = (IUnknown*)mNormalRTV;
    SafeRelease(tmp);
    mNormalRTV = nullptr;

    tmp = (IUnknown*)mNormalTex;
    SafeRelease(tmp);
    mNormalTex = nullptr;

    tmp = (IUnknown*)mDepthSRV;
    SafeRelease(tmp);
    mDepthSRV = nullptr;

    tmp = (IUnknown*)mDepthDSV;
    SafeRelease(tmp);
    mDepthDSV = nullptr;

    tmp = (IUnknown*)mDepthTex;
    SafeRelease(tmp);
    mDepthTex = nullptr;

    tmp = (IUnknown*)mSsaoBlurSRV;
    SafeRelease(tmp);
    mSsaoBlurSRV = nullptr;

    tmp = (IUnknown*)mAoWhiteSRV;
    SafeRelease(tmp);
    mAoWhiteSRV = nullptr;

    tmp = (IUnknown*)mAoWhiteTex;
    SafeRelease(tmp);
    mAoWhiteTex = nullptr;

    tmp = (IUnknown*)mSsaoBlurRTV;
    SafeRelease(tmp);
    mSsaoBlurRTV = nullptr;

    tmp = (IUnknown*)mSsaoBlurTex;
    SafeRelease(tmp);
    mSsaoBlurTex = nullptr;

    tmp = (IUnknown*)mSsaoSRV;
    SafeRelease(tmp);
    mSsaoSRV = nullptr;

    tmp = (IUnknown*)mSsaoRTV;
    SafeRelease(tmp);
    mSsaoRTV = nullptr;

    tmp = (IUnknown*)mSsaoTex;
    SafeRelease(tmp);
    mSsaoTex = nullptr;

    tmp = (IUnknown*)mInstanceVB;
    SafeRelease(tmp);
    mInstanceVB = nullptr;
    mInstanceCapacity = 0;

    tmp = (IUnknown*)mInputLayout;
    SafeRelease(tmp);
    mInputLayout = nullptr;

    mPost.Shutdown();

    tmp = (IUnknown*)mVS;
    SafeRelease(tmp);
    mVS = nullptr;

    tmp = (IUnknown*)mPS;
    SafeRelease(tmp);
    mPS = nullptr;

    tmp = (IUnknown*)mPSMrt;
    SafeRelease(tmp);
    mPSMrt = nullptr;

    {
        std::lock_guard<std::mutex> lock(mWorkMutex);
        mPendingItems.clear();
        mReadyFrame.instances.clear();
        mReadyFrame.batches.clear();
        mReadyFrame.materials.clear();
        mReadyFrame.materialKeys.clear();
        mRenderFrame.instances.clear();
        mRenderFrame.batches.clear();
        mRenderFrame.materials.clear();
        mRenderFrame.materialKeys.clear();
        mPendingValid = false;
        mReadyValid = false;
    }
}

bool RenderSystemD3D11::OnDeviceReset(RenderDeviceD3D11& device)
{
    if (mShaderPath.empty())
        return false;
    return Initialize(device, mShaderPath);
}

void RenderSystemD3D11::BuildSnapshot(Scene& scene, std::vector<SnapshotItem>& outItems)
{
    outItems.clear();
    outItems.reserve(scene.reg.renderers.Entities().size());

    constexpr uint32_t kInstFlag_ReceivesShadows = 1u << 0;
    constexpr uint32_t kInstFlag_CastsShadows = 1u << 1;

    for (auto e : scene.reg.renderers.Entities())
    {
        auto* r = scene.reg.renderers.TryGet(e);
        auto* t = scene.reg.transforms.TryGet(e);
        if (!r || !t)
            continue;

        auto* m = scene.reg.meshes.TryGet(r->mesh);
        if (!m)
            continue;

        SnapshotItem it{};
        it.mesh = m;
        it.transform = *t;
        it.albedo = r->material.albedo;
        it.roughness = r->material.roughness;
        it.metallic = r->material.metallic;
        it.material = r->material;
        it.lightMask = r->lightMask;
        it.flags = 0;
        if (r->receivesShadows)
            it.flags |= kInstFlag_ReceivesShadows;
        // Transparency policy: by default, transparent objects don't cast shadows
        // (avoids incorrect shadowing without a dedicated masked shadow pass).
        const bool isTransparent = (r->material.blendMode == king::MaterialBlendMode::AlphaBlend);
        if (r->castsShadows && !isTransparent)
            it.flags |= kInstFlag_CastsShadows;
        it.boundsCenter = m->boundsCenter;
        it.boundsRadius = m->boundsRadius;
        outItems.push_back(it);
    }
}

void RenderSystemD3D11::EnqueueBuild(std::vector<SnapshotItem>& items, const Frustum& frustum)
{
    if (!mUsePrepareWorker)
        return;
    {
        std::lock_guard<std::mutex> lock(mWorkMutex);
        // Swap so caller keeps a vector with capacity for the next frame.
        mPendingItems.swap(items);
        mPendingFrustum = frustum;
        mPendingValid = true;
    }
    mWorkCv.notify_one();
}

bool RenderSystemD3D11::ConsumeReadyFrame(PreparedFrame& outFrame)
{
    std::lock_guard<std::mutex> lock(mWorkMutex);
    if (!mReadyValid)
        return false;

    outFrame = std::move(mReadyFrame);
    mReadyValid = false;
    return true;
}

void RenderSystemD3D11::BuildPreparedFrame(const std::vector<SnapshotItem>& items, const Frustum& frustum, PreparedFrame& outFrame)
{
    outFrame.instances.clear();
    outFrame.batches.clear();
    outFrame.materials.clear();
    outFrame.materialKeys.clear();

    struct Item
    {
        Mesh* mesh = nullptr;
        InstanceData inst{};
        uint32_t materialIndex = 0;
    };

    thread_local std::vector<Item> visible;
    visible.clear();
    visible.reserve(items.size());

    thread_local std::unordered_map<uint64_t, uint32_t> materialKeyToIndex;
    materialKeyToIndex.clear();
    materialKeyToIndex.reserve(items.size());

    for (const SnapshotItem& s : items)
    {
        if (!s.mesh)
            continue;

        // Frustum culling (sphere)
        Sphere sp{};
        sp.center = {
            s.transform.position.x + s.boundsCenter.x,
            s.transform.position.y + s.boundsCenter.y,
            s.transform.position.z + s.boundsCenter.z
        };
        const float sx = s.transform.scale.x;
        const float sy = s.transform.scale.y;
        const float sz = s.transform.scale.z;
        const float maxScale = (sx > sy) ? ((sx > sz) ? sx : sz) : ((sy > sz) ? sy : sz);
        sp.radius = s.boundsRadius * maxScale;
        if (!frustum.Intersects(sp))
            continue;

        Item it{};
        it.mesh = s.mesh;
        {
            const uint64_t k = MakeMaterialKeyHash(s.material);
            auto mk = materialKeyToIndex.find(k);
            if (mk == materialKeyToIndex.end())
            {
                const uint32_t newIndex = (uint32_t)outFrame.materials.size();
                outFrame.materials.push_back(s.material);
                outFrame.materialKeys.push_back(k);
                materialKeyToIndex.emplace(k, newIndex);
                it.materialIndex = newIndex;
            }
            else
            {
                it.materialIndex = mk->second;
            }
        }
        it.inst.world = TransformToWorld(s.transform);
        it.inst.normal = WorldToNormalMatrix(it.inst.world);
        it.inst.albedo[0] = s.albedo.x;
        it.inst.albedo[1] = s.albedo.y;
        it.inst.albedo[2] = s.albedo.z;
        it.inst.albedo[3] = s.albedo.w;
        it.inst.roughnessMetallic[0] = s.roughness;
        it.inst.roughnessMetallic[1] = s.metallic;
        it.inst.lightMask = s.lightMask;
        it.inst.flags = s.flags;
        visible.push_back(it);
    }

    if (visible.empty())
        return;

    // Batch/sort by pipeline-ish state (right now: mesh binds).
    std::sort(visible.begin(), visible.end(), [](const Item& a, const Item& b)
    {
        if ((uintptr_t)a.mesh != (uintptr_t)b.mesh)
            return (uintptr_t)a.mesh < (uintptr_t)b.mesh;
        return a.materialIndex < b.materialIndex;
    });

    outFrame.instances.reserve(visible.size());
    outFrame.batches.reserve(64);

    Mesh* currentMesh = nullptr;
    uint32_t currentMat = 0;
    Batch currentBatch{};

    for (const Item& v : visible)
    {
        if (v.mesh != currentMesh || v.materialIndex != currentMat)
        {
            if (currentBatch.mesh)
                outFrame.batches.push_back(currentBatch);

            currentMesh = v.mesh;
            currentMat = v.materialIndex;
            currentBatch = {};
            currentBatch.mesh = v.mesh;
            currentBatch.materialIndex = v.materialIndex;
            currentBatch.startInstance = (uint32_t)outFrame.instances.size();
            currentBatch.instanceCount = 0;
        }

        outFrame.instances.push_back(v.inst);
        currentBatch.instanceCount++;
    }

    if (currentBatch.mesh)
        outFrame.batches.push_back(currentBatch);
}

void RenderSystemD3D11::UpdateCameraCB(ID3D11DeviceContext* ctx, const Mat4x4& viewProj, const Float3& cameraPos, float exposure, float aoStrength)
{
    CameraCBData data{};
    data.viewProj = viewProj;
    data.cameraPos[0] = cameraPos.x;
    data.cameraPos[1] = cameraPos.y;
    data.cameraPos[2] = cameraPos.z;
    data.exposure = exposure;
    data.aoStrength = std::clamp(aoStrength, 0.0f, 1.0f);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(ctx->Map(mCameraCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        std::memcpy(mapped.pData, &data, sizeof(data));
        ctx->Unmap(mCameraCB, 0);
    }
}

void RenderSystemD3D11::UpdateLightCB(ID3D11DeviceContext* ctx, const LightCBData& data)
{
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(ctx->Map(mLightCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        std::memcpy(mapped.pData, &data, sizeof(data));
        ctx->Unmap(mLightCB, 0);
    }
}

void RenderSystemD3D11::EnsureMeshBuffers(RenderDeviceD3D11& device, Mesh& mesh)
{
    ID3D11Device* d = device.Device();
    if (!d)
        return;

    if (!mesh.vb && !mesh.vertices.empty())
    {
        D3D11_BUFFER_DESC bd{};
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.ByteWidth = (UINT)(mesh.vertices.size() * sizeof(VertexPN));
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = mesh.vertices.data();

        (void)d->CreateBuffer(&bd, &init, &mesh.vb);
    }

    if (!mesh.ib && !mesh.indices.empty())
    {
        D3D11_BUFFER_DESC bd{};
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.ByteWidth = (UINT)(mesh.indices.size() * sizeof(uint16_t));
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;

        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = mesh.indices.data();

        (void)d->CreateBuffer(&bd, &init, &mesh.ib);
    }
}

void RenderSystemD3D11::EnsureInstanceBuffer(RenderDeviceD3D11& device, size_t requiredInstanceCount)
{
    ID3D11Device* d = device.Device();
    if (!d)
        return;

    if (requiredInstanceCount <= mInstanceCapacity && mInstanceVB)
        return;

    // Grow with headroom to avoid frequent realloc.
    size_t newCapacity = (mInstanceCapacity == 0) ? 1024 : mInstanceCapacity;
    while (newCapacity < requiredInstanceCount)
        newCapacity *= 2;

    IUnknown* tmp = (IUnknown*)mInstanceVB;
    SafeRelease(tmp);
    mInstanceVB = nullptr;

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = (UINT)(newCapacity * sizeof(InstanceData));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    (void)d->CreateBuffer(&bd, nullptr, &mInstanceVB);
    if (mInstanceVB)
        mInstanceCapacity = newCapacity;
}

void RenderSystemD3D11::EnsureHdrTargets(RenderDeviceD3D11& device, bool needSsao)
{
    ID3D11Device* d = device.Device();
    if (!d)
        return;

    const uint32_t w = device.BackBufferWidth();
    const uint32_t h = device.BackBufferHeight();
    if (w == 0 || h == 0)
        return;

    if (mHdrTex && mHdrRTV && mHdrSRV && mHdrW == w && mHdrH == h)
    {
        if (!needSsao)
            return;

        if (mNormalTex && mNormalRTV && mNormalSRV && mDepthTex && mDepthDSV && mDepthSRV &&
            mSsaoTex && mSsaoRTV && mSsaoSRV && mSsaoBlurTex && mSsaoBlurRTV && mSsaoBlurSRV)
            return;
    }

    IUnknown* tmp = nullptr;

    tmp = (IUnknown*)mHdrSRV;
    SafeRelease(tmp);
    mHdrSRV = nullptr;

    tmp = (IUnknown*)mHdrRTV;
    SafeRelease(tmp);
    mHdrRTV = nullptr;

    tmp = (IUnknown*)mHdrTex;
    SafeRelease(tmp);
    mHdrTex = nullptr;

    tmp = (IUnknown*)mNormalSRV;
    SafeRelease(tmp);
    mNormalSRV = nullptr;

    tmp = (IUnknown*)mNormalRTV;
    SafeRelease(tmp);
    mNormalRTV = nullptr;

    tmp = (IUnknown*)mNormalTex;
    SafeRelease(tmp);
    mNormalTex = nullptr;

    tmp = (IUnknown*)mDepthSRV;
    SafeRelease(tmp);
    mDepthSRV = nullptr;

    tmp = (IUnknown*)mDepthDSV;
    SafeRelease(tmp);
    mDepthDSV = nullptr;

    tmp = (IUnknown*)mDepthTex;
    SafeRelease(tmp);
    mDepthTex = nullptr;

    tmp = (IUnknown*)mSsaoBlurSRV;
    SafeRelease(tmp);
    mSsaoBlurSRV = nullptr;

    tmp = (IUnknown*)mSsaoBlurRTV;
    SafeRelease(tmp);
    mSsaoBlurRTV = nullptr;

    tmp = (IUnknown*)mSsaoBlurTex;
    SafeRelease(tmp);
    mSsaoBlurTex = nullptr;

    tmp = (IUnknown*)mSsaoSRV;
    SafeRelease(tmp);
    mSsaoSRV = nullptr;

    tmp = (IUnknown*)mSsaoRTV;
    SafeRelease(tmp);
    mSsaoRTV = nullptr;

    tmp = (IUnknown*)mSsaoTex;
    SafeRelease(tmp);
    mSsaoTex = nullptr;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;

    // HDR target
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(d->CreateTexture2D(&td, nullptr, &mHdrTex)) || !mHdrTex)
        return;

    if (FAILED(d->CreateRenderTargetView(mHdrTex, nullptr, &mHdrRTV)) || !mHdrRTV)
        return;

    if (FAILED(d->CreateShaderResourceView(mHdrTex, nullptr, &mHdrSRV)) || !mHdrSRV)
        return;

    // Normal target (world-space normal encoded to [0,1])
    if (needSsao)
    {
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(d->CreateTexture2D(&td, nullptr, &mNormalTex)) || !mNormalTex)
            return;
        if (FAILED(d->CreateRenderTargetView(mNormalTex, nullptr, &mNormalRTV)) || !mNormalRTV)
            return;
        if (FAILED(d->CreateShaderResourceView(mNormalTex, nullptr, &mNormalSRV)) || !mNormalSRV)
            return;

        // Depth target with SRV
        {
            D3D11_TEXTURE2D_DESC dtd{};
            dtd.Width = w;
            dtd.Height = h;
            dtd.MipLevels = 1;
            dtd.ArraySize = 1;
            dtd.Format = DXGI_FORMAT_R32_TYPELESS;
            dtd.SampleDesc.Count = 1;
            dtd.Usage = D3D11_USAGE_DEFAULT;
            dtd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

            if (FAILED(d->CreateTexture2D(&dtd, nullptr, &mDepthTex)) || !mDepthTex)
                return;

            D3D11_DEPTH_STENCIL_VIEW_DESC dsvd{};
            dsvd.Format = DXGI_FORMAT_D32_FLOAT;
            dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            dsvd.Texture2D.MipSlice = 0;
            if (FAILED(d->CreateDepthStencilView(mDepthTex, &dsvd, &mDepthDSV)) || !mDepthDSV)
                return;

            D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
            srvd.Format = DXGI_FORMAT_R32_FLOAT;
            srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvd.Texture2D.MostDetailedMip = 0;
            srvd.Texture2D.MipLevels = 1;
            if (FAILED(d->CreateShaderResourceView(mDepthTex, &srvd, &mDepthSRV)) || !mDepthSRV)
                return;
        }
    }

    mHdrW = w;
    mHdrH = h;

    if (needSsao)
        EnsureSsaoTargets(device);
}

void RenderSystemD3D11::EnsureSsaoTargets(RenderDeviceD3D11& device)
{
    ID3D11Device* d = device.Device();
    if (!d)
        return;

    const uint32_t w = device.BackBufferWidth();
    const uint32_t h = device.BackBufferHeight();
    if (w == 0 || h == 0)
        return;

    if (mSsaoTex && mSsaoRTV && mSsaoSRV && mSsaoBlurTex && mSsaoBlurRTV && mSsaoBlurSRV && mHdrW == w && mHdrH == h)
        return;

    IUnknown* tmp = nullptr;

    tmp = (IUnknown*)mSsaoBlurSRV;
    SafeRelease(tmp);
    mSsaoBlurSRV = nullptr;

    tmp = (IUnknown*)mSsaoBlurRTV;
    SafeRelease(tmp);
    mSsaoBlurRTV = nullptr;

    tmp = (IUnknown*)mSsaoBlurTex;
    SafeRelease(tmp);
    mSsaoBlurTex = nullptr;

    tmp = (IUnknown*)mSsaoSRV;
    SafeRelease(tmp);
    mSsaoSRV = nullptr;

    tmp = (IUnknown*)mSsaoRTV;
    SafeRelease(tmp);
    mSsaoRTV = nullptr;

    tmp = (IUnknown*)mSsaoTex;
    SafeRelease(tmp);
    mSsaoTex = nullptr;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.Format = DXGI_FORMAT_R8_UNORM;

    if (FAILED(d->CreateTexture2D(&td, nullptr, &mSsaoTex)) || !mSsaoTex)
        return;
    if (FAILED(d->CreateRenderTargetView(mSsaoTex, nullptr, &mSsaoRTV)) || !mSsaoRTV)
        return;
    if (FAILED(d->CreateShaderResourceView(mSsaoTex, nullptr, &mSsaoSRV)) || !mSsaoSRV)
        return;

    if (FAILED(d->CreateTexture2D(&td, nullptr, &mSsaoBlurTex)) || !mSsaoBlurTex)
        return;
    if (FAILED(d->CreateRenderTargetView(mSsaoBlurTex, nullptr, &mSsaoBlurRTV)) || !mSsaoBlurRTV)
        return;
    if (FAILED(d->CreateShaderResourceView(mSsaoBlurTex, nullptr, &mSsaoBlurSRV)) || !mSsaoBlurSRV)
        return;
}

bool RenderSystemD3D11::GetPrimaryDirectionalLightWithTransform(const Scene& scene, Light& outLight, Transform& outXform)
{
    for (auto e : scene.reg.lights.Entities())
    {
        auto* l = scene.reg.lights.TryGet(e);
        if (!l || l->type != LightType::Directional)
            continue;

        outLight = *l;
        if (auto* t = scene.reg.transforms.TryGet(e))
            outXform = *t;
        else
            outXform = {};
        return true;
    }

    outLight = {};
    outXform = {};
    return false;
}

static Float3 NormalizeOrDefault(const Float3& v, const Float3& def)
{
    const float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
    if (len2 <= 1e-8f)
        return def;
    const float invLen = 1.0f / std::sqrt(len2);
    return { v.x * invLen, v.y * invLen, v.z * invLen };
}

void RenderSystemD3D11::GatherLights(const Scene& scene, std::vector<GpuLight>& outLights)
{
    outLights.clear();
    outLights.reserve(scene.reg.lights.Entities().size());

    for (auto e : scene.reg.lights.Entities())
    {
        auto* l = scene.reg.lights.TryGet(e);
        if (!l)
            continue;

        GpuLight gl{};
        gl.type = (uint32_t)l->type;
        gl.groupMask = l->groupMask;
        gl.color[0] = l->color.x;
        gl.color[1] = l->color.y;
        gl.color[2] = l->color.z;
        gl.intensity = l->intensity;

        const Float3 dir = NormalizeOrDefault(l->direction, { 0, -1, 0 });
        gl.dir[0] = dir.x;
        gl.dir[1] = dir.y;
        gl.dir[2] = dir.z;
        gl.range = l->range;

        Float3 pos{ 0, 0, 0 };
        if (auto* t = scene.reg.transforms.TryGet(e))
            pos = t->position;
        gl.pos[0] = pos.x;
        gl.pos[1] = pos.y;
        gl.pos[2] = pos.z;

        gl.innerConeCos = l->innerConeCos;
        gl.outerConeCos = l->outerConeCos;

        outLights.push_back(gl);
        if (outLights.size() >= RenderSystemD3D11::kMaxLights)
            break;
    }
}

void RenderSystemD3D11::RenderGeometryPass(
    RenderDeviceD3D11& device,
    Scene& scene,
    const Frustum& frustum,
    const Mat4x4& viewProj,
    const Float3& cameraPos,
    float cameraNearZ,
    float cameraFarZ,
    const RenderSettings& settings,
    float exposureOverride)
{
    const Mat4x4 I{};
    RenderGeometryPass(device, scene, frustum, viewProj, I, I, cameraPos, cameraNearZ, cameraFarZ, settings, exposureOverride);
}

void RenderSystemD3D11::RenderGeometryPass(
    RenderDeviceD3D11& device,
    Scene& scene,
    const Frustum& frustum,
    const Mat4x4& viewProj,
    const Float3& cameraPos,
    float cameraNearZ,
    float cameraFarZ,
    float exposure)
{
    const Mat4x4 I{};
    RenderGeometryPass(device, scene, frustum, viewProj, I, I, cameraPos, cameraNearZ, cameraFarZ, exposure);
}

void RenderSystemD3D11::RenderGeometryPass(
    RenderDeviceD3D11& device,
    Scene& scene,
    const Frustum& frustum,
    const Mat4x4& viewProj,
    const Mat4x4& view,
    const Mat4x4& proj,
    const Float3& cameraPos,
    float cameraNearZ,
    float cameraFarZ,
    float exposure)
{
    RenderSettings settings{};
    settings.exposure = exposure;
    RenderGeometryPass(device, scene, frustum, viewProj, view, proj, cameraPos, cameraNearZ, cameraFarZ, settings, exposure);
}

void RenderSystemD3D11::RenderGeometryPass(
    RenderDeviceD3D11& device,
    Scene& scene,
    const Frustum& frustum,
    const Mat4x4& viewProj,
    const Mat4x4& view,
    const Mat4x4& proj,
    const Float3& cameraPos,
    float cameraNearZ,
    float cameraFarZ,
    const RenderSettings& settings,
    float exposureOverride)
{
    ID3D11DeviceContext* ctx = device.Context();
    if (!ctx)
        return;

    struct GpuScopeGuard
    {
        king::perf::GpuProfilerD3D11* gpu = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        const char* name = nullptr;

        GpuScopeGuard(king::perf::GpuProfilerD3D11& g, ID3D11DeviceContext* c, const char* n)
            : gpu(&g), ctx(c), name(n)
        {
            if (gpu)
                gpu->BeginScope(ctx, name);
        }

        ~GpuScopeGuard()
        {
            if (gpu)
                gpu->EndScope(ctx, name);
        }
    };

    struct FrameProfilerGuard
    {
        RenderSystemD3D11* rs = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        std::vector<std::pair<const char*, double>> gpuMs;

        FrameProfilerGuard(RenderSystemD3D11* r, ID3D11DeviceContext* c)
            : rs(r), ctx(c)
        {
            if (!rs)
                return;
            rs->mPerf.BeginFrame();
            rs->mGpuPerf.BeginFrame(ctx);
        }

        ~FrameProfilerGuard()
        {
            if (!rs)
                return;

            rs->mGpuPerf.EndFrame(ctx);

            uint32_t gpuFrameIndex = 0;
            if (rs->mGpuPerf.TryGetResults(ctx, gpuFrameIndex, gpuMs))
            {
                for (const auto& p : gpuMs)
                    rs->mPerf.AddGpuMs(p.first, p.second);
            }

            rs->mPerf.EndFrame();
        }
    };

    FrameProfilerGuard frameGuard(this, ctx);
    // Top-level GPU scope so we can see total GPU frame time.
    GpuScopeGuard gpuFrame(mGpuPerf, ctx, "Frame");

    auto IsIdentityMat = [](const Mat4x4& m) -> bool
    {
        // Mat4x4 defaults to identity in this codebase.
        static const float I[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1
        };
        for (int i = 0; i < 16; ++i)
        {
            if (std::fabs(m.m[i] - I[i]) > 1e-6f)
                return false;
        }
        return true;
    };
    king::perf::CpuScope cpuFrame(mPerf, "Frame");

    const bool doSsao = settings.enableSsao;
    const bool doShadowsFeature = settings.enableShadows;
    const bool doTonemap = settings.enableTonemap && mAllowPostProcessing;

    // We only render to the HDR offscreen target if we're going to tonemap
    // (or if SSAO is enabled, since that path currently depends on HDR/G-buffer).
    const bool doHdrTarget = (settings.enableHdr && doTonemap) || doSsao;

    if (doHdrTarget)
        EnsureHdrTargets(device, doSsao);
    if (doShadowsFeature && mShadows)
        mShadows->EnsureResources(device, settings.cascadeCount, settings.shadowMapSize);

    ID3D11RenderTargetView* mainRtv = doHdrTarget ? mHdrRTV : device.RTV();
    if (!mainRtv)
        return;

    if (doSsao && (!mNormalRTV || !mNormalSRV || !mDepthDSV || !mDepthSRV))
        return;

    // Unbind potentially-hazardous SRVs before using them as render targets/DSVs.
    {
        ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
        ctx->PSSetShaderResources(0, 2, nullSrvs);
    }

    device.BeginGpuEvent(L"GeometryPass");

    // Ensure mesh buffers exist (GPU-side immutable buffers).
    for (auto me : scene.reg.meshes.Entities())
    {
        auto* m = scene.reg.meshes.TryGet(me);
        if (m)
            EnsureMeshBuffers(device, *m);
    }

    // Build/consume prepared frame(s)
    PreparedFrame frame;
    BuildSnapshot(scene, mSnapshotScratch);

    if (!ConsumeReadyFrame(frame))
        BuildPreparedFrame(mSnapshotScratch, frustum, frame);

    if (frame.instances.empty() || frame.batches.empty())
    {
        static bool once = false;
        if (!once)
        {
            once = true;
            std::printf("RenderGeometryPass: no visible instances (instances=%zu batches=%zu)\n", frame.instances.size(), frame.batches.size());
        }
        device.EndGpuEvent();
        return;
    }

    // Gather lights
    std::vector<GpuLight> lights;
    GatherLights(scene, lights);

    LightCBData lightCB{};
    lightCB.lightCount = (uint32_t)lights.size();
    for (uint32_t i = 0; i < lightCB.lightCount && i < kMaxLights; ++i)
        lightCB.lights[i] = lights[i];

    // Shadow setup (directional sun)
    lightCB.shadowStrength = 0.0f;
    lightCB.shadowBias = settings.shadowBias;
    lightCB.shadowTexelSize[0] = 0.0f;
    lightCB.shadowTexelSize[1] = 0.0f;
    lightCB.shadowExtras[0] = settings.shadowFadeStartNdc;
    lightCB.shadowExtras[1] = settings.shadowFadeEndNdc;
    lightCB.shadowExtras[2] = std::max(0.0f, settings.shadowSoftness);
    // Pack flags into w as a float (safe for small bitfields).
    uint32_t shadowFlags = 0;
    if (settings.enableShadowFadeOut) shadowFlags |= 1u << 0;
    if (settings.enableShadowPoissonPcf) shadowFlags |= 1u << 1;
    if (settings.enableShadowNormalOffsetBias) shadowFlags |= 1u << 2;
    if (settings.enableShadowReceiverPlaneBias) shadowFlags |= 1u << 3;
    // Bits 8..11: filter quality (0..15)
    shadowFlags |= (std::min(settings.shadowFilterQuality, 15u) & 0xFu) << 8;
    lightCB.shadowExtras[3] = (float)shadowFlags;
    for (uint32_t i = 0; i < kMaxCascades; ++i)
        lightCB.lightViewProj[i] = {};
    lightCB.cascadeSplitsNdc[0] = 1.0f;
    lightCB.cascadeSplitsNdc[1] = 1.0f;
    lightCB.cascadeSplitsNdc[2] = 1.0f;
    // w is unused by normal shading; repurpose for debug view.
    lightCB.cascadeSplitsNdc[3] = (float)settings.shadowDebugView;
    lightCB.cascadeCount = 1;

    // Point shadow setup (single point light cubemap)
    lightCB.pointShadowParams[0] = 0.0f; // enabled
    lightCB.pointShadowParams[1] = settings.pointShadowBias;
    lightCB.pointShadowParams[2] = 0.0f; // invFar
    lightCB.pointShadowParams[3] = std::clamp(settings.pointShadowStrength, 0.0f, 1.0f);
    lightCB.pointShadowTexelSize[0] = 0.0f;
    lightCB.pointShadowTexelSize[1] = 0.0f;

    Light sun{};
    Transform sunXform{};
    const bool haveSun = GetPrimaryDirectionalLightWithTransform(scene, sun, sunXform);

    Mat4x4 cascadeViewProj[kMaxCascades] = {};
    bool doShadows = false;
    ID3D11ShaderResourceView* shadowSrv = nullptr;
    ID3D11SamplerState* shadowSamplerPoint = nullptr;
    ID3D11SamplerState* shadowSamplerLinear = nullptr;
    ID3D11SamplerState* shadowSamplerNonCmp = nullptr;

    if (doShadowsFeature && haveSun && sun.castsShadows && mShadows)
    {
        ShadowsD3D11::Settings shadowSettings{};
        shadowSettings.enable = settings.enableShadows;
        shadowSettings.bias = settings.shadowBias;
        // Shader expects strength in [0,1]. Values > 1 extrapolate the lerp and
        // can produce negative lighting / weird color artifacts.
        shadowSettings.strength = std::clamp(settings.shadowStrength, 0.0f, 1.0f);
        shadowSettings.mapSize = settings.shadowMapSize;
        shadowSettings.cascadeCount = settings.cascadeCount;
        shadowSettings.cascadeLambda = settings.cascadeLambda;
        shadowSettings.debugView = settings.shadowDebugView;
        shadowSettings.debugReadbackOnce = settings.debugShadowReadbackOnce;

        float splitsNdc[4] = {};
        uint32_t cascadeCount = 1;
        float texelSize[2] = {};
        float outBias = settings.shadowBias;
        float outStrength = shadowSettings.strength;

        float shadowFarZ = cameraFarZ;
        if (settings.shadowMaxDistance > 0.0f)
            shadowFarZ = std::min(shadowFarZ, settings.shadowMaxDistance);
        shadowFarZ = std::max(shadowFarZ, cameraNearZ + 0.01f);

        if (mShadows->ComputeCascades(
                viewProj,
                cameraNearZ,
            shadowFarZ,
                sun,
                shadowSettings,
                cascadeViewProj,
                splitsNdc,
                cascadeCount,
                texelSize,
                outBias,
                outStrength))
        {
            lightCB.cascadeCount = cascadeCount;
            lightCB.cascadeSplitsNdc[0] = splitsNdc[0];
            lightCB.cascadeSplitsNdc[1] = splitsNdc[1];
            lightCB.cascadeSplitsNdc[2] = splitsNdc[2];
            lightCB.cascadeSplitsNdc[3] = splitsNdc[3];
            for (uint32_t i = 0; i < cascadeCount && i < kMaxCascades; ++i)
            {
                lightCB.lightViewProj[i] = cascadeViewProj[i];
            }
            lightCB.shadowTexelSize[0] = texelSize[0];
            lightCB.shadowTexelSize[1] = texelSize[1];
            lightCB.shadowBias = outBias;
            lightCB.shadowStrength = std::clamp(outStrength, 0.0f, 1.0f);
            lightCB.shadowMinVisibility = std::clamp(settings.shadowMinVisibility, 0.0f, 1.0f);

            shadowSrv = mShadows->ShadowSRV();
            shadowSamplerPoint = mShadows->ShadowSamplerPoint();
            shadowSamplerLinear = mShadows->ShadowSamplerLinear();
            shadowSamplerNonCmp = mShadows->ShadowSamplerNonCmp();
            doShadows = (shadowSrv != nullptr && shadowSamplerPoint != nullptr && shadowSamplerLinear != nullptr && shadowSamplerNonCmp != nullptr);
        }
    }

    // Determine point light for point shadows (first visible point light).
    bool doPointShadows = false;
    ID3D11ShaderResourceView* pointShadowSrv = nullptr;
    ID3D11SamplerState* pointShadowSampler = nullptr;
    Float3 pointShadowLightPos{ 0, 0, 0 };
    float pointShadowFarZ = 0.0f;
    if (settings.enablePointShadows && lightCB.pointShadowParams[3] > 0.0f)
    {
        for (uint32_t i = 0; i < lightCB.lightCount && i < kMaxLights; ++i)
        {
            const GpuLight& L = lightCB.lights[i];
            if (L.type == 1u /* Point */ && L.range > 0.01f && L.intensity > 0.0f)
            {
                pointShadowLightPos = { L.pos[0], L.pos[1], L.pos[2] };
                pointShadowFarZ = L.range;
                break;
            }
        }
    }

    static bool once = false;
    if (!once)
    {
        once = true;
        std::printf(
            "RenderGeometryPass: instances=%zu batches=%zu lights=%zu hdr=%s shadows=%s deferredContexts=%zu tonemap=%s\n",
            frame.instances.size(),
            frame.batches.size(),
            lights.size(),
            (mHdrRTV && mHdrSRV) ? "yes" : "no",
            doShadows ? "yes" : "no",
            mDeferredContexts.size(),
            (mHdrSRV && device.RTV() && mPost.Fullscreen().VS()) ? "yes" : "no");
    }

    const float exposure = (exposureOverride >= 0.0f) ? exposureOverride : settings.exposure;
    UpdateCameraCB(ctx, viewProj, cameraPos, exposure, settings.aoStrength);
    UpdateLightCB(ctx, lightCB);

    // Pass: Shadow map (directional CSM)
    if (doShadows && lightCB.shadowStrength > 0.0f && mShadows)
    {
        king::perf::CpuScope cpuShadow(mPerf, "ShadowPass");
        GpuScopeGuard gpuShadow(mGpuPerf, ctx, "ShadowPass");
        device.BeginGpuEvent(L"ShadowPass");

        // Build per-cascade caster lists from the existing snapshot.
        // This avoids rebuilding the snapshot and enables cascade-aware caster culling.
        constexpr uint32_t kInstFlag_CastsShadows = 1u << 1;

        const D3D11_VIEWPORT mainVp = device.Viewport();
        const float vpW = std::max(1.0f, mainVp.Width);
        const float vpH = std::max(1.0f, mainVp.Height);
        const float minCasterPx = std::max(0.0f, settings.shadowMinCasterPixels);

        using namespace DirectX;
        const XMMATRIX VP = dx::LoadMat4x4(viewProj);
        auto estimateScreenRadiusPx = [&](const SnapshotItem& s) -> float
        {
            if (s.boundsRadius <= 0.0f)
                return 0.0f;

            // Approximate world-space radius using max scale axis.
            const float sx = std::fabs(s.transform.scale.x);
            const float sy = std::fabs(s.transform.scale.y);
            const float sz = std::fabs(s.transform.scale.z);
            const float scaleMax = std::max(sx, std::max(sy, sz));
            const float rWorld = s.boundsRadius * scaleMax;
            if (rWorld <= 1e-5f)
                return 0.0f;

            const Mat4x4 Wm = TransformToWorld(s.transform);
            const Float3 cws = TransformPoint(Wm, s.boundsCenter);

            const XMVECTOR c0 = XMVectorSet(cws.x, cws.y, cws.z, 1.0f);
            const XMVECTOR c1x = XMVectorSet(cws.x + rWorld, cws.y, cws.z, 1.0f);
            const XMVECTOR c1y = XMVectorSet(cws.x, cws.y + rWorld, cws.z, 1.0f);
            const XMVECTOR c1z = XMVectorSet(cws.x, cws.y, cws.z + rWorld, 1.0f);

            const XMVECTOR p0 = XMVector4Transform(c0, VP);
            const XMVECTOR px = XMVector4Transform(c1x, VP);
            const XMVECTOR py = XMVector4Transform(c1y, VP);
            const XMVECTOR pz = XMVector4Transform(c1z, VP);

            const float w0 = std::max(1e-6f, XMVectorGetW(p0));
            const float wx = std::max(1e-6f, XMVectorGetW(px));
            const float wy = std::max(1e-6f, XMVectorGetW(py));
            const float wz = std::max(1e-6f, XMVectorGetW(pz));

            const float x0 = XMVectorGetX(p0) / w0;
            const float y0 = XMVectorGetY(p0) / w0;

            auto ndcDistPx = [&](const XMVECTOR& p, float w) -> float
            {
                const float xn = XMVectorGetX(p) / w;
                const float yn = XMVectorGetY(p) / w;
                const float dx = (xn - x0) * 0.5f * vpW;
                const float dy = (yn - y0) * 0.5f * vpH;
                return std::sqrt(dx * dx + dy * dy);
            };

            float rx = ndcDistPx(px, wx);
            float ry = ndcDistPx(py, wy);
            float rz = ndcDistPx(pz, wz);
            return std::max(rx, std::max(ry, rz));
        };

        auto intersectsCascade = [&](const SnapshotItem& s, const Mat4x4& cascadeVP) -> bool
        {
            if (s.boundsRadius <= 0.0f)
                return false;

            const float sx = std::fabs(s.transform.scale.x);
            const float sy = std::fabs(s.transform.scale.y);
            const float sz = std::fabs(s.transform.scale.z);
            const float scaleMax = std::max(sx, std::max(sy, sz));
            const float rWorld = s.boundsRadius * scaleMax;
            if (rWorld <= 1e-5f)
                return false;

            const Mat4x4 Wm = TransformToWorld(s.transform);
            const Float3 cws = TransformPoint(Wm, s.boundsCenter);

            const XMMATRIX CVP = dx::LoadMat4x4(cascadeVP);
            const XMVECTOR c0 = XMVectorSet(cws.x, cws.y, cws.z, 1.0f);
            const XMVECTOR c1x = XMVectorSet(cws.x + rWorld, cws.y, cws.z, 1.0f);
            const XMVECTOR c1y = XMVectorSet(cws.x, cws.y + rWorld, cws.z, 1.0f);
            const XMVECTOR c1z = XMVectorSet(cws.x, cws.y, cws.z + rWorld, 1.0f);

            const XMVECTOR p0 = XMVector4Transform(c0, CVP);
            const XMVECTOR px = XMVector4Transform(c1x, CVP);
            const XMVECTOR py = XMVector4Transform(c1y, CVP);
            const XMVECTOR pz = XMVector4Transform(c1z, CVP);

            const float w0 = std::max(1e-6f, XMVectorGetW(p0));
            const float wx = std::max(1e-6f, XMVectorGetW(px));
            const float wy = std::max(1e-6f, XMVectorGetW(py));
            const float wz = std::max(1e-6f, XMVectorGetW(pz));

            const float x0 = XMVectorGetX(p0) / w0;
            const float y0 = XMVectorGetY(p0) / w0;
            const float z0 = XMVectorGetZ(p0) / w0;

            auto ndcDist = [&](const XMVECTOR& p, float w) -> float
            {
                const float xn = XMVectorGetX(p) / w;
                const float yn = XMVectorGetY(p) / w;
                const float dx = xn - x0;
                const float dy = yn - y0;
                return std::sqrt(dx * dx + dy * dy);
            };

            const float rNdcXY = std::max(ndcDist(px, wx), std::max(ndcDist(py, wy), ndcDist(pz, wz)));
            // Conservative z radius: reuse XY radius.
            const float rNdcZ = rNdcXY;

            if (x0 < -1.0f - rNdcXY || x0 > 1.0f + rNdcXY) return false;
            if (y0 < -1.0f - rNdcXY || y0 > 1.0f + rNdcXY) return false;
            // D3D clip z for ortho is [0,1]
            if (z0 < 0.0f - rNdcZ || z0 > 1.0f + rNdcZ) return false;
            return true;
        };

        const uint32_t cascades = std::min(lightCB.cascadeCount, (uint32_t)ShadowsD3D11::kMaxCascades);
        for (uint32_t c = 0; c < cascades; ++c)
        {
            mShadowCasterPtrs[c].clear();
            mShadowCasterPtrs[c].reserve(mSnapshotScratch.size() / 2);
            mShadowDrawBatchesPerCascade[c].clear();
        }
        mShadowInstancesScratch.clear();
        mShadowInstancesScratch.reserve(mSnapshotScratch.size());

        for (const auto& s : mSnapshotScratch)
        {
            if ((s.flags & kInstFlag_CastsShadows) == 0)
                continue;

            if (minCasterPx > 0.0f)
            {
                const float rpx = estimateScreenRadiusPx(s);
                if (rpx < minCasterPx)
                    continue;
            }

            for (uint32_t c = 0; c < cascades; ++c)
            {
                if (intersectsCascade(s, cascadeViewProj[c]))
                    mShadowCasterPtrs[c].push_back(&s);
            }
        }

        for (uint32_t c = 0; c < cascades; ++c)
        {
            auto& ptrs = mShadowCasterPtrs[c];
            std::sort(ptrs.begin(), ptrs.end(), [](const SnapshotItem* a, const SnapshotItem* b)
            {
                return (uintptr_t)a->mesh < (uintptr_t)b->mesh;
            });

            const uint32_t baseInstance = (uint32_t)mShadowInstancesScratch.size();
            Mesh* currentMesh = nullptr;
            ShadowsD3D11::DrawBatch current{};
            current.startInstance = baseInstance;
            current.instanceCount = 0;

            for (const SnapshotItem* sp : ptrs)
            {
                if (!sp || !sp->mesh)
                    continue;

                if (sp->mesh != currentMesh)
                {
                    if (currentMesh && current.instanceCount > 0)
                        mShadowDrawBatchesPerCascade[c].push_back(current);

                    currentMesh = sp->mesh;
                    current = {};
                    current.vb = sp->mesh->vb;
                    current.ib = sp->mesh->ib;
                    current.indexCount = (uint32_t)sp->mesh->indices.size();
                    current.vertexCount = (uint32_t)sp->mesh->vertices.size();
                    current.startInstance = (uint32_t)mShadowInstancesScratch.size();
                    current.instanceCount = 0;
                }

                InstanceData inst{};
                inst.world = TransformToWorld(sp->transform);
                inst.normal = WorldToNormalMatrix(inst.world);
                inst.albedo[0] = sp->albedo.x;
                inst.albedo[1] = sp->albedo.y;
                inst.albedo[2] = sp->albedo.z;
                inst.albedo[3] = sp->albedo.w;
                inst.roughnessMetallic[0] = sp->roughness;
                inst.roughnessMetallic[1] = sp->metallic;
                inst.lightMask = sp->lightMask;
                inst.flags = sp->flags;

                mShadowInstancesScratch.push_back(inst);
                current.instanceCount++;
            }

            if (currentMesh && current.instanceCount > 0)
                mShadowDrawBatchesPerCascade[c].push_back(current);
        }

        static bool sPrintedShadowCasterStats = false;
        if (!sPrintedShadowCasterStats)
        {
            sPrintedShadowCasterStats = true;
            std::printf(
                "ShadowPass: snapshot=%zu instances=%zu cascades=%u map=%ux%u\n",
                mSnapshotScratch.size(),
                mShadowInstancesScratch.size(),
                (unsigned)lightCB.cascadeCount,
                (unsigned)settings.shadowMapSize,
                (unsigned)settings.shadowMapSize);
        }

        bool anyBatches = false;
        for (uint32_t c = 0; c < cascades; ++c)
            anyBatches = anyBatches || !mShadowDrawBatchesPerCascade[c].empty();

        if (!mShadowInstancesScratch.empty() && anyBatches)
        {
            EnsureInstanceBuffer(device, mShadowInstancesScratch.size());
            if (mInstanceVB)
            {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(ctx->Map(mInstanceVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                {
                    std::memcpy(mapped.pData, mShadowInstancesScratch.data(), mShadowInstancesScratch.size() * sizeof(InstanceData));
                    ctx->Unmap(mInstanceVB, 0);
                    mShadows->Render(
                        device,
                        mShadowDrawBatchesPerCascade,
                        mInputLayout,
                        mInstanceVB,
                        (uint32_t)sizeof(InstanceData),
                        cascadeViewProj,
                        lightCB.cascadeCount,
                        settings.debugShadowReadbackOnce);
                }
            }
        }
        else
        {
            static bool sPrintedShadowEmpty = false;
            if (!sPrintedShadowEmpty)
            {
                sPrintedShadowEmpty = true;
                std::printf("ShadowPass: nothing to draw (check castsShadows flags / culling).\n");
            }
        }

        // Restore main viewport/state.
        D3D11_VIEWPORT vp = device.Viewport();
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(device.RS());

        device.EndGpuEvent();
    }

    // Pass: Point shadow cubemap (single shadow-casting point light)
    if (pointShadowFarZ > 0.01f && settings.enablePointShadows && lightCB.pointShadowParams[3] > 0.0f && mVSPointShadow && mPSPointShadow && mPointShadowCB)
    {
        king::perf::CpuScope cpuPointShadow(mPerf, "PointShadowPass");
        GpuScopeGuard gpuPointShadow(mGpuPerf, ctx, "PointShadowPass");
        device.BeginGpuEvent(L"PointShadowPass");

        constexpr uint32_t kInstFlag_CastsShadows = 1u << 1;

        uint32_t desiredSize = std::max(64u, settings.pointShadowMapSize);
        if (mPointShadowSize != desiredSize)
        {
            SafeRelease((IUnknown*&)mPointShadowSRV);
            SafeRelease((IUnknown*&)mPointShadowTex);
            for (int i = 0; i < 6; ++i)
                SafeRelease((IUnknown*&)mPointShadowRTV[i]);
            SafeRelease((IUnknown*&)mPointShadowDepthTex);
            for (int i = 0; i < 6; ++i)
                SafeRelease((IUnknown*&)mPointShadowDSV[i]);
            mPointShadowSize = 0;
        }

        if (mPointShadowSize == 0)
        {
            if (!EnsurePointShadowResources(device, desiredSize, mPointShadowTex, mPointShadowSRV, mPointShadowRTV, mPointShadowDepthTex, mPointShadowDSV))
            {
                device.EndGpuEvent();
                goto PointShadowPassDone;
            }
            mPointShadowSize = desiredSize;
        }

        // Ensure the shadow SRV isn't still bound from last frame.
        {
            ID3D11ShaderResourceView* nullSrv = nullptr;
            ctx->PSSetShaderResources(9, 1, &nullSrv);
        }

        // Build caster batches from snapshot.
        mPointShadowCasterPtrs.clear();
        mPointShadowCasterPtrs.reserve(mSnapshotScratch.size() / 2);
        mPointShadowDrawBatches.clear();
        mPointShadowInstancesScratch.clear();
        mPointShadowInstancesScratch.reserve(mSnapshotScratch.size());

        for (const auto& s : mSnapshotScratch)
        {
            if ((s.flags & kInstFlag_CastsShadows) == 0)
                continue;
            if (!s.mesh)
                continue;
            mPointShadowCasterPtrs.push_back(&s);
        }

        std::sort(mPointShadowCasterPtrs.begin(), mPointShadowCasterPtrs.end(), [](const SnapshotItem* a, const SnapshotItem* b)
        {
            return (uintptr_t)a->mesh < (uintptr_t)b->mesh;
        });

        Mesh* currentMesh = nullptr;
        PointShadowDrawBatch current{};
        current.startInstance = 0;
        current.instanceCount = 0;

        for (const SnapshotItem* sp : mPointShadowCasterPtrs)
        {
            if (!sp || !sp->mesh)
                continue;

            if (sp->mesh != currentMesh)
            {
                if (currentMesh && current.instanceCount > 0)
                    mPointShadowDrawBatches.push_back(current);

                currentMesh = sp->mesh;
                current = {};
                current.vb = sp->mesh->vb;
                current.ib = sp->mesh->ib;
                current.indexCount = (uint32_t)sp->mesh->indices.size();
                current.vertexCount = (uint32_t)sp->mesh->vertices.size();
                current.startInstance = (uint32_t)mPointShadowInstancesScratch.size();
                current.instanceCount = 0;
                current.mesh = sp->mesh;
            }

            InstanceData inst{};
            inst.world = TransformToWorld(sp->transform);
            inst.normal = WorldToNormalMatrix(inst.world);
            inst.albedo[0] = sp->albedo.x;
            inst.albedo[1] = sp->albedo.y;
            inst.albedo[2] = sp->albedo.z;
            inst.albedo[3] = sp->albedo.w;
            inst.roughnessMetallic[0] = sp->roughness;
            inst.roughnessMetallic[1] = sp->metallic;
            inst.lightMask = sp->lightMask;
            inst.flags = sp->flags;
            mPointShadowInstancesScratch.push_back(inst);
            current.instanceCount++;
        }
        if (currentMesh && current.instanceCount > 0)
            mPointShadowDrawBatches.push_back(current);

        if (!mPointShadowInstancesScratch.empty() && !mPointShadowDrawBatches.empty())
        {
            EnsureInstanceBuffer(device, mPointShadowInstancesScratch.size());
            if (mInstanceVB)
            {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(ctx->Map(mInstanceVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                {
                    std::memcpy(mapped.pData, mPointShadowInstancesScratch.data(), mPointShadowInstancesScratch.size() * sizeof(InstanceData));
                    ctx->Unmap(mInstanceVB, 0);

                    // Configure pipeline.
                    ctx->IASetInputLayout(mInputLayout);
                    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    ctx->VSSetShader(mVSPointShadow, nullptr, 0);
                    ctx->PSSetShader(mPSPointShadow, nullptr, 0);
                    ctx->RSSetState(device.RS());
                    ctx->OMSetDepthStencilState(device.DSS(), 0);
                    {
                        const float blendFactor[4] = { 0, 0, 0, 0 };
                        ctx->OMSetBlendState(mBlendOpaque, blendFactor, 0xFFFFFFFFu);
                    }

                    D3D11_VIEWPORT shadowVp{};
                    shadowVp.TopLeftX = 0;
                    shadowVp.TopLeftY = 0;
                    shadowVp.Width = (float)mPointShadowSize;
                    shadowVp.Height = (float)mPointShadowSize;
                    shadowVp.MinDepth = 0.0f;
                    shadowVp.MaxDepth = 1.0f;
                    ctx->RSSetViewports(1, &shadowVp);

                    struct PointShadowCBData
                    {
                        Mat4x4 viewProj;
                        float lightPos[3];
                        float invFar;
                    };
                    static_assert(sizeof(PointShadowCBData) % 16 == 0, "PointShadowCBData must be 16-byte aligned");

                    const float nearZ = std::min(0.1f, pointShadowFarZ * 0.25f);
                    const float invFar = 1.0f / std::max(0.01f, pointShadowFarZ);

                    for (int face = 0; face < 6; ++face)
                    {
                        ID3D11RenderTargetView* rtv = mPointShadowRTV[face];
                        ID3D11DepthStencilView* dsv = mPointShadowDSV[face];
                        if (!rtv || !dsv)
                            continue;

                        const float clearDepth[4] = { 1, 1, 1, 1 };
                        ctx->ClearRenderTargetView(rtv, clearDepth);
                        ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
                        ctx->OMSetRenderTargets(1, &rtv, dsv);

                        PointShadowCBData cb{};
                        cb.viewProj = MakePointShadowViewProj(pointShadowLightPos, face, std::max(0.01f, nearZ), pointShadowFarZ);
                        cb.lightPos[0] = pointShadowLightPos.x;
                        cb.lightPos[1] = pointShadowLightPos.y;
                        cb.lightPos[2] = pointShadowLightPos.z;
                        cb.invFar = invFar;

                        D3D11_MAPPED_SUBRESOURCE mm{};
                        if (SUCCEEDED(ctx->Map(mPointShadowCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mm)))
                        {
                            std::memcpy(mm.pData, &cb, sizeof(cb));
                            ctx->Unmap(mPointShadowCB, 0);
                        }

                        ctx->VSSetConstantBuffers(7, 1, &mPointShadowCB);
                        ctx->PSSetConstantBuffers(7, 1, &mPointShadowCB);

                        for (const auto& b : mPointShadowDrawBatches)
                        {
                            if (!b.vb)
                                continue;

                            ID3D11Buffer* vbs[2] = { b.vb, mInstanceVB };
                            UINT strides[2] = { (UINT)sizeof(VertexPN), (UINT)sizeof(InstanceData) };
                            UINT offsets[2] = { 0u, 0u };
                            ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);

                            if (b.ib && b.indexCount > 0)
                            {
                                ctx->IASetIndexBuffer(b.ib, DXGI_FORMAT_R16_UINT, 0);
                                ctx->DrawIndexedInstanced(b.indexCount, b.instanceCount, 0, 0, b.startInstance);
                            }
                            else
                            {
                                ctx->DrawInstanced(b.vertexCount, b.instanceCount, 0, b.startInstance);
                            }
                        }
                    }

                    doPointShadows = true;
                    pointShadowSrv = mPointShadowSRV;
                    pointShadowSampler = mLinearClamp;
                    lightCB.pointShadowParams[0] = 1.0f;
                    lightCB.pointShadowParams[1] = settings.pointShadowBias;
                    lightCB.pointShadowParams[2] = invFar;
                    lightCB.pointShadowParams[3] = std::clamp(settings.pointShadowStrength, 0.0f, 1.0f);
                    lightCB.pointShadowTexelSize[0] = 1.0f / (float)mPointShadowSize;
                    lightCB.pointShadowTexelSize[1] = 1.0f / (float)mPointShadowSize;
                    UpdateLightCB(ctx, lightCB);
                }
            }
        }

        // Restore main viewport/state.
        {
            D3D11_VIEWPORT vp = device.Viewport();
            ctx->RSSetViewports(1, &vp);
            ctx->RSSetState(device.RS());
        }

        device.EndGpuEvent();
    }

PointShadowPassDone:

    // Kick prep for the NEXT frame using the same snapshot we already built.
    EnqueueBuild(mSnapshotScratch, frustum);

    // Optional: Depth prepass (depth-only). This is an engine-owned pass and does not vary per material.
    if (settings.enableDepthPrepass)
    {
        king::perf::CpuScope cpuDepth(mPerf, "DepthPrepass");
        GpuScopeGuard gpuDepth(mGpuPerf, ctx, "DepthPrepass");

        ID3D11DepthStencilView* dsv = doSsao ? mDepthDSV : device.DSV();
        if (dsv)
        {
            // If SSAO uses its own depth, it is already cleared above; otherwise clear now.
            if (!doSsao)
                ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

            ctx->OMSetRenderTargets(0, nullptr, dsv);
            ctx->OMSetDepthStencilState(device.DSS(), 0);
            ctx->RSSetState(device.RS());
            {
                const float blendFactor[4] = { 0, 0, 0, 0 };
                ctx->OMSetBlendState(mBlendOpaque, blendFactor, 0xFFFFFFFFu);
            }
            {
                D3D11_VIEWPORT vp = device.Viewport();
                ctx->RSSetViewports(1, &vp);
            }

            ctx->IASetInputLayout(mInputLayout);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(mVSDepth ? mVSDepth : mVS, nullptr, 0);
            ctx->PSSetShader(nullptr, nullptr, 0);
            ctx->VSSetConstantBuffers(0, 1, &mCameraCB);

            // Depth-only draws (single-threaded; cheap and avoids extra deferred contexts churn).
            for (const Batch& b : frame.batches)
            {
                if (!b.mesh || !b.mesh->vb)
                    continue;

                ID3D11Buffer* vbs[2] = { b.mesh->vb, mInstanceVB };
                UINT strides[2] = { (UINT)sizeof(VertexPN), (UINT)sizeof(InstanceData) };
                UINT offsets[2] = { 0u, 0u };
                ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);

                if (b.mesh->ib && !b.mesh->indices.empty())
                {
                    ctx->IASetIndexBuffer(b.mesh->ib, DXGI_FORMAT_R16_UINT, 0);
                    ctx->DrawIndexedInstanced((UINT)b.mesh->indices.size(), b.instanceCount, 0, 0, b.startInstance);
                }
                else
                {
                    ctx->DrawInstanced((UINT)b.mesh->vertices.size(), b.instanceCount, 0, b.startInstance);
                }
            }

            // Restore main viewport/state will be re-bound by the geometry pass below.
        }
    }

    // Pass: Geometry
    king::perf::CpuScope cpuGeom(mPerf, "GeometryPass");
    GpuScopeGuard gpuGeom(mGpuPerf, ctx, "GeometryPass");
    const float hdrClear[4] = { 0.06f, 0.06f, 0.08f, 1.0f };
    ctx->ClearRenderTargetView(mainRtv, hdrClear);

    if (doSsao)
    {
        const float normalClear[4] = { 0.5f, 0.5f, 1.0f, 1.0f };
        ctx->ClearRenderTargetView(mNormalRTV, normalClear);
        ctx->ClearDepthStencilView(mDepthDSV, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

        ID3D11RenderTargetView* rtvs[2] = { mainRtv, mNormalRTV };
        ctx->OMSetRenderTargets(2, rtvs, mDepthDSV);
    }
    else
    {
        ID3D11DepthStencilView* dsv = device.DSV();
        if (dsv)
            ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        ctx->OMSetRenderTargets(1, &mainRtv, dsv);
    }
    ctx->OMSetDepthStencilState(device.DSS(), 0);
    ctx->RSSetState(device.RS());
    {
        const float blendFactor[4] = { 0, 0, 0, 0 };
        ctx->OMSetBlendState(mBlendOpaque, blendFactor, 0xFFFFFFFFu);
    }
    {
        D3D11_VIEWPORT vp = device.Viewport();
        ctx->RSSetViewports(1, &vp);
    }

    ctx->IASetInputLayout(mInputLayout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Resolve / create GPU-side material bindings for this frame.
    // These are updated on the immediate context before any deferred command recording begins.
    struct MaterialCBData
    {
        float baseColor[4];
        float emissive[3];
        float roughness;
        float metallic;
        uint32_t flags;
        float _pad0;
        float _pad1;
    };
    static_assert(sizeof(MaterialCBData) % 16 == 0, "MaterialCBData must be 16-byte aligned");

    auto ResolveShaderPath = [&](const king::PbrMaterial& mat) -> std::wstring
    {
        if (mat.shader.empty() || mat.shader == "pbr" || mat.shader == "pbr_forward" || mat.shader == "pbr_test")
            return mShaderPath;

        // If the string looks like an HLSL file, treat it as a path.
        if (EndsWithI(mat.shader, ".hlsl") || EndsWithI(mat.shader, ".hlsli"))
        {
            // Absolute if it has a drive letter or UNC.
            const bool isAbs = (mat.shader.size() >= 2 && mat.shader[1] == ':') || (mat.shader.size() >= 2 && mat.shader[0] == '\\' && mat.shader[1] == '\\');
            if (isAbs)
                return ToWide(mat.shader);
            if (!mShaderDir.empty())
                return mShaderDir + L"\\" + ToWide(mat.shader);
            return ToWide(mat.shader);
        }

        // Unknown symbolic shader name: fall back.
        return mShaderPath;
    };

    auto ResolveShadingModel = [&](const king::PbrMaterial& mat) -> king::MaterialShadingModel
    {
        // Prefer explicit intent.
        king::MaterialShadingModel sm = mat.shadingModel;

        // Convenience: derive from common symbolic names when intent isn't set.
        if (sm == king::MaterialShadingModel::Pbr)
        {
            if (mat.shader == "unlit" || mat.shader == "unlit_color")
                return king::MaterialShadingModel::Unlit;
            if (mat.shader == "rim" || mat.shader == "rim_glow" || mat.shader == "rim_outline_glow")
                return king::MaterialShadingModel::RimGlow;
        }
        return sm;
    };

    auto GetEngineDefinesForMaterial = [&](const king::PbrMaterial& mat) -> std::vector<king::ShaderDefine>
    {
        std::vector<king::ShaderDefine> defs;
        const king::MaterialShadingModel sm = ResolveShadingModel(mat);
        defs.push_back({ "KING_SHADING_MODEL", std::to_string((int)sm) });
        return defs;
    };

    auto GetOrCreateProgram = [&](const std::wstring& hlslPath, const std::vector<king::ShaderDefine>& defines) -> ShaderProgramD3D11*
    {
        const std::wstring cacheKey = hlslPath + L"|" + ToWide(MakeDefinesKey(defines));
        auto it = mProgramCache.find(cacheKey);
        if (it != mProgramCache.end())
            return it->second.get();

        auto prog = std::make_unique<ShaderProgramD3D11>();
        GeometryProgramDesc desc{};
        desc.hlslPath = hlslPath;
        desc.defines = defines;
        std::string err;
        if (!mShaderCache || !prog->Create(device.Device(), *mShaderCache, desc, &err))
        {
            std::printf("ShaderProgram create failed for '%ls': %s\n", hlslPath.c_str(), err.c_str());
            return nullptr;
        }

        ShaderProgramD3D11* out = prog.get();
        mProgramCache.emplace(cacheKey, std::move(prog));
        return out;
    };

    auto GetOrCreateMaterialGpu = [&](uint64_t key, const king::PbrMaterial& mat) -> const MaterialGpu*
    {
        auto it = mMaterialCache.find(key);
        if (it != mMaterialCache.end())
            return &it->second;

        MaterialGpu mg{};

        mg.alphaBlend = (mat.blendMode == king::MaterialBlendMode::AlphaBlend);

        // Engine-owned passes: compile small, shared variants per shading model.
        // Per-material custom HLSL is gated (dev-only) to avoid variant explosion.
        auto AllowCustomShaders = [&]() -> bool
        {
            return EnvFlagA("KING_ALLOW_CUSTOM_SHADERS");
        };

        const std::wstring shaderW = ResolveShaderPath(mat);
        if ((EndsWithI(mat.shader, ".hlsl") || EndsWithI(mat.shader, ".hlsli")) && AllowCustomShaders())
        {
            mg.program = GetOrCreateProgram(shaderW, {});
        }
        else
        {
            mg.program = GetOrCreateProgram(mShaderPath, GetEngineDefinesForMaterial(mat));
        }

        // Material constant buffer (b4) for custom shaders.
        D3D11_BUFFER_DESC bd{};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.ByteWidth = (UINT)sizeof(MaterialCBData);
        (void)device.Device()->CreateBuffer(&bd, nullptr, &mg.materialCB);

        // Textures (t5..t8), loaded via WIC. Paths are interpreted relative to the shader directory.
        auto ResolveTexPath = [&](const std::string& p) -> std::wstring
        {
            if (p.empty())
                return {};
            const bool isAbs = (p.size() >= 2 && p[1] == ':') || (p.size() >= 2 && p[0] == '\\' && p[1] == '\\');
            if (isAbs)
                return ToWide(p);
            if (!mShaderDir.empty())
                return mShaderDir + L"\\" + ToWide(p);
            return ToWide(p);
        };

        mg.albedoSRV = mTextures.GetOrLoad2D(ResolveTexPath(mat.textures.albedo), true);
        mg.normalSRV = mTextures.GetOrLoad2D(ResolveTexPath(mat.textures.normal), false);
        mg.mrSRV = mTextures.GetOrLoad2D(ResolveTexPath(mat.textures.metallicRoughness), false);
        mg.emissiveSRV = mTextures.GetOrLoad2D(ResolveTexPath(mat.textures.emissive), true);

        auto inserted = mMaterialCache.emplace(key, mg);
        return &inserted.first->second;
    };

    std::vector<const MaterialGpu*> frameMaterials;
    frameMaterials.resize(frame.materials.size(), nullptr);
    for (size_t i = 0; i < frame.materials.size(); ++i)
        frameMaterials[i] = GetOrCreateMaterialGpu(frame.materialKeys[i], frame.materials[i]);

    // Update per-material constant buffers ONCE on the immediate context.
    // This keeps deferred command recording safe (no Map() needed on deferred contexts).
    for (size_t i = 0; i < frame.materials.size(); ++i)
    {
        const MaterialGpu* mg = frameMaterials[i];
        if (!mg || !mg->materialCB)
            continue;

        const king::PbrMaterial& mat = frame.materials[i];
        MaterialCBData cbd{};
        cbd.baseColor[0] = mat.albedo.x;
        cbd.baseColor[1] = mat.albedo.y;
        cbd.baseColor[2] = mat.albedo.z;
        cbd.baseColor[3] = mat.albedo.w;
        cbd.emissive[0] = mat.emissive.x;
        cbd.emissive[1] = mat.emissive.y;
        cbd.emissive[2] = mat.emissive.z;
        cbd.roughness = mat.roughness;
        cbd.metallic = mat.metallic;
        cbd.flags = 0;

        D3D11_MAPPED_SUBRESOURCE mm{};
        if (SUCCEEDED(ctx->Map(mg->materialCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mm)))
        {
            std::memcpy(mm.pData, &cbd, sizeof(cbd));
            ctx->Unmap(mg->materialCB, 0);
        }
    }

    // Default shaders (for old materials or compile failures).
    ctx->VSSetShader(mVS, nullptr, 0);
    ctx->PSSetShader(doSsao ? mPSMrt : mPS, nullptr, 0);

    ctx->VSSetConstantBuffers(0, 1, &mCameraCB);
    ctx->PSSetConstantBuffers(0, 1, &mCameraCB);
    ctx->PSSetConstantBuffers(1, 1, &mLightCB);

    if (doShadows && shadowSrv && shadowSamplerPoint && shadowSamplerLinear && shadowSamplerNonCmp)
    {
        ctx->PSSetShaderResources(0, 1, &shadowSrv);
        ID3D11SamplerState* samplers[2] = { shadowSamplerPoint, shadowSamplerLinear };
        ctx->PSSetSamplers(0, 2, samplers);
        ctx->PSSetSamplers(3, 1, &shadowSamplerNonCmp);
    }

    if (doPointShadows && pointShadowSrv && pointShadowSampler)
    {
        ctx->PSSetShaderResources(9, 1, &pointShadowSrv);
        ctx->PSSetSamplers(5, 1, &pointShadowSampler);
    }

    EnsureInstanceBuffer(device, frame.instances.size());
    if (!mInstanceVB)
    {
        device.EndGpuEvent();
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(mInstanceVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        device.EndGpuEvent();
        return;
    }
    std::memcpy(mapped.pData, frame.instances.data(), frame.instances.size() * sizeof(InstanceData));
    ctx->Unmap(mInstanceVB, 0);

    // Deferred-context submission (optional).
    // Note: Spawning threads per frame is extremely expensive; we only record on deferred contexts
    // when explicitly enabled, and we record sequentially to avoid thread creation overhead.
    bool usedDeferred = false;
    if (mAllowDeferredContexts && !mDeferredContexts.empty())
    {
        const size_t totalBatches = frame.batches.size();
        const size_t numWorkers = mDeferredContexts.size();
        const size_t chunk = (totalBatches + numWorkers - 1) / numWorkers;

        struct Recorded
        {
            ID3D11CommandList* list = nullptr;
        };
        std::vector<Recorded> recorded(numWorkers);

        for (size_t i = 0; i < numWorkers; ++i)
        {
            const size_t begin = i * chunk;
            const size_t end = (begin + chunk < totalBatches) ? (begin + chunk) : totalBatches;
            if (begin >= end)
                continue;

            ID3D11DeviceContext* dc = mDeferredContexts[i];
            if (!dc)
                continue;
            {

                dc->ClearState();
                // IMPORTANT: deferred contexts don't inherit the immediate context's
                // render target binding/state. Bind outputs + raster/depth state before draws.
                ID3D11DepthStencilView* dsv = doSsao ? mDepthDSV : device.DSV();
                ID3D11DepthStencilState* dss = device.DSS();
                ID3D11RasterizerState* rs = device.RS();
                D3D11_VIEWPORT vp = device.Viewport();

                if (doSsao)
                {
                    ID3D11RenderTargetView* rtvs[2] = { mainRtv, mNormalRTV };
                    dc->OMSetRenderTargets(2, rtvs, dsv);
                }
                else
                {
                    ID3D11RenderTargetView* rtv = mainRtv;
                    if (rtv)
                        dc->OMSetRenderTargets(1, &rtv, dsv);
                }
                if (dss)
                    dc->OMSetDepthStencilState(dss, 0);
                if (rs)
                    dc->RSSetState(rs);
                dc->RSSetViewports(1, &vp);

                dc->IASetInputLayout(mInputLayout);
                dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

                // Track current bindings to avoid redundant state changes.
                const ShaderProgramD3D11* curProg = nullptr;
                uint32_t curMat = 0xFFFFFFFFu;
                bool curAlphaBlend = false;

                dc->VSSetShader(mVS, nullptr, 0);
                dc->PSSetShader(doSsao ? mPSMrt : mPS, nullptr, 0);

                dc->VSSetConstantBuffers(0, 1, &mCameraCB);
                dc->PSSetConstantBuffers(0, 1, &mCameraCB);
                dc->PSSetConstantBuffers(1, 1, &mLightCB);

                // Default (opaque) output-merger state.
                {
                    const float blendFactor[4] = { 0, 0, 0, 0 };
                    dc->OMSetBlendState(mBlendOpaque, blendFactor, 0xFFFFFFFFu);
                    if (dss)
                        dc->OMSetDepthStencilState(dss, 0);
                    curAlphaBlend = false;
                }

                if (doShadows && shadowSrv && shadowSamplerPoint && shadowSamplerLinear && shadowSamplerNonCmp)
                {
                    dc->PSSetShaderResources(0, 1, &shadowSrv);
                    ID3D11SamplerState* samplers[2] = { shadowSamplerPoint, shadowSamplerLinear };
                    dc->PSSetSamplers(0, 2, samplers);
                    dc->PSSetSamplers(3, 1, &shadowSamplerNonCmp);
                }

                if (doPointShadows && pointShadowSrv && pointShadowSampler)
                {
                    dc->PSSetShaderResources(9, 1, &pointShadowSrv);
                    dc->PSSetSamplers(5, 1, &pointShadowSampler);
                }

                // Material bindings: b4, t5..t8, s4
                ID3D11SamplerState* matSampler = mLinearClamp;
                if (matSampler)
                    dc->PSSetSamplers(4, 1, &matSampler);

                for (size_t bi = begin; bi < end; ++bi)
                {
                    const Batch& b = frame.batches[bi];
                    if (!b.mesh || !b.mesh->vb)
                        continue;

                    const uint32_t mi = b.materialIndex;
                    const MaterialGpu* mg = (mi < frameMaterials.size()) ? frameMaterials[mi] : nullptr;

                    // Shader program selection (custom materials can override).
                    const ShaderProgramD3D11* desiredProg = mg ? mg->program : nullptr;
                    if (doSsao && desiredProg && !desiredProg->HasMrtVariant())
                        desiredProg = nullptr; // can't render into normal MRT without PSMainMRT

                    if (desiredProg != curProg)
                    {
                        if (desiredProg)
                        {
                            dc->IASetInputLayout(desiredProg->InputLayout());
                            dc->VSSetShader(desiredProg->VS(), nullptr, 0);
                            dc->PSSetShader(doSsao && desiredProg->PSMrt() ? desiredProg->PSMrt() : desiredProg->PS(), nullptr, 0);
                        }
                        else
                        {
                            dc->IASetInputLayout(mInputLayout);
                            dc->VSSetShader(mVS, nullptr, 0);
                            dc->PSSetShader(doSsao ? mPSMrt : mPS, nullptr, 0);
                        }
                        curProg = desiredProg;
                        curMat = 0xFFFFFFFFu;
                    }

                    if (mg && mi != curMat)
                    {
                        // Render state derived from material intent.
                        if (mg->alphaBlend != curAlphaBlend)
                        {
                            const float blendFactor[4] = { 0, 0, 0, 0 };
                            dc->OMSetBlendState(mg->alphaBlend ? mBlendAlpha : mBlendOpaque, blendFactor, 0xFFFFFFFFu);
                            if (mg->alphaBlend)
                            {
                                if (mDepthReadOnly)
                                    dc->OMSetDepthStencilState(mDepthReadOnly, 0);
                            }
                            else
                            {
                                if (dss)
                                    dc->OMSetDepthStencilState(dss, 0);
                            }
                            curAlphaBlend = mg->alphaBlend;
                        }

                        // Update per-material constant buffer once per frame on the immediate context.
                        // Here we only bind (safe for deferred).
                        if (mg->materialCB)
                            dc->PSSetConstantBuffers(4, 1, &mg->materialCB);

                        ID3D11ShaderResourceView* srvs[4] = {
                            mg->albedoSRV ? mg->albedoSRV : mTextures.White(),
                            mg->normalSRV ? mg->normalSRV : mTextures.White(),
                            mg->mrSRV ? mg->mrSRV : mTextures.White(),
                            mg->emissiveSRV ? mg->emissiveSRV : mTextures.Black()
                        };
                        dc->PSSetShaderResources(5, 4, srvs);
                        curMat = mi;
                    }

                    ID3D11Buffer* vbs[2] = { b.mesh->vb, mInstanceVB };
                    UINT strides[2] = { (UINT)sizeof(VertexPN), (UINT)sizeof(InstanceData) };
                    UINT offsets[2] = { 0u, 0u };
                    dc->IASetVertexBuffers(0, 2, vbs, strides, offsets);

                    if (b.mesh->ib && !b.mesh->indices.empty())
                    {
                        dc->IASetIndexBuffer(b.mesh->ib, DXGI_FORMAT_R16_UINT, 0);
                        dc->DrawIndexedInstanced((UINT)b.mesh->indices.size(), b.instanceCount, 0, 0, b.startInstance);
                    }
                    else
                    {
                        dc->DrawInstanced((UINT)b.mesh->vertices.size(), b.instanceCount, 0, b.startInstance);
                    }
                }

                ID3D11CommandList* cl = nullptr;
                if (SUCCEEDED(dc->FinishCommandList(FALSE, &cl)))
                    recorded[i].list = cl;
            }
        }

        for (auto& r : recorded)
        {
            if (r.list)
            {
                ctx->ExecuteCommandList(r.list, FALSE);
                r.list->Release();
                r.list = nullptr;
            }
        }

        usedDeferred = true;
    }

    // Fallback: immediate submission (single-threaded draw calls).
    if (!usedDeferred)
    {
        const ShaderProgramD3D11* curProg = nullptr;
        uint32_t curMat = 0xFFFFFFFFu;
        bool curAlphaBlend = false;
        ID3D11SamplerState* matSampler = mLinearClamp;
        if (matSampler)
            ctx->PSSetSamplers(4, 1, &matSampler);

        {
            const float blendFactor[4] = { 0, 0, 0, 0 };
            ctx->OMSetBlendState(mBlendOpaque, blendFactor, 0xFFFFFFFFu);
            ctx->OMSetDepthStencilState(device.DSS(), 0);
            curAlphaBlend = false;
        }

        for (const Batch& b : frame.batches)
        {
            if (!b.mesh || !b.mesh->vb)
                continue;

            const uint32_t mi = b.materialIndex;
            const MaterialGpu* mg = (mi < frameMaterials.size()) ? frameMaterials[mi] : nullptr;

            const ShaderProgramD3D11* desiredProg = mg ? mg->program : nullptr;
            if (doSsao && desiredProg && !desiredProg->HasMrtVariant())
                desiredProg = nullptr;

            if (desiredProg != curProg)
            {
                if (desiredProg)
                {
                    ctx->IASetInputLayout(desiredProg->InputLayout());
                    ctx->VSSetShader(desiredProg->VS(), nullptr, 0);
                    ctx->PSSetShader(doSsao && desiredProg->PSMrt() ? desiredProg->PSMrt() : desiredProg->PS(), nullptr, 0);
                }
                else
                {
                    ctx->IASetInputLayout(mInputLayout);
                    ctx->VSSetShader(mVS, nullptr, 0);
                    ctx->PSSetShader(doSsao ? mPSMrt : mPS, nullptr, 0);
                }
                curProg = desiredProg;
                curMat = 0xFFFFFFFFu;
            }

            if (mg && mi != curMat)
            {
                if (mg->alphaBlend != curAlphaBlend)
                {
                    const float blendFactor[4] = { 0, 0, 0, 0 };
                    ctx->OMSetBlendState(mg->alphaBlend ? mBlendAlpha : mBlendOpaque, blendFactor, 0xFFFFFFFFu);
                    if (mg->alphaBlend)
                    {
                        if (mDepthReadOnly)
                            ctx->OMSetDepthStencilState(mDepthReadOnly, 0);
                    }
                    else
                    {
                        ctx->OMSetDepthStencilState(device.DSS(), 0);
                    }
                    curAlphaBlend = mg->alphaBlend;
                }

                if (mg->materialCB)
                    ctx->PSSetConstantBuffers(4, 1, &mg->materialCB);
                ID3D11ShaderResourceView* srvs[4] = {
                    mg->albedoSRV ? mg->albedoSRV : mTextures.White(),
                    mg->normalSRV ? mg->normalSRV : mTextures.White(),
                    mg->mrSRV ? mg->mrSRV : mTextures.White(),
                    mg->emissiveSRV ? mg->emissiveSRV : mTextures.Black()
                };
                ctx->PSSetShaderResources(5, 4, srvs);
                curMat = mi;
            }

            ID3D11Buffer* vbs[2] = { b.mesh->vb, mInstanceVB };
            UINT strides[2] = { (UINT)sizeof(VertexPN), (UINT)sizeof(InstanceData) };
            UINT offsets[2] = { 0u, 0u };
            ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);

            if (b.mesh->ib && !b.mesh->indices.empty())
            {
                ctx->IASetIndexBuffer(b.mesh->ib, DXGI_FORMAT_R16_UINT, 0);
                ctx->DrawIndexedInstanced((UINT)b.mesh->indices.size(), b.instanceCount, 0, 0, b.startInstance);
            }
            else
            {
                ctx->DrawInstanced((UINT)b.mesh->vertices.size(), b.instanceCount, 0, b.startInstance);
            }
        }
    }

    device.EndGpuEvent();

    // Pass: SSAO + blur
    if (doSsao && mSsaoCB && mSsaoRTV && mSsaoBlurRTV)
    {
        king::perf::CpuScope cpuSsao(mPerf, "SSAOPass");
        GpuScopeGuard gpuSsao(mGpuPerf, ctx, "SSAOPass");
        device.BeginGpuEvent(L"SSAOPass");

        std::string ppErr;
        ID3D11PixelShader* psSsao = mPost.Fullscreen().GetOrCreatePS(device, *mShaderCache, mShaderPath, "PSSsaoMain", {}, &ppErr);
        ID3D11PixelShader* psBlur = mPost.Fullscreen().GetOrCreatePS(device, *mShaderCache, mShaderPath, "PSBlurMain", {}, &ppErr);
        if (!psSsao || !psBlur || !mPost.Fullscreen().VS())
        {
            device.EndGpuEvent();
            return;
        }

        using namespace DirectX;
        const bool haveRealProj = !IsIdentityMat(proj);

        // Preferred path: do SSAO in view-space using the actual projection matrix.
        // Fallback path: preserve the old behavior by treating SSAO space as world-space and using viewProj.
        const XMMATRIX projM = haveRealProj ? dx::LoadMat4x4(proj) : dx::LoadMat4x4(viewProj);
        const XMMATRIX invProjM = XMMatrixInverse(nullptr, projM);

        SsaoCBData scb{};
        scb.proj = haveRealProj ? proj : viewProj;
        scb.invProj = dx::StoreMat4x4(invProjM);
        scb.view = haveRealProj ? view : Mat4x4{};
        scb.invTargetSize[0] = 1.0f / (float)device.BackBufferWidth();
        scb.invTargetSize[1] = 1.0f / (float)device.BackBufferHeight();
        scb.radius = settings.ssaoRadius;
        scb.bias = settings.ssaoBias;

        D3D11_MAPPED_SUBRESOURCE mappedSsao{};
        if (SUCCEEDED(ctx->Map(mSsaoCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedSsao)))
        {
            std::memcpy(mappedSsao.pData, &scb, sizeof(scb));
            ctx->Unmap(mSsaoCB, 0);
        }

        // SSAO target
        const float aoClear[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        ctx->ClearRenderTargetView(mSsaoRTV, aoClear);

        D3D11_VIEWPORT vp0 = device.Viewport();
        mPost.Fullscreen().Begin(ctx, mSsaoRTV, vp0);
        ctx->PSSetShader(psSsao, nullptr, 0);

        ctx->PSSetConstantBuffers(3, 1, &mSsaoCB);

        ID3D11ShaderResourceView* ssaoSrvs[2] = { mDepthSRV, mNormalSRV };
        ctx->PSSetShaderResources(3, 2, ssaoSrvs);

        if (mPointClamp)
            ctx->PSSetSamplers(2, 1, &mPointClamp);

        mPost.Fullscreen().Draw(ctx);

        // Unbind SSAO inputs
        {
            ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
            ctx->PSSetShaderResources(3, 2, nullSrvs);
        }

        // Blur target
        ctx->ClearRenderTargetView(mSsaoBlurRTV, aoClear);
        mPost.Fullscreen().Begin(ctx, mSsaoBlurRTV, vp0);
        ctx->PSSetShader(psBlur, nullptr, 0);
        ctx->PSSetShaderResources(2, 1, &mSsaoSRV);
        if (mLinearClamp)
            ctx->PSSetSamplers(1, 1, &mLinearClamp);

        mPost.Fullscreen().Draw(ctx);

        // Unbind blur input
        {
            ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
            ctx->PSSetShaderResources(2, 1, nullSrv);
        }

        device.EndGpuEvent();
    }

    // Pass: Tonemap HDR -> backbuffer
    if (doTonemap && doHdrTarget && mHdrSRV && device.RTV() && mPost.Fullscreen().VS())
    {
        king::perf::CpuScope cpuTonemap(mPerf, "TonemapPass");
        GpuScopeGuard gpuTonemap(mGpuPerf, ctx, "TonemapPass");
        device.BeginGpuEvent(L"TonemapPass");

        static bool onceTonemap = false;
        if (!onceTonemap)
        {
            onceTonemap = true;
            std::printf("TonemapPass: executing\n");
        }

        // Bind AO (blurred SSAO) if available, otherwise bind a 1x1 white texture.
        ID3D11ShaderResourceView* aoSrv = (doSsao && mSsaoBlurSRV) ? mSsaoBlurSRV : mAoWhiteSRV;

        PostProcessD3D11::Settings pp{};
        pp.enableVignette = settings.enableVignette && mAllowPostProcessing;
        pp.vignetteStrength = settings.vignetteStrength;
        pp.vignettePower = settings.vignettePower;
        pp.enableBloom = settings.enableBloom && mAllowPostProcessing;
        pp.bloomIntensity = settings.bloomIntensity;
        pp.bloomThreshold = settings.bloomThreshold;

        mPost.Execute(ctx, device, *mShaderCache, mHdrSRV, aoSrv, mCameraCB, mLinearClamp, pp);

        device.EndGpuEvent();
    }
}

void RenderSystemD3D11::ReleaseSceneMeshBuffers(Scene& scene)
{
    for (auto me : scene.reg.meshes.Entities())
    {
        auto* m = scene.reg.meshes.TryGet(me);
        if (!m)
            continue;

        if (m->vb)
        {
            m->vb->Release();
            m->vb = nullptr;
        }
        if (m->ib)
        {
            m->ib->Release();
            m->ib = nullptr;
        }
    }
}

} // namespace king::render::d3d11
