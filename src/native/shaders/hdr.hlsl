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
//   ps_tonemap     scene xe + bloom + volumetric terms -> the exact classic
//                  tone chain -> the gamma guest output
//
// Volumetric lighting (shadow-marched sun shafts + directional haze) rides
// the same layout plus the frame's shadow constant slice at b1:
//   ps_vol_linearize  scene depth -> linear view Z, full res (only when the
//                     SSAO pass did not already produce the plane this frame;
//                     VOL_MSAA=1 variant for the MSAA depth buffer)
//   ps_vol_shafts     half res: reconstruct the world position from depth,
//                     march the camera->pixel ray with per-pixel jitter and
//                     test each step's sun visibility against the CSM atlas
//                     and the native static sun-shadow map (falling back to
//                     the baked world-shadow map outside its coverage),
//                     real shadowed air,
//                     so occlusion never depends on on-screen silhouettes
//                     (no ghosting around near characters) and beams appear
//                     wherever shadow volumes cross the view ray.
// The shaft plane and a full-res analytic haze term (Schlick phase on the
// view/sun angle x depth scattering, tinted by the frame's captured fog
// color) both join in ps_tonemap AFTER the AO multiply; in-air light is
// not subject to surface occlusion, and before the tone chain, so they
// bloom and tonemap like scene light.
cbuffer C : register(b0) {
  float4 size;  // xy = destination size in px, zw = 1 / destination size
  float4 src;   // xy = source size in px, zw = 1 / source size
  float4 p0;    // x = bloom threshold (xe space), y = soft knee,
                // z = bloom intensity, w = debug (1 = bloom term, 2 = raw xe,
                // 3 = AO plane, 4 = shaft plane, 5 = haze term)
  float4 p1;    // x = downsample tap spread in source texels (2 on the 4x
                // extraction so the 13-tap footprint covers the whole
                // source block, 1 on the 2x chain), yzw spare
  // Volumetric rows (zero when the effect is off; every term drops out).
  // ps_vol_shafts overloads the tail with the world unprojection instead:
  // src = camera world position (xyz) + march reach, p0 = march steps /
  // strength / phase g, p1.xy = proj m22 / m32, vp/vs0/vs1/vs2 = the
  // row-vector inverse view-projection (world = clip * M).
  float4 vp;    // tonemap/linearize: x = |m00|, y = |m11|, z = m22, w = m32
  float4 vs0;   // tonemap: x = shaft plane resolution divisor (>= 2),
                // w = shaft intensity (yz unused)
  float4 vs1;   // tonemap: xyz = haze tint (xe space), w = haze intensity
  float4 vs2;   // tonemap: xyz = sun direction in AO view space (toward the
                // sun), w = haze density (1 / view unit)
};
// Per-frame shadow / lighting constant slice (the scene pass's b1) -
// ps_vol_shafts samples sun visibility with the exact receiver rows
// the materials use.
#include "scene_frame_cb.hlsli"
Texture2D<float4> tex0 : register(t0);  // per-pass primary input
#ifdef VOL_MSAA
Texture2DMS<float> depth_ms : register(t0);  // ps_vol_linearize MSAA depth
#endif
Texture2D<float> tex0d : register(t0);  // ps_vol_linearize 1x depth
Texture2D<float4> tex1 : register(t1);  // ps_tonemap: the bloom plane
Texture2D<float> tex1d : register(t1);  // ps_vol_shafts: linear depth
// ps_tonemap: the SSAO multiplier plane (fused pre-tonemap apply, replaces
// the classic path's separate full-res composite draw; white when AO is
// off). Bilinear upsample from the AO raster, like the classic composite.
Texture2D<float> ao_plane : register(t2);
// ps_vol_shafts: the CSM atlas (x = light-space depth, y = coverage).
Texture2D<float2> shadow_tex : register(t2);
// ps_tonemap: the half-res shaft plane (white with vs0.w = 0 when shafts
// did not run) and the full-res linear view-Z plane (white with vs1.w = 0
// when no depth was produced this frame).
Texture2D<float4> vol_plane : register(t3);
// ps_vol_shafts: the 512x512 static world-shadow map (x = light-space
// depth; sge(stored >= ray) = lit, uncovered texels hold the far clear).
Texture2D<float4> ws_tex : register(t3);
Texture2D<float> lin_depth : register(t4);
// ps_vol_shafts: the native static sun-shadow atlas (the scene pass's t10,
// three tiles inner/mid/far left to right, x = depth along the material
// sun). White fallback = the far clear = lit everywhere when the map was
// not rendered this frame.
Texture2D<float2> nsm_tex : register(t4);
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

// Device depth -> view-space Z. Row-vector projection with m23 = 1:
// ndc_z = m22 + m32 / z_view.
float LinearZ(float d) {
  return clamp(vp.w / min(d - vp.z, -1e-6), 0.05f, 4e5f);
}

// View-space position from UV + linear Z (|m00|/|m11| make this the SSAO
// passes' possibly-mirrored view space; the sun direction row is folded
// into the same space CPU-side, so angles stay consistent).
float3 ViewPos(float2 uv, float z) {
  float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
  return float3(ndc.x * z / vp.x, ndc.y * z / vp.y, z);
}

// Interleaved gradient noise: per-pixel march phase jitter (the bilinear
// half-res upsample integrates the dither).
float Ign(float2 p) {
  return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

float ps_vol_linearize(VSOut i) : SV_Target {
#ifdef VOL_MSAA
  float d = depth_ms.Load(int2(i.pos.xy), 0);
#else
  float d = tex0d.Load(int3(int2(i.pos.xy), 0));
#endif
  return LinearZ(d);
}

// CSM atlas sun visibility at a world position: the material receiver
// math (see scene.hlsl SampleCsmShadow) with the dynamicobject per-cascade
// biases (air samples are not casters; the world bias is 0 in captures).
// Returns -1 outside every cascade: uncovered air is EXCLUDED from the
// march average rather than assumed lit, so out-of-range distance cannot
// wash the beams out.
float CsmVis(float3 wp) {
  float2 lsv = float2(dot(sh_x.xyz, wp) + sh_x.w, dot(sh_y.xyz, wp) + sh_y.w);
  float2 luv = 0.0;
  float casc = 0.0;
  float2 l2 = lsv * sh_c2.xy + sh_c2.zw;
  if (max(abs(l2.x), abs(l2.y)) < 0.99) { luv = l2; casc = 3.0; }
  float2 l1 = lsv * sh_c1.xy + sh_c1.zw;
  if (max(abs(l1.x), abs(l1.y)) < 0.99) { luv = l1; casc = 2.0; }
  if (max(abs(lsv.x), abs(lsv.y)) < 0.99) { luv = lsv; casc = 1.0; }
  if (casc <= 0.0) {
    return -1.0;
  }
  float2 suv = float2(luv.x / 6.0 + (casc * 2.0 - 1.0) / 6.0,
                      luv.y * -0.5 + 0.5);
  float rd = dot(sh_z.xyz, wp) + sh_z.w - (casc > 1.5 ? 0.015 : 0.007);
  float2 sm2 = shadow_tex.SampleLevel(smp_linear, suv, 0);
  return saturate((sm2.x >= rd ? 1.0 : 0.0) + (1.0 - sm2.y));
}

// Static world-shadow (region-wide baked building/tree shade) visibility;
// -1 outside the map's projection.
float WorldVis(float3 wp) {
  float4 wp4 = float4(wp, 1.0);
  float2 wuv = float2(dot(wp4, dyn_wsx) * 0.5 + 0.5,
                      dot(wp4, dyn_wsy) * -0.5 + 0.5);
  if (any(saturate(wuv) != wuv)) {
    return -1.0;
  }
  return ws_tex.SampleLevel(smp_point, wuv, 0).x >= dot(wp4, dyn_wsz)
             ? 1.0
             : 0.0;
}

// Native static sun-shadow visibility (nsm_tex, the scene pass's t10): the
// static world rendered along the TRUE material sun, so static-geometry
// shafts align with the sun glow; the baked ws map above projects along
// the game's near-vertical utility axis, ~40 degrees off the sun, which
// skews its shadow volumes accordingly. Cascade select mirrors
// SampleStaticSun (three tiles sharing the origin, ratios 6/2/1); a single
// soft-free compare per step is enough for the jittered march average.
// Returns -1 outside the map or with the feature off. Uncovered texels
// hold the far clear = lit.
float NsmVis(float3 wp) {
  if (nsm_p.x <= 0.0) {
    return -1.0;
  }
  float2 uc_far = float2(dot(nsm_x.xyz, wp) + nsm_x.w,
                         dot(nsm_y.xyz, wp) + nsm_y.w);
  if (max(abs(uc_far.x), abs(uc_far.y)) >= 0.99) {
    return -1.0;
  }
  float2 uc0 = uc_far * 6.0;
  float2 uc1 = uc_far * 2.0;
  float tile;
  float2 uc;
  if (max(abs(uc0.x), abs(uc0.y)) < 0.99) {
    tile = 0.0;
    uc = uc0;
  } else if (max(abs(uc1.x), abs(uc1.y)) < 0.99) {
    tile = 1.0;
    uc = uc1;
  } else {
    tile = 2.0;
    uc = uc_far;
  }
  float2 suv = float2(uc.x / 6.0 + (tile * 2.0 + 1.0) / 6.0,
                      uc.y * -0.5 + 0.5);
  // Keep the bilinear tap off the tile seams (the atlas is 3 tiles wide).
  float hx = 0.1666667 / nsm_p2.w;
  suv.x = clamp(suv.x, tile / 3.0 + hx, tile / 3.0 + 0.3333333 - hx);
  float refd = dot(nsm_z.xyz, wp) + nsm_z.w - nsm_p2.x;
  float s = nsm_tex.SampleLevel(smp_linear, suv, 0).x >= refd ? 1.0 : 0.0;
  // The material receive bottoms out at 1 - strength; shadowed air follows
  // the same authored blend so the shafts fade with the surface shadows.
  return 1.0 - nsm_p.x * (1.0 - s);
}

// Shadow-marched volumetric sun shafts (half res). The camera->pixel ray
// is marched with per-pixel jitter; each step's sun visibility comes from
// the CSM atlas plus the native static sun map (baked ws map outside its
// coverage), so the beams are cast by real shadow volumes, never by
// on-screen silhouettes. Output = the
// SHADOWED fraction x Henyey-Greenstein forward-scatter phase x marched
// path fraction: the game's sky and fog already carry the authored
// scattering of fully-lit air, so adding lit-air glow double-counts it
// into a structureless veil; only the shadow volumes' CUTS into that
// glow are new information, and ps_tonemap SUBTRACTS them (crepuscular
// shafts by contrast). Fully lit air contributes exactly zero.
float4 ps_vol_shafts(VSOut i) : SV_Target {
  float d = tex1d.SampleLevel(smp_point, i.uv, 0);
  // World position via the inverse view-projection (row-vector): the clip
  // vector is (ndc * w, ndc_z * w, w) with w = view Z and ndc_z = m22 +
  // m32 / z.
  float2 ndc = float2(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0);
  float zn = p1.x + p1.y / max(d, 1e-4);
  float4 clip = float4(ndc.x * d, ndc.y * d, zn * d, d);
  float4 wp4 = clip.x * vp + clip.y * vs0 + clip.z * vs1 + clip.w * vs2;
  float3 wp = wp4.xyz / max(wp4.w, 1e-6);
  float3 cam = src.xyz;
  float3 ray = wp - cam;
  float dist = length(ray);
  float3 rdir = ray / max(dist, 1e-4);
  float reach = min(dist, src.w);
  // Constant per-meter sample density: a short ray to near geometry needs
  // proportionally fewer steps for the same marched quality (the fixed
  // count oversampled a 5 m ray 8x versus the full-reach case), and the
  // path-fraction scale keeps near-surface terms small enough that the
  // extra variance stays under the tent blur.
  float steps_max = clamp(p0.x, 8.0, 64.0);
  float steps = clamp(ceil(steps_max * reach / max(src.w, 1.0)), 8.0,
                      steps_max);
  float jitter = Ign(i.pos.xy);
  float lit = 0.0;
  float tot = 0.0;
  [loop] for (int k = 0; k < 64; ++k) {
    if (k >= (int)steps) {
      break;
    }
    float3 sp = cam + rdir * ((float(k) + jitter) / steps * reach);
    float cv = CsmVis(sp);
    // Static shade: the native sun map where it covers (true material-sun
    // axis), else the baked ws map. Never both: inside the sun map's range
    // the two would draw the same caster's shadow volume on two different
    // axes and read as a doubled shaft.
    float nv = NsmVis(sp);
    float sv = nv >= 0.0 ? nv : WorldVis(sp);
    if (cv >= 0.0 || sv >= 0.0) {
      lit += min(cv < 0.0 ? 1.0 : cv, sv < 0.0 ? 1.0 : sv);
      tot += 1.0;
    }
  }
  if (tot < 2.5) {
    return float4(0.0, 0.0, 0.0, 1.0);
  }
  float ct = dot(rdir, sh_sun.xyz);
  float g = p0.z;
  // Henyey-Greenstein SHAPE without the 4-pi radiance normalization; the
  // output is a dimming factor, not integrated radiance, so the phase only
  // steers directionality: ~1.25 at 50 degrees off the sun, capped toward
  // the sun axis (the 4-pi-normalized form peaked at 0.03-0.1 at typical
  // viewing angles and made the effect invisible at any intensity).
  float hg = min((1.0 - g * g) /
                     pow(max(1.0 + g * g - 2.0 * g * ct, 1e-3), 1.5),
                 2.0);
  // Path fraction: a near surface has little air in front of it; the term
  // that keeps the shafts off close characters.
  float e = (1.0 - lit / tot) * hg * saturate(reach / max(src.w, 1.0));
  return float4(e, e, e, 1.0);
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
  // Graphics build-up showcase (see scene.hlsl ShowcaseMask): sh_v2.y/z =
  // 256 + the layer mask left/right of the split at sh_v2.w (output
  // pixels), 0 = off. Post layers reveal per pixel by their mask bits:
  // AO 16, volumetrics 64, bloom 128 (SSR gates in its own composite).
  // Compiled in only for the SHOWCASE=1 variant; the default build folds
  // the mask to off and every gate below dead-strips.
#ifndef SHOWCASE
#define SHOWCASE 0
#endif
#if SHOWCASE
  float scv = i.pos.x < sh_v2.w ? sh_v2.y : sh_v2.z;
  int sc_mask = scv < 255.5 ? -1 : (int)(scv + 0.5) - 256;
#else
  const int sc_mask = -1;
#endif
  float ao = ao_plane.SampleLevel(smp_linear, i.uv, 0);
  if (sc_mask >= 0 && (sc_mask & 16) == 0) {
    ao = 1.0;
  }
  float3 x = max(tex0.SampleLevel(smp_point, i.uv, 0).rgb, 0.0) * ao;
  float3 bloom = tex1.SampleLevel(smp_linear, i.uv, 0).rgb * p0.z;
  if (sc_mask >= 0 && (sc_mask & 128) == 0) {
    bloom = 0.0;
  }
  // Volumetric terms, applied AFTER the AO multiply (in-air light is not
  // subject to surface occlusion). The shaft plane carries the marched
  // SHADOWED-air term (see ps_vol_shafts) and dims MULTIPLICATIVELY;
  // shadowed air removes a proportional share of the light seen through
  // it. Against the bright sky that reads as dark crepuscular shafts;
  // against a dim facade it is a slight deepening; fully lit air changes
  // nothing. An ABSOLUTE subtraction was tried and rejected: the game
  // bakes its scatter glow into bright sky pixels only, so subtracting a
  // phase-weighted constant clamped whole shaded street canyons toward
  // black. The half-res plane upsamples depth-aware (weights collapse
  // across depth discontinuities) so shaft edges never bleed onto near
  // silhouettes.
  float shaft = 0.0;
  bool sc_vol = sc_mask < 0 || (sc_mask & 64) != 0;
  if (vs0.w > 0.0 && sc_vol) {
    float dpix = lin_depth.SampleLevel(smp_point, i.uv, 0);
    // One shaft-plane texel in uv (vs0.x = the march resolution divisor).
    float2 vt = size.zw * max(vs0.x, 2.0);
    float2 base = (floor(i.uv / vt - 0.5) + 0.5) * vt;
    float2 f = saturate((i.uv - base) / vt);
    float acc = 0.0;
    float wsum = 0.0;
    [unroll] for (int k = 0; k < 4; ++k) {
      const float2 kC[4] = {float2(0.0, 0.0), float2(1.0, 0.0),
                            float2(0.0, 1.0), float2(1.0, 1.0)};
      float2 su = base + kC[k] * vt;
      float w = (kC[k].x > 0.5 ? f.x : 1.0 - f.x) *
                (kC[k].y > 0.5 ? f.y : 1.0 - f.y);
      float ds = lin_depth.SampleLevel(smp_point, su, 0);
      w *= 1.0 / (1.0 + 8.0 * abs(ds - dpix) / max(dpix, 1.0));
      acc += vol_plane.SampleLevel(smp_point, su, 0).r * w;
      wsum += w;
    }
    shaft = acc / max(wsum, 1e-4) * vs0.w;
  }
  // Directional haze: the phase function's excess over its perpendicular
  // value only; the game's own fog already renders the isotropic part,
  // so adding a floor here just veils the whole frame.
  float3 haze = float3(0.0, 0.0, 0.0);
  if (vs1.w > 0.0 && sc_vol) {
    float d = lin_depth.SampleLevel(smp_point, i.uv, 0);
    float path = 1.0 - exp(-max(vs2.w, 0.0005) * d);
    float3 ray = normalize(ViewPos(i.uv, 1.0));
    float cosv = dot(ray, vs2.xyz);
    const float g = 0.75;
    float denom = max(1.0 - g * cosv, 1e-3);
    float phase = (1.0 - g * g) / (denom * denom);
    phase = min(max(phase - (1.0 - g * g), 0.0), 3.0);
    haze = vs1.xyz * (phase * path * vs1.w);
  }
  if (p0.w > 4.5) {
    return float4(ToneMapScene(haze), 1.0);  // debug: haze term only
  }
  if (p0.w > 3.5) {
    // Debug: the shaft shadowing term only (inverted feel: bright = air
    // that will darken in the composite).
    return float4(ToneMapScene(shaft.xxx), 1.0);
  }
  if (p0.w > 2.5) {
    return float4(ao, ao, ao, 1.0);  // debug: AO multiplier plane
  }
  if (p0.w > 1.5) {
    return float4(saturate(x), 1.0);  // debug: raw xe
  }
  if (p0.w > 0.5) {
    return float4(ToneMapScene(bloom), 1.0);  // debug: bloom term only
  }
  // Dimming ceiling: shadowed air thins the light, it does not extinguish
  // it; without the ceiling a fully shaded sun-facing view (under an
  // overpass) saturated to a solid black void.
  float3 outc =
      ToneMapScene((x + haze) * (1.0 - 0.55 * saturate(shaft)) + bloom);
#if SHOWCASE
  // Showcase divider: a soft white line at the split while a wipe is in
  // flight (the two sides show different stages).
  if (sh_v2.w > 0.5 && abs(sh_v2.y - sh_v2.z) > 0.5) {
    float half_w = max(size.x * 0.0012, 1.5);
    float d = abs(i.pos.x - sh_v2.w);
    outc = lerp(outc, float3(1.0, 1.0, 1.0),
                saturate(1.0 - d / half_w) * 0.85);
  }
#endif
  return float4(outc, 1.0);
}
