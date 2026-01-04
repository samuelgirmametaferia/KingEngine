#include "material.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace king
{

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

    return true;
}

} // namespace king
