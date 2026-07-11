// Selected-object outline composite (park editor / object mover): port of
// postfx_edgedetectstencilPS. The game stencil-marks the selected object,
// resolves stencil to a 576x320 texture and adds |threshold crossings| x
// the outline color (PS c0, the editor blue) onto the frame; our mask pass
// renders the selected items into a small R8 target instead (see
// pso_outline_mask) and this pass composites additively over the resolved
// output. Tap offsets are 1/576 x 1/320 UV fractions like the original, so
// the contour thickness matches at any output resolution.
cbuffer C : register(b0) {
  float4 color;  // rgb = outline color (guest edge-detect PS c0)
};
Texture2D<float4> src : register(t0);
SamplerState smp_clamp : register(s1);
struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};
VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  o.uv = uv;
  return o;
}
float4 ps_main(VSOut i) : SV_Target {
  // Averaged 5x5 neighborhood of the (binary, full-resolution) mask with a
  // fixed UV-fraction pitch -> a smooth 0..1 ramp across the silhouette
  // boundary; the peaked profile 4m(1-m) turns the ramp into an
  // antialiased line of constant screen-fraction width. The core is
  // whitened (the emulated line goes through the game's uber grade +
  // bloom, which lifts its center toward white). A literal port:
  // binarized crossing counts on the 576x320 grid, stairstepped visibly
  // at native output resolutions.
  const float2 t = float2(0.8 / 1152.0, 0.8 / 640.0);
  float m = 0.0;
  [unroll] for (int y = -2; y <= 2; ++y) {
    [unroll] for (int x = -2; x <= 2; ++x) {
      m += src.SampleLevel(smp_clamp, i.uv + float2(x, y) * t, 0).r;
    }
  }
  m /= 25.0;
  float band = saturate(6.4 * m * (1.0 - m));
  float core = band * band * band;
  // Additive blend (ONE, ONE) onto the frame; alpha untouched (ZERO, ONE).
  return float4(color.rgb * band + 0.5 * core, 0.0);
}
