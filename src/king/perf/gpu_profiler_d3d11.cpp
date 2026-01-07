#include "gpu_profiler_d3d11.h"

#include <cstdio>
#include <cstring>

namespace king::perf
{

static void SafeRelease(IUnknown*& p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

GpuProfilerD3D11::~GpuProfilerD3D11()
{
    Shutdown();
}

void GpuProfilerD3D11::Initialize(ID3D11Device* device)
{
    if (!mSettings.enabled)
        return;

    if (mDevice == device && mDevice)
        return;

    Shutdown();

    mDevice = device;
    if (mDevice)
        mDevice->AddRef();

    const uint32_t bf = (mSettings.bufferedFrames < 2) ? 2u : mSettings.bufferedFrames;
    mDisjoint.resize(bf, nullptr);
    for (uint32_t i = 0; i < bf; ++i)
        mDisjoint[i] = CreateDisjointQuery();
}

void GpuProfilerD3D11::Shutdown()
{
    for (auto* q : mDisjoint)
    {
        IUnknown* p = (IUnknown*)q;
        SafeRelease(p);
    }
    mDisjoint.clear();

    for (auto& s : mScopes)
    {
        for (auto* q : s.begin)
        {
            IUnknown* p = (IUnknown*)q;
            SafeRelease(p);
        }
        for (auto* q : s.end)
        {
            IUnknown* p = (IUnknown*)q;
            SafeRelease(p);
        }
        s.begin.clear();
        s.end.clear();
    }
    mScopes.clear();

    IUnknown* dev = (IUnknown*)mDevice;
    SafeRelease(dev);
    mDevice = nullptr;

    mFrameIndex = 0;
}

ID3D11Query* GpuProfilerD3D11::CreateTimestampQuery()
{
    if (!mDevice)
        return nullptr;

    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_TIMESTAMP;
    ID3D11Query* q = nullptr;
    if (FAILED(mDevice->CreateQuery(&qd, &q)))
        return nullptr;
    return q;
}

ID3D11Query* GpuProfilerD3D11::CreateDisjointQuery()
{
    if (!mDevice)
        return nullptr;

    D3D11_QUERY_DESC qd{};
    qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    ID3D11Query* q = nullptr;
    if (FAILED(mDevice->CreateQuery(&qd, &q)))
        return nullptr;
    return q;
}

GpuProfilerD3D11::Scope* GpuProfilerD3D11::FindOrCreateScope(const char* name)
{
    for (auto& s : mScopes)
    {
        if (s.name == name)
            return &s;
        if (s.name && name && std::strcmp(s.name, name) == 0)
            return &s;
    }

    Scope ns{};
    ns.name = name;

    const uint32_t bf = (mSettings.bufferedFrames < 2) ? 2u : mSettings.bufferedFrames;
    ns.begin.resize(bf, nullptr);
    ns.end.resize(bf, nullptr);
    for (uint32_t i = 0; i < bf; ++i)
    {
        ns.begin[i] = CreateTimestampQuery();
        ns.end[i] = CreateTimestampQuery();
    }

    mScopes.push_back(ns);
    return &mScopes.back();
}

void GpuProfilerD3D11::BeginFrame(ID3D11DeviceContext* ctx)
{
    if (!mSettings.enabled || !ctx || mDisjoint.empty())
        return;

    const uint32_t bf = (uint32_t)mDisjoint.size();
    const uint32_t fi = (mFrameIndex % bf);

    if (mDisjoint[fi])
        ctx->Begin(mDisjoint[fi]);
}

void GpuProfilerD3D11::EndFrame(ID3D11DeviceContext* ctx)
{
    if (!mSettings.enabled || !ctx || mDisjoint.empty())
        return;

    const uint32_t bf = (uint32_t)mDisjoint.size();
    const uint32_t fi = (mFrameIndex % bf);

    if (mDisjoint[fi])
        ctx->End(mDisjoint[fi]);

    mFrameIndex++;
}

void GpuProfilerD3D11::BeginScope(ID3D11DeviceContext* ctx, const char* name)
{
    if (!mSettings.enabled || !ctx || !name || mDisjoint.empty())
        return;

    Scope* s = FindOrCreateScope(name);
    const uint32_t bf = (uint32_t)mDisjoint.size();
    const uint32_t fi = (mFrameIndex % bf);

    if (s && s->begin[fi])
        ctx->End(s->begin[fi]);
}

void GpuProfilerD3D11::EndScope(ID3D11DeviceContext* ctx, const char* name)
{
    if (!mSettings.enabled || !ctx || !name || mDisjoint.empty())
        return;

    Scope* s = FindOrCreateScope(name);
    const uint32_t bf = (uint32_t)mDisjoint.size();
    const uint32_t fi = (mFrameIndex % bf);

    if (s && s->end[fi])
        ctx->End(s->end[fi]);
}

bool GpuProfilerD3D11::TryGetResults(ID3D11DeviceContext* ctx, uint32_t& outFrameIndex, std::vector<std::pair<const char*, double>>& outGpuMs)
{
    outGpuMs.clear();

    if (!mSettings.enabled || !ctx || mDisjoint.empty())
        return false;

    const uint32_t bf = (uint32_t)mDisjoint.size();
    if (mFrameIndex < bf)
        return false;

    // Read back a buffered frame behind to avoid stalls and reduce the chance of
    // perpetual S_FALSE when using D3D11_ASYNC_GETDATA_DONOTFLUSH.
    const uint32_t readFrame = mFrameIndex - bf;
    const uint32_t fi = (readFrame % bf);

    if (!mDisjoint[fi])
        return false;

    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dis{};
    HRESULT hr = ctx->GetData(mDisjoint[fi], &dis, sizeof(dis), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (hr != S_OK)
        return false;

    if (dis.Disjoint || dis.Frequency == 0)
        return false;

    const double freq = (double)dis.Frequency;
    outFrameIndex = readFrame;

    outGpuMs.reserve(mScopes.size());

    for (auto& s : mScopes)
    {
        if (!s.begin[fi] || !s.end[fi])
            continue;

        UINT64 t0 = 0;
        UINT64 t1 = 0;
        HRESULT hr0 = ctx->GetData(s.begin[fi], &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        HRESULT hr1 = ctx->GetData(s.end[fi], &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr0 != S_OK || hr1 != S_OK)
            continue;

        if (t1 <= t0)
            continue;

        const double ms = ((double)(t1 - t0) * 1000.0) / freq;
        outGpuMs.push_back({ s.name, ms });
    }

    return true;
}

} // namespace king::perf
