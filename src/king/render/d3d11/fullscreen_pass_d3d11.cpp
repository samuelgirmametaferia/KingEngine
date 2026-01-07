#include "fullscreen_pass_d3d11.h"

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

FullscreenPassCacheD3D11::~FullscreenPassCacheD3D11()
{
    Shutdown();
}

bool FullscreenPassCacheD3D11::Initialize(RenderDeviceD3D11& device, king::ShaderCache& cache, const std::wstring& hlslPath, const char* vsEntry)
{
    Shutdown();

    ID3D11Device* d = device.Device();
    if (!d)
        return false;

    king::CompiledShader vs;
    std::string err;
    if (!cache.CompileVSFromFile(hlslPath.c_str(), vsEntry, {}, vs, &err))
    {
        std::printf("FullscreenPassCacheD3D11: VS compile failed (%s):\n%s\n", vsEntry ? vsEntry : "<null>", err.c_str());
        return false;
    }

    HRESULT hr = d->CreateVertexShader(vs.bytecode->GetBufferPointer(), vs.bytecode->GetBufferSize(), nullptr, &mVS);
    if (FAILED(hr) || !mVS)
        return false;

    // Rasterizer: no culling (fullscreen triangle), depth clip on.
    {
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.FrontCounterClockwise = TRUE;
        rd.DepthClipEnable = TRUE;
        (void)d->CreateRasterizerState(&rd, &mRS);
    }

    // Depth: disabled
    {
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = FALSE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dd.StencilEnable = FALSE;
        (void)d->CreateDepthStencilState(&dd, &mDSSNoDepth);
    }

    // Blend: opaque
    {
        D3D11_BLEND_DESC bd{};
        bd.AlphaToCoverageEnable = FALSE;
        bd.IndependentBlendEnable = FALSE;
        bd.RenderTarget[0].BlendEnable = FALSE;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        (void)d->CreateBlendState(&bd, &mBlendOpaque);
    }

    return true;
}

void FullscreenPassCacheD3D11::Shutdown()
{
    IUnknown* tmp = nullptr;

    for (auto& kv : mPs)
    {
        tmp = (IUnknown*)kv.second;
        SafeRelease(tmp);
        kv.second = nullptr;
    }
    mPs.clear();

    tmp = (IUnknown*)mBlendOpaque;
    SafeRelease(tmp);
    mBlendOpaque = nullptr;

    tmp = (IUnknown*)mDSSNoDepth;
    SafeRelease(tmp);
    mDSSNoDepth = nullptr;

    tmp = (IUnknown*)mRS;
    SafeRelease(tmp);
    mRS = nullptr;

    tmp = (IUnknown*)mVS;
    SafeRelease(tmp);
    mVS = nullptr;
}

ID3D11PixelShader* FullscreenPassCacheD3D11::GetOrCreatePS(
    RenderDeviceD3D11& device,
    king::ShaderCache& cache,
    const std::wstring& hlslPath,
    const char* psEntry,
    const std::vector<king::ShaderDefine>& defines,
    std::string* outError)
{
    ID3D11Device* d = device.Device();
    if (!d)
    {
        if (outError) *outError = "FullscreenPassCacheD3D11: device is null.";
        return nullptr;
    }

    PsKey key{};
    key.path = hlslPath;
    key.entry = psEntry ? psEntry : "";
    key.defineHash = MakeDefineHash(defines);

    auto it = mPs.find(key);
    if (it != mPs.end())
        return it->second;

    king::CompiledShader ps;
    if (!cache.CompilePSFromFile(hlslPath.c_str(), psEntry, defines, ps, outError))
        return nullptr;

    ID3D11PixelShader* shader = nullptr;
    HRESULT hr = d->CreatePixelShader(ps.bytecode->GetBufferPointer(), ps.bytecode->GetBufferSize(), nullptr, &shader);
    if (FAILED(hr) || !shader)
    {
        if (outError) *outError = "CreatePixelShader failed.";
        return nullptr;
    }

    mPs.emplace(std::move(key), shader);
    return shader;
}

void FullscreenPassCacheD3D11::Begin(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp)
{
    if (!ctx)
        return;

    const float blendFactor[4] = { 0, 0, 0, 0 };

    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    if (mDSSNoDepth)
        ctx->OMSetDepthStencilState(mDSSNoDepth, 0);
    ctx->OMSetBlendState(mBlendOpaque, blendFactor, 0xFFFFFFFFu);

    if (mRS)
        ctx->RSSetState(mRS);
    ctx->RSSetViewports(1, &vp);

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ctx->VSSetShader(mVS, nullptr, 0);
}

void FullscreenPassCacheD3D11::Draw(ID3D11DeviceContext* ctx)
{
    if (!ctx)
        return;
    ctx->Draw(3, 0);
}

} // namespace king::render::d3d11
