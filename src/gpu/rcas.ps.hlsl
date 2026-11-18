// FSR 1.0 RCAS (robust contrast-adaptive sharpening) pixel shader against the
// rexglue pack binding contract, the second pass of the performance_mode seed,
// from the EASU output to the full-res composite+UI variant.

// Follows AMD FidelityFX-FSR
// ffx_fsr1.h FsrRcasF (MIT), denoise off.

// renderer_rcas_ps.h holds the bin2c output of dxc -T ps_6_0 -E main -O3
// -Fo rcas.ps.dxil rcas.ps.hlsl.

// SeedFullResVariant fills b4 with
//   [0].x = source texture descriptor (the EASU scratch),
//   [1].x = asuint(con), con = exp2(-sharpness_stops), and
//   [2].xy = source dimensions (uint W, H) for load clamping.
cbuffer XeDescriptorIndices : register(b4, space0)
{
    uint4 xe_descriptor_indices[8];
};
Texture2DArray<float4> xe_textures2d[] : register(t0, space1);
SamplerState xe_samplers[] : register(s0, space0);

float3 Tap(uint desc, int2 p, int2 dim)
{
    p = clamp(p, int2(0, 0), dim - 1);
    return xe_textures2d[NonUniformResourceIndex(desc)]
        .Load(int4(p, 0, 0)).rgb;
}

void main(in float4 pos : SV_Position, in float2 uv : TEXCOORD0,
          out float4 color : SV_Target0)
{
    const uint desc = xe_descriptor_indices[0].x;
    const float con = asfloat(xe_descriptor_indices[1].x);
    const int2 dim = int2(xe_descriptor_indices[2].xy);
    const int2 sp = int2(pos.xy);

    //   b
    // d e f
    //   h
    float3 cb = Tap(desc, sp + int2(0, -1), dim);
    float3 cd = Tap(desc, sp + int2(-1, 0), dim);
    float3 ce = Tap(desc, sp, dim);
    float3 cf = Tap(desc, sp + int2(1, 0), dim);
    float3 ch = Tap(desc, sp + int2(0, 1), dim);

    // Per-channel minimum and maximum of the four neighbors.
    float3 mn4 = min(min(cb, cd), min(cf, ch));
    float3 mx4 = max(max(cb, cd), max(cf, ch));
    // The sharpening lobe with peak (1, -4) is limited so that no output
    // exceeds the local range.
    const float2 peakC = float2(1.0, -4.0);
    float3 hitMin = mn4 / (4.0 * mx4 + 1.0 / 32768.0);
    float3 hitMax = (peakC.x - mx4) / (4.0 * mn4 + peakC.y - 1.0 / 32768.0);
    float3 lobeRGB = max(-hitMin, hitMax);
    // FSR_RCAS_LIMIT = 0.25 - 1/16.
    float lobe = max(-(0.25 - 1.0 / 16.0),
                     min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) *
                 con;
    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    color = float4(((cb + cd + cf + ch) * lobe + ce) * rcpL, 1.0);
}
