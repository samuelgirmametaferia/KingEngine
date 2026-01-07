#pragma once

#include "render_device_d3d11.h"
#include "../../render/shader.h"

#include <d3d11.h>

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace king::render::d3d11
{

class FullscreenPassCacheD3D11
{
public:
    FullscreenPassCacheD3D11() = default;
    ~FullscreenPassCacheD3D11();

    FullscreenPassCacheD3D11(const FullscreenPassCacheD3D11&) = delete;
    FullscreenPassCacheD3D11& operator=(const FullscreenPassCacheD3D11&) = delete;

    bool Initialize(RenderDeviceD3D11& device, king::ShaderCache& cache, const std::wstring& hlslPath, const char* vsEntry);
    void Shutdown();

    ID3D11VertexShader* VS() const { return mVS; }

    ID3D11PixelShader* GetOrCreatePS(
        RenderDeviceD3D11& device,
        king::ShaderCache& cache,
        const std::wstring& hlslPath,
        const char* psEntry,
        const std::vector<king::ShaderDefine>& defines,
        std::string* outError);

    // Convenience: set up common full-screen state (RTV, viewport, no depth).
    void Begin(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp);
    void Draw(ID3D11DeviceContext* ctx);

private:
    struct PsKey
    {
        std::wstring path;
        std::string entry;
        std::string defineHash;

        bool operator==(const PsKey& o) const
        {
            return path == o.path && entry == o.entry && defineHash == o.defineHash;
        }
    };

    struct PsKeyHash
    {
        size_t operator()(const PsKey& k) const
        {
            std::hash<std::wstring> hw;
            std::hash<std::string> hs;
            size_t h = hw(k.path);
            h ^= (hs(k.entry) + 0x9e3779b9 + (h << 6) + (h >> 2));
            h ^= (hs(k.defineHash) + 0x9e3779b9 + (h << 6) + (h >> 2));
            return h;
        }
    };

    static std::string MakeDefineHash(const std::vector<king::ShaderDefine>& defines)
    {
        std::vector<king::ShaderDefine> sorted = defines;
        std::sort(sorted.begin(), sorted.end(), [](const king::ShaderDefine& a, const king::ShaderDefine& b)
        {
            return a.name < b.name;
        });

        std::ostringstream oss;
        for (const auto& d : sorted)
            oss << d.name << '=' << d.value << ';';
        return oss.str();
    }

private:
    ID3D11VertexShader* mVS = nullptr;

    ID3D11RasterizerState* mRS = nullptr;
    ID3D11DepthStencilState* mDSSNoDepth = nullptr;
    ID3D11BlendState* mBlendOpaque = nullptr;

    std::unordered_map<PsKey, ID3D11PixelShader*, PsKeyHash> mPs;
};

} // namespace king::render::d3d11
