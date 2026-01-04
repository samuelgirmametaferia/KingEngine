#include "shader.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#pragma comment(lib, "d3dcompiler.lib")

namespace king
{

static void SafeRelease(IUnknown* p)
{
    if (p) p->Release();
}

CompiledShader::~CompiledShader()
{
    SafeRelease(bytecode);
    bytecode = nullptr;
}

CompiledShader::CompiledShader(CompiledShader&& o) noexcept
{
    bytecode = o.bytecode;
    reflection = std::move(o.reflection);
    o.bytecode = nullptr;
}

CompiledShader& CompiledShader::operator=(CompiledShader&& o) noexcept
{
    if (this == &o) return *this;
    SafeRelease(bytecode);
    bytecode = o.bytecode;
    reflection = std::move(o.reflection);
    o.bytecode = nullptr;
    return *this;
}

ShaderCache::ShaderCache(ID3D11Device* device) : mDevice(device) {}

size_t ShaderCache::KeyHash::operator()(const Key& k) const
{
    std::hash<std::wstring> hw;
    std::hash<std::string> hs;
    size_t h = hw(k.path);
    h ^= (hs(k.entry) + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= (hs(k.target) + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= (hs(k.defineHash) + 0x9e3779b9 + (h << 6) + (h >> 2));
    return h;
}

std::string ShaderCache::MakeDefineHash(const std::vector<ShaderDefine>& defines)
{
    // Stable, order-independent hash string.
    std::vector<ShaderDefine> sorted = defines;
    std::sort(sorted.begin(), sorted.end(), [](const ShaderDefine& a, const ShaderDefine& b)
    {
        return a.name < b.name;
    });

    std::ostringstream oss;
    for (const auto& d : sorted)
    {
        oss << d.name << '=' << d.value << ';';
    }
    return oss.str();
}

void ShaderCache::FillD3DDefines(const std::vector<ShaderDefine>& defines, std::vector<D3D_SHADER_MACRO>& outMacros)
{
    outMacros.clear();
    outMacros.reserve(defines.size() + 1);
    for (const auto& d : defines)
    {
        D3D_SHADER_MACRO m{};
        m.Name = d.name.c_str();
        m.Definition = d.value.c_str();
        outMacros.push_back(m);
    }
    outMacros.push_back(D3D_SHADER_MACRO{ nullptr, nullptr });
}

bool ShaderCache::Reflect(ID3DBlob* bytecode, ShaderReflectionInfo& outInfo, std::string* outError)
{
    ID3D11ShaderReflection* refl = nullptr;
    HRESULT hr = D3DReflect(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), IID_ID3D11ShaderReflection, (void**)&refl);
    if (FAILED(hr))
    {
        if (outError) *outError = "D3DReflect failed.";
        return false;
    }

    D3D11_SHADER_DESC sd{};
    refl->GetDesc(&sd);

    outInfo.resources.clear();
    outInfo.cbuffers.clear();

    for (UINT i = 0; i < sd.BoundResources; ++i)
    {
        D3D11_SHADER_INPUT_BIND_DESC bd{};
        if (SUCCEEDED(refl->GetResourceBindingDesc(i, &bd)))
        {
            ReflectedResource r{};
            r.name = bd.Name ? bd.Name : "";
            r.type = bd.Type;
            r.bindPoint = bd.BindPoint;
            r.bindCount = bd.BindCount;
            outInfo.resources.push_back(std::move(r));
        }
    }

    for (UINT i = 0; i < sd.ConstantBuffers; ++i)
    {
        ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByIndex(i);
        if (!cb) continue;

        D3D11_SHADER_BUFFER_DESC cbd{};
        cb->GetDesc(&cbd);

        // Find its binding point by name.
        uint32_t bindPoint = 0;
        for (const auto& r : outInfo.resources)
        {
            if (r.type == D3D_SIT_CBUFFER && r.name == cbd.Name)
            {
                bindPoint = r.bindPoint;
                break;
            }
        }

        ReflectedCBuffer out{};
        out.name = cbd.Name ? cbd.Name : "";
        out.bindPoint = bindPoint;
        out.sizeBytes = cbd.Size;
        outInfo.cbuffers.push_back(std::move(out));
    }

    refl->Release();
    return true;
}

bool ShaderCache::CompileFromFile(const wchar_t* path, const char* entry, const char* target, const std::vector<ShaderDefine>& defines, CompiledShader& out, std::string* outError)
{
    Key key{};
    key.path = path;
    key.entry = entry;
    key.target = target;
    key.defineHash = MakeDefineHash(defines);

    auto it = mBytecodeCache.find(key);
    if (it != mBytecodeCache.end())
    {
        // Rehydrate blob from cached bytes.
        ID3DBlob* blob = nullptr;
        HRESULT hr = D3DCreateBlob(it->second.size(), &blob);
        if (FAILED(hr))
        {
            if (outError) *outError = "D3DCreateBlob failed.";
            return false;
        }
        std::memcpy(blob->GetBufferPointer(), it->second.data(), it->second.size());

        out = CompiledShader{};
        out.bytecode = blob;
        if (!Reflect(out.bytecode, out.reflection, outError))
            return false;
        return true;
    }

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG;
    flags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    std::vector<D3D_SHADER_MACRO> macros;
    FillD3DDefines(defines, macros);

    ID3DBlob* bytecode = nullptr;
    ID3DBlob* errors = nullptr;

    HRESULT hr = D3DCompileFromFile(
        path,
        macros.data(),
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry,
        target,
        flags,
        0,
        &bytecode,
        &errors
    );

    if (FAILED(hr))
    {
        if (errors)
        {
            const char* msg = (const char*)errors->GetBufferPointer();
            if (outError) *outError = msg ? msg : "Shader compile failed.";
            errors->Release();
        }
        else
        {
            if (outError) *outError = "Shader compile failed.";
        }
        SafeRelease(bytecode);
        return false;
    }

    if (errors) errors->Release();

    // Cache bytes.
    std::vector<uint8_t> bytes(bytecode->GetBufferSize());
    std::memcpy(bytes.data(), bytecode->GetBufferPointer(), bytes.size());
    mBytecodeCache.emplace(key, std::move(bytes));

    out = CompiledShader{};
    out.bytecode = bytecode;
    if (!Reflect(out.bytecode, out.reflection, outError))
        return false;

    return true;
}

bool ShaderCache::CompileVSFromFile(const wchar_t* path, const char* entry, const std::vector<ShaderDefine>& defines, CompiledShader& out, std::string* outError)
{
    return CompileFromFile(path, entry, "vs_5_0", defines, out, outError);
}

bool ShaderCache::CompilePSFromFile(const wchar_t* path, const char* entry, const std::vector<ShaderDefine>& defines, CompiledShader& out, std::string* outError)
{
    return CompileFromFile(path, entry, "ps_5_0", defines, out, outError);
}

} // namespace king
