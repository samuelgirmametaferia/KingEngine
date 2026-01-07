#include "shadows.h"

#include "../../render/shader.h"
#include "../../ecs/components.h"
#include "../../thread_config.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>

namespace
{
static void SafeRelease(IUnknown* p)
{
    if (p) p->Release();
}

struct ShadowCBData
{
    king::Mat4x4 shadowViewProj;
};
static_assert(sizeof(ShadowCBData) % 16 == 0, "ShadowCBData must be 16-byte aligned");

static king::Float3 NormalizeOrDefault(const king::Float3& v, const king::Float3& def)
{
    const float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
    if (len2 < 1e-10f)
        return def;
    const float invLen = 1.0f / std::sqrt(len2);
    return { v.x * invLen, v.y * invLen, v.z * invLen };
}

} // namespace

namespace king::render::d3d11
{

ShadowsD3D11::~ShadowsD3D11()
{
    Shutdown();
}

bool ShadowsD3D11::Initialize(RenderDeviceD3D11& device, ShaderCache& shaderCache, const std::wstring& shaderPath)
{
    ID3D11Device* d = device.Device();
    if (!d)
        return false;

    // Compile shadow VS.
    king::CompiledShader vsShadow;
    std::string shaderErr;
    if (!shaderCache.CompileVSFromFile(shaderPath.c_str(), "VSShadowMain", {}, vsShadow, &shaderErr))
    {
        std::printf("Shader VSShadow compile error:\n%s\n", shaderErr.c_str());
        return false;
    }

    HRESULT hr = d->CreateVertexShader(
        vsShadow.bytecode->GetBufferPointer(),
        vsShadow.bytecode->GetBufferSize(),
        nullptr,
        &mVSShadow);
    if (FAILED(hr))
        return false;

    // Constant buffers (one per cascade).
    D3D11_BUFFER_DESC cbd{};
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cbd.ByteWidth = (UINT)sizeof(ShadowCBData);

    for (uint32_t i = 0; i < kMaxCascades; ++i)
    {
        hr = d->CreateBuffer(&cbd, nullptr, &mShadowCB[i]);
        if (FAILED(hr))
            return false;
    }

    // Comparison samplers.
    {
        D3D11_SAMPLER_DESC sdc{};
        // Point comparison sampling.
        // Our shader does its own multi-tap PCF (rotated Poisson), so using linear
        // comparison here would effectively apply hardware PCF *per tap* and can
        // over-blur/"cook" the result.
        sdc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        sdc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sdc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sdc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sdc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        sdc.MinLOD = 0;
        sdc.MaxLOD = D3D11_FLOAT32_MAX;
        hr = d->CreateSamplerState(&sdc, &mShadowSamplerPoint);
        if (FAILED(hr))
            return false;

        // Linear comparison sampling.
        // When Poisson PCF is disabled, a single SampleCmp with a linear comparison
        // sampler gives hardware 2x2 PCF and avoids harsh "shadow texel squares".
        sdc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        hr = d->CreateSamplerState(&sdc, &mShadowSamplerLinear);
        if (FAILED(hr))
            return false;
    }

    // Non-comparison sampler for raw depth reads (PCSS).
    {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MinLOD = 0;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = d->CreateSamplerState(&sd, &mShadowSamplerNonCmp);
        if (FAILED(hr))
            return false;
    }

    // Create deferred contexts (driven by thread config).
    {
        const king::ThreadConfig& tc = king::GetThreadConfig();
        mShadowRecordThreads = tc.renderShadowRecordThreads;
        uint32_t desired = 1;
        if (tc.renderShadowRecordThreads > 1)
            desired = std::min<uint32_t>(kMaxCascades, tc.renderShadowRecordThreads);
        EnsureDeferredContexts(device, desired);
    }

    return true;
}

void ShadowsD3D11::Shutdown()
{
    ReleaseDeferredContexts();

    SafeRelease((IUnknown*)mShadowReadbackTex);
    mShadowReadbackTex = nullptr;

    SafeRelease((IUnknown*)mShadowRS);
    mShadowRS = nullptr;

    SafeRelease((IUnknown*)mShadowSRV);
    mShadowSRV = nullptr;

    for (uint32_t i = 0; i < kMaxCascades; ++i)
    {
        SafeRelease((IUnknown*)mShadowDSV[i]);
        mShadowDSV[i] = nullptr;
    }

    SafeRelease((IUnknown*)mShadowTex);
    mShadowTex = nullptr;

    SafeRelease((IUnknown*)mShadowSamplerPoint);
    mShadowSamplerPoint = nullptr;

    SafeRelease((IUnknown*)mShadowSamplerLinear);
    mShadowSamplerLinear = nullptr;

    SafeRelease((IUnknown*)mShadowSamplerNonCmp);
    mShadowSamplerNonCmp = nullptr;

    for (uint32_t i = 0; i < kMaxCascades; ++i)
    {
        SafeRelease((IUnknown*)mShadowCB[i]);
        mShadowCB[i] = nullptr;
    }

    SafeRelease((IUnknown*)mVSShadow);
    mVSShadow = nullptr;

    mShadowMapSize = 0;
    mShadowCascadeCount = 0;
}

void ShadowsD3D11::EnsureDeferredContexts(RenderDeviceD3D11& device, uint32_t desired)
{
    if (!mDeferredContexts.empty())
        return;

    ID3D11Device* d = device.Device();
    if (!d)
        return;

    const uint32_t count = (desired == 0) ? 1u : desired;
    mDeferredContexts.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        ID3D11DeviceContext* dc = nullptr;
        if (SUCCEEDED(d->CreateDeferredContext(0, &dc)) && dc)
            mDeferredContexts.push_back(dc);
    }
}

void ShadowsD3D11::ReleaseDeferredContexts()
{
    for (auto* dc : mDeferredContexts)
        SafeRelease((IUnknown*)dc);
    mDeferredContexts.clear();
}

void ShadowsD3D11::EnsureResources(RenderDeviceD3D11& device, uint32_t cascadeCount, uint32_t shadowMapSize)
{
    uint32_t cascades = cascadeCount;
    if (cascades < 1)
        cascades = 1;
    if (cascades > kMaxCascades)
        cascades = kMaxCascades;

    uint32_t size = shadowMapSize;
    if (size < 256)
        size = 256;
    if (size > 4096)
        size = 4096;

    if (mShadowTex && mShadowDSV[0] && mShadowSRV && mShadowRS && mShadowMapSize == size && mShadowCascadeCount == cascades)
        return;

    ID3D11Device* d = device.Device();
    if (!d)
        return;

    // Release old.
    SafeRelease((IUnknown*)mShadowRS);
    mShadowRS = nullptr;

    SafeRelease((IUnknown*)mShadowSRV);
    mShadowSRV = nullptr;

    for (uint32_t i = 0; i < kMaxCascades; ++i)
    {
        SafeRelease((IUnknown*)mShadowDSV[i]);
        mShadowDSV[i] = nullptr;
    }

    SafeRelease((IUnknown*)mShadowTex);
    mShadowTex = nullptr;

    // Shadow format/size changes invalidate readback.
    SafeRelease((IUnknown*)mShadowReadbackTex);
    mShadowReadbackTex = nullptr;

    // Texture array.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = size;
    td.Height = size;
    td.MipLevels = 1;
    td.ArraySize = cascades;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(d->CreateTexture2D(&td, nullptr, &mShadowTex)) || !mShadowTex)
        return;

    for (uint32_t i = 0; i < cascades; ++i)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvd{};
        dsvd.Format = DXGI_FORMAT_D32_FLOAT;
        dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvd.Texture2DArray.MipSlice = 0;
        dsvd.Texture2DArray.FirstArraySlice = i;
        dsvd.Texture2DArray.ArraySize = 1;
        if (FAILED(d->CreateDepthStencilView(mShadowTex, &dsvd, &mShadowDSV[i])) || !mShadowDSV[i])
            return;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_R32_FLOAT;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvd.Texture2DArray.MostDetailedMip = 0;
    srvd.Texture2DArray.MipLevels = 1;
    srvd.Texture2DArray.FirstArraySlice = 0;
    srvd.Texture2DArray.ArraySize = cascades;
    if (FAILED(d->CreateShaderResourceView(mShadowTex, &srvd, &mShadowSRV)) || !mShadowSRV)
        return;

    mShadowViewport.TopLeftX = 0.0f;
    mShadowViewport.TopLeftY = 0.0f;
    mShadowViewport.Width = (float)size;
    mShadowViewport.Height = (float)size;
    mShadowViewport.MinDepth = 0.0f;
    mShadowViewport.MaxDepth = 1.0f;

    // Rasterizer: front-face cull + bias.
    // Front-face culling is a common trick to reduce self-shadowing (acne)
    // on surfaces facing the light (e.g., the "top" faces you're noticing).
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_FRONT;
    rs.DepthClipEnable = TRUE;
    rs.DepthBias = 300;
    rs.SlopeScaledDepthBias = 1.25f;
    rs.DepthBiasClamp = 0.0f;
    (void)d->CreateRasterizerState(&rs, &mShadowRS);

    mShadowMapSize = size;
    mShadowCascadeCount = cascades;
}

bool ShadowsD3D11::ComputeCascades(
    const Mat4x4& cameraViewProj,
    float cameraNearZ,
    float cameraFarZ,
    const Light& sun,
    const Settings& settings,
    Mat4x4 outCascadeViewProj[kMaxCascades],
    float outCascadeSplitsNdc[4],
    uint32_t& outCascadeCount,
    float outShadowTexelSize[2],
    float& outShadowBias,
    float& outShadowStrength)
{
    using namespace DirectX;

    outShadowBias = settings.bias;
    outShadowStrength = settings.strength;
    outShadowTexelSize[0] = 0.0f;
    outShadowTexelSize[1] = 0.0f;

    if (!settings.enable || settings.strength <= 0.0f || !mShadowSRV || !mShadowDSV[0] || !mVSShadow)
        return false;

    const float nearZ = (cameraNearZ > 1e-4f) ? cameraNearZ : 0.1f;
    const float farZ = (cameraFarZ > nearZ + 1e-3f) ? cameraFarZ : (nearZ + 100.0f);

    // Convert view-space z (positive forward, LH) to D3D NDC z in [0,1].
    const float a = farZ / (farZ - nearZ);
    const float b = (-nearZ * farZ) / (farZ - nearZ);
    auto ndcFromViewZ = [&](float z) -> float
    {
        const float zz = (z > nearZ + 1e-4f) ? z : (nearZ + 1e-4f);
        return a + (b / zz);
    };

    uint32_t cascadeCount = settings.cascadeCount;
    if (cascadeCount < 1)
        cascadeCount = 1;
    if (cascadeCount > kMaxCascades)
        cascadeCount = kMaxCascades;

    const float lambda = std::clamp(settings.cascadeLambda, 0.0f, 1.0f);

    float splitViewZ[kMaxCascades] = {};
    for (uint32_t i = 0; i < cascadeCount; ++i)
    {
        const float fi = (float)(i + 1) / (float)cascadeCount;
        const float logSplit = nearZ * std::pow(farZ / nearZ, fi);
        const float linSplit = nearZ + (farZ - nearZ) * fi;
        splitViewZ[i] = lambda * logSplit + (1.0f - lambda) * linSplit;
    }

    outCascadeCount = cascadeCount;
    outCascadeSplitsNdc[0] = ndcFromViewZ(splitViewZ[0]);
    outCascadeSplitsNdc[1] = (cascadeCount > 1) ? ndcFromViewZ(splitViewZ[1]) : 1.0f;
    outCascadeSplitsNdc[2] = 1.0f;
    outCascadeSplitsNdc[3] = (float)settings.debugView;

    const XMMATRIX vp = XMLoadFloat4x4((const XMFLOAT4X4*)&cameraViewProj);
    const XMMATRIX invVP = XMMatrixInverse(nullptr, vp);

    Float3 sunDir = NormalizeOrDefault(sun.direction, { 0, -1, 0 });
    const XMVECTOR dir = XMVectorSet(sunDir.x, sunDir.y, sunDir.z, 0.0f);

    XMVECTOR up = XMVectorSet(0, 1, 0, 0);
    const float dY = std::fabs(XMVectorGetY(dir));
    if (dY > 0.98f)
        up = XMVectorSet(0, 0, 1, 0);

    const float shadowSize = (mShadowMapSize > 0) ? (float)mShadowMapSize : mShadowViewport.Width;

    auto storeMat = [](const DirectX::XMMATRIX& m) -> Mat4x4
    {
        Mat4x4 out{};
        DirectX::XMStoreFloat4x4((DirectX::XMFLOAT4X4*)&out, m);
        return out;
    };

    auto computeCascadeVP = [&](float zNearNdc, float zFarNdc) -> Mat4x4
    {
        const XMVECTOR clipCorners[8] = {
            XMVectorSet(-1, -1, zNearNdc, 1), XMVectorSet( 1, -1, zNearNdc, 1), XMVectorSet( 1,  1, zNearNdc, 1), XMVectorSet(-1,  1, zNearNdc, 1),
            XMVectorSet(-1, -1, zFarNdc,  1), XMVectorSet( 1, -1, zFarNdc,  1), XMVectorSet( 1,  1, zFarNdc,  1), XMVectorSet(-1,  1, zFarNdc,  1),
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

        const XMVECTOR eye = XMVectorSubtract(center, XMVectorScale(dir, 100.0f));
        const XMMATRIX lv = XMMatrixLookAtLH(eye, center, up);

        XMVECTOR minV = XMVectorSet(FLT_MAX, FLT_MAX, FLT_MAX, 1.0f);
        XMVECTOR maxV = XMVectorSet(-FLT_MAX, -FLT_MAX, -FLT_MAX, 1.0f);
        for (int i = 0; i < 8; ++i)
        {
            const XMVECTOR ls = XMVector3TransformCoord(frustumCornersWS[i], lv);
            minV = XMVectorMin(minV, ls);
            maxV = XMVectorMax(maxV, ls);
        }

        const float minX = XMVectorGetX(minV);
        const float minY = XMVectorGetY(minV);
        const float minZ = XMVectorGetZ(minV);
        const float maxX = XMVectorGetX(maxV);
        const float maxY = XMVectorGetY(maxV);
        const float maxZ = XMVectorGetZ(maxV);

        // Padding trades coverage vs sharpness. Too much padding wastes resolution.
        const float baseExtentX = (maxX - minX);
        const float baseExtentY = (maxY - minY);
        const float baseExtentZ = (maxZ - minZ);

        const float xyPad = std::max(2.0f, 0.05f * std::max(baseExtentX, baseExtentY));
        const float zPad = std::max(10.0f, 0.10f * baseExtentZ);

        float nearLS = minZ - zPad;
        float farLS = maxZ + zPad;
        if (farLS < nearLS + 1.0f)
            farLS = nearLS + 1.0f;

        // Snap ortho bounds to shadow texel grid.
        const float extentX = (maxX + xyPad) - (minX - xyPad);
        const float extentY = (maxY + xyPad) - (minY - xyPad);
        const float texelSizeX = extentX / shadowSize;
        const float texelSizeY = extentY / shadowSize;

        float cx = (minX + maxX) * 0.5f;
        float cy = (minY + maxY) * 0.5f;
        cx = std::floor(cx / texelSizeX + 0.5f) * texelSizeX;
        cy = std::floor(cy / texelSizeY + 0.5f) * texelSizeY;

        const float minXS = cx - 0.5f * extentX;
        const float maxXS = cx + 0.5f * extentX;
        const float minYS = cy - 0.5f * extentY;
        const float maxYS = cy + 0.5f * extentY;

        const XMMATRIX lp = XMMatrixOrthographicOffCenterLH(minXS, maxXS, minYS, maxYS, nearLS, farLS);
        return storeMat(lv * lp);
    };

    float prevZ = 0.0f;
    for (uint32_t c = 0; c < cascadeCount; ++c)
    {
        const float zFar = (c == 0) ? outCascadeSplitsNdc[0] : (c == 1 ? outCascadeSplitsNdc[1] : 1.0f);
        outCascadeViewProj[c] = computeCascadeVP(prevZ, zFar);
        prevZ = zFar;
    }

    outShadowTexelSize[0] = 1.0f / shadowSize;
    outShadowTexelSize[1] = 1.0f / shadowSize;

    return true;
}

void ShadowsD3D11::EnsureReadbackTexture(RenderDeviceD3D11& device)
{
    ID3D11Device* d = device.Device();
    if (!d)
        return;

    if (mShadowReadbackTex)
        return;

    if (!mShadowTex || mShadowMapSize == 0)
        return;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = mShadowMapSize;
    td.Height = mShadowMapSize;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    (void)d->CreateTexture2D(&td, nullptr, &mShadowReadbackTex);
}

void ShadowsD3D11::Render(
    RenderDeviceD3D11& device,
    const std::vector<DrawBatch> batchesPerCascade[kMaxCascades],
    ID3D11InputLayout* inputLayout,
    ID3D11Buffer* instanceVB,
    uint32_t instanceStride,
    const Mat4x4 cascadeViewProj[kMaxCascades],
    uint32_t cascadeCount,
    bool debugReadbackOnce)
{
    ID3D11DeviceContext* ctx = device.Context();
    if (!ctx || !mShadowDSV[0] || !mShadowRS || !mVSShadow || !instanceVB || !inputLayout)
        return;

    uint32_t cascades = cascadeCount;
    if (cascades < 1)
        cascades = 1;
    if (cascades > kMaxCascades)
        cascades = kMaxCascades;

    // Update per-cascade constant buffers on the immediate context (no contention).
    D3D11_MAPPED_SUBRESOURCE mapped{};
    for (uint32_t c = 0; c < cascades; ++c)
    {
        ShadowCBData scb{};
        scb.shadowViewProj = cascadeViewProj[c];
        if (SUCCEEDED(ctx->Map(mShadowCB[c], 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            std::memcpy(mapped.pData, &scb, sizeof(scb));
            ctx->Unmap(mShadowCB[c], 0);
        }
    }

    // Record per-cascade command lists in parallel.
    struct Recorded
    {
        ID3D11CommandList* list = nullptr;
    };

    std::vector<Recorded> recorded(cascades);

    const uint32_t requestedThreads = mShadowRecordThreads;
    const uint32_t ctxCount = (uint32_t)mDeferredContexts.size();

    auto RecordCascade = [&](ID3D11DeviceContext* dc, uint32_t c)
    {
        if (!dc)
            return;

        dc->ClearState();

        // Bind shadow outputs.
        dc->OMSetRenderTargets(0, nullptr, mShadowDSV[c]);
        dc->ClearDepthStencilView(mShadowDSV[c], D3D11_CLEAR_DEPTH, 1.0f, 0);
        dc->RSSetViewports(1, &mShadowViewport);
        dc->RSSetState(mShadowRS);

        dc->IASetInputLayout(inputLayout);
        dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        dc->VSSetShader(mVSShadow, nullptr, 0);
        dc->PSSetShader(nullptr, nullptr, 0);

        // Shadow VS reads ShadowCB at b2.
        dc->VSSetConstantBuffers(2, 1, &mShadowCB[c]);

        const std::vector<DrawBatch>& batches = batchesPerCascade[c];
        for (const DrawBatch& b : batches)
        {
            if (!b.vb || b.instanceCount == 0)
                continue;

            ID3D11Buffer* vbs[2] = { b.vb, instanceVB };
            UINT strides[2] = { (UINT)sizeof(king::VertexPN), (UINT)instanceStride };
            UINT offsets[2] = { 0u, 0u };
            dc->IASetVertexBuffers(0, 2, vbs, strides, offsets);

            if (b.ib && b.indexCount > 0)
            {
                dc->IASetIndexBuffer(b.ib, DXGI_FORMAT_R16_UINT, 0);
                dc->DrawIndexedInstanced(b.indexCount, b.instanceCount, 0, 0, b.startInstance);
            }
            else if (b.vertexCount > 0)
            {
                dc->DrawInstanced(b.vertexCount, b.instanceCount, 0, b.startInstance);
            }
        }

        ID3D11CommandList* cl = nullptr;
        if (SUCCEEDED(dc->FinishCommandList(FALSE, &cl)))
            recorded[c].list = cl;
    };

    // If configured for 0/1 threads, record sequentially (avoid per-frame thread creation).
    if (requestedThreads <= 1)
    {
        for (uint32_t c = 0; c < cascades; ++c)
        {
            ID3D11DeviceContext* dc = (ctxCount > 0) ? mDeferredContexts[c % ctxCount] : nullptr;
            if (!dc)
            {
                // Fallback: execute directly on the immediate context.
                ctx->OMSetRenderTargets(0, nullptr, mShadowDSV[c]);
                ctx->ClearDepthStencilView(mShadowDSV[c], D3D11_CLEAR_DEPTH, 1.0f, 0);
                ctx->RSSetViewports(1, &mShadowViewport);
                ctx->RSSetState(mShadowRS);
                ctx->IASetInputLayout(inputLayout);
                ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ctx->VSSetShader(mVSShadow, nullptr, 0);
                ctx->PSSetShader(nullptr, nullptr, 0);
                ctx->VSSetConstantBuffers(2, 1, &mShadowCB[c]);

                const std::vector<DrawBatch>& batches = batchesPerCascade[c];
                for (const DrawBatch& b : batches)
                {
                    if (!b.vb || b.instanceCount == 0)
                        continue;

                    ID3D11Buffer* vbs[2] = { b.vb, instanceVB };
                    UINT strides[2] = { (UINT)sizeof(king::VertexPN), (UINT)instanceStride };
                    UINT offsets[2] = { 0u, 0u };
                    ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);

                    if (b.ib && b.indexCount > 0)
                    {
                        ctx->IASetIndexBuffer(b.ib, DXGI_FORMAT_R16_UINT, 0);
                        ctx->DrawIndexedInstanced(b.indexCount, b.instanceCount, 0, 0, b.startInstance);
                    }
                    else if (b.vertexCount > 0)
                    {
                        ctx->DrawInstanced(b.vertexCount, b.instanceCount, 0, b.startInstance);
                    }
                }
            }
            else
            {
                RecordCascade(dc, c);
            }
        }
    }
    else
    {
        const uint32_t maxThreads = (ctxCount > 0) ? ctxCount : 1u;
        const uint32_t workerCount = std::min<uint32_t>(std::min<uint32_t>(requestedThreads, cascades), maxThreads);

        std::atomic<uint32_t> next{ 0u };
        std::vector<std::thread> threads;
        threads.reserve(workerCount);

        for (uint32_t t = 0; t < workerCount; ++t)
        {
            ID3D11DeviceContext* dc = (ctxCount > 0) ? mDeferredContexts[t % ctxCount] : nullptr;
            threads.emplace_back([&, dc]()
            {
                for (;;)
                {
                    const uint32_t c = next.fetch_add(1u);
                    if (c >= cascades)
                        break;
                    RecordCascade(dc, c);
                }
            });
        }

        for (auto& t : threads)
            t.join();
    }

    // Execute command lists.
    for (uint32_t c = 0; c < cascades; ++c)
    {
        if (recorded[c].list)
        {
            ctx->ExecuteCommandList(recorded[c].list, FALSE);
            SafeRelease((IUnknown*)recorded[c].list);
            recorded[c].list = nullptr;
        }
    }

    // Optional one-time diagnostics: read back slice 0.
    static bool sPrintedShadowReadback = false;
    if (debugReadbackOnce && !sPrintedShadowReadback && mShadowTex)
    {
        sPrintedShadowReadback = true;
        EnsureReadbackTexture(device);

        if (mShadowReadbackTex)
        {
            const UINT srcSub = D3D11CalcSubresource(0, 0, 1);
            ctx->CopySubresourceRegion(mShadowReadbackTex, 0, 0, 0, 0, mShadowTex, srcSub, nullptr);

            D3D11_MAPPED_SUBRESOURCE rb{};
            if (SUCCEEDED(ctx->Map(mShadowReadbackTex, 0, D3D11_MAP_READ, 0, &rb)))
            {
                float minD = 1.0f;
                float maxD = 0.0f;
                uint32_t written = 0;

                const uint32_t w = mShadowMapSize;
                const uint32_t h = mShadowMapSize;
                for (uint32_t y = 0; y < h; ++y)
                {
                    const uint8_t* row = (const uint8_t*)rb.pData + (size_t)y * (size_t)rb.RowPitch;
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        const float d = *(const float*)(row + (size_t)x * 4);
                        minD = std::min(minD, d);
                        maxD = std::max(maxD, d);
                        if (d < 0.9999f) ++written;
                    }
                }

                ctx->Unmap(mShadowReadbackTex, 0);
                std::printf("ShadowReadback(slice0): size=%ux%u min=%f max=%f writtenPixels=%u\n", w, h, minD, maxD, written);
            }
        }
    }
}

} // namespace king::render::d3d11
