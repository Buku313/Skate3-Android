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
  float4 dir;   // xy = blur axis, z = kernel scale (the game's PS c0.x, 8)
  float4 tint;  // rgb = the game's PS c1 of the blur pass: the menu fade
                // modulate. Both blur ucodes end in `mul oC0, r0, c1`, so it
                // applies PER PASS (squared overall). Gameplay popups stage
                // (1,1,1), why the original port had no multiply, the
                // pause menu stages ~(0.35,0.33,0.32) = the darkened
                // backdrop (measured in a live pause-menu capture).
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
  return float4(c * tint.rgb, 1.0);
}
// Prefiltered downsample of the (higher-res) native output into the game's
// 1152x640 blur space (dir.xy = source texel size). A 4x4 grid of bilinear
// taps covers reduction ratios up to ~4x (4K -> 1152 is 3.33x); narrower
// footprints undersampled the scale and the aliased detail crawled as the
// scene animated: the whole blurred backdrop shimmered, worst around
// high-contrast edges. This pass reads the second constant register as the
// source UV rect (xy = scale, zw = offset): the photo grab center-crops the
// 16:9 band out of a wide output; (1,1,0,0) = full frame.
float4 ps_down(VSOut i) : SV_Target {
  float4 rect = tint;
  float2 uv = i.uv * rect.xy + rect.zw;
  float3 c = 0.0;
  [unroll] for (int y = 0; y < 4; ++y) {
    [unroll] for (int x = 0; x < 4; ++x) {
      float2 off = float2(float(x) - 1.5, float(y) - 1.5) * dir.xy;
      c += src.SampleLevel(smp_clamp, uv + off, 0).rgb;
    }
  }
  return float4(c / 16.0, 1.0);
}
// postfx_basictex: oC0 = tex (verified: `max oC0, r0, r0`).
float4 ps_blit(VSOut i) : SV_Target {
  return float4(src.SampleLevel(smp_clamp, i.uv, 0).rgb, 1.0);
}
// Host settings-menu backdrop: clean separable GAUSSIAN, deliberately NOT the
// game's popup chain above (that one reproduces the console's 1152x640
// lattice on purpose). Runs on a
// half-res intermediate; weights are computed and normalised in-shader so the
// sigma is a free runtime constant.
//   dir.xy = blur axis in UV units (one texel along the axis)
//   dir.z  = sigma in (half-res) pixels
//   dir.w  = tap radius, ceil(3*sigma), capped by the caller
float4 ps_menu_gauss(VSOut i) : SV_Target {
  const float sigma = max(dir.z, 0.001);
  const int radius = int(dir.w);
  float3 acc = src.SampleLevel(smp_clamp, i.uv, 0).rgb;
  float wsum = 1.0;
  for (int k = 1; k <= radius; ++k) {
    float w = exp(-0.5 * float(k) * float(k) / (sigma * sigma));
    acc += src.SampleLevel(smp_clamp, i.uv + dir.xy * k, 0).rgb * w;
    acc += src.SampleLevel(smp_clamp, i.uv - dir.xy * k, 0).rgb * w;
    wsum += 2.0 * w;
  }
  return float4(acc / wsum, 1.0);
}
