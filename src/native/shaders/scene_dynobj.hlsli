// dynamicobject.fx props (cam_pos.w = -(20 + variant): -21 default,
// -22 alphatest). Rigid movable objects (dispensers, dumpsters, benches,
// cans). Lit with the game's own dynamicobject model (verified exact
// against an offline ucode evaluation): key sun light + bounce +
// flat ambient, gated by min(CSM, max(static world shade, c8.w floor)),
// then fog -> exposure -> tonemap -> sqrt and the postfx uber 1.41.
// v2: misc.z
// carries the material bind flags (1 = base normal at t5, 2 = detail at
// t8 sampled at uv * misc.w, 4 = spec/ecc masks at t9 - the env fams 1-4
// encoding); the mapped world normal drives key/bounce, the real
// GetTangentLight kd replaces the flat fold, and the phong spec returns.
// With no maps bound the flat-map fold 0.39 * 2.39562 = 0.93429 and the
// vertex normal apply (the v1 look).
float4 ShadeDynObject(VSOut i, float4 albedo) {
// dynobj variants only:
  // -21/-22; the exact water family (-30) and sky (-40) pass through.
  if (cam_pos.w < -21.5) {
    clip(albedo.a - 0.1176);  // dynamicobject.alphatest: ALPHAREF 30
  }
  float3 dlin = albedo.rgb * albedo.rgb;
  float3 n = dot(i.nrm, i.nrm) > 0.01
                 ? normalize(i.nrm)
                 : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
  uint v2f = (uint)(misc.z + 0.5);
  // Tangent axes for the kd sign dots / normal mapping / tangent-space
  // spec mirror: the stored per-vertex frame when the mesh carries one
  // (packed by DecodeMesh from the usage-6/7 tangent+binormal pair),
  // screen-space UV-gradient frame otherwise.
  float3 kt = float3(1.0, 0.0, 0.0), kb = float3(0.0, 1.0, 0.0);
  if (v2f != 0u) {
    float3 tt_s, bb_s;
    ScreenTangentFrame(n, i.rpos, i.uv, tt_s, bb_s);
    KdAxes(i.tanb, n, tt_s, bb_s, kt, kb);
  }
  // GetTangentLight's fixed SIGNED tangent-space light: the signs come
  // from the frame axes dotted with the dynobj bank's own sun (c9:
  // dyn_sun, not the world families' sh_sun).
  float3 Lt = float3(0.58 * sign(dot(kt, dyn_sun.xyz)),
                     0.62 * sign(dot(kb, dyn_sun.xyz)), 0.39);
  // vnd = (2*base.xy + 2*detail.xy - 2, 2*base.z - 1), NORMALIZED for
  // both the kd dot and the TBN mapping (both dynobj variants).
  float3 vndn = float3(0.0, 0.0, 1.0);
  float3 wn = n;
  float kd = 0.93429;
  if ((v2f & 1u) != 0u) {
    float3 nmv = normal_map.Sample(smp, i.uv).rgb;
    float2 dxy = (v2f & 2u) != 0u
                     ? detail_map.Sample(smp, i.uv * misc.w).rg
                     : float2(0.5, 0.5);
    vndn = normalize(
        float3(nmv.xy * 2.0 + dxy * 2.0 - 2.0, nmv.z * 2.0 - 1.0));
    wn = normalize(vndn.x * kt + vndn.y * kb + vndn.z * n);
    kd = dot(vndn, Lt) * 2.39562;
  }
  float ndl = dot(wn, dyn_sun.xyz);
  float key = saturate(ndl);  // key light gated on N.L >= 0
  float bounce = saturate(dot(wn, float3(-dyn_sun.x, dyn_sun.y, -dyn_sun.z)));
  // CSM shadow (shared receiver rows at t7). extra_bias -1 = the game's
  // own per-cascade dynamicobject receive bias (props are casters;
  // without it their flat tops self-shadow and flicker).
  float s = min(SampleCsmShadowSoft(i.rpos + cam_pos.xyz, -1.0, wn, i.pos.xy),
                SampleStaticSun(i.rpos + cam_pos.xyz, wn, i.pos.xy));
  // Static world-shadow term: the game's baked building/tree shade,
  // sampled from the natively re-rendered map at t4 (flag 8) with the
  // PS's own 4-tap PCF (point taps at +-0.5 texels, manual bilinear:
  // tap.x >= ray depth reads lit; uncovered texels hold the far clear).
  // Without the map the term is 1 and the min collapses to the CSM:
  // props standing in baked shade lit at full key (the newspaper-machine
  // mint brightening).
  float world = 1.0;
  if ((v2f & 8u) != 0u) {
    float4 wp4 = float4(i.rpos + cam_pos.xyz, 1.0);
    float2 wuv = float2(dot(wp4, dyn_wsx) * 0.5 + 0.5,
                        dot(wp4, dyn_wsy) * -0.5 + 0.5);
    float rdw = dot(wp4, dyn_wsz);
    float2 st = wuv * 512.0 - 0.5;
    float2 fw = frac(st);
    int2 b0 = int2(floor(st));
    float4 t;
    t.x = decal_art.Load(int3(clamp(b0, int2(0, 0), int2(511, 511)), 0)).x >=
                  rdw
              ? 1.0
              : 0.0;
    t.y = decal_art.Load(int3(clamp(b0 + int2(1, 0), int2(0, 0),
                                    int2(511, 511)),
                              0)).x >= rdw
              ? 1.0
              : 0.0;
    t.z = decal_art.Load(int3(clamp(b0 + int2(0, 1), int2(0, 0),
                                    int2(511, 511)),
                              0)).x >= rdw
              ? 1.0
              : 0.0;
    t.w = decal_art.Load(int3(clamp(b0 + int2(1, 1), int2(0, 0),
                                    int2(511, 511)),
                              0)).x >= rdw
              ? 1.0
              : 0.0;
    world = lerp(lerp(t.x, t.y, fw.x), lerp(t.z, t.w, fw.x), fw.y);
  }
  float shadow = min(s * (ndl >= 0.0 ? 1.0 : 0.0), max(world, dyn_misc.y));
  float3 lighting = key * shadow + bounce * dyn_amb.w + dyn_amb.rgb;
  float3 lin = lighting * kd * dlin;
  // Phong spec vs the material's spec mask (t9.x) / eccentricity (t9.y),
  // x shadow, tint (2.1, 1.8, 1.5). Two variants: the default PS
  // reflects the fixed SIGNED tangent-space light about vnd with the eye
  // transformed into tangent space (it shares the kd sign dots); the
  // alphatest PS reflects the fixed WORLD light (-0.14, 0.5, 0.9) about
  // wN exactly like baseenvironment.
  if ((v2f & 4u) != 0u) {
    float2 m2 = spec2_map.Sample(smp, i.uv).rg;
    float3 E = -normalize(i.rpos);
    float bp;
    if (cam_pos.w < -21.5) {
      float3 Ls = float3(-0.14, 0.5, 0.9);
      float3 refl = Ls - 2.0 * wn * dot(wn, Ls);
      bp = saturate(dot(E, -refl));
    } else {
      float3 Et = normalize(float3(dot(E, kt), dot(E, kb), dot(E, n)));
      float3 refl = Lt - 2.0 * dot(vndn, Lt) * vndn;
      bp = saturate(dot(Et, -refl));
    }
    float ks = bp > 0.0 ? pow(max(bp, 1e-6), 10.0 + 290.0 * m2.y) : 0.0;
    lin += ks * float3(2.1, 1.8, 1.5) * (shadow * m2.x);
  }
  // Fog -> exposure -> tonemap -> sqrt, then the 1.41 uber scene multiplier.
  float fdist = length(i.rpos);
  float f1 = saturate(fdist * sh_fogp.x + sh_fogp.y);
  if (sh_fogp.z != 1.0) {
    f1 = pow(max(f1, 1e-6), sh_fogp.z);
  }
  float3 fog_rgb = sh_fogc.rgb * f1;
  float fog_a = (1.0 + sh_fogc.a * f1) * dyn_misc.x;  // x material multiplier
  float3 xe = (lin * fog_a + fog_rgb) * dyn_sun.w;
  return ToneOut(xe, 1.0, false);

}
