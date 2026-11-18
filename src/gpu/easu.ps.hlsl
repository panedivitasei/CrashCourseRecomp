// FSR 1.0 EASU (edge-adaptive spatial upsampling) pixel shader against the
// rexglue pack binding contract, with which the performance_mode seed blit
// reconstructs the half-res scene into the full-res composite+UI variant.

// Algorithm follows AMD FidelityFX-FSR ffx_fsr1.h
// (FsrEasuF), MIT-licensed; 12-tap Load variant (no gathers) for clarity.

// renderer_easu_ps.h holds the bin2c output of dxc -T ps_6_0 -E main -O3
// -Fo easu.ps.dxil easu.ps.hlsl.

// SeedFullResVariant fills b4 with
//   [0].x = source texture descriptor index (half-res sibling color[0]),
//   [1]   = asuint(con0) = { inW/outW, inH/outH, 0.5*inW/outW - 0.5,
//                            0.5*inH/outH - 0.5 }, and
//   [2].xy = input texture dimensions (uint W, H) for load clamping.
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

// Luma proxy of the FsrEasuF kernel for edge analysis.
float Luma(float3 c) { return c.g * 2.0 + (c.r + c.b) * 0.5; }

// FsrEasuSet accumulates edge direction and feature length from one corner of
// the inner quad, weighted by the bilinear weight of the output sample point.
void EasuSet(inout float2 dir, inout float len, float w,
             float lA, float lB, float lC, float lD, float lE)
{
    // The direction comes from the luma gradients around corner C, with A
    // above, B left, D right and E below.
    float lenX = max(abs(lD - lC), abs(lC - lB));
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) / max(lenX, 1.0 / 32768.0));
    lenX *= lenX;
    len += lenX * w;
    float lenY = max(abs(lE - lC), abs(lC - lA));
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) / max(lenY, 1.0 / 32768.0));
    lenY *= lenY;
    len += lenY * w;
}

// FsrEasuTap weights one tap with an anisotropic elliptical 2-lobe kernel
// around the sample point.
void EasuTap(inout float3 aC, inout float aW, float2 off, float2 dir,
             float2 len, float lob, float clp, float3 c)
{
    // The tap offset rotates into the edge frame and scales by the ellipse.
    float2 v;
    v.x = off.x * dir.x + off.y * dir.y;
    v.y = off.x * -dir.y + off.y * dir.x;
    v *= len;
    float d2 = min(dot(v, v), clp);
    // The window is (25/16 * (2/5 * d2 - 1)^2 - (25/16 - 1)) * (lob * d2 - 1)^2.
    float wB = 2.0 / 5.0 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

void main(in float4 pos : SV_Position, in float2 uv : TEXCOORD0,
          out float4 color : SV_Target0)
{
    const uint desc = xe_descriptor_indices[0].x;
    const float4 con0 = asfloat(xe_descriptor_indices[1]);
    const int2 dim = int2(xe_descriptor_indices[2].xy);

    // The output integer pixel maps to an input sample position in texel space.
    float2 pp = floor(pos.xy) * con0.xy + con0.zw;
    float2 fp = floor(pp);
    pp -= fp;
    int2 sp = int2(fp);

    //    b c
    //  e f g h
    //  i j k l
    //    n o
    float3 cb = Tap(desc, sp + int2(0, -1), dim);
    float3 cc = Tap(desc, sp + int2(1, -1), dim);
    float3 ce = Tap(desc, sp + int2(-1, 0), dim);
    float3 cf = Tap(desc, sp + int2(0, 0), dim);
    float3 cg = Tap(desc, sp + int2(1, 0), dim);
    float3 ch = Tap(desc, sp + int2(2, 0), dim);
    float3 ci = Tap(desc, sp + int2(-1, 1), dim);
    float3 cj = Tap(desc, sp + int2(0, 1), dim);
    float3 ck = Tap(desc, sp + int2(1, 1), dim);
    float3 cl = Tap(desc, sp + int2(2, 1), dim);
    float3 cn = Tap(desc, sp + int2(0, 2), dim);
    float3 co = Tap(desc, sp + int2(1, 2), dim);

    float lb = Luma(cb), lc = Luma(cc), le = Luma(ce), lf = Luma(cf);
    float lg = Luma(cg), lh = Luma(ch), li = Luma(ci), lj = Luma(cj);
    float lk = Luma(ck), ll = Luma(cl), ln = Luma(cn), lo = Luma(co);

    // Edge direction and length come from the four inner-quad corners, bilinear
    // weighted, with the corner and neighbor assignments of FsrEasuF.
    float2 dir = 0.0;
    float len = 0.0;
    EasuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), lb, le, lf, lg, lj); // f
    EasuSet(dir, len, pp.x * (1.0 - pp.y),         lc, lf, lg, lh, lk); // g
    EasuSet(dir, len, (1.0 - pp.x) * pp.y,         lf, li, lj, lk, ln); // j
    EasuSet(dir, len, pp.x * pp.y,                 lg, lj, lk, ll, lo); // k

    // The direction is normalized, and flat areas fall back to axis-aligned.
    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < 1.0 / 32768.0;
    dirR = rsqrt(max(dirR, 1.0 / 32768.0));
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;

    // The feature length shapes the kernel by stretching it along the edge.
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) /
                    max(abs(dir.x), abs(dir.y));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;

    // The 12 taps accumulate through the anisotropic kernel.
    float3 aC = 0.0;
    float aW = 0.0;
    EasuTap(aC, aW, float2(0.0, -1.0) - pp, dir, len2, lob, clp, cb);
    EasuTap(aC, aW, float2(1.0, -1.0) - pp, dir, len2, lob, clp, cc);
    EasuTap(aC, aW, float2(-1.0, 0.0) - pp, dir, len2, lob, clp, ce);
    EasuTap(aC, aW, float2(0.0, 0.0) - pp, dir, len2, lob, clp, cf);
    EasuTap(aC, aW, float2(1.0, 0.0) - pp, dir, len2, lob, clp, cg);
    EasuTap(aC, aW, float2(2.0, 0.0) - pp, dir, len2, lob, clp, ch);
    EasuTap(aC, aW, float2(-1.0, 1.0) - pp, dir, len2, lob, clp, ci);
    EasuTap(aC, aW, float2(0.0, 1.0) - pp, dir, len2, lob, clp, cj);
    EasuTap(aC, aW, float2(1.0, 1.0) - pp, dir, len2, lob, clp, ck);
    EasuTap(aC, aW, float2(2.0, 1.0) - pp, dir, len2, lob, clp, cl);
    EasuTap(aC, aW, float2(0.0, 2.0) - pp, dir, len2, lob, clp, cn);
    EasuTap(aC, aW, float2(1.0, 2.0) - pp, dir, len2, lob, clp, co);

    float3 pix = aC / max(aW, 1.0 / 32768.0);
    // Deringing clamps the result to the range of the inner quad.
    float3 mn = min(min(cf, cg), min(cj, ck));
    float3 mx = max(max(cf, cg), max(cj, ck));
    color = float4(clamp(pix, mn, mx), 1.0);
}
