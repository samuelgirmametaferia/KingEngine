#pragma once

#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace king
{

struct ShaderDefine
{
    std::string name;
    std::string value;
};

struct ReflectedResource
{
    std::string name;
    D3D_SHADER_INPUT_TYPE type = D3D_SIT_TEXTURE;
    uint32_t bindPoint = 0;
    uint32_t bindCount = 0;
};

struct ReflectedCBuffer
{
    std::string name;
    uint32_t bindPoint = 0;
    uint32_t sizeBytes = 0;
};

struct ShaderReflectionInfo
{
    std::vector<ReflectedResource> resources;
    std::vector<ReflectedCBuffer> cbuffers;
};

struct CompiledShader
{
    // Blob is kept for input layout creation/reflection.
    ID3DBlob* bytecode = nullptr;
    ShaderReflectionInfo reflection;

    ~CompiledShader();
    CompiledShader() = default;
    CompiledShader(const CompiledShader&) = delete;
    CompiledShader& operator=(const CompiledShader&) = delete;
    CompiledShader(CompiledShader&&) noexcept;
    CompiledShader& operator=(CompiledShader&&) noexcept;
};

// D3D11 HLSL shader compiler + cache.
class ShaderCache
{
public:
    explicit ShaderCache(ID3D11Device* device);

    bool CompileVSFromFile(const wchar_t* path, const char* entry, const std::vector<ShaderDefine>& defines, CompiledShader& out, std::string* outError);
    bool CompilePSFromFile(const wchar_t* path, const char* entry, const std::vector<ShaderDefine>& defines, CompiledShader& out, std::string* outError);

private:
    struct Key
    {
        std::wstring path;
        std::string entry;
        std::string target;
        std::string defineHash;

        bool operator==(const Key& o) const
        {
            return path == o.path && entry == o.entry && target == o.target && defineHash == o.defineHash;
        }
    };

    struct KeyHash
    {
        size_t operator()(const Key& k) const;
    };

    bool CompileFromFile(const wchar_t* path, const char* entry, const char* target, const std::vector<ShaderDefine>& defines, CompiledShader& out, std::string* outError);

    static std::string MakeDefineHash(const std::vector<ShaderDefine>& defines);
    static void FillD3DDefines(const std::vector<ShaderDefine>& defines, std::vector<D3D_SHADER_MACRO>& outMacros);
    static bool Reflect(ID3DBlob* bytecode, ShaderReflectionInfo& outInfo, std::string* outError);

    ID3D11Device* mDevice = nullptr;
    std::unordered_map<Key, std::vector<uint8_t>, KeyHash> mBytecodeCache;
};

} // namespace king
