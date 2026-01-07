# KingEngine – Render/Engine Improvements Checklist

This file tracks the improvements we identified and whether they’re implemented.

## Rendering correctness
- [x] Renderer-owned resize handling (queue resize from window, apply at start-of-frame)
- [x] Update camera aspect on resize inside RenderSystem
- [x] Skip rendering when minimized / zero-sized
- [x] Correct constant buffer binding + reflection logging for both VS and PS
- [x] Use sRGB render target view for gamma-correct output
- [x] Handle device removed/reset (recreate D3D device + resources; basic recovery in place)

## Architecture
- [x] Move D3D globals out of main into a RenderDevice class
- [x] Split CameraSystem / LightingSystem / RenderSystem into separate compilation units
- [x] Add a render graph or at least explicit pass structure

## Performance
- [x] Switch cube mesh to indexed drawing (IB)
- [x] Batch/sort draw calls by pipeline state
- [x] Instancing for identical meshes
- [x] Replace per-draw Map/Unmap with ring-buffer or structured buffer

## Features (near-term)
- [x] Basic camera controls (WASD + mouse look)
- [ ] Add proper material inputs (roughness/metallic/normal)
- [x] Add linear workflow + HDR and tonemapping
- [x] Lighting as first-class ECS data (directional/point/spot components)
- [x] Default "sun" directional light exists by default
- [x] Multi-light forward rendering path (fixed max lights)
- [x] Light grouping / light masks (lights affect only selected renderables)
- [x] Shadow mapping for directional light (single map)
- [x] Optional: cascaded shadow maps (2–3 splits)
- [x] Shadow filtering (PCF) + depth bias tuning

### Directional-light–centric shadows (quality/stability)
- [x] Main directional uses cascaded shadow maps (CSM) with practical split scheme blending logarithmic + linear partitioning
- [x] Snap each cascade to texel-sized increments in light space to prevent shimmering during camera movement
- [x] Fit each cascade’s orthographic projection tightly to the camera frustum slice every frame (maximize effective texel usage)
- [x] Biasing is not “just depth bias”: combine slope-scaled depth bias + shader normal-offset bias + receiver-plane depth bias (reduce acne without peter-panning)
- [x] Filtering uses PCF with a rotated Poisson/disk kernel; filter radius scales with cascade index so distant shadows are softer and consistent
- [x] Implement cascade transition blending to avoid abrupt cascade boundary changes
- [x] Implement distance-based shadow fade-out (avoid abrupt far shadow cutoff)
- [x] Render shadow depth in linear depth (orthographic shadow projection)
- [ ] Optional: reversed-Z depth for improved precision (where feasible)
- [x] Transparent objects: by default don’t cast shadows (until a masked shadow pass exists)
- [x] Cull small casters below a screen-space threshold from shadow rendering (avoid flicker and wasted fill)
- [ ] Point/spot shadows are optional: cube-map or atlas-based, aggressively culled, with dynamic resolution based on screen influence
- [x] Integrate shadows with AO so shadowed areas remain perceptually grounded (no binary dark overlay)
- [x] Optional: SSAO

#### Directional shadows – remaining issues & next fixes
- [x] Shadows should not be pitch black: add `shadowMinVisibility` (minimum visibility in full shadow) and apply it in shading
- [x] Shadow texel density vs distance: expose a shadow coverage clamp (`shadowMaxDistance`) to improve texel density without increasing map size
- [x] Cascade-aware caster culling: cull casters against each cascade’s light frustum/ortho bounds (not just camera frustum) to reduce work and improve effective texel usage
- [x] Avoid rebuilding scene snapshot twice per frame: build one snapshot and derive both main draw list and shadow caster list from it
- [x] Reduce per-frame allocations: persist and reuse shadow snapshot/caster lists and draw-batch vectors (clear + reserve) to avoid realloc churn
- [x] Fix normal transform for non-uniform scale (inverse-transpose) so biasing and N·L are stable and predictable
- [x] Shadow filter quality ladder: default to PCF 3x3, allow PCF 5x5 / Poisson as opt-in, and document the perf/quality tradeoff
- [x] Soft shadows: PCSS-style penumbra (blocker search + variable-radius filter), structured so softness can be overridden per material/object

- [x] Frustum culling at entity level (skip invisible objects)
- [ ] Optional: GPU occlusion culling (Hi-Z / depth pre-pass reuse)

## Tooling
- [ ] Shader hot reload
- [x] GPU markers for PIX/RenderDoc
