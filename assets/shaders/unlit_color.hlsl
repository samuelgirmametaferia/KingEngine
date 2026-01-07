// Minimal custom shader example for King (D3D11).
// This matches the engine's fixed vertex + instance input layout.
//
// Required entry points for geometry:
//   VSMain
//   PSMain
// Optional (needed when SSAO MRT path is enabled):
//   PSMainMRT (writes HDR + Normal target)
//
// Engine binding contract (geometry pass):
//   b0: CameraCB
//   b1: LightCB (optional)
//   t0/s0/s1/s3: shadow map + samplers (optional)
//   b4: MaterialCB (optional)
//   t5..t8: material textures (optional)
//   s4: material sampler (optional)

cbuffer CameraCB : register(b0)
{
    row_major float4x4 gViewProj;
    float3 gCameraPos;
    float gExposure;

    float gAoStrength;
    float3 _padPost;
};

cbuffer MaterialCB : register(b4)
{
    float4 gMatBaseColor;
    float3 gMatEmissive;
    float gMatRoughness;

    float gMatMetallic;
    uint gMatFlags;
    float2 _padMat;
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

    float2 rm : TEXCOORD8;
    uint lightMask : TEXCOORD9;
    uint flags : TEXCOORD10;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float3 nrm : TEXCOORD0;
    float4 col : COLOR0;
};

VSOut VSMain(VSIn input)
{
    VSOut o;

    // Engine provides world matrix as ROWS in iRow*. Build COLUMNS explicitly.
    float4 localPos = float4(input.pos, 1.0);
    float4 col0 = float4(input.iRow0.x, input.iRow1.x, input.iRow2.x, input.iRow3.x);
    float4 col1 = float4(input.iRow0.y, input.iRow1.y, input.iRow2.y, input.iRow3.y);
    float4 col2 = float4(input.iRow0.z, input.iRow1.z, input.iRow2.z, input.iRow3.z);
    float4 col3 = float4(input.iRow0.w, input.iRow1.w, input.iRow2.w, input.iRow3.w);

    float4 wpos;
    wpos.x = dot(localPos, col0);
    wpos.y = dot(localPos, col1);
    wpos.z = dot(localPos, col2);
    wpos.w = dot(localPos, col3);
    o.pos = mul(wpos, gViewProj);

    // Normal matrix is also provided as ROWS.
    float4 localNrm = float4(input.nrm, 0.0);
    float4 ncol0 = float4(input.nRow0.x, input.nRow1.x, input.nRow2.x, input.nRow3.x);
    float4 ncol1 = float4(input.nRow0.y, input.nRow1.y, input.nRow2.y, input.nRow3.y);
    float4 ncol2 = float4(input.nRow0.z, input.nRow1.z, input.nRow2.z, input.nRow3.z);
    float3 wn;
    wn.x = dot(localNrm, ncol0);
    wn.y = dot(localNrm, ncol1);
    wn.z = dot(localNrm, ncol2);
    o.nrm = normalize(wn);

    o.col = input.albedo * gMatBaseColor + float4(gMatEmissive, 0.0);
    return o;
}

float4 PSMain(VSOut input) : SV_Target0
{
    return input.col;
}

struct PSOutMRT
{
    float4 hdr : SV_Target0;
    float4 normal : SV_Target1;
};

PSOutMRT PSMainMRT(VSOut input)
{
    PSOutMRT o;
    o.hdr = input.col;
    // Encode normal for SSAO path (same convention as pbr_test.hlsl: 0.5 + 0.5*n)
    o.normal = float4(input.nrm * 0.5f + 0.5f, 1.0f);
    return o;
}
