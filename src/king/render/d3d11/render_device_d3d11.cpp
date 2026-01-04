#include "render_device_d3d11.h"

#include <cstdio>
#include <d3d11_1.h>
#include <string>

namespace king::render::d3d11
{

static HRESULT TryCreateDeviceAndSwapChain(
    HWND hwnd,
    uint32_t width,
    uint32_t height,
    D3D_DRIVER_TYPE driverType,
    UINT createFlags,
    IDXGISwapChain** outSwapChain,
    ID3D11Device** outDevice,
    ID3D11DeviceContext** outContext,
    D3D_FEATURE_LEVEL* outFeatureLevel)
{
    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = width;
    scd.BufferDesc.Height = height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    return D3D11CreateDeviceAndSwapChain(
        nullptr,
        driverType,
        nullptr,
        createFlags,
        featureLevels,
        (UINT)(sizeof(featureLevels) / sizeof(featureLevels[0])),
        D3D11_SDK_VERSION,
        &scd,
        outSwapChain,
        outDevice,
        outFeatureLevel,
        outContext);
}

RenderDeviceD3D11::~RenderDeviceD3D11()
{
    Shutdown();
}

void RenderDeviceD3D11::SafeRelease(IUnknown*& p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

void RenderDeviceD3D11::Shutdown()
{
    IUnknown* tmp = nullptr;

    tmp = (IUnknown*)mAnnotation;
    SafeRelease(tmp);
    mAnnotation = nullptr;

    tmp = (IUnknown*)mRTV;
    SafeRelease(tmp);
    mRTV = nullptr;

    tmp = (IUnknown*)mDSV;
    SafeRelease(tmp);
    mDSV = nullptr;

    tmp = (IUnknown*)mDSS;
    SafeRelease(tmp);
    mDSS = nullptr;

    tmp = (IUnknown*)mRS;
    SafeRelease(tmp);
    mRS = nullptr;

    tmp = (IUnknown*)mSwapChain;
    SafeRelease(tmp);
    mSwapChain = nullptr;

    if (mContext)
    {
        mContext->ClearState();
        mContext->Flush();
    }

    tmp = (IUnknown*)mContext;
    SafeRelease(tmp);
    mContext = nullptr;

    tmp = (IUnknown*)mDevice;
    SafeRelease(tmp);
    mDevice = nullptr;

    mBackBufferW = 0;
    mBackBufferH = 0;
    mResizeQueued = false;
    mQueuedW = 0;
    mQueuedH = 0;
}

HRESULT RenderDeviceD3D11::CreateRenderTarget()
{
    IUnknown* tmp = (IUnknown*)mRTV;
    SafeRelease(tmp);
    mRTV = nullptr;

    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = mSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (FAILED(hr))
        return hr;

    // Prefer sRGB RTV for correct gamma output; fallback to default if unsupported.
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtvDesc.Texture2D.MipSlice = 0;

    hr = mDevice->CreateRenderTargetView(backBuffer, &rtvDesc, &mRTV);
    if (FAILED(hr))
        hr = mDevice->CreateRenderTargetView(backBuffer, nullptr, &mRTV);

    backBuffer->Release();
    return hr;
}

HRESULT RenderDeviceD3D11::CreateDepthTarget(uint32_t width, uint32_t height)
{
    IUnknown* tmp = (IUnknown*)mDSV;
    SafeRelease(tmp);
    mDSV = nullptr;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    ID3D11Texture2D* depthTex = nullptr;
    HRESULT hr = mDevice->CreateTexture2D(&td, nullptr, &depthTex);
    if (FAILED(hr))
    {
        if (depthTex)
            depthTex->Release();
        return hr;
    }

    hr = mDevice->CreateDepthStencilView(depthTex, nullptr, &mDSV);
    depthTex->Release();
    return hr;
}

HRESULT RenderDeviceD3D11::ResizeInternal(uint32_t width, uint32_t height)
{
    if (!mSwapChain)
        return S_OK;

    mBackBufferW = width;
    mBackBufferH = height;

    if (mContext)
        mContext->OMSetRenderTargets(0, nullptr, nullptr);

    // ResizeBuffers requires that all views referencing the backbuffer are released.
    IUnknown* tmp = (IUnknown*)mRTV;
    SafeRelease(tmp);
    mRTV = nullptr;

    tmp = (IUnknown*)mDSV;
    SafeRelease(tmp);
    mDSV = nullptr;

    HRESULT hr = mSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr))
        return hr;

    hr = CreateRenderTarget();
    if (FAILED(hr))
        return hr;

    hr = CreateDepthTarget(width, height);
    if (FAILED(hr))
        return hr;

    mViewport.TopLeftX = 0.0f;
    mViewport.TopLeftY = 0.0f;
    mViewport.Width = (float)width;
    mViewport.Height = (float)height;
    mViewport.MinDepth = 0.0f;
    mViewport.MaxDepth = 1.0f;

    return S_OK;
}

HRESULT RenderDeviceD3D11::CreateStates()
{
    // Depth testing state
    IUnknown* tmp = (IUnknown*)mDSS;
    SafeRelease(tmp);
    mDSS = nullptr;

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    dsd.StencilEnable = FALSE;

    HRESULT hr = mDevice->CreateDepthStencilState(&dsd, &mDSS);
    if (FAILED(hr))
        return hr;

    // Rasterizer state: disable culling for now (iterating quickly).
    tmp = (IUnknown*)mRS;
    SafeRelease(tmp);
    mRS = nullptr;

    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;

    hr = mDevice->CreateRasterizerState(&rs, &mRS);
    return hr;
}

HRESULT RenderDeviceD3D11::Initialize(HWND hwnd, uint32_t width, uint32_t height)
{
    Shutdown();

    UINT createFlags = 0;
#if defined(_DEBUG)
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    std::printf("InitD3D: creating device/swapchain (%ux%u)\n", width, height);

    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = TryCreateDeviceAndSwapChain(
        hwnd,
        width,
        height,
        D3D_DRIVER_TYPE_HARDWARE,
        createFlags,
        &mSwapChain,
        &mDevice,
        &mContext,
        &fl);

#if defined(_DEBUG)
    if (FAILED(hr) && (createFlags & D3D11_CREATE_DEVICE_DEBUG))
    {
        std::printf("Retrying without D3D11 debug layer...\n");
        hr = TryCreateDeviceAndSwapChain(
            hwnd,
            width,
            height,
            D3D_DRIVER_TYPE_HARDWARE,
            createFlags & ~D3D11_CREATE_DEVICE_DEBUG,
            &mSwapChain,
            &mDevice,
            &mContext,
            &fl);
    }
#endif

    if (FAILED(hr))
    {
        std::printf("Retrying with WARP software device...\n");
        hr = TryCreateDeviceAndSwapChain(
            hwnd,
            width,
            height,
            D3D_DRIVER_TYPE_WARP,
            0,
            &mSwapChain,
            &mDevice,
            &mContext,
            &fl);
    }

    if (FAILED(hr))
        return hr;

    std::printf("D3D device created. Feature level: 0x%X\n", (unsigned)fl);

    // Try to acquire annotation interface for GPU markers.
    if (mContext)
    {
        ID3DUserDefinedAnnotation* ann = nullptr;
        if (SUCCEEDED(mContext->QueryInterface(__uuidof(ID3DUserDefinedAnnotation), (void**)&ann)) && ann)
        {
            mAnnotation = ann;
        }
    }

    hr = ResizeInternal(width, height);
    if (FAILED(hr))
        return hr;

    hr = CreateStates();
    if (FAILED(hr))
        return hr;

    // Seed resize state.
    QueueResize(width, height);

    return S_OK;
}

void RenderDeviceD3D11::BeginGpuEvent(std::wstring_view name)
{
    if (!mAnnotation)
        return;
    auto* ann = (ID3DUserDefinedAnnotation*)mAnnotation;
    ann->BeginEvent(std::wstring(name).c_str());
}

void RenderDeviceD3D11::EndGpuEvent()
{
    if (!mAnnotation)
        return;
    auto* ann = (ID3DUserDefinedAnnotation*)mAnnotation;
    ann->EndEvent();
}

void RenderDeviceD3D11::QueueResize(uint32_t width, uint32_t height)
{
    mResizeQueued = true;
    mQueuedW = width;
    mQueuedH = height;
}

bool RenderDeviceD3D11::ApplyQueuedResize(uint32_t* outWidth, uint32_t* outHeight)
{
    if (!mResizeQueued)
        return false;

    if (mQueuedW == 0 || mQueuedH == 0)
        return false;

    // Keep the resize queued until it succeeds. Some transitions (e.g. dragging out of
    // maximized) can briefly cause ResizeBuffers to fail; dropping the queue would
    // leave the swapchain and camera aspect out of sync.
    HRESULT hr = ResizeInternal(mQueuedW, mQueuedH);
    if (FAILED(hr))
        return false;

    mResizeQueued = false;

    if (outWidth)
        *outWidth = mQueuedW;
    if (outHeight)
        *outHeight = mQueuedH;

    return true;
}

void RenderDeviceD3D11::BeginFrame(const float clearColor[4])
{
    if (!mContext || !mRTV)
        return;

    mContext->ClearRenderTargetView(mRTV, clearColor);
    if (mDSV)
        mContext->ClearDepthStencilView(mDSV, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    mContext->OMSetRenderTargets(1, &mRTV, mDSV);
    mContext->OMSetDepthStencilState(mDSS, 0);
    mContext->RSSetState(mRS);
    mContext->RSSetViewports(1, &mViewport);
}

HRESULT RenderDeviceD3D11::Present(uint32_t syncInterval)
{
    if (!mSwapChain)
        return E_FAIL;

    return mSwapChain->Present(syncInterval, 0);
}

bool RenderDeviceD3D11::IsDeviceLost(HRESULT hr) const
{
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;
}

HRESULT RenderDeviceD3D11::GetDeviceRemovedReason() const
{
    return mDevice ? mDevice->GetDeviceRemovedReason() : E_FAIL;
}

bool RenderDeviceD3D11::RecoverFromDeviceLost(HWND hwnd)
{
    uint32_t w = mBackBufferW;
    uint32_t h = mBackBufferH;

    if (w == 0 || h == 0)
    {
        RECT r{};
        GetClientRect(hwnd, &r);
        w = (uint32_t)((r.right > r.left) ? (r.right - r.left) : 1280);
        h = (uint32_t)((r.bottom > r.top) ? (r.bottom - r.top) : 720);
    }

    return SUCCEEDED(Initialize(hwnd, w, h));
}

} // namespace king::render::d3d11
