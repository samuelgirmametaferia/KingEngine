#pragma once

#include "../../ecs/scene.h"
#include "../../scene/frustum.h"
#include "../../render/material.h"
#include "render_device_d3d11.h"
#include "shadows.h"
#include "shader_program_d3d11.h"
#include "texture_manager_d3d11.h"
#include "post_process_d3d11.h"

#include "../../perf/perf_analyzer.h"
#include "../../perf/gpu_profiler_d3d11.h"

#include <d3d11.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <unordered_map>

namespace king::render::d3d11
{

class RenderSystemD3D11
{
public:
    struct RenderSettings
    {
        // Passes/features
        bool enableHdr = true;
        bool enableTonemap = true;
        bool enableShadows = true;
        bool enablePointShadows = true;
        bool enableSsao = false;

        // Optional: render a depth-only prepass before the main geometry pass.
        // This can improve overdraw and is a prerequisite for certain effects.
        bool enableDepthPrepass = false;

        // Exposure (used by tonemap). If you also pass exposure as an argument,
        // the argument wins.
        float exposure = 1.0f;

        // Post (final output)
        // If > 0, tonemap pass multiplies HDR by AO (SSAO or white fallback).
        // Set to 0 to disable any AO darkening.
        float aoStrength = 0.0f;

        // Post effects (run before tonemap)
        bool enableVignette = false;
        float vignetteStrength = 0.18f;
        float vignettePower = 2.2f;

        bool enableBloom = false;
        float bloomIntensity = 0.65f;
        float bloomThreshold = 1.10f;

        // Point-light shadows (single shadow-casting point light supported for now)
        uint32_t pointShadowMapSize = 1024;
        float pointShadowBias = 0.05f;       // world units
        float pointShadowStrength = 1.0f;    // 0..1

        // Shadows
        float shadowBias = 0.00125f;
        float shadowStrength = 1.0f;
        // Minimum visibility in full shadow (0=black, 1=no shadow darkening).
        float shadowMinVisibility = 0.20f;
        uint32_t shadowMapSize = 1024; // e.g. 512/1024/2048
        uint32_t cascadeCount = 3; // 1..3
        float cascadeLambda = 0.55f;

        // If > 0, clamps the effective shadow distance to improve texel density.
        // (Shadows will stop covering geometry beyond this distance.)
        float shadowMaxDistance = 0.0f;

        // Distance-based shadow fade-out near the far plane.
        // Default off to avoid visible "fade line" in content that isn't tuned for it.
        bool enableShadowFadeOut = false;
        float shadowFadeStartNdc = 0.92f;
        float shadowFadeEndNdc = 0.99f;

        // Shadow edge softness control.
        // 0 = hard (fastest), 1 = default softness, >1 = softer (more blur / fewer zig-zags).
        float shadowSoftness = 1.0f;

        // Shadow sampling/bias options (disable to revert toward simpler behavior).
        bool enableShadowPoissonPcf = true;
        bool enableShadowNormalOffsetBias = true;
        bool enableShadowReceiverPlaneBias = true;

        // Shadow filter quality:
        // 0 = hard (single tap, still uses linear compare)
        // 1 = PCF 3x3 (good default; reduces zig-zags)
        // 2 = PCF 5x5 (softer, more expensive)
        // 3 = Poisson 9-tap (soft, stable; more expensive)
        // 4 = PCSS (variable penumbra; most expensive)
        uint32_t shadowFilterQuality = 1;

        // Cull tiny casters from the shadow pass based on approximate screen-space radius.
        // Default 0 (disabled) because it adds CPU work proportional to caster count.
        float shadowMinCasterPixels = 0.0f;

        // Shadow diagnostics (debug)
        // 0=off, 1=show shadow factor (grayscale)
        uint32_t shadowDebugView = 0;
        // If true, prints shadow-map min/max depth once.
        bool debugShadowReadbackOnce = false;

        // SSAO
        float ssaoRadius = 0.6f;
        float ssaoBias = 0.02f;
    };

    RenderSystemD3D11();
    ~RenderSystemD3D11();

    RenderSystemD3D11(const RenderSystemD3D11&) = delete;
    RenderSystemD3D11& operator=(const RenderSystemD3D11&) = delete;

    bool Initialize(RenderDeviceD3D11& device, const std::wstring& shaderPath);
    void Shutdown();

    // Call after a device reset/recreate.
    bool OnDeviceReset(RenderDeviceD3D11& device);

    // Optional: feeds FPS into the perf overlay.
    void SetFps(double fps) { mPerf.SetFps(fps); }

    // Perf output control (useful for automated stress tests).
    void SetPerfEnabled(bool enabled) { mPerf.SetEnabled(enabled); }
    void SetGpuPerfEnabled(bool enabled) { mGpuPerf.SetEnabled(enabled); }
    void SetPerfPrintToStdout(bool enabled) { mPerf.SetPrintToStdout(enabled); }
    void SetPerfPrintEveryNFrames(uint32_t n) { mPerf.SetPrintEveryNFrames(n); }
    const std::vector<king::perf::PerfAnalyzer::Sample>& PerfSamples() const { return mPerf.Samples(); }

    void RenderGeometryPass(
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
        float exposureOverride = -1.0f);

    // Back-compat overload (uses default settings).
    void RenderGeometryPass(
        RenderDeviceD3D11& device,
        Scene& scene,
        const Frustum& frustum,
        const Mat4x4& viewProj,
        const Mat4x4& view,
        const Mat4x4& proj,
        const Float3& cameraPos,
        float cameraNearZ,
        float cameraFarZ,
        float exposure = 1.0f);

    // Back-compat overloads: view/proj not provided (SSAO falls back to prior viewProj-based behavior).
    void RenderGeometryPass(
        RenderDeviceD3D11& device,
        Scene& scene,
        const Frustum& frustum,
        const Mat4x4& viewProj,
        const Float3& cameraPos,
        float cameraNearZ,
        float cameraFarZ,
        const RenderSettings& settings,
        float exposureOverride = -1.0f);

    void RenderGeometryPass(
        RenderDeviceD3D11& device,
        Scene& scene,
        const Frustum& frustum,
        const Mat4x4& viewProj,
        const Float3& cameraPos,
        float cameraNearZ,
        float cameraFarZ,
        float exposure = 1.0f);

    // Releases any per-mesh GPU buffers stored in the scene meshes.
    static void ReleaseSceneMeshBuffers(Scene& scene);

private:
    struct CameraCBData
    {
        Mat4x4 viewProj;
        float cameraPos[3];
        float exposure;

        float aoStrength;
        float _padPost[3];
    };

    struct SsaoCBData
    {
        Mat4x4 proj;
        Mat4x4 invProj;
        Mat4x4 view;
        float invTargetSize[2];
        float radius;
        float bias;
    };

    static constexpr uint32_t kMaxLights = 16;
    static constexpr uint32_t kMaxCascades = 3;

    struct GpuLight
    {
        // 16-byte register 0
        uint32_t type;
        uint32_t groupMask;
        uint32_t _padU0;
        uint32_t _padU1;

        // 16-byte register 1
        float color[3];
        float intensity;

        // 16-byte register 2
        float dir[3];
        float range;

        // 16-byte register 3
        float pos[3];
        float innerConeCos;

        // 16-byte register 4
        float outerConeCos;
        float _padF0[3];
    };

    struct LightCBData
    {
        uint32_t lightCount;
        uint32_t _pad0[3];
        GpuLight lights[kMaxLights];

        Mat4x4 lightViewProj[kMaxCascades];
        float cascadeSplitsNdc[4];
        uint32_t cascadeCount;
        uint32_t _pad1[3];

        float shadowTexelSize[2];
        float shadowBias;
        float shadowStrength;

        float shadowMinVisibility;
        float _padShadowMin[3];

        // x=fadeStartNdc, y=fadeEndNdc, z=shadowSoftness, w=shadowFlags (packed as float)
        float shadowExtras[4];

            // Point-shadow params:
            // x = enabled (0/1)
            // y = bias (world units)
            // z = invFar (1/range)
            // w = strength (0..1)
            float pointShadowParams[4];
            float pointShadowTexelSize[2];
            float _padPointShadow[2];
    };

    struct InstanceData
    {
        Mat4x4 world;
        // Inverse-transpose of world matrix; used for correct normal transforms under non-uniform scale.
        Mat4x4 normal;
        float albedo[4];
        float roughnessMetallic[2];
        uint32_t lightMask;
        uint32_t flags;
    };

    struct SnapshotItem
    {
        Mesh* mesh = nullptr;
        Transform transform{};
        Float4 albedo{ 1, 1, 1, 1 };
        float roughness = 0.5f;
        float metallic = 0.0f;
        PbrMaterial material{};
        uint32_t lightMask = 0xFFFFFFFFu;
        uint32_t flags = 0;

        Float3 boundsCenter{ 0, 0, 0 };
        float boundsRadius = 0.0f;
    };

    struct Batch
    {
        Mesh* mesh = nullptr;
        uint32_t materialIndex = 0;
        uint32_t startInstance = 0;
        uint32_t instanceCount = 0;
    };

    struct PreparedFrame
    {
        std::vector<InstanceData> instances;
        std::vector<Batch> batches;
        std::vector<PbrMaterial> materials;
        std::vector<uint64_t> materialKeys;
    };

    void UpdateCameraCB(ID3D11DeviceContext* ctx, const Mat4x4& viewProj, const Float3& cameraPos, float exposure, float aoStrength);
    void UpdateLightCB(ID3D11DeviceContext* ctx, const LightCBData& data);

    void EnsureHdrTargets(RenderDeviceD3D11& device, bool needSsao);
    void EnsureSsaoTargets(RenderDeviceD3D11& device);

    static bool GetPrimaryDirectionalLightWithTransform(const Scene& scene, Light& outLight, Transform& outXform);
    static void GatherLights(const Scene& scene, std::vector<GpuLight>& outLights);

    void EnsureMeshBuffers(RenderDeviceD3D11& device, Mesh& mesh);
    void EnsureInstanceBuffer(RenderDeviceD3D11& device, size_t requiredInstanceCount);

    void EnsureDeferredContexts(RenderDeviceD3D11& device);
    void ReleaseDeferredContexts();

    void StartWorker();
    void StopWorker();

    static void BuildSnapshot(Scene& scene, std::vector<SnapshotItem>& outItems);
    void EnqueueBuild(std::vector<SnapshotItem>& items, const Frustum& frustum);
    bool ConsumeReadyFrame(PreparedFrame& outFrame);
    static void BuildPreparedFrame(const std::vector<SnapshotItem>& items, const Frustum& frustum, PreparedFrame& outFrame);

private:
    std::wstring mShaderPath;
    std::unique_ptr<king::ShaderCache> mShaderCache;
    std::wstring mShaderDir;

    TextureManagerD3D11 mTextures;

    struct MaterialGpu
    {
        ShaderProgramD3D11* program = nullptr;
        ID3D11Buffer* materialCB = nullptr;
        ID3D11ShaderResourceView* albedoSRV = nullptr;
        ID3D11ShaderResourceView* normalSRV = nullptr;
        ID3D11ShaderResourceView* mrSRV = nullptr;
        ID3D11ShaderResourceView* emissiveSRV = nullptr;

        bool alphaBlend = false;
    };

    std::unordered_map<std::wstring, std::unique_ptr<ShaderProgramD3D11>> mProgramCache;
    std::unordered_map<uint64_t, MaterialGpu> mMaterialCache;

    std::unique_ptr<ShadowsD3D11> mShadows;

    // Dev perf analyzer (CPU + optional GPU query timings).
    king::perf::PerfAnalyzer mPerf;
    king::perf::GpuProfilerD3D11 mGpuPerf;

    // Scratch buffers reused per-frame (avoid alloc churn for snapshot/shadow building).
    std::vector<SnapshotItem> mSnapshotScratch;
    std::vector<const SnapshotItem*> mShadowCasterPtrs[3];
    std::vector<InstanceData> mShadowInstancesScratch;
    std::vector<ShadowsD3D11::DrawBatch> mShadowDrawBatchesPerCascade[3];

        // Point shadow scratch (single cubemap)
        std::vector<const SnapshotItem*> mPointShadowCasterPtrs;
        std::vector<InstanceData> mPointShadowInstancesScratch;
        struct PointShadowDrawBatch
        {
            ID3D11Buffer* vb = nullptr;
            ID3D11Buffer* ib = nullptr;
            uint32_t indexCount = 0;
            uint32_t vertexCount = 0;
            uint32_t startInstance = 0;
            uint32_t instanceCount = 0;
            Mesh* mesh = nullptr;
        };
        std::vector<PointShadowDrawBatch> mPointShadowDrawBatches;

        ID3D11VertexShader* mVSPointShadow = nullptr;
        ID3D11PixelShader* mPSPointShadow = nullptr;
        ID3D11Buffer* mPointShadowCB = nullptr;

        ID3D11Texture2D* mPointShadowTex = nullptr; // R32_FLOAT cubemap (stored linear depth normalized)
        ID3D11ShaderResourceView* mPointShadowSRV = nullptr;
        ID3D11RenderTargetView* mPointShadowRTV[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };

        ID3D11Texture2D* mPointShadowDepthTex = nullptr; // D32 cubemap for depth testing
        ID3D11DepthStencilView* mPointShadowDSV[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };

        uint32_t mPointShadowSize = 0;

    ID3D11VertexShader* mVS = nullptr;
    ID3D11VertexShader* mVSDepth = nullptr;
    ID3D11PixelShader* mPS = nullptr;
    ID3D11PixelShader* mPSMrt = nullptr;
    ID3D11InputLayout* mInputLayout = nullptr;

    PostProcessD3D11 mPost;

    ID3D11Buffer* mCameraCB = nullptr;
    ID3D11Buffer* mLightCB = nullptr;
    ID3D11Buffer* mSsaoCB = nullptr;

    ID3D11SamplerState* mLinearClamp = nullptr;
    ID3D11SamplerState* mPointClamp = nullptr;

    // Derived render state (driven by material intent).
    ID3D11BlendState* mBlendOpaque = nullptr;
    ID3D11BlendState* mBlendAlpha = nullptr;
    ID3D11DepthStencilState* mDepthReadOnly = nullptr;

    // HDR render target
    ID3D11Texture2D* mHdrTex = nullptr;
    ID3D11RenderTargetView* mHdrRTV = nullptr;
    ID3D11ShaderResourceView* mHdrSRV = nullptr;
    uint32_t mHdrW = 0;
    uint32_t mHdrH = 0;

    // Normal buffer (for SSAO)
    ID3D11Texture2D* mNormalTex = nullptr;
    ID3D11RenderTargetView* mNormalRTV = nullptr;
    ID3D11ShaderResourceView* mNormalSRV = nullptr;

    // Depth buffer with SRV (for SSAO)
    ID3D11Texture2D* mDepthTex = nullptr;
    ID3D11DepthStencilView* mDepthDSV = nullptr;
    ID3D11ShaderResourceView* mDepthSRV = nullptr;

    // SSAO + blur
    ID3D11Texture2D* mSsaoTex = nullptr;
    ID3D11RenderTargetView* mSsaoRTV = nullptr;
    ID3D11ShaderResourceView* mSsaoSRV = nullptr;

    ID3D11Texture2D* mSsaoBlurTex = nullptr;
    ID3D11RenderTargetView* mSsaoBlurRTV = nullptr;
    ID3D11ShaderResourceView* mSsaoBlurSRV = nullptr;

    // Fallback AO texture (1x1 white) bound when SSAO is disabled.
    ID3D11Texture2D* mAoWhiteTex = nullptr;
    ID3D11ShaderResourceView* mAoWhiteSRV = nullptr;

    ID3D11Buffer* mInstanceVB = nullptr;
    size_t mInstanceCapacity = 0;

    // Deferred contexts for parallel draw recording.
    std::vector<ID3D11DeviceContext*> mDeferredContexts;

    // CPU prep worker (builds batches/instances off-thread).
    std::thread mWorker;
    std::mutex mWorkMutex;
    std::condition_variable mWorkCv;
    bool mWorkerExit = false;
    bool mPendingValid = false;
    bool mReadyValid = false;
    Frustum mPendingFrustum{};
    std::vector<SnapshotItem> mPendingItems;
    PreparedFrame mReadyFrame;
    PreparedFrame mRenderFrame;

    bool mUsePrepareWorker = true;
    bool mAllowDeferredContexts = false;
    bool mAllowPostProcessing = true;
};

} // namespace king::render::d3d11
