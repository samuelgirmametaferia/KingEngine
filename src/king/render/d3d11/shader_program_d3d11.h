#pragma once

#include "../../render/shader.h"

#include <d3d11.h>
#include <string>
#include <vector>

namespace king::render::d3d11
{

struct GeometryProgramDesc
{
    std::wstring hlslPath;
    std::string vsEntry = "VSMain";
    std::string psEntry = "PSMain";
    std::string psMrtEntry = "PSMainMRT"; // optional; used when SSAO MRT path is enabled

    std::vector<king::ShaderDefine> defines;
};

class ShaderProgramD3D11
{
public:
    ShaderProgramD3D11() = default;
    ~ShaderProgramD3D11();

    ShaderProgramD3D11(const ShaderProgramD3D11&) = delete;
    ShaderProgramD3D11& operator=(const ShaderProgramD3D11&) = delete;

    bool Create(ID3D11Device* device, king::ShaderCache& cache, const GeometryProgramDesc& desc, std::string* outError);
    void Destroy();

    ID3D11VertexShader* VS() const { return mVS; }
    ID3D11PixelShader* PS() const { return mPS; }
    ID3D11PixelShader* PSMrt() const { return mPSMrt; }
    ID3D11InputLayout* InputLayout() const { return mInputLayout; }

    bool HasMrtVariant() const { return mPSMrt != nullptr; }

private:
    ID3D11VertexShader* mVS = nullptr;
    ID3D11PixelShader* mPS = nullptr;
    ID3D11PixelShader* mPSMrt = nullptr;
    ID3D11InputLayout* mInputLayout = nullptr;
};

} // namespace king::render::d3d11
