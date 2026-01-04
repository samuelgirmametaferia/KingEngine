#pragma once

#include "../math/types.h"

#include <string>
#include <unordered_map>

namespace king
{

struct TextureSet
{
    // Paths or asset IDs. Keep as strings for now (bindings-friendly).
    std::string albedo;
    std::string normal;
    std::string metallicRoughness;
    std::string emissive;
};

struct PbrMaterial
{
    // Core PBR params (data-driven)
    Float4 albedo{ 1, 1, 1, 1 };
    float roughness = 0.5f;
    float metallic = 0.0f;
    Float3 emissive{ 0, 0, 0 };

    // Link to a shader and textures
    std::string shader; // e.g. "pbr_deferred", "pbr_forward"
    TextureSet textures;

    // Arbitrary extra scalar parameters
    std::unordered_map<std::string, float> scalars;
};

// Simple data-driven material loader.
// Format (whitespace separated, # comments):
//   shader pbr
//   albedo 1 1 1 1
//   roughness 0.5
//   metallic 0.0
//   emissive 0 0 0
//   tex_albedo path
//   tex_normal path
//   tex_mr path
//   tex_emissive path
bool LoadMaterialFile(const char* path, PbrMaterial& outMaterial, std::string* outError);

} // namespace king
