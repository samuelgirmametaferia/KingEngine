#include "post_process_d3d11.h"

#include "render_device_d3d11.h"

#include <cstring>
#include <cstdio>

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

PostProcessD3D11::~PostProcessD3D11()
{
    Shutdown();
}

bool PostProcessD3D11::Initialize(RenderDeviceD3D11& device, king::ShaderCache& cache, const std::wstring& shaderPath)
{
    Shutdown();

    mShaderPath = shaderPath;

    if (!mFullscreen.Initialize(device, cache, shaderPath, "VSFullscreenMain"))
        return false;

    ID3D11Device* d = device.Device();
    if (!d)
        return false;

    D3D11_BUFFER_DESC cbd{};
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cbd.ByteWidth = (UINT)sizeof(PostCBData);

    if ((cbd.ByteWidth % 16u) != 0u)
    {
        std::printf("PostProcessD3D11: PostCB size %u is not 16-byte aligned.\n", (unsigned)cbd.ByteWidth);
        return false;
    }

    HRESULT hr = d->CreateBuffer(&cbd, nullptr, &mPostCB);
    if (FAILED(hr) || !mPostCB)
    {
        std::printf("PostProcessD3D11: CreateBuffer(PostCB) failed hr=0x%08X\n", (unsigned)hr);
        return false;
    }

    return true;
}

void PostProcessD3D11::Shutdown()
{
    IUnknown* tmp = nullptr;

    tmp = (IUnknown*)mPingSRV;
    SafeRelease(tmp);
    mPingSRV = nullptr;

    tmp = (IUnknown*)mPingRTV;
    SafeRelease(tmp);
    mPingRTV = nullptr;

    tmp = (IUnknown*)mPingTex;
    SafeRelease(tmp);
    mPingTex = nullptr;

    tmp = (IUnknown*)mBloomASRV;
    SafeRelease(tmp);
    mBloomASRV = nullptr;

    tmp = (IUnknown*)mBloomARTV;
    SafeRelease(tmp);
    mBloomARTV = nullptr;

    tmp = (IUnknown*)mBloomATex;
    SafeRelease(tmp);
    mBloomATex = nullptr;

    tmp = (IUnknown*)mBloomBSRV;
    SafeRelease(tmp);
    mBloomBSRV = nullptr;

    tmp = (IUnknown*)mBloomBRTV;
    SafeRelease(tmp);
    mBloomBRTV = nullptr;

    tmp = (IUnknown*)mBloomBTex;
    SafeRelease(tmp);
    mBloomBTex = nullptr;

    mW = 0;
    mH = 0;
    mBloomW = 0;
    mBloomH = 0;

    tmp = (IUnknown*)mPostCB;
    SafeRelease(tmp);
    mPostCB = nullptr;

    mFullscreen.Shutdown();

    mShaderPath.clear();
}

void PostProcessD3D11::EnsureIntermediate(RenderDeviceD3D11& device)
{
    ID3D11Device* d = device.Device();
    if (!d)
        return;

    const uint32_t w = device.BackBufferWidth();
    const uint32_t h = device.BackBufferHeight();
    if (w == 0 || h == 0)
        return;

    if (mPingTex && mPingRTV && mPingSRV && mW == w && mH == h)
        return;

    IUnknown* tmp = nullptr;

    tmp = (IUnknown*)mPingSRV;
    SafeRelease(tmp);
    mPingSRV = nullptr;

    tmp = (IUnknown*)mPingRTV;
    SafeRelease(tmp);
    mPingRTV = nullptr;

    tmp = (IUnknown*)mPingTex;
    SafeRelease(tmp);
    mPingTex = nullptr;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

    if (FAILED(d->CreateTexture2D(&td, nullptr, &mPingTex)) || !mPingTex)
        return;

    if (FAILED(d->CreateRenderTargetView(mPingTex, nullptr, &mPingRTV)) || !mPingRTV)
        return;

    if (FAILED(d->CreateShaderResourceView(mPingTex, nullptr, &mPingSRV)) || !mPingSRV)
        return;

    mW = w;
    mH = h;
}

void PostProcessD3D11::EnsureBloomTargets(RenderDeviceD3D11& device)
{
    ID3D11Device* d = device.Device();
    if (!d)
        return;

    const uint32_t w = device.BackBufferWidth();
    const uint32_t h = device.BackBufferHeight();
    if (w == 0 || h == 0)
        return;

    const uint32_t bw = (w > 1) ? (w / 2) : 1;
    const uint32_t bh = (h > 1) ? (h / 2) : 1;
    if (mBloomATex && mBloomARTV && mBloomASRV && mBloomBTex && mBloomBRTV && mBloomBSRV && mBloomW == bw && mBloomH == bh)
        return;

    IUnknown* tmp = nullptr;

    tmp = (IUnknown*)mBloomASRV;
    SafeRelease(tmp);
    mBloomASRV = nullptr;

    tmp = (IUnknown*)mBloomARTV;
    SafeRelease(tmp);
    mBloomARTV = nullptr;

    tmp = (IUnknown*)mBloomATex;
    SafeRelease(tmp);
    mBloomATex = nullptr;

    tmp = (IUnknown*)mBloomBSRV;
    SafeRelease(tmp);
    mBloomBSRV = nullptr;

    tmp = (IUnknown*)mBloomBRTV;
    SafeRelease(tmp);
    mBloomBRTV = nullptr;

    tmp = (IUnknown*)mBloomBTex;
    SafeRelease(tmp);
    mBloomBTex = nullptr;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = bw;
    td.Height = bh;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

    if (FAILED(d->CreateTexture2D(&td, nullptr, &mBloomATex)) || !mBloomATex)
        return;
    if (FAILED(d->CreateRenderTargetView(mBloomATex, nullptr, &mBloomARTV)) || !mBloomARTV)
        return;
    if (FAILED(d->CreateShaderResourceView(mBloomATex, nullptr, &mBloomASRV)) || !mBloomASRV)
        return;

    if (FAILED(d->CreateTexture2D(&td, nullptr, &mBloomBTex)) || !mBloomBTex)
        return;
    if (FAILED(d->CreateRenderTargetView(mBloomBTex, nullptr, &mBloomBRTV)) || !mBloomBRTV)
        return;
    if (FAILED(d->CreateShaderResourceView(mBloomBTex, nullptr, &mBloomBSRV)) || !mBloomBSRV)
        return;

    mBloomW = bw;
    mBloomH = bh;
}

void PostProcessD3D11::Execute(
    ID3D11DeviceContext* ctx,
    RenderDeviceD3D11& device,
    king::ShaderCache& cache,
    ID3D11ShaderResourceView* hdrSrv,
    ID3D11ShaderResourceView* aoSrv,
    ID3D11Buffer* cameraCB,
    ID3D11SamplerState* linearClamp,
    const Settings& settings)
{
    if (!ctx || !hdrSrv)
        return;

    std::string err;

    // Always update the post constant buffer each frame so toggles take effect immediately.
    // This also prevents stale bloom intensity from sampling a null bloom SRV.
    const uint32_t w = device.BackBufferWidth();
    const uint32_t h = device.BackBufferHeight();
    const uint32_t bw = (w > 1) ? (w / 2) : 1;
    const uint32_t bh = (h > 1) ? (h / 2) : 1;
    if (mPostCB)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(mPostCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            PostCBData cb{};
            cb.vignetteStrength = settings.vignetteStrength;
            cb.vignettePower = settings.vignettePower;
            cb.bloomIntensity = (settings.enableBloom ? settings.bloomIntensity : 0.0f);
            cb.bloomThreshold = settings.bloomThreshold;
            cb.invBloomSize[0] = (bw > 0) ? (1.0f / (float)bw) : 0.0f;
            cb.invBloomSize[1] = (bh > 0) ? (1.0f / (float)bh) : 0.0f;
            cb.invPostSize[0] = (w > 0) ? (1.0f / (float)w) : 0.0f;
            cb.invPostSize[1] = (h > 0) ? (1.0f / (float)h) : 0.0f;
            std::memcpy(mapped.pData, &cb, sizeof(cb));
            ctx->Unmap(mPostCB, 0);
        }
    }

    ID3D11ShaderResourceView* currentSrv = hdrSrv;

    ID3D11ShaderResourceView* bloomSrv = nullptr;

    // Bloom: extract + separable blur at half res.
    if (settings.enableBloom && settings.bloomIntensity > 1e-4f)
    {
        EnsureBloomTargets(device);
        if (mBloomARTV && mBloomASRV && mBloomBRTV && mBloomBSRV)
        {
            ID3D11PixelShader* psExtract = mFullscreen.GetOrCreatePS(device, cache, mShaderPath, "PSBloomExtractMain", {}, &err);
            ID3D11PixelShader* psBlurH = mFullscreen.GetOrCreatePS(device, cache, mShaderPath, "PSBloomBlurHMain", {}, &err);
            ID3D11PixelShader* psBlurV = mFullscreen.GetOrCreatePS(device, cache, mShaderPath, "PSBloomBlurVMain", {}, &err);

            if (psExtract && psBlurH && psBlurV && mPostCB)
            {
                D3D11_VIEWPORT vp{};
                vp.TopLeftX = 0;
                vp.TopLeftY = 0;
                vp.Width = (float)mBloomW;
                vp.Height = (float)mBloomH;
                vp.MinDepth = 0.0f;
                vp.MaxDepth = 1.0f;

                const float clear[4] = { 0, 0, 0, 0 };

                // Extract (HDR -> bloomA)
                ctx->ClearRenderTargetView(mBloomARTV, clear);
                mFullscreen.Begin(ctx, mBloomARTV, vp);
                ctx->PSSetShader(psExtract, nullptr, 0);
                ctx->PSSetConstantBuffers(6, 1, &mPostCB);
                ctx->PSSetShaderResources(1, 1, &currentSrv);
                if (linearClamp)
                    ctx->PSSetSamplers(1, 1, &linearClamp);
                mFullscreen.Draw(ctx);
                {
                    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
                    ctx->PSSetShaderResources(1, 1, nullSrv);
                }

                // Blur H (A -> B)
                ctx->ClearRenderTargetView(mBloomBRTV, clear);
                mFullscreen.Begin(ctx, mBloomBRTV, vp);
                ctx->PSSetShader(psBlurH, nullptr, 0);
                ctx->PSSetConstantBuffers(6, 1, &mPostCB);
                ctx->PSSetShaderResources(1, 1, &mBloomASRV);
                if (linearClamp)
                    ctx->PSSetSamplers(1, 1, &linearClamp);
                mFullscreen.Draw(ctx);
                {
                    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
                    ctx->PSSetShaderResources(1, 1, nullSrv);
                }

                // Blur V (B -> A)
                ctx->ClearRenderTargetView(mBloomARTV, clear);
                mFullscreen.Begin(ctx, mBloomARTV, vp);
                ctx->PSSetShader(psBlurV, nullptr, 0);
                ctx->PSSetConstantBuffers(6, 1, &mPostCB);
                ctx->PSSetShaderResources(1, 1, &mBloomBSRV);
                if (linearClamp)
                    ctx->PSSetSamplers(1, 1, &linearClamp);
                mFullscreen.Draw(ctx);
                {
                    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
                    ctx->PSSetShaderResources(1, 1, nullSrv);
                }

                bloomSrv = mBloomASRV;
            }
        }
    }

    // Optional vignette in HDR space (writes into an intermediate HDR RT).
    if (settings.enableVignette)
    {
        EnsureIntermediate(device);
        if (mPingRTV && mPingSRV && mPostCB)
        {
            ID3D11PixelShader* ps = mFullscreen.GetOrCreatePS(device, cache, mShaderPath, "PSVignetteMain", {}, &err);
            if (ps)
            {
                const float clear[4] = { 0, 0, 0, 0 };
                ctx->ClearRenderTargetView(mPingRTV, clear);

                const D3D11_VIEWPORT vp = device.Viewport();
                mFullscreen.Begin(ctx, mPingRTV, vp);
                ctx->PSSetShader(ps, nullptr, 0);

                ctx->PSSetConstantBuffers(6, 1, &mPostCB);
                ctx->PSSetShaderResources(1, 1, &currentSrv);
                if (linearClamp)
                    ctx->PSSetSamplers(1, 1, &linearClamp);

                mFullscreen.Draw(ctx);

                ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
                ctx->PSSetShaderResources(1, 1, nullSrv);

                currentSrv = mPingSRV;
            }
        }
    }

    // Final: tonemap to backbuffer.
    ID3D11RenderTargetView* outRtv = device.RTV();
    if (!outRtv)
        return;

    ID3D11PixelShader* psTonemap = mFullscreen.GetOrCreatePS(device, cache, mShaderPath, "PSTonemapMain", {}, &err);
    if (!psTonemap)
        return;

    const D3D11_VIEWPORT vp = device.Viewport();
    mFullscreen.Begin(ctx, outRtv, vp);

    ctx->PSSetShader(psTonemap, nullptr, 0);

    if (cameraCB)
        ctx->PSSetConstantBuffers(0, 1, &cameraCB);

    ctx->PSSetShaderResources(1, 1, &currentSrv);
    if (aoSrv)
        ctx->PSSetShaderResources(2, 1, &aoSrv);

    if (bloomSrv)
        ctx->PSSetShaderResources(3, 1, &bloomSrv);
    else
    {
        ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
        ctx->PSSetShaderResources(3, 1, nullSrv);
    }

    if (mPostCB)
        ctx->PSSetConstantBuffers(6, 1, &mPostCB);

    if (linearClamp)
        ctx->PSSetSamplers(1, 1, &linearClamp);

    mFullscreen.Draw(ctx);

    // Unbind SRVs so HDR targets can be rebound as RTVs next frame.
    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
    ctx->PSSetShaderResources(1, 1, nullSrv);
    ctx->PSSetShaderResources(2, 1, nullSrv);
    ctx->PSSetShaderResources(3, 1, nullSrv);
}

} // namespace king::render::d3d11
