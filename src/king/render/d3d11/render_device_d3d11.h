#pragma once

#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>
#include <string_view>

namespace king::render::d3d11
{

class RenderDeviceD3D11
{
public:
    RenderDeviceD3D11() = default;
    ~RenderDeviceD3D11();

    RenderDeviceD3D11(const RenderDeviceD3D11&) = delete;
    RenderDeviceD3D11& operator=(const RenderDeviceD3D11&) = delete;

    HRESULT Initialize(HWND hwnd, uint32_t width, uint32_t height);
    void Shutdown();

    ID3D11Device* Device() const { return mDevice; }
    ID3D11DeviceContext* Context() const { return mContext; }
    IDXGISwapChain* SwapChain() const { return mSwapChain; }

    ID3D11RenderTargetView* RTV() const { return mRTV; }
    ID3D11DepthStencilView* DSV() const { return mDSV; }
    ID3D11DepthStencilState* DSS() const { return mDSS; }
    ID3D11RasterizerState* RS() const { return mRS; }
    const D3D11_VIEWPORT& Viewport() const { return mViewport; }

    void QueueResize(uint32_t width, uint32_t height);

    // Applies any queued resize at a stable point (start-of-frame).
    // Returns true if a resize was applied and outputs the new size.
    bool ApplyQueuedResize(uint32_t* outWidth, uint32_t* outHeight);

    // Frame helpers
    void BeginFrame(const float clearColor[4]);

    // GPU markers (PIX/RenderDoc). Safe no-op if not supported.
    void BeginGpuEvent(std::wstring_view name);
    void EndGpuEvent();

    // Presents. Returns the HRESULT from Present.
    HRESULT Present(uint32_t syncInterval = 1);

    bool IsDeviceLost(HRESULT hr) const;
    HRESULT GetDeviceRemovedReason() const;

    // Attempts to recover from device lost by recreating the device/swapchain.
    bool RecoverFromDeviceLost(HWND hwnd);

    uint32_t BackBufferWidth() const { return mBackBufferW; }
    uint32_t BackBufferHeight() const { return mBackBufferH; }

private:
    static void SafeRelease(IUnknown*& p);

    HRESULT CreateRenderTarget();
    HRESULT CreateDepthTarget(uint32_t width, uint32_t height);
    HRESULT ResizeInternal(uint32_t width, uint32_t height);
    HRESULT CreateStates();

private:
    ID3D11Device* mDevice = nullptr;
    ID3D11DeviceContext* mContext = nullptr;
    IDXGISwapChain* mSwapChain = nullptr;

    ID3D11RenderTargetView* mRTV = nullptr;
    ID3D11DepthStencilView* mDSV = nullptr;

    ID3D11DepthStencilState* mDSS = nullptr;
    ID3D11RasterizerState* mRS = nullptr;

    // Optional (available with the debug layer / tooling):
    // https://learn.microsoft.com/windows/win32/api/d3d11_1/nn-d3d11_1-id3duserdefinedannotation
    void* mAnnotation = nullptr;

    D3D11_VIEWPORT mViewport{};

    uint32_t mBackBufferW = 0;
    uint32_t mBackBufferH = 0;

    bool mResizeQueued = false;
    uint32_t mQueuedW = 0;
    uint32_t mQueuedH = 0;
};

} // namespace king::render::d3d11
