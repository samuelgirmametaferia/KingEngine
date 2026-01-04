// Validation shader for King.
// Goal: make the renderer feel "complete" without exploding complexity.

#define MAX_LIGHTS 16

cbuffer CameraCB : register(b0)
{
    row_major float4x4 gViewProj;
    float3 gCameraPos;
    float gExposure;
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

    row_major float4x4 gLightViewProj;
    float2 gShadowTexelSize;
    float gShadowBias;
    float gShadowStrength;
};

Texture2D<float> gShadowMap : register(t0);
SamplerComparisonState gShadowSampler : register(s0);

struct VSIn
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;

    // Per-instance data (slot 1)
    float4 iRow0 : TEXCOORD4;
    float4 iRow1 : TEXCOORD5;
    float4 iRow2 : TEXCOORD6;
    float4 iRow3 : TEXCOORD7;
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

    // Note: this is not the correct inverse-transpose normal transform for non-uniform scale,
    // but it's fine for our current validation scene.
    float4 localNrm = float4(v.nrm, 0.0);
    o.nrm.x = dot(localNrm, col0);
    o.nrm.y = dot(localNrm, col1);
    o.nrm.z = dot(localNrm, col2);
    o.albedo = v.albedo;
    o.rm = v.rm;
    o.lightMask = v.lightMask;
    o.flags = v.flags;
    return o;
}

static float SampleShadowPCF(float3 wpos)
{
    if (gShadowStrength <= 0.0)
        return 1.0;

    float4 lp = mul(float4(wpos, 1.0), gLightViewProj);
    float3 ndc = lp.xyz / max(lp.w, 1e-6);

    float2 uv = ndc.xy * 0.5 + 0.5;
    // Outside shadow map: treat as lit.
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return 1.0;
    if (ndc.z < 0.0 || ndc.z > 1.0)
        return 1.0;

    float depth = ndc.z - gShadowBias;

    float sum = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 o = float2((float)x, (float)y) * gShadowTexelSize;
            sum += gShadowMap.SampleCmpLevelZero(gShadowSampler, uv + o, depth);
        }
    }
    return sum / 9.0;
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

    const float3 baseColor = i.albedo.rgb;
    float3 color = 0.05 * baseColor; // ambient

    // Directional shadow (applies to directional lights only)
    float shadow = 1.0;
    const bool receivesShadows = (i.flags & 1u) != 0;
    if (receivesShadows)
        shadow = lerp(1.0, SampleShadowPCF(i.wpos), gShadowStrength);

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
        float3 contrib = ndotl * atten * (baseColor * lightColor);

        // Apply directional shadow only to directional lights.
        if (Ld.type == 0u)
            contrib *= shadow;

        color += contrib;
    }

    // Output is HDR (tone mapping is a separate pass).
    return float4(color, i.albedo.a);
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
    return mul(wpos, gLightViewProj);
}

// Fullscreen tonemap pass
Texture2D<float4> gHdr : register(t1);
SamplerState gLinearClamp : register(s1);

struct FSOut
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

FSOut VSTonemapMain(uint vid : SV_VertexID)
{
    // Fullscreen triangle
    float2 p;
    if (vid == 0) p = float2(-1.0, -1.0);
    else if (vid == 1) p = float2(-1.0, 3.0);
    else p = float2(3.0, -1.0);

    FSOut o;
    o.pos = float4(p, 0.0, 1.0);
    o.uv = p * 0.5 + 0.5;
    return o;
}

float4 PSTonemapMain(FSOut i) : SV_TARGET
{
    float3 hdr = gHdr.Sample(gLinearClamp, i.uv).rgb;
    hdr *= max(gExposure, 0.0);
    float3 ldr = ApplyTonemapACES(hdr);
    return float4(ldr, 1.0);
}
