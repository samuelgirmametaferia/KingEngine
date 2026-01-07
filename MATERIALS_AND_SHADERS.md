# King Engine: Materials & Custom Shaders (D3D11)

## What you can do now

- Set `MeshRenderer.material.shader` to either:
  - empty / `"pbr"` / `"pbr_forward"` (uses the built-in shader at `RenderSystemD3D11::Initialize(shaderPath)`), or
  - an HLSL filename like `"unlit_color.hlsl"` (loaded relative to the built-in shader directory), or
  - an absolute HLSL path.

The renderer will compile/load that shader and use it for the geometry pass.

## Engine binding contract (geometry pass)

Your custom HLSL must match the engine’s fixed vertex + instance input layout (see `VertexPN` + `InstanceData`).

**Required entry points**
- `VSMain`
- `PSMain`

**Optional (required if SSAO is enabled)**
- `PSMainMRT` (must write `SV_Target0` = HDR color and `SV_Target1` = normal buffer)

**Registers used by the engine**
- Constant buffers:
  - `b0` = `CameraCB`
  - `b1` = `LightCB` (bound for geometry pass; you can ignore it)
  - `b4` = `MaterialCB` (per-material; optional for your shader)
- Textures / SRVs:
  - `t0` = `gShadowMap` (if shadows are enabled)
  - `t5..t8` = material textures (optional): albedo, normal, metallicRoughness, emissive
- Samplers:
  - `s0,s1,s3` = shadow samplers
  - `s4` = material sampler

## Example

- Sample custom shader: [assets/shaders/unlit_color.hlsl](assets/shaders/unlit_color.hlsl)
- Set a material’s shader string to `"unlit_color.hlsl"` to use it.

## Notes / current limitations

- The engine’s mesh vertex format currently has **no UVs** (`VertexPN` only has position+normal). Material textures are still bound for custom shaders, but you’ll need to generate UVs in your shader (procedural mapping) or extend the vertex format later.
- Shader programs are cached by HLSL path; materials are cached by a stable CPU key derived from material params + texture paths.
