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
- [ ] Optional: cascaded shadow maps (2–3 splits)
- [x] Shadow filtering (PCF) + depth bias tuning
- [ ] Optional: SSAO

- [x] Frustum culling at entity level (skip invisible objects)
- [ ] Optional: GPU occlusion culling (Hi-Z / depth pre-pass reuse)

## Tooling
- [ ] Shader hot reload
- [x] GPU markers for PIX/RenderDoc
