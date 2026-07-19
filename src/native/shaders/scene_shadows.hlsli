// Shadow receivers: the game-exact CSM atlas sampler, its
// contact-hardening PCSS variant, and the native static sun-shadow map
// sampler. Expects the including file to declare the b1 rows
// (scene_frame_cb.hlsli), the shadow_atlas / static_sun textures and
// ShowcaseMask before this include.
// Native CSM atlas sample at a world position: finest covering cascade,
// s = saturate(infront + 1 - coverage). Returns 1 (lit) when uncovered or
// shadows are off. extra_bias suppresses receiver self-shadow acne on
// surfaces that are themselves casters (characters, held board), in DEPTH
// UNITS like the game's own bias constants (receiver c5.x, the dynobj
// literals); a world-metric bias was tried for the editor face band and
// REVERTED: the band was the HAT casting (the game's shadow passes skip
// it, see shadow_caster parity), not under-biasing, and the 0.144 m value
// erased the legit head-onto-neck shadow. A NEGATIVE extra_bias selects
// the game's dynamicobject receive bias instead: a per-cascade literal
// 0.007 (finest) / 0.015 (outer) replacing sh_misc.x
// (from the dynamicobject_defaultPS ucode). Props
// are casters themselves, and with only the world bias (c5.x = 0 in every
// capture) their flat tops compared against their own atlas depth; the
// lit/dark whole-surface flicker on benches, signs and sails as the
// camera-following cascades drifted frame to frame.
float SampleCsmShadow(float3 wp, float extra_bias) {
  if (sh_misc.y <= 0.0) {
    return 1.0;
  }
  float2 lsv = float2(dot(sh_x.xyz, wp) + sh_x.w, dot(sh_y.xyz, wp) + sh_y.w);
  float2 luv = 0.0;
  float casc = 0.0;
  float2 l2 = lsv * sh_c2.xy + sh_c2.zw;
  if (max(abs(l2.x), abs(l2.y)) < 0.99) { luv = l2; casc = 3.0; }
  float2 l1 = lsv * sh_c1.xy + sh_c1.zw;
  if (max(abs(l1.x), abs(l1.y)) < 0.99) { luv = l1; casc = 2.0; }
  if (max(abs(lsv.x), abs(lsv.y)) < 0.99) { luv = lsv; casc = 1.0; }
  if (casc <= 0.0) {
    return 1.0;
  }
  float2 suv = float2(luv.x / 6.0 + (casc * 2.0 - 1.0) / 6.0,
                      luv.y * -0.5 + 0.5);
  // Character receivers: the game's EXACT sampling (cacstamp_skin_nisPS
  // ucode + live captured constants; offline-validated against the
  // emulated frame). Reference = saturate(raw_depth - c9[cascade])
  // (the per-cascade biases REPLACE the world receiver bias), NINE POINT
  // taps at +-1 game-texel of the 512 tile (u step 1/1536 of the 3-tile
  // atlas), binary sge each, averaged x 1/9 (literal c255.w). No coverage
  // term: uncovered texels hold the far clear and compare lit.
  if (extra_bias > 0.0 && sh_char.w > 0.0) {
    float cbias = casc > 2.5 ? sh_char.z : (casc > 1.5 ? sh_char.y : sh_char.x);
    float refd = saturate(dot(sh_z.xyz, wp) + sh_z.w - cbias);
    // Tap offsets in game-map texels, BILINEAR-filtered like the game's
    // fetches; the atlas blur dilates depth outward exactly so filtered
    // compares stay valid across coverage edges; point-snapped taps
    // quantized the penumbra into a stippled banding ("glisten"). Taps
    // clamp half a texel inside the tile so filtering never bleeds the
    // neighboring cascade.
    float2 atlas_dim = float2(sh_misc.z, sh_misc.w);
    float2 game_texel = float2(1.0 / 1536.0, 1.0 / 512.0);
    float tile_x0 = (casc * 2.0 - 2.0) / 6.0;
    float tile_x1 = casc * 2.0 / 6.0;
    float acc = 0.0;
    [unroll] for (int k = 0; k < 9; ++k) {
      const float2 kOfs[9] = {float2(0.0, 0.0),   float2(-1.0, -1.0),
                              float2(0.0, -1.0),  float2(1.0, -1.0),
                              float2(-1.0, 0.0),  float2(1.0, 0.0),
                              float2(-1.0, 1.0),  float2(0.0, 1.0),
                              float2(1.0, 1.0)};
      float2 s = suv + kOfs[k] * game_texel;
      s.x = clamp(s.x, tile_x0 + 0.5 / atlas_dim.x, tile_x1 - 0.5 / atlas_dim.x);
      s.y = clamp(s.y, 0.5 / atlas_dim.y, 1.0 - 0.5 / atlas_dim.y);
      acc += shadow_atlas.Sample(smp_clamp, s).x >= refd ? 1.0 : 0.0;
    }
    return acc / 9.0;
  }
  float bias = extra_bias < 0.0 ? (casc > 1.5 ? 0.015 : 0.007)
                                : sh_misc.x + extra_bias;
  float rd = dot(sh_z.xyz, wp) + sh_z.w - bias;
  float2 sm2 = shadow_atlas.Sample(smp_clamp, suv);
  return saturate((sm2.x >= rd ? 1.0 : 0.0) + (1.0 - sm2.y));
}
// Contact-hardening soft shadows (PCSS) over the same cascade atlas,
// enabled by sh_pcss.x. A blocker search estimates the average caster
// depth inside a world-space search radius; the penumbra half-width then
// follows the sun's angular size times the caster-to-receiver distance
// along the light axis (converted between world meters, depth-map units
// and light-space UV via the CPU-computed sh_pcss2 row scales), and a
// rotated poisson-disk PCF filters at that width; shadows are crisp at
// the contact point and soften with caster height, instead of the single
// fixed-width penumbra the blur chain bakes in. Tap semantics match the
// exact path (sat(infront + 1 - coverage); the blur pass dilates depth
// outward so filtered compares stay valid across coverage edges), and the
// per-class bias conventions of SampleCsmShadow carry over via extra_bias.
// The receiver normal drives an additional slope-scaled bias (sh_pcss.w)
// so large static casters, ground sheets, facades at grazing sun angles,
// never compare against their own atlas depth.
static const float2 kPcssTaps[12] = {
    float2(-0.326212, -0.405805), float2(-0.840144, -0.073580),
    float2(-0.695914, 0.457137),  float2(-0.203345, 0.620716),
    float2(0.962340, -0.194983),  float2(0.473434, -0.480026),
    float2(0.519456, 0.767022),   float2(0.185461, -0.893124),
    float2(0.507431, 0.064425),   float2(0.896420, 0.412458),
    float2(-0.321940, -0.932615), float2(-0.791559, -0.597705)};
float CsmSoftTap(float2 suv, float refd, float tile_x0, float tile_x1) {
  // Taps clamp half a texel inside the tile so filtering never bleeds the
  // neighboring cascade (same convention as the exact character taps).
  suv.x = clamp(suv.x, tile_x0 + 0.5 / sh_misc.z, tile_x1 - 0.5 / sh_misc.z);
  suv.y = clamp(suv.y, 0.5 / sh_misc.w, 1.0 - 0.5 / sh_misc.w);
  float2 sm = shadow_atlas.Sample(smp_clamp, suv);
  return saturate((sm.x >= refd ? 1.0 : 0.0) + (1.0 - sm.y));
}
// Poisson-rotation angle for the CSM atlas sampler: interleaved gradient
// noise of the SCREEN pixel: stable per pixel, smooth under motion. A
// world-position hash was rejected (sin(43758 * x) amplifies
// sub-millimeter float wobble of the reconstructed position into a fresh
// rotation every frame; penumbras boiled). The static sun-shadow sampler
// uses NO noise at all; its soft-compare Vogel kernel is smooth without
// it (verified offline), and every noise scheme read as speckle, stripes
// or grain there.
float PcssRotation(float2 px) {
  return 6.2831853 *
         frac(52.9829189 * frac(0.06711056 * px.x + 0.00583715 * px.y));
}
float SampleCsmShadowSoft(float3 wp, float extra_bias, float3 nrm,
                          float2 px) {
  // Showcase: until the shadow layer is revealed, render fully lit.
  int sc_mask = ShowcaseMask(px.x);
  if (sc_mask >= 0 && (sc_mask & 8) == 0) {
    return 1.0;
  }
  if (sh_pcss.x <= 0.5) {
    return SampleCsmShadow(wp, extra_bias);
  }
  if (sh_misc.y <= 0.0) {
    return 1.0;
  }
  float2 lsv = float2(dot(sh_x.xyz, wp) + sh_x.w, dot(sh_y.xyz, wp) + sh_y.w);
  float2 luv = 0.0;
  float casc = 0.0;
  float2 cscale = float2(1.0, 1.0);
  float2 l2 = lsv * sh_c2.xy + sh_c2.zw;
  if (max(abs(l2.x), abs(l2.y)) < 0.99) { luv = l2; casc = 3.0; cscale = sh_c2.xy; }
  float2 l1 = lsv * sh_c1.xy + sh_c1.zw;
  if (max(abs(l1.x), abs(l1.y)) < 0.99) { luv = l1; casc = 2.0; cscale = sh_c1.xy; }
  if (max(abs(lsv.x), abs(lsv.y)) < 0.99) { luv = lsv; casc = 1.0; cscale = float2(1.0, 1.0); }
  if (casc <= 0.0) {
    return 1.0;
  }
  // Receiver bias: the same per-class conventions as SampleCsmShadow
  // (character per-cascade rows / dynobj literals / world bias), plus the
  // slope-scaled static term.
  float bias;
  if (extra_bias > 0.0 && sh_char.w > 0.0) {
    bias = casc > 2.5 ? sh_char.z : (casc > 1.5 ? sh_char.y : sh_char.x);
  } else if (extra_bias < 0.0) {
    bias = casc > 1.5 ? 0.015 : 0.007;
  } else {
    bias = sh_misc.x + extra_bias;
  }
  float ndl = 1.0;
  if (dot(nrm, nrm) > 1e-4) {
    ndl = abs(dot(normalize(nrm), sh_sun.xyz));
  }
  float slope = sqrt(saturate(1.0 - ndl * ndl)) / max(ndl, 0.12);
  bias += sh_pcss.w * (1.0 + slope);
  float refd = dot(sh_z.xyz, wp) + sh_z.w - bias;
  float2 suv0 = float2(luv.x / 6.0 + (casc * 2.0 - 1.0) / 6.0,
                       luv.y * -0.5 + 0.5);
  float tile_x0 = (casc * 2.0 - 2.0) / 6.0;
  float tile_x1 = casc * 2.0 / 6.0;
  // Light-space UV per meter for this cascade; luv -> atlas uv is
  // (x / 6, y / 2) (three tiles across, [-1,1] vertical).
  float2 uvpm = sh_pcss2.z * cscale;
  float2 luv_to_suv = float2(1.0 / 6.0, 0.5);
  float ang = PcssRotation(px);
  float2 rot = float2(cos(ang), sin(ang));
  // Blocker search over 7 taps in the search radius. The penumbra follows
  // the NEAREST blocker (max depth below the reference), not the average:
  // the search disk can straddle unrelated far casters; a receiver near a
  // building's shadow region would average the building's depth into its
  // own contact shadow and smear it to the max radius (the fully-blurred
  // character shadow). The nearest blocker is the one whose penumbra the
  // receiver is actually inside.
  float2 rsearch = sh_pcss2.x * uvpm * luv_to_suv;
  float bmax = -1.0;
  {
    float2 s = suv0;
    s.x = clamp(s.x, tile_x0 + 0.5 / sh_misc.z, tile_x1 - 0.5 / sh_misc.z);
    s.y = clamp(s.y, 0.5 / sh_misc.w, 1.0 - 0.5 / sh_misc.w);
    float d = shadow_atlas.Sample(smp_clamp, s).x;
    if (d < refd) { bmax = max(bmax, d); }
  }
  [unroll] for (int k = 0; k < 12; k += 2) {
    float2 o = kPcssTaps[k];
    o = float2(o.x * rot.x - o.y * rot.y, o.x * rot.y + o.y * rot.x);
    float2 s = suv0 + o * rsearch;
    s.x = clamp(s.x, tile_x0 + 0.5 / sh_misc.z, tile_x1 - 0.5 / sh_misc.z);
    s.y = clamp(s.y, 0.5 / sh_misc.w, 1.0 - 0.5 / sh_misc.w);
    float d = shadow_atlas.Sample(smp_clamp, s).x;
    if (d < refd) { bmax = max(bmax, d); }
  }
  if (bmax < 0.0) {
    // No caster inside the search footprint: every filter tap would
    // compare lit (uncovered texels hold the far clear).
    return 1.0;
  }
  float dist_m = max(refd - bmax, 0.0) * sh_pcss2.y;
  float pen_m = min(dist_m * sh_pcss.y, sh_pcss.z);
  // Filter radius: the penumbra in atlas units, floored at a fraction of a
  // physical atlas texel so edges always stay filtered.
  float2 rfilt = max(pen_m * uvpm, sh_pcss2.w * 2.0 / sh_misc.w) * luv_to_suv;
  float acc = CsmSoftTap(suv0, refd, tile_x0, tile_x1);
  [unroll] for (int j = 0; j < 12; ++j) {
    float2 o = kPcssTaps[j];
    o = float2(o.x * rot.x - o.y * rot.y, o.x * rot.y + o.y * rot.x);
    acc += CsmSoftTap(suv0 + o * rfilt, refd, tile_x0, tile_x1);
  }
  return acc / 13.0;
}
// Native static sun-shadow receive (t10). The game's cascade transforms
// use a stylized near-vertical projection (fine for its own dynamic
// casters inside a ~12 m height window, ~40 degrees away from the
// material sun); tall static geometry needs a true sun-aligned
// projection, so static shade comes from this separate camera-centered
// ortho map instead of the CSM atlas. The term bottoms out at
// 1 - strength (nsm_p.x), keeping live static shade believable where the
// baked lighting's own sun disagrees, and fades out at the map edge so
// coverage never pops. Contact-hardening PCSS matching the atlas sampler
// (nearest-blocker search + rotated poisson PCF) when PCSS is enabled;
// a single compare otherwise. Uncovered texels hold the far clear = lit.
// Clamped tap into one tile of the three-tile static sun atlas (inner,
// mid, far left to right); taps never bleed across the tile seams.
float NsmTap(float2 suv, float refd, float tile_u0) {
  float hx = 0.1666667 / nsm_p2.w;  // half a texel in atlas u (3 tiles wide)
  float hy = 0.5 / nsm_p2.w;
  suv.x = clamp(suv.x, tile_u0 + hx, tile_u0 + 0.3333333 - hx);
  suv.y = clamp(suv.y, hy, 1.0 - hy);
  // Explicit LOD (the map is single-mip): the PCF loop below is dynamic
  // and implicit-gradient sampling is not allowed in divergent flow.
  return static_sun.SampleLevel(smp_clamp, suv, 0.0).x;
}
float SampleStaticSun(float3 wp, float3 nrm, float2 px) {
  if (nsm_p.x <= 0.0) {
    return 1.0;
  }
  // Showcase: until the shadow layer is revealed, render fully lit.
  int sc_mask = ShowcaseMask(px.x);
  if (sc_mask >= 0 && (sc_mask & 8) == 0) {
    return 1.0;
  }
  float2 uc_far = float2(dot(nsm_x.xyz, wp) + nsm_x.w,
                         dot(nsm_y.xyz, wp) + nsm_y.w);
  // Screen-pixel footprint in far-tile uc, BEFORE any divergent return
  // (gradient op; the enclosing family branches are uniform per draw).
  // Scales the filter floor with viewing distance so far shadows
  // integrate over the map's texel grid instead of aliasing against it:
  // the shadow-map analogue of texture mip filtering. Close-ups keep the
  // 2-texel floor untouched.
  float pxfoot = max(fwidth(uc_far.x), fwidth(uc_far.y));
  float edge = max(abs(uc_far.x), abs(uc_far.y));
  if (edge >= 0.99) {
    return 1.0;
  }
  // Fade at the outer (far-tile) boundary only; the near->far handoff
  // needs none (the far tile covers the near region too).
  float fade = saturate((0.97 - edge) / 0.07);
  // Cascade select (3 tiles sharing the origin, so each finer coordinate
  // is an exact rescale of the far one). Ratios 6/2/1: the inner cascade
  // trades peak density for reach; foliage dapple at 16x density read as
  // over-detailed and only engaged very close; 6x at a third of the far
  // radius is the calibrated compromise. Integer 1:2:6 chain keeps every
  // tile on the far-texel snap grid. Keep in sync with
  // RenderStaticSunMap's kRatioInner/kRatioMid.
  const float kRatioInner = 6.0;
  const float kRatioMid = 2.0;
  float2 uc0 = uc_far * kRatioInner;
  float2 uc1 = uc_far * kRatioMid;
  float tile;
  float2 uc;
  float scale;
  if (max(abs(uc0.x), abs(uc0.y)) < 0.99) {
    tile = 0.0;
    uc = uc0;
    scale = kRatioInner;
  } else if (max(abs(uc1.x), abs(uc1.y)) < 0.99) {
    tile = 1.0;
    uc = uc1;
    scale = kRatioMid;
  } else {
    tile = 2.0;
    uc = uc_far;
    scale = 1.0;
  }
  float tile_u0 = tile / 3.0;
  float ucpm = nsm_p.y * scale;  // uc units per meter in this tile
  float2 suv = float2(uc.x / 6.0 + (tile * 2.0 + 1.0) / 6.0,
                      uc.y * -0.5 + 0.5);
  const float2 uc_to_suv = float2(1.0 / 6.0, 0.5);
  float ndl = 1.0;
  if (dot(nrm, nrm) > 1e-4) {
    ndl = abs(dot(normalize(nrm), sh_sun.xyz));
  }
  float slope = sqrt(saturate(1.0 - ndl * ndl)) / max(ndl, 0.12);
  float refd = dot(nsm_z.xyz, wp) + nsm_z.w - (nsm_p2.x + nsm_p2.y * slope);
  if (sh_pcss.x <= 0.5) {
    float s0 = NsmTap(suv, refd, tile_u0) >= refd ? 1.0 : 0.0;
    return 1.0 - fade * nsm_p.x * (1.0 - s0);
  }
  // FIXED kernels throughout this sampler: no per-pixel noise. Verified
  // offline against synthetic roof/wall/streetlamp depth maps at the real
  // map scales: every noise scheme (world hash, interleaved gradient
  // noise, white hash, radius jitter) renders the penumbra as speckle,
  // stripes or grain, while fixed taps with the soft compares below give
  // a clean smooth gradient (the penumbra fraction varies smoothly across
  // receivers, and bilinear map sampling fills between taps). Denser
  // blocker searches were also tried offline and REJECTED; unstable
  // nearest-blocker picks around small casters draw dark veins.
  float2 rsearch = sh_pcss2.x * ucpm * uc_to_suv;
  float bmax = -1.0;
  {
    float d = NsmTap(suv, refd, tile_u0);
    if (d < refd) { bmax = d; }
  }
  [unroll] for (int k = 0; k < 12; k += 3) {
    float d = NsmTap(suv + kPcssTaps[k] * rsearch, refd, tile_u0);
    if (d < refd) { bmax = max(bmax, d); }
  }
  if (bmax < 0.0) {
    return 1.0;
  }
  float dist_m = max(refd - bmax, 0.0) * nsm_p.z;
  float pen_m = min(dist_m * sh_pcss.y, sh_pcss.z);
  // Filter floor: 2 physical texels, raised to ~2 screen-pixel footprints
  // at distance (lab-calibrated distance anti-aliasing).
  float floor_uc = max(nsm_p.w, 2.0 * pxfoot * scale);
  float r_uc = max(pen_m * ucpm, floor_uc);
  float2 rfilt = r_uc * uc_to_suv;
  // Vogel-spiral PCF with SOFT depth compares: binary taps quantize the
  // penumbra into N discrete levels and the stable per-pixel rotation
  // arranges the quantization into a visible cross-hatch ("fine mesh")
  // pattern; a continuous per-tap contribution over a small depth window
  // turns the same taps into a smooth gradient. The window rides the
  // receiver bias so contact shadows stay grounded. Wide penumbras (tall
  // roof edges cap the radius) undersample the base kernel and re-weave
  // the mesh: they spend a second Vogel ring instead; the branch is
  // spatially coherent across a penumbra.
  float w = max(nsm_p2.x * 2.0, 1e-5);
  // Tap count follows the FINAL radius (penumbra or adaptive floor,
  // whichever won); wide kernels undersample at 13 taps.
  int n = r_uc > 0.3 * ucpm ? 26 : 13;
  float acc = 0.0;
  [loop] for (int j = 0; j < n; ++j) {
    float r = sqrt((float(j) + 0.5) / float(n));
    float a = float(j) * 2.399963;
    float2 o = float2(cos(a), sin(a)) * r;
    float d = NsmTap(suv + o * rfilt, refd, tile_u0);
    acc += saturate((d - refd) / w + 0.5);
  }
  float s = acc / float(n);
  return 1.0 - fade * nsm_p.x * (1.0 - s);
}
