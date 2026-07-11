// 2D/APT overlay shader. Verified against a captured HUD draw stream:
// vertices are {float4 pos, float2 uv} in 1280x720
// APT movie space; the game's VS constants apply as clip = ortho * (world *
// pos) with c0..c3 the ortho rows, c4..c7 the element's transform rows and
// c8 the color multiplier, used exactly as staged.
cbuffer C : register(b0) {
  float4 m[10];  // m[0..3] proj rows, m[4..7] world rows, m[8] color,
                 // m[9].x = apply D3D9 half-pixel (2D ortho draws only)
};
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s1);
struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
  float4 color : COLOR0;
};
VSOut vs_main(float4 p : POSITION, float2 uv : TEXCOORD0, float4 color : COLOR0) {
  float4 wp = float4(dot(p, m[4]), dot(p, m[5]), dot(p, m[6]), dot(p, m[7]));
  VSOut o;
  o.pos = float4(dot(wp, m[0]), dot(wp, m[1]), dot(wp, m[2]), dot(wp, m[3]));
  // D3D9 half-pixel convention: the art bakes half-texel UVs expecting
  // pixel centers at integer coordinates; without this the clock-face
  // quadrant tiles show their wrapped border rows as dark seam lines.
  // Scaled for the 2D ortho; must not apply to 3D (world-space
  // SimpleDraw markers), where m[0].x is a projection scale.
  if (m[9].x > 0.0) {
    o.pos.x -= 0.5 * m[0].x * o.pos.w;
    o.pos.y -= 0.5 * m[1].y * o.pos.w;
  }
  o.uv = uv;
  o.color = color;
  return o;
}
float4 ps_main(VSOut i) : SV_Target {
  return tex.Sample(smp, i.uv) * m[8] * i.color;
}
