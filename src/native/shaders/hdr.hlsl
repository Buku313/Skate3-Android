// HDR post chain: bloom pyramid + the scene tonemap, applied once over the
// float scene intermediate (see the HDR path in skate3_native_scene.cpp).
//
// The material shaders' shared tone chain, fog -> exposure -> tonemap ->
// sqrt -> the postfx uber's 1.41 scene multiplier, is IDENTICAL across every
// branch after the per-branch exposure multiply, so under HDR the branches
// write that chain's INPUT (pre-tonemap linear, "xe") into an RGBA16F target
// and this pass applies the chain exactly once. Bloom energy is injected
// pre-tonemap (x + bloom * intensity), so intensity 0 reproduces the classic
// in-material output bit-for-bit modulo fp16 storage rounding.
//
// Pass chain (fullscreen triangles, own binding layout = the SSAO shape):
//   ps_bloom_down  13-tap Jimenez downsample; the BLOOM_FIRST=1 variant adds
//                  the soft-knee threshold + per-group Karis luma weighting
//                  (anti-firefly) on the mip-0 extraction
//   ps_bloom_up    3x3 tent upsample, additive-blended onto the next-larger
//                  level (ONE/ONE at the PSO)
//   ps_tonemap     scene xe + bloom -> the exact classic tone chain -> the
//                  gamma guest output
cbuffer C : register(b0) {
  float4 size;  // xy = destination size in px, zw = 1 / destination size
  float4 src;   // xy = source size in px, zw = 1 / source size
  float4 p0;    // x = bloom threshold (xe space), y = soft knee,
                // z = bloom intensity, w = debug (1 = bloom term, 2 = raw xe)
  float4 p1;    // x = downsample tap spread in source texels (2 on the 4x
                // extraction so the 13-tap footprint covers the whole
                // source block, 1 on the 2x chain), yzw spare
};
Texture2D<float4> tex0 : register(t0);  // per-pass primary input
Texture2D<float4> tex1 : register(t1);  // ps_tonemap: the bloom plane
// ps_tonemap: the SSAO multiplier plane (fused pre-tonemap apply, replaces
// the classic path's separate full-res composite draw; white when AO is
// off). Bilinear upsample from the AO raster, like the classic composite.
Texture2D<float> ao_plane : register(t2);
SamplerState smp_point : register(s0);
SamplerState smp_linear : register(s1);

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

// The exact scene tone chain (see scene.hlsl ToneOut): input = the
// materials' post-fog, post-exposure linear value.
float3 ToneMapScene(float3 x) {
  float3 t1 = saturate(1.0 - x);
  float3 tm = max(x * 0.25 + 0.75, 1.0) - t1 * t1;
  return saturate(sqrt(max(tm * 0.5, 0.0)) * 1.41);
}

// Soft-knee brightpass (kept in xe space, where 1.0 is the tonemapper's
// saturation point): quadratic ramp from (threshold - knee) to
// (threshold + knee), linear excess beyond.
float3 SoftThreshold(float3 c) {
  float br = max(c.r, max(c.g, c.b));
  float rq = clamp(br - p0.x + p0.y, 0.0, 2.0 * p0.y);
  rq = rq * rq / (4.0 * p0.y + 1e-4);
  return c * (max(rq, br - p0.x) / max(br, 1e-4));
}

// 13-tap downsample (Jimenez, SIGGRAPH 2014): five overlapping 2x2 groups,
// the half-texel-centered inner group at weight 0.5 and four outer groups at
// 0.125 each. On the first (extraction) pass each group is Karis-weighted by
// 1/(1 + luma) before mixing, which stops single HDR-bright samples
// (specular fireflies, the sun-core texels) from flickering as they cross
// pyramid texel grids.
float4 ps_bloom_down(VSOut i) : SV_Target {
  float2 ts = src.zw * p1.x;
  float3 tA = tex0.SampleLevel(smp_linear, i.uv + ts * float2(-2, -2), 0).rgb;
  float3 tB = tex0.SampleLevel(smp_linear, i.uv + ts * float2(0, -2), 0).rgb;
  float3 tC = tex0.SampleLevel(smp_linear, i.uv + ts * float2(2, -2), 0).rgb;
  float3 tD = tex0.SampleLevel(smp_linear, i.uv + ts * float2(-1, -1), 0).rgb;
  float3 tE = tex0.SampleLevel(smp_linear, i.uv + ts * float2(1, -1), 0).rgb;
  float3 tF = tex0.SampleLevel(smp_linear, i.uv + ts * float2(-2, 0), 0).rgb;
  float3 tG = tex0.SampleLevel(smp_linear, i.uv, 0).rgb;
  float3 tH = tex0.SampleLevel(smp_linear, i.uv + ts * float2(2, 0), 0).rgb;
  float3 tI = tex0.SampleLevel(smp_linear, i.uv + ts * float2(-1, 1), 0).rgb;
  float3 tJ = tex0.SampleLevel(smp_linear, i.uv + ts * float2(1, 1), 0).rgb;
  float3 tK = tex0.SampleLevel(smp_linear, i.uv + ts * float2(-2, 2), 0).rgb;
  float3 tL = tex0.SampleLevel(smp_linear, i.uv + ts * float2(0, 2), 0).rgb;
  float3 tM = tex0.SampleLevel(smp_linear, i.uv + ts * float2(2, 2), 0).rgb;
  float3 g0 = (tD + tE + tI + tJ) * 0.25;  // inner group
  float3 g1 = (tA + tB + tF + tG) * 0.25;
  float3 g2 = (tB + tC + tG + tH) * 0.25;
  float3 g3 = (tF + tG + tK + tL) * 0.25;
  float3 g4 = (tG + tH + tL + tM) * 0.25;
#ifdef BLOOM_FIRST
  float w0 = 0.5 / (1.0 + dot(g0, float3(0.2126, 0.7152, 0.0722)));
  float w1 = 0.125 / (1.0 + dot(g1, float3(0.2126, 0.7152, 0.0722)));
  float w2 = 0.125 / (1.0 + dot(g2, float3(0.2126, 0.7152, 0.0722)));
  float w3 = 0.125 / (1.0 + dot(g3, float3(0.2126, 0.7152, 0.0722)));
  float w4 = 0.125 / (1.0 + dot(g4, float3(0.2126, 0.7152, 0.0722)));
  float3 c = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) /
             (w0 + w1 + w2 + w3 + w4);
  return float4(SoftThreshold(max(c, 0.0)), 1.0);
#else
  return float4(g0 * 0.5 + (g1 + g2 + g3 + g4) * 0.125, 1.0);
#endif
}

// 3x3 tent upsample of the next-smaller level; the PSO blends it additively
// onto the destination level (progressive upsampling accumulates every
// pyramid level's contribution with widening support).
float4 ps_bloom_up(VSOut i) : SV_Target {
  float2 ts = src.zw;
  float3 c = tex0.SampleLevel(smp_linear, i.uv + ts * float2(-1, -1), 0).rgb;
  c += tex0.SampleLevel(smp_linear, i.uv + ts * float2(0, -1), 0).rgb * 2.0;
  c += tex0.SampleLevel(smp_linear, i.uv + ts * float2(1, -1), 0).rgb;
  c += tex0.SampleLevel(smp_linear, i.uv + ts * float2(-1, 0), 0).rgb * 2.0;
  c += tex0.SampleLevel(smp_linear, i.uv, 0).rgb * 4.0;
  c += tex0.SampleLevel(smp_linear, i.uv + ts * float2(1, 0), 0).rgb * 2.0;
  c += tex0.SampleLevel(smp_linear, i.uv + ts * float2(-1, 1), 0).rgb;
  c += tex0.SampleLevel(smp_linear, i.uv + ts * float2(0, 1), 0).rgb * 2.0;
  c += tex0.SampleLevel(smp_linear, i.uv + ts * float2(1, 1), 0).rgb;
  return float4(c * (1.0 / 16.0), 1.0);
}

// Final tonemap into the gamma guest output. tex0 = the full-res scene xe
// plane (point-sampled, 1:1), tex1 = bloom pyramid level 0 (bilinear
// upsample; the white fallback rides here with intensity 0 when bloom is
// off).
float4 ps_tonemap(VSOut i) : SV_Target {
  float ao = ao_plane.SampleLevel(smp_linear, i.uv, 0);
  float3 x = max(tex0.SampleLevel(smp_point, i.uv, 0).rgb, 0.0) * ao;
  float3 bloom = tex1.SampleLevel(smp_linear, i.uv, 0).rgb * p0.z;
  if (p0.w > 2.5) {
    return float4(ao, ao, ao, 1.0);  // debug: AO multiplier plane
  }
  if (p0.w > 1.5) {
    return float4(saturate(x), 1.0);  // debug: raw xe
  }
  if (p0.w > 0.5) {
    return float4(ToneMapScene(bloom), 1.0);  // debug: bloom term only
  }
  return float4(ToneMapScene(x + bloom), 1.0);
}
