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
- [ ] Move D3D globals out of main into a RenderDevice class
- [ ] Split CameraSystem / LightingSystem / RenderSystem into separate compilation units
- [ ] Add a render graph or at least explicit pass structure

## Performance
- [ ] Switch cube mesh to indexed drawing (IB)
- [ ] Batch/sort draw calls by pipeline state
- [ ] Instancing for identical meshes
- [ ] Replace per-draw Map/Unmap with ring-buffer or structured buffer

## Features (near-term)
- [ ] Basic camera controls (WASD + mouse look)
- [ ] Add proper material inputs (roughness/metallic/normal)
- [ ] Add linear workflow and tonemapping
- [ ] Shadow mapping (directional)

## Tooling
- [ ] Shader hot reload
- [ ] GPU markers for PIX/RenderDoc
