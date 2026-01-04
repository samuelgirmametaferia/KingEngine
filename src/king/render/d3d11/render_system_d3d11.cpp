#include "render_system_d3d11.h"

#include "../../math/dxmath.h"
#include "../shader.h"

#include <cstring>
#include <cstdio>
#include <cmath>
#include <cfloat>
#include <vector>
#include <algorithm>

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
    unsigned hc = std::thread::hardware_concurrency();
    unsigned desired = (hc > 1) ? (hc - 1) : 0;
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
        for (;;)
        {
            std::vector<SnapshotItem> items;
            Frustum fr{};

            {
                std::unique_lock<std::mutex> lock(mWorkMutex);
                mWorkCv.wait(lock, [this]() { return mWorkerExit || mPendingValid; });
                if (mWorkerExit)
                    return;

                items = mPendingItems;
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

    ID3D11Device* d = device.Device();
    if (!d)
        return false;

    // Compile shaders using King shader cache.
    king::ShaderCache shaderCache(d);
    king::CompiledShader vs;
    king::CompiledShader ps;
    king::CompiledShader vsShadow;
    king::CompiledShader vsTonemap;
    king::CompiledShader psTonemap;
    std::string shaderErr;

    if (!shaderCache.CompileVSFromFile(shaderPath.c_str(), "VSMain", {}, vs, &shaderErr))
    {
        std::printf("Shader VS compile error:\n%s\n", shaderErr.c_str());
        return false;
    }

    if (!shaderCache.CompilePSFromFile(shaderPath.c_str(), "PSMain", {}, ps, &shaderErr))
    {
        std::printf("Shader PS compile error:\n%s\n", shaderErr.c_str());
        return false;
    }

    if (!shaderCache.CompileVSFromFile(shaderPath.c_str(), "VSShadowMain", {}, vsShadow, &shaderErr))
    {
        std::printf("Shader VSShadow compile error:\n%s\n", shaderErr.c_str());
        return false;
    }

    if (!shaderCache.CompileVSFromFile(shaderPath.c_str(), "VSTonemapMain", {}, vsTonemap, &shaderErr))
    {
        std::printf("Shader VSTonemap compile error:\n%s\n", shaderErr.c_str());
        return false;
    }

    if (!shaderCache.CompilePSFromFile(shaderPath.c_str(), "PSTonemapMain", {}, psTonemap, &shaderErr))
    {
        std::printf("Shader PSTonemap compile error:\n%s\n", shaderErr.c_str());
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
        return false;

    hr = d->CreatePixelShader(ps.bytecode->GetBufferPointer(), ps.bytecode->GetBufferSize(), nullptr, &mPS);
    if (FAILED(hr))
        return false;

    hr = d->CreateVertexShader(vsShadow.bytecode->GetBufferPointer(), vsShadow.bytecode->GetBufferSize(), nullptr, &mVSShadow);
    if (FAILED(hr))
        return false;

    hr = d->CreateVertexShader(vsTonemap.bytecode->GetBufferPointer(), vsTonemap.bytecode->GetBufferSize(), nullptr, &mVSTonemap);
    if (FAILED(hr))
        return false;

    hr = d->CreatePixelShader(psTonemap.bytecode->GetBufferPointer(), psTonemap.bytecode->GetBufferSize(), nullptr, &mPSTonemap);
    if (FAILED(hr))
        return false;

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },

        // Per-instance data (slot 1)
        { "TEXCOORD",  4, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0,  D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD",  5, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD",  6, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD",  7, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "COLOR",     0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 64, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD",  8, DXGI_FORMAT_R32G32_FLOAT,       1, 80, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD",  9, DXGI_FORMAT_R32_UINT,           1, 88, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD", 10, DXGI_FORMAT_R32_UINT,           1, 92, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
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

        D3D11_SAMPLER_DESC sdc{};
        sdc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        sdc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sdc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sdc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sdc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        sdc.MinLOD = 0;
        sdc.MaxLOD = D3D11_FLOAT32_MAX;
        hr = d->CreateSamplerState(&sdc, &mShadowSampler);
        if (FAILED(hr))
            return false;
    }

    // Instance buffer: created lazily (capacity grows with scene).
    mInstanceCapacity = 0;

    EnsureDeferredContexts(device);

    EnsureShadowResources(device);

    StartWorker();

    return true;
}

void RenderSystemD3D11::Shutdown()
{
    StopWorker();

    ReleaseDeferredContexts();

    IUnknown* tmp = nullptr;

    tmp = (IUnknown*)mCameraCB;
    SafeRelease(tmp);
    mCameraCB = nullptr;

    tmp = (IUnknown*)mLightCB;
    SafeRelease(tmp);
    mLightCB = nullptr;

    tmp = (IUnknown*)mLinearClamp;
    SafeRelease(tmp);
    mLinearClamp = nullptr;

    tmp = (IUnknown*)mShadowSampler;
    SafeRelease(tmp);
    mShadowSampler = nullptr;

    tmp = (IUnknown*)mShadowRS;
    SafeRelease(tmp);
    mShadowRS = nullptr;

    tmp = (IUnknown*)mShadowSRV;
    SafeRelease(tmp);
    mShadowSRV = nullptr;

    tmp = (IUnknown*)mShadowDSV;
    SafeRelease(tmp);
    mShadowDSV = nullptr;

    tmp = (IUnknown*)mShadowTex;
    SafeRelease(tmp);
    mShadowTex = nullptr;

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

    tmp = (IUnknown*)mInstanceVB;
    SafeRelease(tmp);
    mInstanceVB = nullptr;
    mInstanceCapacity = 0;

    tmp = (IUnknown*)mInputLayout;
    SafeRelease(tmp);
    mInputLayout = nullptr;

    tmp = (IUnknown*)mVSShadow;
    SafeRelease(tmp);
    mVSShadow = nullptr;

    tmp = (IUnknown*)mVSTonemap;
    SafeRelease(tmp);
    mVSTonemap = nullptr;

    tmp = (IUnknown*)mPSTonemap;
    SafeRelease(tmp);
    mPSTonemap = nullptr;

    tmp = (IUnknown*)mVS;
    SafeRelease(tmp);
    mVS = nullptr;

    tmp = (IUnknown*)mPS;
    SafeRelease(tmp);
    mPS = nullptr;

    {
        std::lock_guard<std::mutex> lock(mWorkMutex);
        mPendingItems.clear();
        mReadyFrame.instances.clear();
        mReadyFrame.batches.clear();
        mRenderFrame.instances.clear();
        mRenderFrame.batches.clear();
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
        it.lightMask = r->lightMask;
        it.flags = 0;
        if (r->receivesShadows)
            it.flags |= kInstFlag_ReceivesShadows;
        if (r->castsShadows)
            it.flags |= kInstFlag_CastsShadows;
        it.boundsCenter = m->boundsCenter;
        it.boundsRadius = m->boundsRadius;
        outItems.push_back(it);
    }
}

void RenderSystemD3D11::EnqueueBuild(std::vector<SnapshotItem>&& items, const Frustum& frustum)
{
    {
        std::lock_guard<std::mutex> lock(mWorkMutex);
        mPendingItems = std::move(items);
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

    struct Item
    {
        Mesh* mesh = nullptr;
        InstanceData inst{};
    };

    std::vector<Item> visible;
    visible.reserve(items.size());

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
        it.inst.world = TransformToWorld(s.transform);
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
        return (uintptr_t)a.mesh < (uintptr_t)b.mesh;
    });

    outFrame.instances.reserve(visible.size());
    outFrame.batches.reserve(64);

    Mesh* currentMesh = nullptr;
    Batch currentBatch{};

    for (const Item& v : visible)
    {
        if (v.mesh != currentMesh)
        {
            if (currentBatch.mesh)
                outFrame.batches.push_back(currentBatch);

            currentMesh = v.mesh;
            currentBatch = {};
            currentBatch.mesh = v.mesh;
            currentBatch.startInstance = (uint32_t)outFrame.instances.size();
            currentBatch.instanceCount = 0;
        }

        outFrame.instances.push_back(v.inst);
        currentBatch.instanceCount++;
    }

    if (currentBatch.mesh)
        outFrame.batches.push_back(currentBatch);
}

void RenderSystemD3D11::UpdateCameraCB(ID3D11DeviceContext* ctx, const Mat4x4& viewProj, const Float3& cameraPos, float exposure)
{
    CameraCBData data{};
    data.viewProj = viewProj;
    data.cameraPos[0] = cameraPos.x;
    data.cameraPos[1] = cameraPos.y;
    data.cameraPos[2] = cameraPos.z;
    data.exposure = exposure;

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

void RenderSystemD3D11::EnsureShadowResources(RenderDeviceD3D11& device)
{
    if (mShadowTex && mShadowDSV && mShadowSRV && mShadowRS)
        return;

    ID3D11Device* d = device.Device();
    if (!d)
        return;

    // Fixed-size directional shadow map for now.
    const uint32_t size = 2048;

    IUnknown* tmp = nullptr;

    tmp = (IUnknown*)mShadowRS;
    SafeRelease(tmp);
    mShadowRS = nullptr;

    tmp = (IUnknown*)mShadowSRV;
    SafeRelease(tmp);
    mShadowSRV = nullptr;

    tmp = (IUnknown*)mShadowDSV;
    SafeRelease(tmp);
    mShadowDSV = nullptr;

    tmp = (IUnknown*)mShadowTex;
    SafeRelease(tmp);
    mShadowTex = nullptr;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = size;
    td.Height = size;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(d->CreateTexture2D(&td, nullptr, &mShadowTex)) || !mShadowTex)
        return;

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvd{};
    dsvd.Format = DXGI_FORMAT_D32_FLOAT;
    dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dsvd.Texture2D.MipSlice = 0;
    if (FAILED(d->CreateDepthStencilView(mShadowTex, &dsvd, &mShadowDSV)) || !mShadowDSV)
        return;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_R32_FLOAT;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MostDetailedMip = 0;
    srvd.Texture2D.MipLevels = 1;
    if (FAILED(d->CreateShaderResourceView(mShadowTex, &srvd, &mShadowSRV)) || !mShadowSRV)
        return;

    mShadowViewport.TopLeftX = 0.0f;
    mShadowViewport.TopLeftY = 0.0f;
    mShadowViewport.Width = (float)size;
    mShadowViewport.Height = (float)size;
    mShadowViewport.MinDepth = 0.0f;
    mShadowViewport.MaxDepth = 1.0f;

    // Depth bias tuning is scene-dependent; start with conservative values.
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    rs.DepthBias = 1500;
    rs.SlopeScaledDepthBias = 2.0f;
    rs.DepthBiasClamp = 0.0f;
    (void)d->CreateRasterizerState(&rs, &mShadowRS);
}

void RenderSystemD3D11::EnsureHdrTargets(RenderDeviceD3D11& device)
{
    ID3D11Device* d = device.Device();
    if (!d)
        return;

    const uint32_t w = device.BackBufferWidth();
    const uint32_t h = device.BackBufferHeight();
    if (w == 0 || h == 0)
        return;

    if (mHdrTex && mHdrRTV && mHdrSRV && mHdrW == w && mHdrH == h)
        return;

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

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(d->CreateTexture2D(&td, nullptr, &mHdrTex)) || !mHdrTex)
        return;

    if (FAILED(d->CreateRenderTargetView(mHdrTex, nullptr, &mHdrRTV)) || !mHdrRTV)
        return;

    if (FAILED(d->CreateShaderResourceView(mHdrTex, nullptr, &mHdrSRV)) || !mHdrSRV)
        return;

    mHdrW = w;
    mHdrH = h;
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
    float exposure)
{
    ID3D11DeviceContext* ctx = device.Context();
    if (!ctx)
        return;

    EnsureHdrTargets(device);
    EnsureShadowResources(device);

    ID3D11RenderTargetView* mainRtv = mHdrRTV ? mHdrRTV : device.RTV();
    if (!mainRtv)
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
    std::vector<SnapshotItem> snapshot;
    BuildSnapshot(scene, snapshot);

    if (!ConsumeReadyFrame(frame))
        BuildPreparedFrame(snapshot, frustum, frame);

    // Kick prep for the NEXT frame.
    EnqueueBuild(std::move(snapshot), frustum);

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
    lightCB.shadowBias = 0.00125f;
    lightCB.shadowTexelSize[0] = 0.0f;
    lightCB.shadowTexelSize[1] = 0.0f;
    lightCB.lightViewProj = {};

    Light sun{};
    Transform sunXform{};
    const bool haveSun = GetPrimaryDirectionalLightWithTransform(scene, sun, sunXform);
    const bool doShadows = haveSun && sun.castsShadows && mShadowDSV && mShadowSRV && mVSShadow;

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
            (mHdrSRV && device.RTV() && mVSTonemap && mPSTonemap) ? "yes" : "no");
    }

    Mat4x4 lightViewProj{};
    if (doShadows)
    {
        using namespace DirectX;

        const XMMATRIX vp = dx::LoadMat4x4(viewProj);
        const XMMATRIX invVP = XMMatrixInverse(nullptr, vp);

        // D3D NDC: z in [0,1]
        const XMVECTOR clipCorners[8] = {
            XMVectorSet(-1, -1, 0, 1), XMVectorSet( 1, -1, 0, 1), XMVectorSet( 1,  1, 0, 1), XMVectorSet(-1,  1, 0, 1),
            XMVectorSet(-1, -1, 1, 1), XMVectorSet( 1, -1, 1, 1), XMVectorSet( 1,  1, 1, 1), XMVectorSet(-1,  1, 1, 1),
        };

        XMVECTOR frustumCornersWS[8] = {};
        XMVECTOR center = XMVectorZero();
        for (int i = 0; i < 8; ++i)
        {
            XMVECTOR w = XMVector4Transform(clipCorners[i], invVP);
            w = XMVectorScale(w, 1.0f / XMVectorGetW(w));
            frustumCornersWS[i] = w;
            center = XMVectorAdd(center, w);
        }
        center = XMVectorScale(center, 1.0f / 8.0f);

        Float3 sunDir = NormalizeOrDefault(sun.direction, { 0, -1, 0 });
        const XMVECTOR dir = XMVectorSet(sunDir.x, sunDir.y, sunDir.z, 0.0f);

        XMVECTOR up = XMVectorSet(0, 1, 0, 0);
        const float dY = std::fabs(XMVectorGetY(dir));
        if (dY > 0.98f)
            up = XMVectorSet(0, 0, 1, 0);

        const XMVECTOR eye = XMVectorSubtract(center, XMVectorScale(dir, 100.0f));
        const XMMATRIX lightView = XMMatrixLookAtLH(eye, center, up);

        XMVECTOR minV = XMVectorSet(FLT_MAX, FLT_MAX, FLT_MAX, 1.0f);
        XMVECTOR maxV = XMVectorSet(-FLT_MAX, -FLT_MAX, -FLT_MAX, 1.0f);
        for (int i = 0; i < 8; ++i)
        {
            const XMVECTOR ls = XMVector3TransformCoord(frustumCornersWS[i], lightView);
            minV = XMVectorMin(minV, ls);
            maxV = XMVectorMax(maxV, ls);
        }

        const float minX = XMVectorGetX(minV);
        const float minY = XMVectorGetY(minV);
        const float minZ = XMVectorGetZ(minV);
        const float maxX = XMVectorGetX(maxV);
        const float maxY = XMVectorGetY(maxV);
        const float maxZ = XMVectorGetZ(maxV);

        const float xyPad = 10.0f;
        const float zPad = 50.0f;

        const XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(
            minX - xyPad,
            maxX + xyPad,
            minY - xyPad,
            maxY + xyPad,
            minZ - zPad,
            maxZ + zPad);

        const XMMATRIX lvp = lightView * lightProj;
        lightViewProj = dx::StoreMat4x4(lvp);
        lightCB.lightViewProj = lightViewProj;

        const float shadowSize = mShadowViewport.Width;
        lightCB.shadowTexelSize[0] = 1.0f / shadowSize;
        lightCB.shadowTexelSize[1] = 1.0f / shadowSize;
        lightCB.shadowStrength = 1.0f;
    }

    UpdateCameraCB(ctx, viewProj, cameraPos, exposure);
    UpdateLightCB(ctx, lightCB);

    // Pass: Shadow map (directional)
    if (doShadows && lightCB.shadowStrength > 0.0f)
    {
        device.BeginGpuEvent(L"ShadowPass");

        // Bind shadow DSV only.
        ctx->OMSetRenderTargets(0, nullptr, mShadowDSV);
        ctx->ClearDepthStencilView(mShadowDSV, D3D11_CLEAR_DEPTH, 1.0f, 0);
        ctx->RSSetViewports(1, &mShadowViewport);
        if (mShadowRS)
            ctx->RSSetState(mShadowRS);

        ctx->IASetInputLayout(mInputLayout);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        ctx->VSSetShader(mVSShadow, nullptr, 0);
        ctx->PSSetShader(nullptr, nullptr, 0);

        // Shadow VS reads LightCB (b1) for gLightViewProj.
        ctx->VSSetConstantBuffers(1, 1, &mLightCB);

        // Build a shadow-only frame (only casters). Use the same culling frustum for now.
        std::vector<SnapshotItem> shadowItems;
        shadowItems.reserve(frame.instances.size());
        constexpr uint32_t kInstFlag_CastsShadows = 1u << 1;

        // Rebuild from scene snapshot to include caster flags.
        std::vector<SnapshotItem> shadowSnapshot;
        BuildSnapshot(scene, shadowSnapshot);
        for (const auto& s : shadowSnapshot)
        {
            if ((s.flags & kInstFlag_CastsShadows) != 0)
                shadowItems.push_back(s);
        }

        PreparedFrame shadowFrame;
        BuildPreparedFrame(shadowItems, frustum, shadowFrame);

        if (!shadowFrame.instances.empty() && !shadowFrame.batches.empty())
        {
            EnsureInstanceBuffer(device, shadowFrame.instances.size());
            if (mInstanceVB)
            {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(ctx->Map(mInstanceVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                {
                    std::memcpy(mapped.pData, shadowFrame.instances.data(), shadowFrame.instances.size() * sizeof(InstanceData));
                    ctx->Unmap(mInstanceVB, 0);

                    for (const Batch& b : shadowFrame.batches)
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
                }
            }
        }

        // Restore main viewport/state.
        D3D11_VIEWPORT vp = device.Viewport();
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(device.RS());

        device.EndGpuEvent();
    }

    // Pass: Geometry into HDR
    // Clear HDR target + depth (depth was already cleared by BeginFrame, but clear again for safety).
    const float hdrClear[4] = { 0.06f, 0.06f, 0.08f, 1.0f };
    ctx->ClearRenderTargetView(mainRtv, hdrClear);
    if (device.DSV())
        ctx->ClearDepthStencilView(device.DSV(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    ctx->OMSetRenderTargets(1, &mainRtv, device.DSV());
    ctx->OMSetDepthStencilState(device.DSS(), 0);
    ctx->RSSetState(device.RS());
    {
        D3D11_VIEWPORT vp = device.Viewport();
        ctx->RSSetViewports(1, &vp);
    }

    ctx->IASetInputLayout(mInputLayout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ctx->VSSetShader(mVS, nullptr, 0);
    ctx->PSSetShader(mPS, nullptr, 0);

    ctx->VSSetConstantBuffers(0, 1, &mCameraCB);
    ctx->PSSetConstantBuffers(0, 1, &mCameraCB);
    ctx->PSSetConstantBuffers(1, 1, &mLightCB);

    if (doShadows && mShadowSRV && mShadowSampler)
    {
        ctx->PSSetShaderResources(0, 1, &mShadowSRV);
        ctx->PSSetSamplers(0, 1, &mShadowSampler);
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

    // Parallel draw submission using deferred contexts + command lists.
    // Note: all resource updates are done on the immediate context above.
    bool usedDeferred = false;
    if (!mDeferredContexts.empty())
    {
        const size_t totalBatches = frame.batches.size();
        const size_t numWorkers = mDeferredContexts.size();
        const size_t chunk = (totalBatches + numWorkers - 1) / numWorkers;

        struct Recorded
        {
            ID3D11CommandList* list = nullptr;
        };
        std::vector<Recorded> recorded(numWorkers);
        std::vector<std::thread> threads;
        threads.reserve(numWorkers);

        for (size_t i = 0; i < numWorkers; ++i)
        {
            const size_t begin = i * chunk;
            const size_t end = (begin + chunk < totalBatches) ? (begin + chunk) : totalBatches;
            if (begin >= end)
                continue;

            ID3D11DeviceContext* dc = mDeferredContexts[i];
            threads.emplace_back([&, dc, i, begin, end]()
            {
                if (!dc)
                    return;

                dc->ClearState();
                // IMPORTANT: deferred contexts don't inherit the immediate context's
                // render target binding/state. Bind outputs + raster/depth state before draws.
                ID3D11RenderTargetView* rtv = mainRtv;
                ID3D11DepthStencilView* dsv = device.DSV();
                ID3D11DepthStencilState* dss = device.DSS();
                ID3D11RasterizerState* rs = device.RS();
                D3D11_VIEWPORT vp = device.Viewport();

                if (rtv)
                    dc->OMSetRenderTargets(1, &rtv, dsv);
                if (dss)
                    dc->OMSetDepthStencilState(dss, 0);
                if (rs)
                    dc->RSSetState(rs);
                dc->RSSetViewports(1, &vp);

                dc->IASetInputLayout(mInputLayout);
                dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

                dc->VSSetShader(mVS, nullptr, 0);
                dc->PSSetShader(mPS, nullptr, 0);

                dc->VSSetConstantBuffers(0, 1, &mCameraCB);
                dc->PSSetConstantBuffers(0, 1, &mCameraCB);
                dc->PSSetConstantBuffers(1, 1, &mLightCB);

                if (doShadows && mShadowSRV && mShadowSampler)
                {
                    dc->PSSetShaderResources(0, 1, &mShadowSRV);
                    dc->PSSetSamplers(0, 1, &mShadowSampler);
                }

                for (size_t bi = begin; bi < end; ++bi)
                {
                    const Batch& b = frame.batches[bi];
                    if (!b.mesh || !b.mesh->vb)
                        continue;

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
            });
        }

        for (auto& t : threads)
            t.join();

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
    }

    device.EndGpuEvent();

    // Pass: Tonemap HDR -> backbuffer
    if (mHdrSRV && device.RTV() && mVSTonemap && mPSTonemap)
    {
        device.BeginGpuEvent(L"TonemapPass");

        static bool onceTonemap = false;
        if (!onceTonemap)
        {
            onceTonemap = true;
            std::printf("TonemapPass: executing\n");
        }

        ID3D11RenderTargetView* outRtv = device.RTV();
        ctx->OMSetRenderTargets(1, &outRtv, nullptr);
        ctx->OMSetDepthStencilState(nullptr, 0);

        D3D11_VIEWPORT vp = device.Viewport();
        ctx->RSSetViewports(1, &vp);

        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        ctx->VSSetShader(mVSTonemap, nullptr, 0);
        ctx->PSSetShader(mPSTonemap, nullptr, 0);

        ctx->PSSetConstantBuffers(0, 1, &mCameraCB);

        ctx->PSSetShaderResources(1, 1, &mHdrSRV);
        if (mLinearClamp)
            ctx->PSSetSamplers(1, 1, &mLinearClamp);

        ctx->Draw(3, 0);

        // Unbind HDR SRV so it can be rebound as an RTV next frame.
        ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
        ctx->PSSetShaderResources(1, 1, nullSrv);

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
