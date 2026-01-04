// Minimal lit shader used to validate King ECS rendering.
// This is NOT full PBR yet.

cbuffer CameraCB : register(b0)
{
    row_major float4x4 gViewProj;
};

cbuffer LightCB : register(b1)
{
    float3 gLightDir;
    float gLightIntensity;
    float3 gLightColor;
    float _pad0;
};

cbuffer ObjectCB : register(b2)
{
    row_major float4x4 gWorld;
    float4 gAlbedo;
};

struct VSIn
{
    float3 pos : POSITION;
    float3 nrm : NORMAL;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float3 nrm : TEXCOORD0;
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    float4 wpos = mul(float4(v.pos, 1.0), gWorld);
    o.pos = mul(wpos, gViewProj);
    o.nrm = mul(v.nrm, (float3x3)gWorld);
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET
{
    float3 N = normalize(i.nrm);
    float3 L = normalize(-gLightDir);
    float ndotl = saturate(dot(N, L));

    float3 ambient = 0.08 * gAlbedo.rgb;
    float3 diffuse = (ndotl * gLightIntensity) * (gAlbedo.rgb * gLightColor);
    return float4(ambient + diffuse, gAlbedo.a);
}
