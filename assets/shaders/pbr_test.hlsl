// Validation shader for King.
// Goal: make the renderer feel "complete" without exploding complexity.

// Engine-owned material specialization (static decisions resolved at compile time).
// 0 = PBR (lit), 1 = Unlit, 2 = RimGlow
#ifndef KING_SHADING_MODEL
#define KING_SHADING_MODEL 0
#endif

#define MAX_LIGHTS 16

cbuffer CameraCB : register(b0)
{
    row_major float4x4 gViewProj;
    float3 gCameraPos;
    float gExposure;

    float gAoStrength;
    float3 _padPost;
};

// Frozen material uniform layout (instances differ only by values).
cbuffer MaterialCB : register(b4)
{
    float4 gMatBaseColor;
    float3 gMatEmissive;
    float gMatRoughness;

    float gMatMetallic;
    uint gMatFlags;
    float2 _padMat;
};

struct LightData
{
    uint type;
    uint groupMask;
    uint _padU0;
    uint _padU1;

    float3 color;
    float intensity;

    float3 dir;
    float range;

    float3 pos;
    float innerConeCos;

    float outerConeCos;
    float3 _padF0;
};

cbuffer LightCB : register(b1)
{
    uint gLightCount;
    uint3 _pad0;
    LightData gLights[MAX_LIGHTS];

    row_major float4x4 gLightViewProj[3];
    float4 gCascadeSplitsNdc; // x=split1, y=split2, z=split3(typically 1), w=unused
    uint gCascadeCount;
    float3 _padCsm;

    float2 gShadowTexelSize;
    float gShadowBias;
    float gShadowStrength;

    float gShadowMinVisibility;
    float3 _padShadowMin;

    // x=fadeStartNdc, y=fadeEndNdc, z=shadowSoftness, w=shadowFlags (packed as float)
    float4 gShadowExtras;
    // Point shadow params:
    // x = enabled (0/1)
    // y = bias (world units)
    // z = invFar (1/range)
    // w = strength (0..1)
    float4 gPointShadowParams;
    float2 gPointShadowTexelSize;
    float2 _padPointShadow;
};

cbuffer ShadowCB : register(b2)
{
    row_major float4x4 gShadowViewProj;
};

Texture2DArray<float> gShadowMap : register(t0);
SamplerComparisonState gShadowSamplerPoint : register(s0);
SamplerComparisonState gShadowSamplerLinear : register(s1);
SamplerState gShadowSamplerNonCmp : register(s3);
// Point-light shadow cubemap (stores linear depth normalized: dist / far)
TextureCube<float> gPointShadowMap : register(t9);
SamplerState gPointShadowSampler : register(s5);

// SSAO resources (used by SSAO/blur/tonemap passes)
Texture2D<float> gSsao : register(t2);
cbuffer PointShadowCB : register(b7)
{
    row_major float4x4 gPointShadowViewProj;
    float3 gPointShadowLightPos;
    float gPointShadowInvFar;
};

struct VSPointShadowOut
{
    float4 pos : SV_POSITION;
    float3 wpos : TEXCOORD0;
};

// Minimal input for point-shadow pass. Must match the engine input layout.
struct VSInPointShadow
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;

    // Per-instance transform rows (slot 1)
    float4 iRow0 : TEXCOORD4;
    float4 iRow1 : TEXCOORD5;
    float4 iRow2 : TEXCOORD6;
    float4 iRow3 : TEXCOORD7;
};

VSPointShadowOut VSPointShadowMain(VSInPointShadow v)
{
    VSPointShadowOut o;

    float4 localPos = float4(v.pos, 1.0);

    float4 col0 = float4(v.iRow0.x, v.iRow1.x, v.iRow2.x, v.iRow3.x);
    float4 col1 = float4(v.iRow0.y, v.iRow1.y, v.iRow2.y, v.iRow3.y);
    float4 col2 = float4(v.iRow0.z, v.iRow1.z, v.iRow2.z, v.iRow3.z);
    float4 col3 = float4(v.iRow0.w, v.iRow1.w, v.iRow2.w, v.iRow3.w);

    float4 wpos;
    wpos.x = dot(localPos, col0);
    wpos.y = dot(localPos, col1);
    wpos.z = dot(localPos, col2);
    wpos.w = dot(localPos, col3);

    o.pos = mul(wpos, gPointShadowViewProj);
    o.wpos = wpos.xyz;
    return o;
}

float PSPointShadowMain(VSPointShadowOut i) : SV_TARGET
{
    float dist = length(i.wpos - gPointShadowLightPos);
    return saturate(dist * gPointShadowInvFar);
}

// SSAO pass inputs
Texture2D<float> gDepth : register(t3);
Texture2D<float4> gNormal : register(t4);
SamplerState gPointClamp : register(s2);

cbuffer SsaoCB : register(b3)
{
    row_major float4x4 gSsaoProj;
    row_major float4x4 gSsaoInvProj;
    row_major float4x4 gSsaoView;
    float2 gSsaoInvTargetSize;
    float gSsaoRadius;
    float gSsaoBias;
};

struct VSIn
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;

    // Per-instance data (slot 1)
    float4 iRow0 : TEXCOORD4;
    float4 iRow1 : TEXCOORD5;
    float4 iRow2 : TEXCOORD6;
    float4 iRow3 : TEXCOORD7;

    // Inverse-transpose of world (for correct normals under non-uniform scale)
    float4 nRow0 : TEXCOORD11;
    float4 nRow1 : TEXCOORD12;
    float4 nRow2 : TEXCOORD13;
    float4 nRow3 : TEXCOORD14;

    float4 albedo : COLOR0;

    float2 rm : TEXCOORD8;     // x=roughness, y=metallic (not fully used yet)
    uint lightMask : TEXCOORD9;
    uint flags : TEXCOORD10;   // bit0 receivesShadows
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float3 wpos : TEXCOORD0;
    float3 nrm : TEXCOORD1;
    float4 albedo : COLOR0;
    float2 rm : TEXCOORD2;
    uint lightMask : TEXCOORD3;
    uint flags : TEXCOORD4;
};

VSOut VSMain(VSIn v)
{
    VSOut o;

    // Treat iRow* as ROWS of the world matrix (as provided by C++).
    float4 localPos = float4(v.pos, 1.0);

    // mul(localPos, world) uses row-vector convention, so each output component
    // is a dot against a COLUMN of the matrix.
    float4 col0 = float4(v.iRow0.x, v.iRow1.x, v.iRow2.x, v.iRow3.x);
    float4 col1 = float4(v.iRow0.y, v.iRow1.y, v.iRow2.y, v.iRow3.y);
    float4 col2 = float4(v.iRow0.z, v.iRow1.z, v.iRow2.z, v.iRow3.z);
    float4 col3 = float4(v.iRow0.w, v.iRow1.w, v.iRow2.w, v.iRow3.w);

    float4 wpos;
    wpos.x = dot(localPos, col0);
    wpos.y = dot(localPos, col1);
    wpos.z = dot(localPos, col2);
    wpos.w = dot(localPos, col3);
    o.pos = mul(wpos, gViewProj);
    o.wpos = wpos.xyz;

    // Correct normal transform: inverse-transpose(world) applied to direction (w=0)
    float4 ncol0 = float4(v.nRow0.x, v.nRow1.x, v.nRow2.x, v.nRow3.x);
    float4 ncol1 = float4(v.nRow0.y, v.nRow1.y, v.nRow2.y, v.nRow3.y);
    float4 ncol2 = float4(v.nRow0.z, v.nRow1.z, v.nRow2.z, v.nRow3.z);
    float4 localNrm = float4(v.nrm, 0.0);
    o.nrm.x = dot(localNrm, ncol0);
    o.nrm.y = dot(localNrm, ncol1);
    o.nrm.z = dot(localNrm, ncol2);
    o.albedo = v.albedo;
    o.rm = v.rm;
    o.lightMask = v.lightMask;
    o.flags = v.flags;
    return o;
}

// Depth-only vertex shader (prepass)
float4 VSDepthMain(VSIn v) : SV_POSITION
{
    float4 localPos = float4(v.pos, 1.0);

    float4 col0 = float4(v.iRow0.x, v.iRow1.x, v.iRow2.x, v.iRow3.x);
    float4 col1 = float4(v.iRow0.y, v.iRow1.y, v.iRow2.y, v.iRow3.y);
    float4 col2 = float4(v.iRow0.z, v.iRow1.z, v.iRow2.z, v.iRow3.z);
    float4 col3 = float4(v.iRow0.w, v.iRow1.w, v.iRow2.w, v.iRow3.w);

    float4 wpos;
    wpos.x = dot(localPos, col0);
    wpos.y = dot(localPos, col1);
    wpos.z = dot(localPos, col2);
    wpos.w = dot(localPos, col3);
    return mul(wpos, gViewProj);
}

static float Hash12Shadow(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

static float SampleShadowCascadeFiltered(uint cascade, float2 uv, float depth, float radiusTexels, float rot)
{
    // 8-tap poisson + center. Rotated per-pixel to hide banding.
    // Offsets are in texels.
    static const float2 kPoisson[8] = {
        float2(-0.613,  0.617),
        float2( 0.170, -0.040),
        float2(-0.299, -0.781),
        float2( 0.645,  0.493),
        float2(-0.651,  0.285),
        float2( 0.421, -0.512),
        float2( 0.108,  0.807),
        float2( 0.844, -0.212)
    };

    float s = sin(rot);
    float c = cos(rot);
    float2x2 R = float2x2(c, -s, s, c);

    float sum = 0.0;
    sum += gShadowMap.SampleCmpLevelZero(gShadowSamplerPoint, float3(uv, (float)cascade), depth);

    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        float2 o = mul(kPoisson[i], R) * (radiusTexels * gShadowTexelSize);
        sum += gShadowMap.SampleCmpLevelZero(gShadowSamplerPoint, float3(uv + o, (float)cascade), depth);
    }

    return sum / 9.0;
}

static float SampleShadowCascadeDepth(uint cascade, float2 uv)
{
    // Shadow map stores depth in [0,1].
    return gShadowMap.SampleLevel(gShadowSamplerNonCmp, float3(uv, (float)cascade), 0);
}

static float SampleShadowCascadePCFDepth(uint cascade, float2 uv, float receiverDepth, float radiusTexels, float rot)
{
    // Depth-based PCF (manual compare). Uses the same 8-tap Poisson + center kernel.
    // This is used by PCSS after computing a variable penumbra radius.
    static const float2 kPoisson[8] = {
        float2(-0.613,  0.617),
        float2( 0.170, -0.040),
        float2(-0.299, -0.781),
        float2( 0.645,  0.493),
        float2(-0.651,  0.285),
        float2( 0.421, -0.512),
        float2( 0.108,  0.807),
        float2( 0.844, -0.212)
    };

    float s = sin(rot);
    float c = cos(rot);
    float2x2 R = float2x2(c, -s, s, c);

    float sum = 0.0;
    float d0 = SampleShadowCascadeDepth(cascade, uv);
    sum += (d0 >= receiverDepth) ? 1.0 : 0.0;

    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        float2 o = mul(kPoisson[i], R) * (radiusTexels * gShadowTexelSize);
        float d = SampleShadowCascadeDepth(cascade, uv + o);
        sum += (d >= receiverDepth) ? 1.0 : 0.0;
    }

    return sum / 9.0;
}

static float SampleShadowCascadePCSS(uint cascade, float2 uv, float receiverDepth, float baseRadiusTexels, float rot)
{
    // PCSS (Percentage-Closer Soft Shadows):
    // 1) Blocker search (average occluder depth)
    // 2) Compute penumbra size based on receiver vs blocker depth
    // 3) Variable-radius PCF

    // Blocker search radius: modest and proportional to base radius.
    float searchRadius = clamp(baseRadiusTexels * 2.0, 2.0, 8.0);

    static const float2 kPoisson[8] = {
        float2(-0.613,  0.617),
        float2( 0.170, -0.040),
        float2(-0.299, -0.781),
        float2( 0.645,  0.493),
        float2(-0.651,  0.285),
        float2( 0.421, -0.512),
        float2( 0.108,  0.807),
        float2( 0.844, -0.212)
    };

    float s = sin(rot);
    float c = cos(rot);
    float2x2 R = float2x2(c, -s, s, c);

    float blockerSum = 0.0;
    float blockerCount = 0.0;

    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        float2 o = mul(kPoisson[i], R) * (searchRadius * gShadowTexelSize);
        float d = SampleShadowCascadeDepth(cascade, uv + o);
        // Treat samples closer than the receiver as blockers.
        if (d < receiverDepth)
        {
            blockerSum += d;
            blockerCount += 1.0;
        }
    }

    // No blockers => fully lit.
    if (blockerCount < 0.5)
        return 1.0;

    float avgBlocker = blockerSum / blockerCount;
    avgBlocker = max(avgBlocker, 1e-4);

    // Penumbra grows with (receiver - blocker) distance.
    float penumbra = (receiverDepth - avgBlocker) / avgBlocker;

    // Convert to a filter radius in texels.
    // Scale is intentionally conservative to avoid over-blurring.
    float radius = baseRadiusTexels * (1.0 + penumbra * 24.0);
    radius = clamp(radius, 1.0, 12.0);

    return SampleShadowCascadePCFDepth(cascade, uv, receiverDepth, radius, rot);
}

static float GetShadowSoftness(float4 albedo, float2 rm, uint flags)
{
    // Global softness by default.
    // Future: override per object/material by reading instance params (e.g. rm, flags, material id).
    return max(0.0, gShadowExtras.z);
}

static uint GetShadowFilterQuality(float4 albedo, float2 rm, uint flags)
{
    // Global quality by default, packed into shadow flags.
    // Future: override per object/material.
    uint shadowFlags = (uint)gShadowExtras.w;
    uint q = (shadowFlags >> 8) & 0xFu;
    return q;
}

static float SampleShadowCascadePcfGrid(uint cascade, float2 uv, float depth, float radiusTexels, uint gridRadius)
{
    // Grid PCF using hardware 2x2 comparison filtering per tap (linear comparison sampler).
    // gridRadius=1 => 3x3 taps, gridRadius=2 => 5x5 taps.
    float2 stepUv = max(radiusTexels, 1.0) * gShadowTexelSize;
    float sum = 0.0;
    float wsum = 0.0;

    [loop]
    for (int y = -(int)gridRadius; y <= (int)gridRadius; ++y)
    {
        [loop]
        for (int x = -(int)gridRadius; x <= (int)gridRadius; ++x)
        {
            float2 o = float2((float)x, (float)y) * stepUv;
            // Simple tent weight so center is stronger.
            float wx = 1.0 - abs((float)x) / (float)(gridRadius + 1);
            float wy = 1.0 - abs((float)y) / (float)(gridRadius + 1);
            float w = wx * wy;
            sum += w * gShadowMap.SampleCmpLevelZero(gShadowSamplerLinear, float3(uv + o, (float)cascade), depth);
            wsum += w;
        }
    }

    return (wsum > 1e-6) ? (sum / wsum) : 1.0;
}

static float SampleShadowCascadeSoftCross(uint cascade, float2 uv, float depth, float radiusTexels)
{
    // Cheap soft-shadow fallback: 5 taps (center + cross) using hardware PCF.
    // This reduces visible "zig-zag" edges without the full Poisson cost.
    float2 o = (radiusTexels * gShadowTexelSize);

    float sum = 0.0;
    sum += gShadowMap.SampleCmpLevelZero(gShadowSamplerLinear, float3(uv, (float)cascade), depth);
    sum += gShadowMap.SampleCmpLevelZero(gShadowSamplerLinear, float3(uv + float2( o.x, 0.0), (float)cascade), depth);
    sum += gShadowMap.SampleCmpLevelZero(gShadowSamplerLinear, float3(uv + float2(-o.x, 0.0), (float)cascade), depth);
    sum += gShadowMap.SampleCmpLevelZero(gShadowSamplerLinear, float3(uv + float2(0.0,  o.y), (float)cascade), depth);
    sum += gShadowMap.SampleCmpLevelZero(gShadowSamplerLinear, float3(uv + float2(0.0, -o.y), (float)cascade), depth);
    return sum * 0.2;
}

static float ReceiverPlaneDepthBias(float2 uv, float depth)
{
    // Receiver-plane depth bias (RPDB).
    // Estimates how much the projected depth changes across 1 shadow-map texel
    // and biases accordingly to reduce acne without large constant offsets.
    float2 duvdx = ddx(uv);
    float2 duvdy = ddy(uv);
    float dzdx = ddx(depth);
    float dzdy = ddy(depth);

    float det = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    if (abs(det) < 1e-6)
        return 0.0;

    float invDet = 1.0 / det;
    float dzdu = (dzdx * duvdy.y - dzdy * duvdx.y) * invDet;
    float dzdv = (dzdy * duvdx.x - dzdx * duvdy.x) * invDet;

    // Convert depth change per unit-UV to per-texel depth change.
    float rpdb = abs(dzdu) * gShadowTexelSize.x + abs(dzdv) * gShadowTexelSize.y;
    return rpdb;
}

static float SamplePointShadowFiltered(float3 wpos, float3 lightPos, float dist)
{
    if (gPointShadowParams.x <= 0.5)
        return 1.0;

    // Normalize depth to match cubemap storage.
    float invFar = max(gPointShadowParams.z, 1e-6);
    float distN = dist * invFar;
    float biasN = gPointShadowParams.y * invFar;

    float3 dir = wpos - lightPos;
    float lenDir = max(length(dir), 1e-6);
    float3 D = dir / lenDir;

    // 5-tap compare using small angular offsets.
    float3 up = (abs(D.y) < 0.95) ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 U = normalize(cross(up, D));
    float3 V = cross(D, U);
    float eps = 1.75 * gPointShadowTexelSize.x;

    float ref = distN - biasN;

    float s0 = gPointShadowMap.SampleLevel(gPointShadowSampler, D, 0).r;
    float s1 = gPointShadowMap.SampleLevel(gPointShadowSampler, normalize(D + U * eps), 0).r;
    float s2 = gPointShadowMap.SampleLevel(gPointShadowSampler, normalize(D - U * eps), 0).r;
    float s3 = gPointShadowMap.SampleLevel(gPointShadowSampler, normalize(D + V * eps), 0).r;
    float s4 = gPointShadowMap.SampleLevel(gPointShadowSampler, normalize(D - V * eps), 0).r;

    float sum = 0.0;
    sum += step(ref, s0);
    sum += step(ref, s1);
    sum += step(ref, s2);
    sum += step(ref, s3);
    sum += step(ref, s4);
    return sum / 5.0;
}

static float SampleShadowFiltered(float3 wpos, float3 N, float4 albedo, float2 rm, uint flags)
{
    if (gShadowStrength <= 0.0)
        return 1.0;

    uint shadowFlags = (uint)gShadowExtras.w;
    const bool enableFadeOut = (shadowFlags & (1u << 0)) != 0u;
    const bool enablePoissonFlag = (shadowFlags & (1u << 1)) != 0u;
    const bool enableNormalOffset = (shadowFlags & (1u << 2)) != 0u;
    const bool enableRpdb = (shadowFlags & (1u << 3)) != 0u;

    uint filterQ = GetShadowFilterQuality(albedo, rm, flags);
    // Back-compat: if quality isn't set (0) but Poisson is enabled, treat as Poisson.
    // Otherwise default to 3x3.
    if (filterQ == 0u && enablePoissonFlag)
        filterQ = 3u;
    if (filterQ > 4u)
        filterQ = 4u;

    // Find a directional light direction for slope bias.
    // (Shadows are for the primary sun; in this demo there is usually only one directional.)
    float3 Ldir = float3(0.0, -1.0, 0.0);
    [unroll]
    for (uint li = 0; li < min(gLightCount, (uint)MAX_LIGHTS); ++li)
    {
        if (gLights[li].type == 0u)
        {
            Ldir = normalize(-gLights[li].dir);
            break;
        }
    }
    float ndotl = saturate(dot(normalize(N), Ldir));
    // Increase bias on grazing angles to reduce acne without blowing out contact shadows.
    float bias = gShadowBias * (1.0 + (1.0 - ndotl) * 2.5);

    // Select cascade based on camera clip-space depth.
    float4 clip = mul(float4(wpos, 1.0), gViewProj);
    float ndcZ = clip.z / max(clip.w, 1e-6);

    // Optional fade out near far plane.
    float fade = 1.0;
    if (enableFadeOut)
    {
        float fadeStart = gShadowExtras.x;
        float fadeEnd = gShadowExtras.y;
        if (fadeEnd > fadeStart)
            fade = saturate((fadeEnd - ndcZ) / max(fadeEnd - fadeStart, 1e-6));
    }

    uint cascade = 0;
    if (gCascadeCount > 1 && ndcZ > gCascadeSplitsNdc.x) cascade = 1;
    if (gCascadeCount > 2 && ndcZ > gCascadeSplitsNdc.y) cascade = 2;
    cascade = min(cascade, (uint)2);

    // Blend between cascades near split planes to avoid hard transitions.
    uint cascade2 = cascade;
    float blend = 0.0;
    if (gCascadeCount > 1 && cascade < (gCascadeCount - 1))
    {
        float split = (cascade == 0) ? gCascadeSplitsNdc.x : gCascadeSplitsNdc.y;
        // Transition region in NDC depth; keep small.
        const float kTransition = 0.02;
        float d = split - ndcZ;
        if (d < kTransition)
        {
            blend = saturate(1.0 - d / kTransition);
            cascade2 = cascade + 1;
        }
    }

    float rot = Hash12Shadow(wpos.xz * 0.17 + ndcZ.xx) * 6.2831853;

    // Radius in texels: slightly larger for farther cascades.
    float t = (gCascadeCount > 1) ? (float)cascade / max((float)(gCascadeCount - 1), 1.0) : 0.0;
    float softness = GetShadowSoftness(albedo, rm, flags);
    float radiusTexels = lerp(1.15, 2.10, t) * max(softness, 0.0);
    radiusTexels = clamp(radiusTexels, 0.0, 8.0);

    // Shader normal-offset bias: a small offset along the surface normal to reduce acne.
    // Keep this intentionally subtle to avoid peter-panning; scale slightly with cascade.
    float3 Nb = normalize(N);
    float normalOffsetWorld = (1.0 - ndotl) * lerp(0.0015, 0.0035, t);
    float3 wposB = enableNormalOffset ? (wpos + Nb * normalOffsetWorld) : wpos;

    // Sample cascade 1
    {
        float4 lp = mul(float4(wposB, 1.0), gLightViewProj[cascade]);
        float3 ndc = lp.xyz / max(lp.w, 1e-6);
        float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
            return 1.0;
        if (ndc.z < 0.0 || ndc.z > 1.0)
            return 1.0;

        float depth = ndc.z;
        float rpdb = 0.0;
        if (enableRpdb)
        {
            rpdb = ReceiverPlaneDepthBias(uv, depth);
            // Scale RPDB modestly with filter radius; clamp to avoid pathological cases.
            rpdb = min(rpdb * (radiusTexels / 1.25), 0.0025);
        }
        float depthBiased = depth - bias - rpdb;

        float s0 = 1.0;
        if (filterQ == 4u)
        {
            // PCSS (variable penumbra)
            s0 = SampleShadowCascadePCSS(cascade, uv, depthBiased, max(radiusTexels, 1.0), rot);
        }
        else if (filterQ == 3u)
        {
            // Poisson 9-tap (stable, softer)
            s0 = SampleShadowCascadeFiltered(cascade, uv, depthBiased, max(radiusTexels, 0.0), rot);
        }
        else if (filterQ == 2u)
        {
            // 5x5 grid PCF
            s0 = SampleShadowCascadePcfGrid(cascade, uv, depthBiased, max(radiusTexels, 1.0), 2u);
        }
        else if (filterQ == 1u)
        {
            // 3x3 grid PCF (good default)
            s0 = SampleShadowCascadePcfGrid(cascade, uv, depthBiased, max(radiusTexels, 1.0), 1u);
        }
        else
        {
            // Hard (still linear compare to avoid texel-block harshness)
            s0 = gShadowMap.SampleCmpLevelZero(gShadowSamplerLinear, float3(uv, (float)cascade), depthBiased);
        }

        if (blend <= 0.0 || cascade2 == cascade)
            return lerp(1.0, s0, fade);

        // Sample cascade 2 for blending
        float4 lp2 = mul(float4(wposB, 1.0), gLightViewProj[cascade2]);
        float3 ndc2 = lp2.xyz / max(lp2.w, 1e-6);
        float2 uv2 = ndc2.xy * float2(0.5, -0.5) + 0.5;
        if (uv2.x < 0.0 || uv2.x > 1.0 || uv2.y < 0.0 || uv2.y > 1.0 || ndc2.z < 0.0 || ndc2.z > 1.0)
            return s0;

        float depth2 = ndc2.z;
        float rpdb2 = 0.0;
        if (enableRpdb)
        {
            rpdb2 = ReceiverPlaneDepthBias(uv2, depth2);
            rpdb2 = min(rpdb2 * (radiusTexels / 1.25), 0.0025);
        }
        float depth2Biased = depth2 - bias - rpdb2;

        float s1 = 1.0;
        if (filterQ == 4u)
        {
            s1 = SampleShadowCascadePCSS(cascade2, uv2, depth2Biased, max(radiusTexels, 1.0), rot);
        }
        else if (filterQ == 3u)
        {
            s1 = SampleShadowCascadeFiltered(cascade2, uv2, depth2Biased, max(radiusTexels, 0.0), rot);
        }
        else if (filterQ == 2u)
        {
            s1 = SampleShadowCascadePcfGrid(cascade2, uv2, depth2Biased, max(radiusTexels, 1.0), 2u);
        }
        else if (filterQ == 1u)
        {
            s1 = SampleShadowCascadePcfGrid(cascade2, uv2, depth2Biased, max(radiusTexels, 1.0), 1u);
        }
        else
        {
            s1 = gShadowMap.SampleCmpLevelZero(gShadowSamplerLinear, float3(uv2, (float)cascade2), depth2Biased);
        }
        float s = lerp(s0, s1, blend);
        return lerp(1.0, s, fade);
    }
}

static float3 ApplyTonemapACES(float3 x)
{
    // ACES fitted (very common, simple and stable).
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 PSMain(VSOut i) : SV_TARGET
{
    float3 N = normalize(i.nrm);

    const float3 baseColor = i.albedo.rgb * gMatBaseColor.rgb;

#if KING_SHADING_MODEL == 1
    // Unlit: base + emissive (HDR)
    float3 color = baseColor + gMatEmissive;
    return float4(color, i.albedo.a * gMatBaseColor.a);
#elif KING_SHADING_MODEL == 2
    // Rim glow: stylized silhouette glow using emissive
    float3 V = normalize(gCameraPos - i.wpos);
    float ndv = saturate(dot(N, V));
    float rim = pow(saturate(1.0 - ndv), 3.0);
    float3 color = baseColor + gMatEmissive * (rim * 2.5);
    return float4(color, i.albedo.a * gMatBaseColor.a);
#else
    // PBR-ish (current simple lit path)
    float roughness = saturate(gMatRoughness);
    float metallic = saturate(gMatMetallic);

    // Simple hemisphere ambient so the unlit side isn't crushed.
    float hemi = saturate(N.y * 0.5 + 0.5);
    float3 ambient = baseColor * (0.10 + 0.20 * hemi);
    float3 color = ambient;

    // Directional shadow (applies to directional lights only)
    // Debug view selection is encoded in gCascadeSplitsNdc.w (see C++ LightCB setup).
    const uint dbgView = (uint)gCascadeSplitsNdc.w;

    float rawShadow = 1.0;
    float shadow = 1.0;
    const bool receivesShadows = (i.flags & 1u) != 0;
    if (receivesShadows)
    {
        rawShadow = SampleShadowFiltered(i.wpos, N, float4(baseColor, i.albedo.a * gMatBaseColor.a), float2(gMatRoughness, gMatMetallic), i.flags);
        float vis = lerp(saturate(gShadowMinVisibility), 1.0, saturate(rawShadow));
        shadow = lerp(1.0, vis, saturate(gShadowStrength));
    }

    // Debug views:
    // 1 = raw shadow factor (1=lit, 0=shadow)
    // 2 = receivesShadows flag
    // 3 = castsShadows flag (from instance flags)
    if (dbgView == 1u)
        return float4(rawShadow.xxx, 1.0);
    if (dbgView == 2u)
        return float4((receivesShadows ? 1.0 : 0.0).xxx, 1.0);
    if (dbgView == 3u)
        return float4(((i.flags & 2u) != 0 ? 1.0 : 0.0).xxx, 1.0);

    // Apply some shadowing to ambient so shadows are still visible when
    // point lights dominate the scene.
    color *= lerp(1.0, shadow, 0.55);

    // Specular (very small, stable PBR-ish approximation):
    // - F0 blends from dielectric 0.04 to baseColor for metals
    // - roughness drives spec power
    float3 F0 = lerp(0.04.xxx, baseColor, metallic);
    float3 diffColor = baseColor * (1.0 - metallic);

    [loop]
    for (uint li = 0; li < min(gLightCount, (uint)MAX_LIGHTS); ++li)
    {
        LightData Ld = gLights[li];
        if ((i.lightMask & Ld.groupMask) == 0u)
            continue;

        float3 lightColor = Ld.color * Ld.intensity;
        float3 L = 0.0;
        float atten = 1.0;

        if (Ld.type == 0u) // Directional
        {
            L = normalize(-Ld.dir);
        }
        else
        {
            float3 toLight = Ld.pos - i.wpos;
            float dist = length(toLight);
            if (dist > Ld.range)
                continue;
            L = toLight / max(dist, 1e-6);

            float a0 = saturate(1.0 - dist / max(Ld.range, 1e-3));
            atten = a0 * a0;

            if (Ld.type == 2u) // Spot
            {
                float3 spotDir = normalize(Ld.dir);
                float cosTheta = dot(normalize(i.wpos - Ld.pos), spotDir);
                float denom = max(Ld.innerConeCos - Ld.outerConeCos, 1e-5);
                float spot = saturate((cosTheta - Ld.outerConeCos) / denom);
                atten *= spot;
            }
        }

        float ndotl = saturate(dot(N, L));
        if (ndotl <= 1e-5)
            continue;

        float3 V = normalize(gCameraPos - i.wpos);
        float3 H = normalize(L + V);
        float ndoth = saturate(dot(N, H));

        float a = 1.0 - roughness;
        float specPower = lerp(8.0, 256.0, a * a);
        float specTerm = pow(ndoth, specPower);

        float3 diffuse = diffColor * ndotl;
        float3 specular = F0 * (specTerm * ndotl);
        float3 contrib = (diffuse + specular) * atten * lightColor;

        // Apply directional shadow only to directional lights.
        if (Ld.type == 0u)
            contrib *= shadow;

        // Apply point-light shadow only to point lights.
        if (Ld.type == 1u)
        {
            float ps = SamplePointShadowFiltered(i.wpos, Ld.pos, length(i.wpos - Ld.pos));
            contrib *= lerp(1.0, ps, saturate(gPointShadowParams.w));
        }

        color += contrib;
    }

    // Output is HDR (tone mapping is a separate pass).
    color += gMatEmissive;
    return float4(color, i.albedo.a * gMatBaseColor.a);
#endif
}

// MRT variant (HDR + normals) used by SSAO path.
struct PSOut
{
    float4 hdr : SV_TARGET0;
    float4 normal : SV_TARGET1;
};

PSOut PSMainMRT(VSOut i)
{
    float4 hdr = PSMain(i);
    float3 N = normalize(i.nrm);
    PSOut o;
    o.hdr = hdr;
    o.normal = float4(N * 0.5 + 0.5, 1.0);
    return o;
}

// Shadow-only vertex shader (depth-only pass)
float4 VSShadowMain(VSIn v) : SV_POSITION
{
    float4 localPos = float4(v.pos, 1.0);

    float4 col0 = float4(v.iRow0.x, v.iRow1.x, v.iRow2.x, v.iRow3.x);
    float4 col1 = float4(v.iRow0.y, v.iRow1.y, v.iRow2.y, v.iRow3.y);
    float4 col2 = float4(v.iRow0.z, v.iRow1.z, v.iRow2.z, v.iRow3.z);
    float4 col3 = float4(v.iRow0.w, v.iRow1.w, v.iRow2.w, v.iRow3.w);

    float4 wpos;
    wpos.x = dot(localPos, col0);
    wpos.y = dot(localPos, col1);
    wpos.z = dot(localPos, col2);
    wpos.w = dot(localPos, col3);
    return mul(wpos, gShadowViewProj);
}

// Fullscreen post/tonemap
Texture2D<float4> gPostIn : register(t1);
Texture2D<float4> gBloom : register(t3);
SamplerState gLinearClamp : register(s1);

cbuffer PostCB : register(b6)
{
    float gVignetteStrength;
    float gVignettePower;
    float gBloomIntensity;
    float gBloomThreshold;

    float2 gInvBloomSize;
    float2 gInvPostSize;
    float2 _padPostParams;
}

struct FSOut
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

FSOut VSFullscreenMain(uint vid : SV_VertexID)
{
    // Fullscreen triangle with correct UVs.
    // D3D textures have (0,0) at top-left. These UVs are authored so the image
    // is not upside down without any additional flipping.
    float2 p;
    float2 uv;
    if (vid == 0) { p = float2(-1.0, -1.0); uv = float2(0.0, 1.0); }
    else if (vid == 1) { p = float2(-1.0, 3.0); uv = float2(0.0, -1.0); }
    else { p = float2(3.0, -1.0); uv = float2(2.0, 1.0); }

    FSOut o;
    o.pos = float4(p, 0.0, 1.0);
    o.uv = uv;
    return o;
}

// Back-compat entrypoint (existing C++ compiled this by name).
FSOut VSTonemapMain(uint vid : SV_VertexID)
{
    return VSFullscreenMain(vid);
}

static float3 LinearToSrgb(float3 x)
{
    x = saturate(x);
    float3 lo = x * 12.92;
    float3 hi = 1.055 * pow(x, 1.0 / 2.4) - 0.055;
    float3 m = step(0.0031308, x);
    return lerp(lo, hi, m);
}

float4 PSVignetteMain(FSOut i) : SV_TARGET
{
    float3 hdr = gPostIn.Sample(gLinearClamp, i.uv).rgb;

    // Aspect-correct radial vignette.
    float2 uv = saturate(i.uv);
    float2 p = uv - 0.5;

    // invPostSize = (1/W, 1/H) => aspect = W/H.
    float invW = max(gInvPostSize.x, 1e-6);
    float invH = max(gInvPostSize.y, 1e-6);
    float aspect = invH / invW;

    float2 q = float2(aspect, 1.0);
    float r = length(p * q);

    // Normalize so corners map near 1.0.
    float norm = 2.0 / sqrt(aspect * aspect + 1.0);
    float rn = saturate(r * norm);

    float power = max(gVignettePower, 0.25);
    float v = pow(rn, power);
    float strength = saturate(gVignetteStrength);
    float mul = 1.0 - strength * v;
    return float4(hdr * mul, 1.0);
}

float4 PSBloomExtractMain(FSOut i) : SV_TARGET
{
    float3 hdr = gPostIn.Sample(gLinearClamp, i.uv).rgb;
    float thr = max(gBloomThreshold, 0.0);

    // Simple bright-pass (keep only components above threshold).
    float3 bright = max(hdr - thr.xxx, 0.0);
    return float4(bright, 1.0);
}

static float3 BloomBlur(float2 uv, float2 dir)
{
    // 5-tap separable blur (fast, good enough for a first bloom).
    // dir should be (invW,0) for H, (0,invH) for V.
    float3 c0 = gPostIn.Sample(gLinearClamp, uv).rgb;
    float3 c1 = gPostIn.Sample(gLinearClamp, uv + dir * 1.0).rgb;
    float3 c2 = gPostIn.Sample(gLinearClamp, uv - dir * 1.0).rgb;
    float3 c3 = gPostIn.Sample(gLinearClamp, uv + dir * 2.0).rgb;
    float3 c4 = gPostIn.Sample(gLinearClamp, uv - dir * 2.0).rgb;

    // Approx gaussian-ish weights.
    return c0 * 0.40 + (c1 + c2) * 0.24 + (c3 + c4) * 0.06;
}

float4 PSBloomBlurHMain(FSOut i) : SV_TARGET
{
    float2 dir = float2(max(gInvBloomSize.x, 0.0), 0.0);
    return float4(BloomBlur(i.uv, dir), 1.0);
}

float4 PSBloomBlurVMain(FSOut i) : SV_TARGET
{
    float2 dir = float2(0.0, max(gInvBloomSize.y, 0.0));
    return float4(BloomBlur(i.uv, dir), 1.0);
}

float4 PSTonemapMain(FSOut i) : SV_TARGET
{
    float3 hdr = gPostIn.Sample(gLinearClamp, i.uv).rgb;

    // Optional bloom (already blurred). Add in HDR before tonemap.
    float bloomIntensity = max(gBloomIntensity, 0.0);
    if (bloomIntensity > 1e-4)
    {
        float3 b = gBloom.Sample(gLinearClamp, i.uv).rgb;
        hdr += b * bloomIntensity;
    }

    // Optional AO integration.
    float aoStrength = saturate(gAoStrength);
    if (aoStrength > 1e-3)
    {
        float ao = gSsao.Sample(gLinearClamp, i.uv).r;
        hdr *= lerp(1.0, ao, aoStrength);
    }

    hdr *= max(gExposure, 0.0);
    float3 ldr = ApplyTonemapACES(hdr);
    // Swapchain backbuffer is typically UNORM (linear). Convert to sRGB for correct display.
    ldr = LinearToSrgb(ldr);
    return float4(ldr, 1.0);
}

// SSAO pass
static float Hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

static float3 ReconstructSsaoPosition(float2 uv)
{
    float z = gDepth.Sample(gPointClamp, uv).r;

    // Convert UV (top-left origin) to NDC (+Y up).
    float2 ndc;
    ndc.x = uv.x * 2.0 - 1.0;
    ndc.y = (1.0 - uv.y) * 2.0 - 1.0;

    float4 clip = float4(ndc, z, 1.0);
    float4 p = mul(clip, gSsaoInvProj);
    p.xyz /= max(p.w, 1e-6);
    return p.xyz;
}

float4 PSSsaoMain(FSOut i) : SV_TARGET
{
    float3 nW = normalize(gNormal.Sample(gPointClamp, i.uv).xyz * 2.0 - 1.0);
    float3 n = normalize(mul(float4(nW, 0.0), gSsaoView).xyz);
    float3 p = ReconstructSsaoPosition(i.uv);

    // Degenerate normal (likely background)
    if (dot(n, n) < 0.25)
        return 1.0;

    // Build a basis around the normal.
    float r = Hash12(i.uv / max(gSsaoInvTargetSize, 1e-6));
    float3 randDir = normalize(float3(r * 2.0 - 1.0, frac(r * 13.37) * 2.0 - 1.0, frac(r * 7.77) * 2.0 - 1.0));
    float3 t = normalize(randDir - n * dot(randDir, n));
    float3 b = cross(n, t);

    float occ = 0.0;
    const int kSamples = 8;
    float3 samples[kSamples] = {
        float3( 0.25, 0.05, 0.15), float3(-0.20, 0.10, 0.25), float3( 0.05, 0.25, 0.10), float3(-0.30,-0.05, 0.20),
        float3( 0.35, 0.10, 0.40), float3(-0.10, 0.35, 0.30), float3( 0.10,-0.35, 0.30), float3(-0.35, 0.15, 0.45)
    };

    [unroll]
    for (int si = 0; si < kSamples; ++si)
    {
        float3 s = samples[si];
        float3 dir = normalize(t * s.x + b * s.y + n * abs(s.z));
        float3 p2 = p + dir * gSsaoRadius;

        float4 clip = mul(float4(p2, 1.0), gSsaoProj);
        float3 ndc = clip.xyz / max(clip.w, 1e-6);

        float2 uv;
        uv.x = ndc.x * 0.5 + 0.5;
        uv.y = 1.0 - (ndc.y * 0.5 + 0.5);

        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
            continue;

        float sceneZ = gDepth.Sample(gPointClamp, uv).r;
        float testZ = ndc.z - gSsaoBias;
        occ += (sceneZ < testZ) ? 1.0 : 0.0;
    }

    float ao = 1.0 - (occ / (float)kSamples);
    ao = saturate(ao);
    return float4(ao, ao, ao, 1.0);
}

float4 PSBlurMain(FSOut i) : SV_TARGET
{
    float2 o = gSsaoInvTargetSize;
    float sum = 0.0;
    sum += gSsao.Sample(gLinearClamp, i.uv + float2(-o.x, 0)).r;
    sum += gSsao.Sample(gLinearClamp, i.uv + float2( o.x, 0)).r;
    sum += gSsao.Sample(gLinearClamp, i.uv + float2(0, -o.y)).r;
    sum += gSsao.Sample(gLinearClamp, i.uv + float2(0,  o.y)).r;
    sum += gSsao.Sample(gLinearClamp, i.uv).r;
    float ao = sum / 5.0;
    return float4(ao, ao, ao, 1.0);
}
