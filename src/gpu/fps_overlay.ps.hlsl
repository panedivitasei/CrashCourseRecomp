cbuffer XeDescriptorIndices : register(b4, space0)
{
    uint4 xe_descriptor_indices[8];
};
Texture2DArray<float4> xe_textures2d[] : register(t0, space1);
SamplerState xe_samplers[] : register(s0, space0);

void main(in float4 pos : SV_Position, in float2 uv : TEXCOORD0, out float4 color : SV_Target0)
{
    // Alpha-preserving variant of the blit pixel shader, which lets the
    // transparent background of the FPS strip reach the alpha blender.
    color = xe_textures2d[NonUniformResourceIndex(xe_descriptor_indices[0].x)]
        .SampleLevel(xe_samplers[0], float3(uv, 0.0), 0.0);
}
