#pragma once

#include <d3d11.h>
#include <string>
#include <unordered_map>

namespace king::render::d3d11
{

class TextureManagerD3D11
{
public:
    TextureManagerD3D11() = default;
    ~TextureManagerD3D11();

    TextureManagerD3D11(const TextureManagerD3D11&) = delete;
    TextureManagerD3D11& operator=(const TextureManagerD3D11&) = delete;

    bool Initialize(ID3D11Device* device);
    void Shutdown();

    // Returns an SRV for the requested texture path. If loading fails, returns a fallback.
    ID3D11ShaderResourceView* GetOrLoad2D(const std::wstring& path, bool srgb);

    // Common fallbacks.
    ID3D11ShaderResourceView* White() const { return mWhiteSRV; }
    ID3D11ShaderResourceView* Black() const { return mBlackSRV; }

private:
    struct Key
    {
        std::wstring path;
        bool srgb = false;

        bool operator==(const Key& o) const { return path == o.path && srgb == o.srgb; }
    };

    struct KeyHash
    {
        size_t operator()(const Key& k) const;
    };

    bool CreateSolidColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a, bool srgb, ID3D11ShaderResourceView** outSrv);
    bool LoadWicRgba8(const std::wstring& path, std::vector<uint8_t>& outRgba, uint32_t& outW, uint32_t& outH);
    ID3D11ShaderResourceView* CreateTextureFromRgba8(const uint8_t* rgba, uint32_t w, uint32_t h, bool srgb);

private:
    ID3D11Device* mDevice = nullptr;

    // Fallback SRVs.
    ID3D11ShaderResourceView* mWhiteSRV = nullptr;
    ID3D11ShaderResourceView* mBlackSRV = nullptr;

    std::unordered_map<Key, ID3D11ShaderResourceView*, KeyHash> mCache;

    // WIC factory (created lazily on first decode).
    void* mWicFactory = nullptr;
};

} // namespace king::render::d3d11
