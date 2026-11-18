// ImGui overlay vertex shader that passes through positions the renderer maps
// to clip space when it copies the vertices.
void main(in float2 in_pos : POSITION, in float2 in_uv : TEXCOORD0, in float4 in_color : COLOR0,
          out float4 pos : SV_Position, out float2 uv : TEXCOORD0, out float4 color : COLOR0)
{
    pos = float4(in_pos, 0.0, 1.0);
    uv = in_uv;
    color = in_color;
}
