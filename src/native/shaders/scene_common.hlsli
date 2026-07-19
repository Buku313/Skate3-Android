// Shared shading helpers: screen-space / stored tangent frames and the
// output encode (the exact scene tone chain and its HDR pre-tonemap
// passthrough).
// Screen-space UV-gradient tangent frame (cotangent reconstruction from the
// position/uv derivatives; signs/lengths calibrated against the meshes' REAL
// stored TBN; see the env-family branch notes). The RAW screen frame is
// exact for the GetTangentLight kd SIGN terms under ANY UV orientation:
// bowl/ramp walls map their texture rotated relative to world-up, and
// mirrored islands flip dP/dU exactly like the stored tangent does.
void ScreenTangentFrame(float3 wn, float3 rpos, float2 uv,
                        out float3 tt, out float3 bb) {
  float3 dp1 = ddx(rpos), dp2 = ddy(rpos);
  float2 du1 = ddx(uv), du2 = ddy(uv);
  float3 dp2p = cross(dp2, wn), dp1p = cross(wn, dp1);
  tt = dp2p * du1.x + dp1p * du2.x;
  bb = dp2p * du1.y + dp1p * du2.y;
  tt *= -rsqrt(max(dot(tt, tt), 1e-12));
  bb *= rsqrt(max(dot(bb, bb), 1e-12));
}
// kd/normal-mapping axes: prefer the mesh's STORED frame (binormal +
// handedness in tanb, T = cross(B, N) x handedness x calibration polarity)
// - authoring decides per UV island whether the frame follows a mirror,
// which no derivative frame can know (per-island brightness steps on the
// wooden ramp panels / faint bowl striping). The screen frame is the
// fallback for meshes without one.
void KdAxes(float4 tanb, float3 wn, float3 tt_s, float3 bb_s,
            out float3 kt, out float3 kb) {
  kt = tt_s;
  kb = bb_s;
  if (tanb.w > 0.1) {
    kb = normalize(tanb.xyz);
    kt = cross(kb, wn) * ((tanb.w > 0.6 ? 1.0 : -1.0) * sh_v2.x);
  }
}
// ---- Output encode --------------------------------------------------------
// Every exact branch ends in the same chain: x = the post-fog,
// post-exposure linear value ("xe"), then
//   tonemap = max(x*0.25 + 0.75, 1) - sat(1 - x)^2
//   out     = sat(sqrt(0.5 * tonemap) * 1.41)      (the uber's 1.41 fold)
// Under HDR=1 the branch returns xe itself into the float scene
// intermediate and the host post pass (hdr.hlsl ps_tonemap) applies the
// chain exactly once, which is what lets real bloom inject energy
// pre-tonemap. `reduced` = environmentdiffuse's cheap curve 1 - sat(1-x)^2;
// it equals the full curve evaluated at min(x, 1) (the two only differ past
// the x = 1 saturation point), so the HDR encode is a clamp.
float4 ToneOut(float3 xe, float a, bool reduced) {
#ifdef HDR
  float3 x = max(xe, 0.0);
  return float4(reduced ? min(x, 1.0) : x, a);
#else
  float3 t1 = saturate(1.0 - xe);
  float3 tm = reduced ? 1.0 - t1 * t1 : max(xe * 0.25 + 0.75, 1.0) - t1 * t1;
  return float4(saturate(sqrt(max(tm * 0.5, 0.0)) * 1.41), a);
#endif
}
// Passthrough for values authored in the FINAL gamma space (the legacy
// empirical branches, water, debug visualizations): under HDR they encode
// as the tone chain's exact inverse, so the host tonemap restores them.
float3 PassGamma(float3 c) {
#ifdef HDR
  float3 tm = c * c * (2.0 / (1.41 * 1.41));
  float3 lo = 1.0 - sqrt(saturate(1.0 - tm));
  float3 hi = 4.0 * tm - 3.0;
  return lerp(lo, hi, step(1.0, tm));  // per-channel select (fxc + dxc)
#else
  return c;
#endif
}
