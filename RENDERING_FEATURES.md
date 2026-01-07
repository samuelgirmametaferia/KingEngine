# KingEngine — Rendering Features

This document lists:
- **What the renderer already supports today** (shipping features)
- **What’s missing** (roadmap)
- A **priority order** for implementation

Scope: `src/king/render/d3d11/*` + `assets/shaders/pbr_test.hlsl`.

---

## Current Features (Implemented)

### Pipeline / Passes
- **Geometry pass**
  - D3D11 forward shading (single pass) with instancing.
  - Optional MRT variant when SSAO is enabled (HDR + normal output).
- **Directional shadow pass**
  - Cascaded shadow maps (CSM) into a depth `Texture2DArray`.
  - Per-cascade rendering recorded via deferred contexts/command lists inside the shadow module.
- **SSAO + blur pass** (optional)
  - Fullscreen SSAO using depth + normal buffers.
  - Blur pass to smooth AO.
- **Tonemap pass** (optional)
  - Fullscreen triangle tonemap from HDR to backbuffer.
  - ACES-fitted tonemapper.

### Color / Output
- **HDR offscreen target**: `R16G16B16A16_FLOAT`.
- **sRGB backbuffer RTV when supported** (`R8G8B8A8_UNORM_SRGB` RTV on top of an UNORM swapchain backbuffer).

### Materials / Shading
- Per-instance material parameters:
  - `albedo` (float4)
  - `roughness`, `metallic` (currently only partially used; base lighting is mostly diffuse)
- Correct normal handling:
  - Per-instance **inverse-transpose normal matrix** (fixes non-uniform scale).

### Lighting
- Up to **16 lights** in the GPU light buffer.
- Light types (as used by the shader):
  - Directional
  - Point
  - Spot (cone attenuation)
- **Light group masks** (per-light `groupMask` AND per-instance `lightMask`).

### Shadows (Directional)
- **CSM (1–3 cascades)** with split control (`cascadeLambda`).
- Shadow map filtering ladder (`shadowFilterQuality`):
  - 0 hard (still linear-compare)
  - 1 PCF 3×3 (default)
  - 2 PCF 5×5
  - 3 Rotated Poisson 9-tap
  - 4 PCSS (blocker search + variable-radius PCF)
- Bias / stability features:
  - Constant bias
  - Optional normal-offset bias
  - Optional receiver-plane depth bias (RPDB)
  - Optional distance fade-out
- Content controls:
  - `shadowMinVisibility` avoids pitch-black shadows.
  - `shadowMaxDistance` clamps cascade far distance for texel density.
- CPU optimizations:
  - Single scene snapshot per frame
  - Cascade-aware caster culling
  - Allocation reuse via persistent scratch vectors

### Performance Tooling (Dev)
- Per-pass CPU timers (RAII scopes).
- GPU timers via timestamp/disjoint queries (buffered readback).

---

## Roadmap (Prioritized)

These are **missing or incomplete** features, ordered by engine usefulness and expected effort.

### P0 (High impact / low risk)
1. **Backface culling enabled by default** (performance + correctness for closed meshes).
2. **VSync toggle** (easy stability lever; reduces noise in perf profiling).
3. **GPU “Frame” scope** (makes total GPU time visible, not just per-pass).
4. **Fix SSAO projection correctness** (SSAO currently uses invViewProj as a proxy; better to feed real projection).

### P1 (High impact / medium effort)
1. **Proper PBR specular** using roughness/metallic (Cook-Torrance / GGX or a simple approximation).
2. **Normal mapping + texture sampling path** (albedo/normal/roughness/metallic textures).
3. **Sky/IBL** (skybox + diffuse/specular image-based lighting) to anchor material response.
4. **Bloom** (common HDR post effect).

### P2 (Medium impact / larger changes)
1. **MSAA** (requires MSAA render targets + resolve).
2. **Transparency pipeline** (sorted transparent objects; potentially separate pass).
3. **Directional light stabilization improvements** (more advanced cascade stabilization, optional contact shadows, etc.).

---

## Implementation Order (What we’ll do next)

We’ll implement P0 items first.
- Start with **backface culling** and **VSync toggle**.
- Next: **GPU Frame scope** (so the perf overlay shows total GPU time).

When you approve, we move to P1: **PBR specular + textures**.
