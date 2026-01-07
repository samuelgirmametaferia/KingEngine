#pragma once

#include "render_device_d3d11.h"

#include "../../math/types.h"

#include <d3d11.h>

#include <cstdint>
#include <string>
#include <vector>

namespace king
{
class ShaderCache;
struct Light;
}

namespace king::render::d3d11
{

class ShadowsD3D11
{
public:
    struct Settings
    {
        bool enable = true;
        float bias = 0.00125f;
        float strength = 1.0f;
        uint32_t mapSize = 1024;
        uint32_t cascadeCount = 3; // 1..3
        float cascadeLambda = 0.55f;

        // 0=off, 1=show shadow factor (grayscale)
        uint32_t debugView = 0;
        bool debugReadbackOnce = false;
    };

    struct DrawBatch
    {
        ID3D11Buffer* vb = nullptr;
        ID3D11Buffer* ib = nullptr;
        uint32_t indexCount = 0;
        uint32_t vertexCount = 0;
        uint32_t startInstance = 0;
        uint32_t instanceCount = 0;
    };

    static constexpr uint32_t kMaxCascades = 3;

    ShadowsD3D11() = default;
    ~ShadowsD3D11();

    ShadowsD3D11(const ShadowsD3D11&) = delete;
    ShadowsD3D11& operator=(const ShadowsD3D11&) = delete;

    bool Initialize(RenderDeviceD3D11& device, ShaderCache& shaderCache, const std::wstring& shaderPath);
    void Shutdown();

    void EnsureResources(RenderDeviceD3D11& device, uint32_t cascadeCount, uint32_t shadowMapSize);

    // Computes cascade view-projection matrices and split depths.
    // Returns false if shadows should be disabled for this frame.
    bool ComputeCascades(
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
        float& outShadowStrength);

    void Render(
        RenderDeviceD3D11& device,
        const std::vector<DrawBatch> batchesPerCascade[kMaxCascades],
        ID3D11InputLayout* inputLayout,
        ID3D11Buffer* instanceVB,
        uint32_t instanceStride,
        const Mat4x4 cascadeViewProj[kMaxCascades],
        uint32_t cascadeCount,
        bool debugReadbackOnce);

    ID3D11ShaderResourceView* ShadowSRV() const { return mShadowSRV; }
    // Point comparison sampler: required when doing manual multi-tap PCF in shader.
    ID3D11SamplerState* ShadowSamplerPoint() const { return mShadowSamplerPoint; }
    // Linear comparison sampler: useful for the non-Poisson fallback (hardware 2x2 PCF).
    ID3D11SamplerState* ShadowSamplerLinear() const { return mShadowSamplerLinear; }
    // Non-comparison sampler: required for PCSS (sampling raw shadow-map depth values).
    ID3D11SamplerState* ShadowSamplerNonCmp() const { return mShadowSamplerNonCmp; }

private:
    void EnsureReadbackTexture(RenderDeviceD3D11& device);
    void EnsureDeferredContexts(RenderDeviceD3D11& device, uint32_t desired);
    void ReleaseDeferredContexts();

private:
    uint32_t mShadowMapSize = 0;
    uint32_t mShadowCascadeCount = 0;

    ID3D11VertexShader* mVSShadow = nullptr;

    // Per-cascade constant buffers (avoid cross-thread Map races).
    ID3D11Buffer* mShadowCB[kMaxCascades]{};

    ID3D11SamplerState* mShadowSamplerPoint = nullptr;
    ID3D11SamplerState* mShadowSamplerLinear = nullptr;
    ID3D11SamplerState* mShadowSamplerNonCmp = nullptr;

    // Shadow map (directional CSM)
    ID3D11Texture2D* mShadowTex = nullptr;
    ID3D11DepthStencilView* mShadowDSV[kMaxCascades]{};
    ID3D11ShaderResourceView* mShadowSRV = nullptr;
    D3D11_VIEWPORT mShadowViewport{};
    ID3D11RasterizerState* mShadowRS = nullptr;

    // Shadow diagnostics
    ID3D11Texture2D* mShadowReadbackTex = nullptr;

    // Deferred contexts for parallel shadow recording (one per cascade).
    std::vector<ID3D11DeviceContext*> mDeferredContexts;

    // Cached init-time setting (avoid per-frame thread_config lookups).
    uint32_t mShadowRecordThreads = 0;
};

} // namespace king::render::d3d11
