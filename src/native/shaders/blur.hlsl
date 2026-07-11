// Popup background blur: EXACT port of the game's dedicated pass chain
// (captured from a live frame with the Rewards popup up):
//   blur_hBlurPS:  11 gaussian taps along +X over the finished frame,
//                  offsets k * 0.0003125 * PS c0.x (c0.x = 8 -> 0.0025/tap,
//                  +/-1.25% of the buffer), literal weights below (sum 1);
//   blur_vBlurPS:  the same kernel along +Y;
//   postfx_basictex: plain fullscreen REPLACE of the frame with the result.
// The game runs it at its fixed 1152x640 internal resolution and the
// console's bilinear upscale of that buffer is what reads as the "frosted
// glass" lattice, so the passes here render into 1152x640 intermediates and
// stretch back, reproducing both the kernel and the lattice.
cbuffer C : register(b0) {
  float4 dir;  // xy = blur axis, z = kernel scale (the game's PS c0.x, 8)
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
  // Literal kernel from the blur_hBlurPS ucode (c251..c255).
  const float w[6] = {0.2005654, 0.1769984, 0.1216491, 0.0651141,
                      0.0271436, 0.0088122};
  float2 step = dir.xy * (0.0003125 * dir.z);
  float3 c = src.SampleLevel(smp_clamp, i.uv, 0).rgb * w[0];
  [unroll] for (int k = 1; k < 6; ++k) {
    c += src.SampleLevel(smp_clamp, i.uv + step * k, 0).rgb * w[k];
    c += src.SampleLevel(smp_clamp, i.uv - step * k, 0).rgb * w[k];
  }
  return float4(c, 1.0);
}
// Prefiltered downsample of the (higher-res) native output into the game's
// 1152x640 blur space (dir.xy = source texel size). A 4x4 grid of bilinear
// taps covers reduction ratios up to ~4x (4K -> 1152 is 3.33x); narrower
// footprints undersampled the scale and the aliased detail crawled as the
// scene animated: the whole blurred backdrop shimmered, worst around
// high-contrast edges.
float4 ps_down(VSOut i) : SV_Target {
  float3 c = 0.0;
  [unroll] for (int y = 0; y < 4; ++y) {
    [unroll] for (int x = 0; x < 4; ++x) {
      float2 off = float2(float(x) - 1.5, float(y) - 1.5) * dir.xy;
      c += src.SampleLevel(smp_clamp, i.uv + off, 0).rgb;
    }
  }
  return float4(c / 16.0, 1.0);
}
// postfx_basictex: oC0 = tex (verified: `max oC0, r0, r0`).
float4 ps_blit(VSOut i) : SV_Target {
  return float4(src.SampleLevel(smp_clamp, i.uv, 0).rgb, 1.0);
}
