#include "shader_program_d3d11.h"

#include "../../ecs/components.h"

#include <cstring>

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

ShaderProgramD3D11::~ShaderProgramD3D11()
{
    Destroy();
}

void ShaderProgramD3D11::Destroy()
{
    IUnknown* tmp = nullptr;

    tmp = (IUnknown*)mInputLayout;
    SafeRelease(tmp);
    mInputLayout = nullptr;

    tmp = (IUnknown*)mPSMrt;
    SafeRelease(tmp);
    mPSMrt = nullptr;

    tmp = (IUnknown*)mPS;
    SafeRelease(tmp);
    mPS = nullptr;

    tmp = (IUnknown*)mVS;
    SafeRelease(tmp);
    mVS = nullptr;
}

bool ShaderProgramD3D11::Create(ID3D11Device* device, king::ShaderCache& cache, const GeometryProgramDesc& desc, std::string* outError)
{
    Destroy();

    if (!device)
    {
        if (outError) *outError = "ShaderProgramD3D11::Create: device is null.";
        return false;
    }

    king::CompiledShader vs;
    king::CompiledShader ps;
    king::CompiledShader psMrt;

    if (!cache.CompileVSFromFile(desc.hlslPath.c_str(), desc.vsEntry.c_str(), desc.defines, vs, outError))
        return false;
    if (!cache.CompilePSFromFile(desc.hlslPath.c_str(), desc.psEntry.c_str(), desc.defines, ps, outError))
        return false;

    // MRT PS is optional: only required when SSAO path is enabled.
    bool haveMrt = false;
    if (!desc.psMrtEntry.empty())
    {
        std::string mrtErr;
        if (cache.CompilePSFromFile(desc.hlslPath.c_str(), desc.psMrtEntry.c_str(), desc.defines, psMrt, &mrtErr))
            haveMrt = true;
    }

    HRESULT hr = device->CreateVertexShader(vs.bytecode->GetBufferPointer(), vs.bytecode->GetBufferSize(), nullptr, &mVS);
    if (FAILED(hr))
    {
        if (outError) *outError = "CreateVertexShader failed.";
        Destroy();
        return false;
    }

    hr = device->CreatePixelShader(ps.bytecode->GetBufferPointer(), ps.bytecode->GetBufferSize(), nullptr, &mPS);
    if (FAILED(hr))
    {
        if (outError) *outError = "CreatePixelShader failed.";
        Destroy();
        return false;
    }

    if (haveMrt)
    {
        hr = device->CreatePixelShader(psMrt.bytecode->GetBufferPointer(), psMrt.bytecode->GetBufferSize(), nullptr, &mPSMrt);
        if (FAILED(hr))
        {
            // Non-fatal: allow program to exist without MRT; renderer will fall back.
            mPSMrt = nullptr;
        }
    }

    // Fixed engine vertex format (VertexPN + instance data).
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },

        // Per-instance data (slot 1)
        { "TEXCOORD",  4, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0,  D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD",  5, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD",  6, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD",  7, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        // Normal matrix rows (inverse-transpose of world)
        { "TEXCOORD", 11, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 64,  D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD", 12, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 80,  D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD", 13, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 96,  D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD", 14, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 112, D3D11_INPUT_PER_INSTANCE_DATA, 1 },

        { "COLOR",     0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 128, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD",  8, DXGI_FORMAT_R32G32_FLOAT,       1, 144, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD",  9, DXGI_FORMAT_R32_UINT,           1, 152, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD", 10, DXGI_FORMAT_R32_UINT,           1, 156, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };

    hr = device->CreateInputLayout(
        layout,
        (UINT)(sizeof(layout) / sizeof(layout[0])),
        vs.bytecode->GetBufferPointer(),
        vs.bytecode->GetBufferSize(),
        &mInputLayout);
    if (FAILED(hr))
    {
        if (outError) *outError = "CreateInputLayout failed.";
        Destroy();
        return false;
    }

    return true;
}

} // namespace king::render::d3d11
