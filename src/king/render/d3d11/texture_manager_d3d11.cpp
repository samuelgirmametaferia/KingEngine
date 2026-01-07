#include "texture_manager_d3d11.h"

#include <wincodec.h>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

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

size_t TextureManagerD3D11::KeyHash::operator()(const Key& k) const
{
    std::hash<std::wstring> hw;
    size_t h = hw(k.path);
    h ^= (k.srgb ? 0x9e3779b97f4a7c15ull : 0ull) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
}

TextureManagerD3D11::~TextureManagerD3D11()
{
    Shutdown();
}

bool TextureManagerD3D11::Initialize(ID3D11Device* device)
{
    Shutdown();
    mDevice = device;
    if (mDevice)
        mDevice->AddRef();

    // Create common fallbacks.
    if (!CreateSolidColor(255, 255, 255, 255, true, &mWhiteSRV))
        return false;
    if (!CreateSolidColor(0, 0, 0, 255, true, &mBlackSRV))
        return false;

    return true;
}

void TextureManagerD3D11::Shutdown()
{
    for (auto& kv : mCache)
    {
        IUnknown* p = (IUnknown*)kv.second;
        SafeRelease(p);
    }
    mCache.clear();

    IUnknown* tmp = nullptr;

    tmp = (IUnknown*)mWhiteSRV;
    SafeRelease(tmp);
    mWhiteSRV = nullptr;

    tmp = (IUnknown*)mBlackSRV;
    SafeRelease(tmp);
    mBlackSRV = nullptr;

    tmp = (IUnknown*)mWicFactory;
    SafeRelease(tmp);
    mWicFactory = nullptr;

    tmp = (IUnknown*)mDevice;
    SafeRelease(tmp);
    mDevice = nullptr;
}

bool TextureManagerD3D11::CreateSolidColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a, bool srgb, ID3D11ShaderResourceView** outSrv)
{
    if (!outSrv)
        return false;
    *outSrv = nullptr;

    uint8_t rgba[4] = { r, g, b, a };
    ID3D11ShaderResourceView* srv = CreateTextureFromRgba8(rgba, 1, 1, srgb);
    if (!srv)
        return false;

    *outSrv = srv;
    return true;
}

ID3D11ShaderResourceView* TextureManagerD3D11::CreateTextureFromRgba8(const uint8_t* rgba, uint32_t w, uint32_t h, bool srgb)
{
    if (!mDevice || !rgba || w == 0 || h == 0)
        return nullptr;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = rgba;
    init.SysMemPitch = w * 4;

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = mDevice->CreateTexture2D(&td, &init, &tex);
    if (FAILED(hr))
        return nullptr;

    ID3D11ShaderResourceView* srv = nullptr;
    hr = mDevice->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();

    if (FAILED(hr))
        return nullptr;

    return srv;
}

bool TextureManagerD3D11::LoadWicRgba8(const std::wstring& path, std::vector<uint8_t>& outRgba, uint32_t& outW, uint32_t& outH)
{
    outRgba.clear();
    outW = 0;
    outH = 0;

    if (!mWicFactory)
    {
        IWICImagingFactory* factory = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(hr) || !factory)
            return false;
        mWicFactory = factory;
    }

    IWICImagingFactory* factory = (IWICImagingFactory*)mWicFactory;

    IWICBitmapDecoder* decoder = nullptr;
    HRESULT hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr) || !decoder)
        return false;

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame)
    {
        decoder->Release();
        return false;
    }

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0)
    {
        frame->Release();
        decoder->Release();
        return false;
    }

    IWICFormatConverter* conv = nullptr;
    hr = factory->CreateFormatConverter(&conv);
    if (FAILED(hr) || !conv)
    {
        frame->Release();
        decoder->Release();
        return false;
    }

    hr = conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
    {
        conv->Release();
        frame->Release();
        decoder->Release();
        return false;
    }

    outRgba.resize((size_t)w * (size_t)h * 4u);
    hr = conv->CopyPixels(nullptr, w * 4u, (UINT)outRgba.size(), outRgba.data());

    conv->Release();
    frame->Release();
    decoder->Release();

    if (FAILED(hr))
    {
        outRgba.clear();
        return false;
    }

    outW = (uint32_t)w;
    outH = (uint32_t)h;
    return true;
}

ID3D11ShaderResourceView* TextureManagerD3D11::GetOrLoad2D(const std::wstring& path, bool srgb)
{
    if (path.empty())
        return srgb ? mWhiteSRV : mWhiteSRV;

    Key k{};
    k.path = path;
    k.srgb = srgb;

    auto it = mCache.find(k);
    if (it != mCache.end())
        return it->second;

    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    if (!LoadWicRgba8(path, rgba, w, h))
    {
        // Cache negative result as fallback to avoid spamming WIC.
        ID3D11ShaderResourceView* fallback = mWhiteSRV;
        if (fallback)
            fallback->AddRef();
        mCache.emplace(k, fallback);
        return fallback;
    }

    ID3D11ShaderResourceView* srv = CreateTextureFromRgba8(rgba.data(), w, h, srgb);
    if (!srv)
    {
        ID3D11ShaderResourceView* fallback = mWhiteSRV;
        if (fallback)
            fallback->AddRef();
        mCache.emplace(k, fallback);
        return fallback;
    }

    mCache.emplace(k, srv);
    return srv;
}

} // namespace king::render::d3d11
