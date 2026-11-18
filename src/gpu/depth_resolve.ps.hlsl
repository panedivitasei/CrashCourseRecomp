// Depth-resolve pixel shader that transfers a region of a depth render target
// texel for texel into an R32_FLOAT resolve destination by reading the depth
// through an R32_FLOAT SRV, which avoids the whole-plane staging copy D3D12
// requires for a partial copy out of a depth-stencil resource.

// renderer_depth_ps.h holds the bin2c output of dxc -T ps_6_0 -E main -O3
// -Fo depth_resolve.ps.dxil depth_resolve.ps.hlsl.

// The depth-resolve path in ResolveRegion fills b4 with
//   [0].x = source depth descriptor index (R32_FLOAT view of the DSV),
//   [1].xy = asint(src_origin - dst_origin), added to SV_Position to reach
//            the source texel, and
//   [2].xy = source extent (w, h) for clamping a resolve rect that exceeds
//            the source.
cbuffer XeDescriptorIndices : register(b4, space0)
{
    uint4 xe_descriptor_indices[8];
};
Texture2DArray<float4> xe_textures2d[] : register(t0, space1);

void main(in float4 pos : SV_Position, out float depth : SV_Target0)
{
    const uint desc = xe_descriptor_indices[0].x;
    const int2 delta = int2(asint(xe_descriptor_indices[1].x),
                            asint(xe_descriptor_indices[1].y));
    const int2 dim = int2(xe_descriptor_indices[2].xy);
    const int2 sp = clamp(int2(pos.xy) + delta, int2(0, 0), dim - 1);
    depth = xe_textures2d[NonUniformResourceIndex(desc)]
                .Load(int4(sp, 0, 0)).r;
}
