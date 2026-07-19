// Water families, hand-ported from the game's own pixel shaders and
// verified per-pixel against the ucode: flowingwateralpha (fam 30),
// the ocean surface (fam 31) and the baked ocean horizon sheet
// (fam 32). Callers pass the shared world-family locals (squared
// diffuse, fog terms, scene exposure).
// flowingwateralpha (fam 30): the canal/waterfall water shader,
// hand-ported from flowingwateralpha_defaultPS and verified per-pixel
// against the ucode (dual time-scrolled ripple-normal taps -> TBN ->
// lightmap-lit body + phong sun spec + cube reflection, then the
// standard fog/exposure/tonemap chain; alpha = the spec term's green
// channel, floored; reflection strength drives opacity over the bed).
float4 ShadeFlowingWater(VSOut i, float3 dlin, float fog_a,
                         float3 fog_rgb, float expo) {
  float t = wat_p3.x;
  float3 n1 = macro.Sample(smp, i.uv * wat_p2.xy + wat_p1.xy * t).rgb;
  float3 n2 = macro.Sample(smp, i.uv * wat_p2.zw + wat_p1.zw * t).rgb;
  float3 nsum = 2.0 * n1 + 2.0 * n2 - 2.0;
  // vNormal = normalize(m_params[0].xzw * (n1 + n2)) in tangent space.
  float3 vn = normalize(float3(nsum.x * wat_p0.x, nsum.y * wat_p0.z,
                               nsum.z * wat_p0.w));
  float3 wn0 = dot(i.nrm, i.nrm) > 0.01
                   ? normalize(i.nrm)
                   : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
  // Stored frame: tanb = the mesh's usage-6 TANGENT here (see the water
  // pack in DecodeMesh), sentinel sign = the unwrap sign.x;
  // B = sign.x * cross(N, T), exactly the game's VS.
  float3 wt, wb;
  if (i.tanb.w > 0.1) {
    wt = normalize(i.tanb.xyz);
    wb = cross(wn0, wt) * (i.tanb.w > 0.6 ? 1.0 : -1.0);
  } else {
    ScreenTangentFrame(wn0, i.rpos, i.uv, wt, wb);
  }
  float3 wn = normalize(vn.x * wt + vn.y * wb + vn.z * wn0);
  float3 vd = -normalize(i.rpos);
  float s = min(SampleCsmShadowSoft(i.rpos + cam_pos.xyz, 0.0, wn, i.pos.xy),
                SampleStaticSun(i.rpos + cam_pos.xyz, wn, i.pos.xy));
  // Lightmap at the unwrap + 0.01 * vNormal.xz (the ripple refraction
  // wobble), console fetch semantics, min-clamped like every world
  // family. White-fallback pages skip the clamp (see the env fetch).
  float2 wlm_uv = i.uv2 + 0.01 * vn.xz;
  float3 wlm = lightmap.SampleLevel(smp_clamp, wlm_uv, 0.0).rgb;
  float3 lml = tint.r > 0.0 ? min(wlm * wlm, s + sh_color.rgb)
                            : wlm * wlm;
  // GetTangentLight on the WORLD normal: fixed axis (0.58, 0.62, 0.39)
  // with xy signs following the sun direction, x 1/normalize(axis).z.
  float kd = (wn.x * 0.58 * sign(sh_sun.x) +
              wn.y * 0.62 * sign(sh_sun.y) + wn.z * 0.39) * 2.39562;
  float3 lin = lml * kd * dlin;
  // Spec/reflection masks (t4): x = phong mask, z = reflection mask,
  // thresholded by m_params[3].y. Zero until resolved: matte water.
  float2 wmask = float2(0.0, 0.0);
  if (overlay.x > 0.0) {
    float4 wm4 = decal_art.Sample(smp, i.uv);
    wmask = saturate(float2(wm4.x, wm4.z) - wat_p3.y);
  }
  // Phong vs the real sun, power m_params[3].z, tint (2.1, 1.8, 1.5),
  // scaled by sat(clamped lightmap green - 0.1).
  float3 wrl = 2.0 * wn * dot(wn, sh_sun.xyz) - sh_sun.xyz;
  float ks = pow(max(saturate(dot(vd, wrl)), 1e-6), wat_p3.z);
  float3 wspec =
      ks * wmask.x * float3(2.1, 1.8, 1.5) * saturate(lml.g - 0.1);
  // Cube reflection, same shape as fams 5/6: reflect(E, wN) with xy
  // negated, luminosity lerped toward 1 by 0.3 * sat(4*mask - 2.6),
  // x mask x 1.5. misc.y = the cube LOD bias (640p-gradient parity).
  float3 wrv = vd - 2.0 * wn * dot(vd, wn);
  float3 wcube =
      overlay.y > 0.0
          ? env_cube.SampleBias(smp, float3(-wrv.x, -wrv.y, wrv.z),
                                misc.y).rgb
          : float3(0.0, 0.0, 0.0);
  float wrlum = 0.3 * saturate(4.0 * wmask.y - 2.6);
  float3 wrefl = wcube * (lml.g + wrlum * (1.0 - lml.g)) * wmask.y * 1.5;
  float3 wspec_term = wspec + wrefl;
  lin += wspec_term;
  float3 wxe = (lin * (fog_a * wat_p0.y) + fog_rgb) * expo;
  return ToneOut(wxe, max(wspec_term.g, wat_p3.w), false);

}

// Exact ocean surface (fam 31, ocean_defaultPS; verified per-pixel
// against the ucode): PCA-compressed dual-component normal map, lerped
// toward flat by the overlay-scaled tonedown, cube reflection x squared
// lightmap x fresnel, ward anisotropic sun spec. OPAQUE (the game's PS
// outputs alpha 0 and draws without blending).
float4 ShadeOcean(VSOut i, float fog_a, float3 fog_rgb, float expo) {
  float2 suv = i.uv * oc_p1.w;
  float4 c0v = macro.Sample(smp, suv) * 2.0 - 1.0;       // t3: comp 0
  float4 c1v = detail_map.Sample(smp, suv) * 2.0 - 1.0;  // t8: comp 1
  // PCA normal in the shader's Z-UP basis; mean/weights are stored in
  // 0..1 texture space and folded 2x-1 after the sum.
  float3 pca = (float3(dot(c0v, oc_w0) + dot(c1v, oc_w1),
                       dot(c0v, oc_w2) + dot(c1v, oc_w3),
                       dot(c0v, oc_w4) + dot(c1v, oc_w5)) +
                oc_mean.xyz) * 2.0 - 1.0;
  // tonedown = 2 * m_params[1].z * sat(dot(c1v, wB1) + overlay.x);
  // the ucode folds the second PCA blue dot into the overlay term.
  float ovx = overlay.z > 0.0
                  ? spec2_map.Sample(smp, i.uv * overlay.w).x
                  : 1.0;
  float otd = (2.0 * oc_p1.z) * saturate(dot(c1v, oc_w5) + ovx);
  float3 nt = normalize(lerp(float3(0.0, 0.0, 1.0), pca, otd));
  nt = normalize(max(abs(nt), 0.001));
  float3 N = float3(nt.x, nt.z, nt.y);  // shader Z-up -> world Y-up
  float3 E = -normalize(i.rpos);
  // Cube reflection: reflect(E, N) with xy negated, x the SQUARED
  // single-tap lightmap (no chromaticity, no CSM clamp in this PS).
  float3 orv = E - 2.0 * N * dot(E, N);
  float3 ocube =
      overlay.y > 0.0
          ? env_cube.SampleBias(smp, float3(-orv.x, -orv.y, orv.z),
                                misc.y).rgb
          : float3(0.0, 0.0, 0.0);
  float2 olm_uv = i.uv2 + 0.01 * nt.xy;
  float3 olm = lightmap.SampleLevel(smp_clamp, olm_uv, 0.0).rgb;
  float3 orefl = ocube * (olm * olm);
  float NV = abs(dot(N, E));
  float F = 0.25 + 0.75 * pow(1.0 - NV, 5.0);
  // Ward anisotropic sun spec (spreads oc_p2.xy, roughness oc_p2.z).
  float3 oud = normalize(float3(-N.z, 0.0, N.x));
  float3 ovd = cross(N, oud);
  float dNE = dot(N, E);
  float dNL = dot(N, sh_sun.xyz);
  float oax = oc_p2.z / oc_p2.x;
  float oay = oc_p2.z / oc_p2.y;
  float3 H = normalize(sh_sun.xyz + E);
  float eu = dot(H, oud) / oax;
  float ev = dot(H, ovd) / oay;
  float oden = sqrt(abs(dNL * dNE)) * 12.566371 * oax * oay;
  float ward = exp(-2.0 * (eu * eu + ev * ev) / (1.0 + dot(H, N))) /
               max(oden, 1e-6);
  float3 ocol = orefl * F + ward * oc_p0.rgb;
  float3 oxe = (ocol * (fog_a * oc_p1.y) + fog_rgb) * expo;
  return ToneOut(oxe, 1.0, false);

}

// Baked ocean horizon sheet (fam 32, oceanreflection_defaultPS;
// verified per-pixel): squared sea texture x multiplier, fogged and
// tonemapped; alpha ramps with world height between the orf.z/orf.y
// bounds (the bright haze band fading in toward the horizon).
float4 ShadeOceanReflection(VSOut i, float3 dlin, float fog_a,
                            float3 fog_rgb, float expo) {
  float3 rcol = dlin * orf.x;
  float ra = saturate((i.rpos.y + cam_pos.y - orf.z) /
                      max(orf.y - orf.z, 1e-4));
  float3 rxe = (rcol * fog_a + fog_rgb) * expo;
  return ToneOut(rxe, ra, false);

}
