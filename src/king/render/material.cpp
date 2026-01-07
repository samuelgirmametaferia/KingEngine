#include "material.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace king
{

static bool EqualsI(const std::string& a, const char* b)
{
    if (!b)
        return false;
    if (a.size() != std::strlen(b))
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return false;
    }
    return true;
}

static void DeriveIntentFromShaderName(PbrMaterial& m)
{
    // Convenience defaults so existing content keeps working.
    // Custom HLSL path keeps the default (PBR/opaque) unless explicitly overridden.
    if (m.shader.empty())
        return;

    if (m.shader.size() >= 5)
    {
        const auto s = m.shader;
        const auto n = s.size();
        const auto suf = s.substr(n - 5);
        if (EqualsI(suf, ".hlsl"))
            return;
    }

    if (EqualsI(m.shader, "unlit") || EqualsI(m.shader, "unlit_color"))
        m.shadingModel = MaterialShadingModel::Unlit;
    else if (EqualsI(m.shader, "rim") || EqualsI(m.shader, "rim_glow") || EqualsI(m.shader, "rim_outline_glow"))
        m.shadingModel = MaterialShadingModel::RimGlow;
    else
        m.shadingModel = MaterialShadingModel::Pbr;
}

static bool ReadLine(std::ifstream& in, std::string& line)
{
    std::getline(in, line);
    return (bool)in;
}

static std::string Trim(const std::string& s)
{
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b = s.size();
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool LoadMaterialFile(const char* path, PbrMaterial& outMaterial, std::string* outError)
{
    std::ifstream f(path);
    if (!f)
    {
        if (outError) *outError = std::string("Failed to open material file: ") + path;
        return false;
    }

    std::string line;
    int lineNo = 0;

    while (ReadLine(f, line))
    {
        ++lineNo;
        line = Trim(line);
        if (line.empty()) continue;
        if (!line.empty() && line[0] == '#') continue;

        // Strip inline comments.
        if (auto pos = line.find('#'); pos != std::string::npos)
            line = Trim(line.substr(0, pos));
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string key;
        iss >> key;
        if (key.empty()) continue;

        if (key == "shader")
        {
            iss >> outMaterial.shader;
        }
        else if (key == "blend")
        {
            std::string v;
            iss >> v;
            if (EqualsI(v, "opaque"))
                outMaterial.blendMode = MaterialBlendMode::Opaque;
            else if (EqualsI(v, "alpha") || EqualsI(v, "alphablend") || EqualsI(v, "transparent"))
                outMaterial.blendMode = MaterialBlendMode::AlphaBlend;
            else
            {
                if (outError)
                    *outError = "Unknown blend mode at " + std::to_string(lineNo) + ": " + v;
                return false;
            }
        }
        else if (key == "shading")
        {
            std::string v;
            iss >> v;
            if (EqualsI(v, "pbr") || EqualsI(v, "lit"))
                outMaterial.shadingModel = MaterialShadingModel::Pbr;
            else if (EqualsI(v, "unlit"))
                outMaterial.shadingModel = MaterialShadingModel::Unlit;
            else if (EqualsI(v, "rim") || EqualsI(v, "rimglow") || EqualsI(v, "rim_glow"))
                outMaterial.shadingModel = MaterialShadingModel::RimGlow;
            else
            {
                if (outError)
                    *outError = "Unknown shading model at " + std::to_string(lineNo) + ": " + v;
                return false;
            }
        }
        else if (key == "albedo")
        {
            iss >> outMaterial.albedo.x >> outMaterial.albedo.y >> outMaterial.albedo.z >> outMaterial.albedo.w;
        }
        else if (key == "roughness")
        {
            iss >> outMaterial.roughness;
        }
        else if (key == "metallic")
        {
            iss >> outMaterial.metallic;
        }
        else if (key == "emissive")
        {
            iss >> outMaterial.emissive.x >> outMaterial.emissive.y >> outMaterial.emissive.z;
        }
        else if (key == "tex_albedo")
        {
            iss >> outMaterial.textures.albedo;
        }
        else if (key == "tex_normal")
        {
            iss >> outMaterial.textures.normal;
        }
        else if (key == "tex_mr")
        {
            iss >> outMaterial.textures.metallicRoughness;
        }
        else if (key == "tex_emissive")
        {
            iss >> outMaterial.textures.emissive;
        }
        else
        {
            // Allow scalar overrides: scalar <name> <value>
            if (key == "scalar")
            {
                std::string name;
                float v = 0.0f;
                iss >> name >> v;
                if (!name.empty()) outMaterial.scalars[name] = v;
            }
            else
            {
                if (outError)
                {
                    *outError = "Unknown key at " + std::to_string(lineNo) + ": " + key;
                }
                return false;
            }
        }
    }

    if (outMaterial.shader.empty())
        outMaterial.shader = "pbr";

    // If the file didn't specify intent explicitly, infer a sane default.
    DeriveIntentFromShaderName(outMaterial);

    return true;
}

} // namespace king
