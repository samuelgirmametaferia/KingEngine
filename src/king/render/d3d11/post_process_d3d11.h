#pragma once

#include "fullscreen_pass_d3d11.h"

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace king
{
class ShaderCache;
}

namespace king::render::d3d11
{

class PostProcessD3D11
{
public:
    struct Settings
    {
        bool enableVignette = false;
        float vignetteStrength = 0.18f;
        float vignettePower = 2.2f;

        bool enableBloom = false;
        float bloomIntensity = 0.65f;
        float bloomThreshold = 1.10f;
    };

    PostProcessD3D11() = default;
    ~PostProcessD3D11();

    PostProcessD3D11(const PostProcessD3D11&) = delete;
    PostProcessD3D11& operator=(const PostProcessD3D11&) = delete;

    bool Initialize(RenderDeviceD3D11& device, king::ShaderCache& cache, const std::wstring& shaderPath);
    void Shutdown();

    // Runs post chain (currently optional vignette in HDR) and then tonemaps to the backbuffer.
    // - hdrSrv must be bound to t1 for the fullscreen shaders.
    // - aoSrv must be bound to t2 for tonemap (can be a 1x1 white fallback).
    void Execute(
        ID3D11DeviceContext* ctx,
        RenderDeviceD3D11& device,
        king::ShaderCache& cache,
        ID3D11ShaderResourceView* hdrSrv,
        ID3D11ShaderResourceView* aoSrv,
        ID3D11Buffer* cameraCB,
        ID3D11SamplerState* linearClamp,
        const Settings& settings);

    FullscreenPassCacheD3D11& Fullscreen() { return mFullscreen; }

private:
    struct PostCBData
    {
        float vignetteStrength;
        float vignettePower;
        float bloomIntensity;
        float bloomThreshold;

        float invBloomSize[2];
        float invPostSize[2];
        float _pad[4];
    };

    static_assert(sizeof(PostCBData) % 16 == 0, "PostCBData must be 16-byte aligned");

    void EnsureIntermediate(RenderDeviceD3D11& device);
    void EnsureBloomTargets(RenderDeviceD3D11& device);

private:
    std::wstring mShaderPath;

    FullscreenPassCacheD3D11 mFullscreen;

    ID3D11Buffer* mPostCB = nullptr;

    ID3D11Texture2D* mPingTex = nullptr;
    ID3D11RenderTargetView* mPingRTV = nullptr;
    ID3D11ShaderResourceView* mPingSRV = nullptr;

    ID3D11Texture2D* mBloomATex = nullptr;
    ID3D11RenderTargetView* mBloomARTV = nullptr;
    ID3D11ShaderResourceView* mBloomASRV = nullptr;

    ID3D11Texture2D* mBloomBTex = nullptr;
    ID3D11RenderTargetView* mBloomBRTV = nullptr;
    ID3D11ShaderResourceView* mBloomBSRV = nullptr;

    uint32_t mBloomW = 0;
    uint32_t mBloomH = 0;

    uint32_t mW = 0;
    uint32_t mH = 0;
};

} // namespace king::render::d3d11
