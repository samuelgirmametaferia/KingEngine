#pragma once

#include "../../ecs/scene.h"
#include "../../scene/frustum.h"
#include "../../render/material.h"
#include "render_device_d3d11.h"

#include <d3d11.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace king::render::d3d11
{

class RenderSystemD3D11
{
public:
    RenderSystemD3D11() = default;
    ~RenderSystemD3D11();

    RenderSystemD3D11(const RenderSystemD3D11&) = delete;
    RenderSystemD3D11& operator=(const RenderSystemD3D11&) = delete;

    bool Initialize(RenderDeviceD3D11& device, const std::wstring& shaderPath);
    void Shutdown();

    // Call after a device reset/recreate.
    bool OnDeviceReset(RenderDeviceD3D11& device);

    void RenderGeometryPass(
        RenderDeviceD3D11& device,
        Scene& scene,
        const Frustum& frustum,
        const Mat4x4& viewProj,
        const Float3& cameraPos,
        float exposure = 1.0f);

    // Releases any per-mesh GPU buffers stored in the scene meshes.
    static void ReleaseSceneMeshBuffers(Scene& scene);

private:
    struct CameraCBData
    {
        Mat4x4 viewProj;
        float cameraPos[3];
        float exposure;
    };

    static constexpr uint32_t kMaxLights = 16;

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

        Mat4x4 lightViewProj;
        float shadowTexelSize[2];
        float shadowBias;
        float shadowStrength;
    };

    struct InstanceData
    {
        Mat4x4 world;
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
        uint32_t lightMask = 0xFFFFFFFFu;
        uint32_t flags = 0;

        Float3 boundsCenter{ 0, 0, 0 };
        float boundsRadius = 0.0f;
    };

    struct Batch
    {
        Mesh* mesh = nullptr;
        uint32_t startInstance = 0;
        uint32_t instanceCount = 0;
    };

    struct PreparedFrame
    {
        std::vector<InstanceData> instances;
        std::vector<Batch> batches;
    };

    void UpdateCameraCB(ID3D11DeviceContext* ctx, const Mat4x4& viewProj, const Float3& cameraPos, float exposure);
    void UpdateLightCB(ID3D11DeviceContext* ctx, const LightCBData& data);

    void EnsureShadowResources(RenderDeviceD3D11& device);
    void EnsureHdrTargets(RenderDeviceD3D11& device);

    static bool GetPrimaryDirectionalLightWithTransform(const Scene& scene, Light& outLight, Transform& outXform);
    static void GatherLights(const Scene& scene, std::vector<GpuLight>& outLights);

    void EnsureMeshBuffers(RenderDeviceD3D11& device, Mesh& mesh);
    void EnsureInstanceBuffer(RenderDeviceD3D11& device, size_t requiredInstanceCount);

    void EnsureDeferredContexts(RenderDeviceD3D11& device);
    void ReleaseDeferredContexts();

    void StartWorker();
    void StopWorker();

    static void BuildSnapshot(Scene& scene, std::vector<SnapshotItem>& outItems);
    void EnqueueBuild(std::vector<SnapshotItem>&& items, const Frustum& frustum);
    bool ConsumeReadyFrame(PreparedFrame& outFrame);
    static void BuildPreparedFrame(const std::vector<SnapshotItem>& items, const Frustum& frustum, PreparedFrame& outFrame);

private:
    std::wstring mShaderPath;

    ID3D11VertexShader* mVS = nullptr;
    ID3D11PixelShader* mPS = nullptr;
    ID3D11InputLayout* mInputLayout = nullptr;

    ID3D11VertexShader* mVSShadow = nullptr;

    ID3D11VertexShader* mVSTonemap = nullptr;
    ID3D11PixelShader* mPSTonemap = nullptr;

    ID3D11Buffer* mCameraCB = nullptr;
    ID3D11Buffer* mLightCB = nullptr;

    ID3D11SamplerState* mLinearClamp = nullptr;
    ID3D11SamplerState* mShadowSampler = nullptr;

    // Shadow map (directional)
    ID3D11Texture2D* mShadowTex = nullptr;
    ID3D11DepthStencilView* mShadowDSV = nullptr;
    ID3D11ShaderResourceView* mShadowSRV = nullptr;
    D3D11_VIEWPORT mShadowViewport{};
    ID3D11RasterizerState* mShadowRS = nullptr;

    // HDR render target
    ID3D11Texture2D* mHdrTex = nullptr;
    ID3D11RenderTargetView* mHdrRTV = nullptr;
    ID3D11ShaderResourceView* mHdrSRV = nullptr;
    uint32_t mHdrW = 0;
    uint32_t mHdrH = 0;

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
};

} // namespace king::render::d3d11
