// Rim/outline glow shader (custom material demo for King).
// Produces a stylized "outline" via rim lighting and emissive.
//
// Entry points:
//   VSMain
//   PSMain
//   PSMainMRT (optional, required when SSAO MRT path is enabled)

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

    // Inverse-transpose of world
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
    float3 wpos : TEXCOORD0;
    float3 nrm  : TEXCOORD1;
    float4 col  : COLOR0;
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
    o.wpos = wpos.xyz;

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

    o.col = input.albedo * gMatBaseColor;
    return o;
}

static float3 ApplyRim(float3 baseRgb, float3 nrm, float3 wpos)
{
    float3 V = normalize(gCameraPos - wpos);
    float ndv = saturate(dot(nrm, V));

    // Rim factor: stronger near silhouettes.
    float rim = pow(saturate(1.0 - ndv), 3.0);

    // Use emissive as the rim color; allow overbright HDR ("glow") by scaling.
    float3 rimColor = gMatEmissive;
    float rimStrength = 2.5;

    float3 outRgb = baseRgb + rimColor * (rim * rimStrength);
    return outRgb;
}

float4 PSMain(VSOut input) : SV_Target0
{
    float3 rgb = ApplyRim(input.col.rgb, input.nrm, input.wpos);
    return float4(rgb, input.col.a);
}

struct PSOutMRT
{
    float4 hdr : SV_Target0;
    float4 normal : SV_Target1;
};

PSOutMRT PSMainMRT(VSOut input)
{
    PSOutMRT o;
    float3 rgb = ApplyRim(input.col.rgb, input.nrm, input.wpos);
    o.hdr = float4(rgb, input.col.a);
    o.normal = float4(input.nrm * 0.5f + 0.5f, 1.0f);
    return o;
}
