#pragma once

#include <d3d11.h>
#include <cstdint>
#include <vector>

namespace king::perf
{

class GpuProfilerD3D11
{
public:
    struct Settings
    {
        bool enabled = true;
        // We read back a few frames behind to avoid stalls.
        // Using 3 reduces "partial-ready" results where early scopes are ready
        // but later ones (like Frame) are still executing on the GPU.
        uint32_t bufferedFrames = 3;
    };

    struct Scope
    {
        const char* name = nullptr;
        std::vector<ID3D11Query*> begin;
        std::vector<ID3D11Query*> end;
    };

    explicit GpuProfilerD3D11(Settings s = {}) : mSettings(s) {}
    ~GpuProfilerD3D11();

    GpuProfilerD3D11(const GpuProfilerD3D11&) = delete;
    GpuProfilerD3D11& operator=(const GpuProfilerD3D11&) = delete;

    void SetEnabled(bool enabled) { mSettings.enabled = enabled; }
    bool Enabled() const { return mSettings.enabled; }

    void Initialize(ID3D11Device* device);
    void Shutdown();

    void BeginFrame(ID3D11DeviceContext* ctx);
    void EndFrame(ID3D11DeviceContext* ctx);

    void BeginScope(ID3D11DeviceContext* ctx, const char* name);
    void EndScope(ID3D11DeviceContext* ctx, const char* name);

    // Tries to fetch results for the most recently completed frame.
    // Returns true if disjoint/timestamps were ready.
    bool TryGetResults(ID3D11DeviceContext* ctx, uint32_t& outFrameIndex, std::vector<std::pair<const char*, double>>& outGpuMs);

private:
    Scope* FindOrCreateScope(const char* name);
    ID3D11Query* CreateTimestampQuery();
    ID3D11Query* CreateDisjointQuery();

private:
    Settings mSettings{};
    ID3D11Device* mDevice = nullptr;

    std::vector<Scope> mScopes;

    std::vector<ID3D11Query*> mDisjoint;
    uint32_t mFrameIndex = 0;
};

} // namespace king::perf
