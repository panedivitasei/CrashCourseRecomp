// ImGui overlay pixel shader that multiplies the vertex color by the texture and
// sampler named in xe_descriptor_indices[0] and preserves alpha for the blender.
cbuffer XeDescriptorIndices : register(b4, space0)
{
    uint4 xe_descriptor_indices[8];
};
Texture2DArray<float4> xe_textures2d[] : register(t0, space1);
SamplerState xe_samplers[] : register(s0, space0);

void main(in float4 pos : SV_Position, in float2 uv : TEXCOORD0, in float4 vertex_color : COLOR0,
          out float4 color : SV_Target0)
{
    color = vertex_color *
        xe_textures2d[NonUniformResourceIndex(xe_descriptor_indices[0].x)]
            .SampleLevel(xe_samplers[xe_descriptor_indices[0].y], float3(uv, 0.0), 0.0);
}
