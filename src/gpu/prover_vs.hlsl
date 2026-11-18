// Pack-prover vertex shader with NINTERP (0-16) constant interpolants read from
// float constant rows 240 and up, compiled once per count because the PSO path
// requires an exact VS/PS signature match.

// tools/gen_prover_vs.sh regenerates the header with a dxc loop over the 17
// variants.
cbuffer XeFloatConstants : register(b1, space0)
{
    float4 xe_fc[256];
};

void main(uint id : SV_VertexID,
          out float4 pos : SV_Position
#if NINTERP >= 1
          , out float4 v0 : TEXCOORD0
#endif
#if NINTERP >= 2
          , out float4 v1 : TEXCOORD1
#endif
#if NINTERP >= 3
          , out float4 v2 : TEXCOORD2
#endif
#if NINTERP >= 4
          , out float4 v3 : TEXCOORD3
#endif
#if NINTERP >= 5
          , out float4 v4 : TEXCOORD4
#endif
#if NINTERP >= 6
          , out float4 v5 : TEXCOORD5
#endif
#if NINTERP >= 7
          , out float4 v6 : TEXCOORD6
#endif
#if NINTERP >= 8
          , out float4 v7 : TEXCOORD7
#endif
#if NINTERP >= 9
          , out float4 v8 : TEXCOORD8
#endif
#if NINTERP >= 10
          , out float4 v9 : TEXCOORD9
#endif
#if NINTERP >= 11
          , out float4 v10 : TEXCOORD10
#endif
#if NINTERP >= 12
          , out float4 v11 : TEXCOORD11
#endif
#if NINTERP >= 13
          , out float4 v12 : TEXCOORD12
#endif
#if NINTERP >= 14
          , out float4 v13 : TEXCOORD13
#endif
#if NINTERP >= 15
          , out float4 v14 : TEXCOORD14
#endif
#if NINTERP >= 16
          , out float4 v15 : TEXCOORD15
#endif
          )
{
    float2 xy = float2((id == 1) ? 3.0 : -1.0, (id == 2) ? -3.0 : 1.0);
    pos = float4(xy, 0.5, 1.0);
#if NINTERP >= 1
    v0 = xe_fc[240];
#endif
#if NINTERP >= 2
    v1 = xe_fc[241];
#endif
#if NINTERP >= 3
    v2 = xe_fc[242];
#endif
#if NINTERP >= 4
    v3 = xe_fc[243];
#endif
#if NINTERP >= 5
    v4 = xe_fc[244];
#endif
#if NINTERP >= 6
    v5 = xe_fc[245];
#endif
#if NINTERP >= 7
    v6 = xe_fc[246];
#endif
#if NINTERP >= 8
    v7 = xe_fc[247];
#endif
#if NINTERP >= 9
    v8 = xe_fc[248];
#endif
#if NINTERP >= 10
    v9 = xe_fc[249];
#endif
#if NINTERP >= 11
    v10 = xe_fc[250];
#endif
#if NINTERP >= 12
    v11 = xe_fc[251];
#endif
#if NINTERP >= 13
    v12 = xe_fc[252];
#endif
#if NINTERP >= 14
    v13 = xe_fc[253];
#endif
#if NINTERP >= 15
    v14 = xe_fc[254];
#endif
#if NINTERP >= 16
    v15 = xe_fc[255];
#endif
}
