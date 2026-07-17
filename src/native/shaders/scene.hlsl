// Face-normal shading uses the camera-relative world position: interpolating
// absolute world coordinates (hundreds of meters) destroys ddx/ddy precision
// and produces per-pixel noise.
cbuffer C : register(b0) {
  row_major float4x4 world;
  row_major float4x4 mvp;
  float4 tint;
  float4 cam_pos;
  // Per-material color multiplier (w > 0 enables): CAS hair is a grayscale
  // texture times the character's hair color.
  float4 mat_tint;
  // x = macroOverlayUVScale, y = macroOverlayOpacity, z > 0 = macro overlay
  // texture bound at t3, w > 0 = environment.decal item with its `decal`
  // art bound at t4 (composited in-shader over the diffuse, exactly like
  // the game's decalenvironment PS: lerp by the art's alpha; opaque).
  // Macro and decal art are INDEPENDENT: decal ground/wall sections carry
  // the same macrooverlay as their non-decal neighbors, sharing one slot
  // dropped the macro grime there and rendered alternating plaza sections
  // ~1.4x too bright (the large-scale ground checkerboard).
  float4 overlay;
  // x > 0 = environment.transparent item (alpha-blended sub-pass): shading
  // follows the game's transparentenvironment.fx; the opaque pass's
  // alpha-test turned the soft mist gradients into solid white cloud blobs.
  // yzw = the global distance-fog RAMP (scale/bias/exponent from main-pass
  // VS c5), and mat_tint doubles as the linear-space fog COLOR (rgb +
  // transmittance scale in w, VS c6) for transparent items; the root
  // signature is capped at 64 DWORDs, so fog rides in slots transparent
  // items never use otherwise. Fog is currently applied to transparent
  // items only (the km-distant mist sheets; everything else we render is
  // near enough for fog to be negligible).
  float4 misc;
};
// Per-frame dynamic-shadow (CSM) receiver constants, captured from the
// game's world-material PIXEL banks.
cbuffer S : register(b1) {
  float4 sh_x;      // light-space X row (xyz) + translation (w)   [PS c0]
  float4 sh_y;      // light-space Y row                           [PS c3]
  float4 sh_z;      // depth row (height ramp)                     [PS c4]
  float4 sh_c1;     // cascade 1 scale.xy + offset.zw              [PS c1]
  float4 sh_c2;     // cascade 2 scale.xy + offset.zw              [PS c2]
  float4 sh_color;  // shadow color rgb [PS c8] + its luma in w
  float4 sh_misc;   // x = depth bias [PS c5.x], y = enable,
                    // zw = atlas dimensions (3*tile, tile)
  // Exact world-shading frame rows (consumed by the env-family branch):
  float4 sh_sun;    // xyz = sun direction [PS c6], w = scene exposure [c10.x]
  float4 sh_env;    // x = material multiplier [PS c11.y], yz = tree lightmap
                    // scale/floor [tree PS c0.xy], w = tree tint mult [c4.y]
  float4 sh_fogp;   // xyz = global fog ramp scale/bias/exp [VS c5],
                    // w = proxyworld scale [proxy PS c3.y]
  float4 sh_fogc;   // fog color rgb + transmittance scale in w [VS c6]
  // dynamicobject.fx frame-global lighting rows (see FrameScene::dynobj_rows).
  float4 dyn_sun;   // xyz = sun direction (PS c9), w = scene exposure (c13.x)
  float4 dyn_amb;   // xyz = flat ambient (c15.rgb), w = bounce scale (c15.w)
  float4 dyn_misc;  // x = material multiplier (c14.y),
                    // y = static world-shadow floor (c8.w)
  // Character CSM receive biases (game char PS bank, finest cascade first)
  // + enable in w: the exact 9-tap character shadow sampling
  // (cacstamp_skin_nisPS port; see SampleCsmShadow). sh_misc.zw carry the
  // atlas dimensions for the point-snapped taps.
  float4 sh_char;
  // World-shading v2: x = stored-tangent polarity (the sign relating
  // cross(binormal, normal) x handedness to the game's tangent; +-1,
  // cvar-tunable against the emulated reference), yzw spare.
  float4 sh_v2;
};
// Character-family lighting (defaultcharacter.fx and friends): canonical
// per-draw rows captured from the guest PIXEL constant bank at palette
// capture (CaptureCharLighting has the per-family register maps; the math
// below was validated offline by executing the game's own pixel shaders).
// Enabled per draw via cam_pos.w = family (0 = not a character / capture
// failed -> the legacy empirical shading below).
cbuffer CH : register(b2) {
  float4 ch_light;  // xyz = sun direction, w = hair fresnel power
  float4 ch_key;    // rgb = key (sun) color, w = exposure
  float4 ch_amb;    // rgb = flat ambient, w = SH ambient multiplier / hair ambient
  float4 ch_sh[9];  // SH irradiance rows, pre-scaled (see capture);
                    // vehicles keep spec color + power in row 0 instead
  float4 ch_tintA;  // CAC diffuse tint / livingworld red-mask tint (w = apply)
  float4 ch_tintB;  // livingworld blue-mask tint / hair fresnel tint (w = strand-alpha scale)
  float4 ch_misc;   // x = alpha out, y = family
};
Texture2D<float4> diffuse : register(t0);
Texture2D<float4> lightmap : register(t1);
Texture2D<float4> macro : register(t3);
Texture2D<float4> decal_art : register(t4);
// Paired second descriptor of the t4 table: the reflective families'
// (fam 5/6) normal map. Only valid, and only sampled, when overlay.w == 4.
Texture2D<float4> normal_map : register(t5);
Texture2D<float2> shadow_atlas : register(t7);
TextureCube<float4> env_cube : register(t6);
// World-shading v2 material maps (the t8/t9 pair table): the environment
// families' detail normal map (t8, sampled at uv * misc.w) and the decal
// families' spec/ecc masks (t9). Sampled only when the misc.z bind flags
// say so (fams 1-4; see the env branch).
Texture2D<float4> detail_map : register(t8);
Texture2D<float4> spec2_map : register(t9);
// Raw bone palette: 3 float4 rows per bone, column-vector affine [R | t],
// applied with explicit dots (StructuredBuffer<float4x4> default packing is
// column-major and would silently transpose the matrices).
StructuredBuffer<float4> bones : register(t2);
SamplerState smp : register(s0);
// s1 = bilinear CLAMP (shared with the 2D pass). Decal art must clamp: the
// art UV runs far outside [0,1] across big ground sheets and the art's
// transparent border keeps everything outside the single placement clear;
// wrap sampling tiled the graffiti across the whole plaza.
SamplerState smp_clamp : register(s1);
struct VSOut {
  float4 pos : SV_Position;
  float3 rpos : TEXCOORD0;
  float2 uv : TEXCOORD1;
  float2 uv2 : TEXCOORD2;
  float3 nrm : TEXCOORD3;
  float2 uv3 : TEXCOORD4;
  // Stored tangent frame (world-shading v2): xyz = the mesh's authored
  // binormal (world-rotated; zero when the mesh carries none), w = the
  // tangent handedness sign. Rides the static meshes' otherwise-unused
  // blend-weight bytes (see DecodeMesh).
  float4 tanb : TEXCOORD5;
};
VSOut vs_main(float3 p : POSITION, float2 uv : TEXCOORD0, float2 uv2 : TEXCOORD1,
              float4 bw : BLENDWEIGHT0, uint4 bi : BLENDINDICES0,
              float3 nrm : NORMAL0, float2 uv3 : TEXCOORD2) {
  VSOut o;
  float4 mp = float4(p, 1.0);
  float3 n = nrm;
  // tint.g > 0 marks a skinned item: the bone palette (row-vector matrices)
  // maps model space to world space; mvp is then just view*proj.
  float wsum = dot(bw, float4(1, 1, 1, 1));
  if (tint.g > 0.0 && wsum > 0.001) {
    float3 skinned = float3(0, 0, 0);
    float3 sn = float3(0, 0, 0);
    // Guest blend indices are plain bone numbers (verified live: byte
    // streams like 02 00 03 01); bone k = palette rows 3k..3k+2.
    [unroll] for (int k = 0; k < 4; ++k) {
      uint row = bi[k] * 3u;
      skinned += bw[k] * float3(dot(mp, bones[row]), dot(mp, bones[row + 1]),
                                dot(mp, bones[row + 2]));
      sn += bw[k] * float3(dot(n, bones[row].xyz), dot(n, bones[row + 1].xyz),
                           dot(n, bones[row + 2].xyz));
    }
    mp = float4(skinned / wsum, 1.0);
    n = sn;
  } else {
    n = mul(n, (float3x3)world);
  }
  o.pos = mul(mp, mvp);
  o.rpos = mul(mp, world).xyz - cam_pos.xyz;
  o.uv = uv;
  o.uv2 = uv2;
  o.nrm = n;
  o.uv3 = uv3;
  // Stored binormal + handedness from the blend-weight bytes (statics
  // only; skinned items use those bytes for actual weights). w passes
  // RAW: ~0 = no stored frame, ~0.39 = negative handedness, ~0.78 =
  // positive (the DecodeMesh sentinel bytes 0/100/200).
  float3 sb = bw.xyz * 2.0 - 1.0;
  o.tanb = tint.g > 0.0 ? float4(0.0, 0.0, 0.0, 0.0)
                        : float4(mul(sb, (float3x3)world), bw.w);
  return o;
}
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
float4 ps_main(VSOut i) : SV_Target {
  if (tint.a > 0.0) {
    return tint;
  }
  float4 albedo = diffuse.Sample(smp, i.uv);
  // Alpha-tested foliage/fences; opaque formats sample alpha = 1. Character
  // diffuse packs GLOSS in alpha; never clip characters. tint.g > 0 marks
  // them: set for bones-bound skinned items AND for ropa cloth garments
  // rendered rigid (sim-active player tees, clipping their gloss alpha
  // discarded every pixel: the invisible-shirt bug; their decode writes
  // zero blend weights, so the VS skinning branch stays off).
  if (tint.g == 0.0 && overlay.w < 0.5 && cam_pos.w > -20.5) {
    // environment.transparent alpha-tests its SQUARED alpha at ref 16/255
    // (transparentenvironment.xml: ALPHAREF 16, PS outputs a = diffuse.a^2).
    // Exact env families (cam_pos.w < 0) use the game's world ALPHAREF 30.
    // dynamicobject items (cam_pos.w <= -21) are excluded here and clip in
    // their own branch (only the .alphatest variant tests, at ALPHAREF 30).
    // Fam 13 (reflective_trans glass, cam_pos.w = -13) never alpha-tests;
    // it alpha-BLENDS in the sorted sub-pass.
    if (cam_pos.w > -12.5 || cam_pos.w < -13.5) {
      float aref = cam_pos.w < -0.5 ? 0.1176 : 0.35;
      clip(misc.x > 0.0 ? albedo.a * albedo.a - 0.0627 : albedo.a - aref);
    }
  }
  // Exact sky dome (cam_pos.w = -40; sky_defaultPS transcribed from the
  // Skate 3 ucode in a live capture). The emulated frame's big sun
  // glow is computed IN the dome shader: a 1D radial gradient (the sky
  // material's `specular` channel, bound at t4 here) indexed by the sine of
  // the angle between the dome direction and the sun, its rgb^2 amplified
  // by 1/sat(a + 0.01), the alpha falloff makes the core HDR-bright, then
  // added to the squared panorama and run through the standard exposure/
  // tonemap/sqrt chain (no fog on the sky) and the postfx uber 1.41.
  // mat_tint.xyz = sun dir, mat_tint.w = the level sky elevation;
  // overlay.x = sun angular scale, overlay.y = pre-tone multiplier,
  // misc.y = scene exposure.
  if (cam_pos.w < -39.5 && cam_pos.w > -40.5) {
    // The dome mesh is camera-relative: world = mesh + (cam.x, sky_h,
    // cam.z), so the shader's dome-local direction (sky.fx In.vPos) is
    // rpos + (0, cam.y - sky_h, 0).
    float3 dome = normalize(i.rpos + float3(0.0, cam_pos.y - mat_tint.w, 0.0));
    float3 lin = albedo.rgb * albedo.rgb;
    float dotPL = saturate(dot(dome, mat_tint.xyz));
    float sinA = sqrt(saturate(1.0 - dotPL * dotPL));
    float4 sun = decal_art.Sample(smp_clamp, float2(sinA / overlay.x, 0.5 / 16.0));
    lin += sun.rgb * sun.rgb / saturate(sun.a + 0.01);
    float3 xe = lin * overlay.y * misc.y;
    float3 t1 = saturate(1.0 - xe);
    float3 tm = max(xe * 0.25 + 0.75, 1.0) - t1 * t1;
    float3 cc = saturate(sqrt(max(tm * 0.5, 0.0)) * 1.41);
    return float4(cc, 1.0);
  }
  // dynamicobject.fx props (cam_pos.w = -(20 + variant): -21 default,
  // -22 alphatest). Rigid movable objects (dispensers, dumpsters, benches,
  // cans). Lit with the game's own dynamicobject model (verified exact):
  // key sun light + bounce + flat ambient, gated by
  // the CSM shadow (the static world-shadow map is approximated as fully lit
  // - its floor c8.w bounds it), then fog -> exposure -> tonemap -> sqrt and
  // the postfx uber 1.41. v1 uses the geometric normal (cross of the mesh
  // tangent/binormal) with the flat normal-map kd 0.93429; the base/detail/
  // spec normal maps are v2.
  if (cam_pos.w < -20.5) {
    if (cam_pos.w < -21.5) {
      clip(albedo.a - 0.1176);  // dynamicobject.alphatest: ALPHAREF 30
    }
    float3 dlin = albedo.rgb * albedo.rgb;
    float3 n = dot(i.nrm, i.nrm) > 0.01
                   ? normalize(i.nrm)
                   : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
    float ndl = dot(n, dyn_sun.xyz);
    float key = saturate(ndl);  // key light gated on N.L >= 0
    float bounce = saturate(dot(n, float3(-dyn_sun.x, dyn_sun.y, -dyn_sun.z)));
    // CSM shadow (shared receiver rows at t5); the static world-shadow map is
    // not rendered natively, so its term is 1 and the min collapses to the
    // CSM (bounded below by the c8.w floor, dyn_misc.y). extra_bias -1 =
    // the game's own per-cascade dynamicobject receive bias (props are
    // casters; without it their flat tops self-shadow and flicker).
    float s = SampleCsmShadow(i.rpos + cam_pos.xyz, -1.0);
    float shadow = min(s * (ndl >= 0.0 ? 1.0 : 0.0), max(1.0, dyn_misc.y));
    float3 lighting = key * shadow + bounce * dyn_amb.w + dyn_amb.rgb;
    // GetTangentLight with the neutral (flat) normal map: 0.39 * 2.39562.
    float3 lin = lighting * 0.93429 * dlin;
    // Fog -> exposure -> tonemap -> sqrt, then the 1.41 uber scene multiplier.
    float fdist = length(i.rpos);
    float f1 = saturate(fdist * sh_fogp.x + sh_fogp.y);
    if (sh_fogp.z != 1.0) {
      f1 = pow(max(f1, 1e-6), sh_fogp.z);
    }
    float3 fog_rgb = sh_fogc.rgb * f1;
    float fog_a = (1.0 + sh_fogc.a * f1) * dyn_misc.x;  // x material multiplier
    float3 xe = (lin * fog_a + fog_rgb) * dyn_sun.w;
    float3 t1 = saturate(1.0 - xe);
    float3 tm = max(xe * 0.25 + 0.75, 1.0) - t1 * t1;
    return float4(saturate(sqrt(max(tm * 0.5, 0.0)) * 1.41), 1.0);
  }
  // Character families: the game's own lighting in LINEAR space (diffuse is
  // gamma -> square it), then the exact tone chain from the disassembly and
  // the postfx uber's 1.41 scene multiplier (which the empirical world
  // shading already folds into its constants; without it characters sit
  // ~30% darker than their surroundings, measured on an F11 A/B pair).
  if (cam_pos.w > 0.5) {
    float fam = cam_pos.w;
    float3 dlin = albedo.rgb * albedo.rgb;
    float3 cn = dot(i.nrm, i.nrm) > 0.01
                    ? normalize(i.nrm)
                    : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
    float ndl = saturate(dot(cn, ch_light.xyz));
    float3 vd = -normalize(i.rpos);
    float3 lin;
    float out_a = 1.0;
    if (fam > 5.5) {
      // Traffic vehicles (vehicle.fx fam 6 body / vehicle_glass.fx fam 7
      // windows, disassembled from vehicle_defaultPS): paint recolor where
      // the diffuse green channel is below the mask threshold (red-channel
      // mask * colorize_red + blue-channel mask * colorize_blue, the taxi
      // yellow), key light + the livingworld flat ambient, phong specular
      // along the reflected sun, and an environment-cube reflection scaled
      // by fresnel and the gloss packed in the diffuse alpha. Glass keeps
      // only the reflection terms (its tint rows are zero) and blends at
      // the captured alpha. overlay.y > 0 = the material's cube resolved
      // at t6 (same convention as water).
      float3 sel;
      if (fam < 6.5) {
        sel = dlin.g > 0.001225
                  ? dlin
                  : ch_tintA.rgb * dlin.r + ch_tintB.rgb * dlin.b;
      } else {
        sel = ch_tintA.rgb;
      }
      // DXN panel normal map (the material's `normal` channel, riding the
      // macro slot; overlay.z > 0 = resolved). The vertex layout carries no
      // tangent frame, so build a screen-space cotangent frame from the
      // position/uv derivatives; it reproduces the authored panel shading
      // including mirrored UV islands. Skipping the map entirely shades the
      // hinged panels by their vertex normals, which face away from the sun
      // - the dark ambient-blue "misdrawn shadow" that stopped at the door
      // seam (verified against the ucode: flat map = the artifact, real
      // map = the emulated car).
      float3 vn = cn;
      if (overlay.z > 0.5) {
        float2 nm = macro.Sample(smp, i.uv).rg * 2.0 - 1.0;
        float3 dp1 = ddx(i.rpos), dp2 = ddy(i.rpos);
        float2 du1 = ddx(i.uv), du2 = ddy(i.uv);
        float3 dp2p = cross(dp2, cn), dp1p = cross(cn, dp1);
        float3 tt = dp2p * du1.x + dp1p * du2.x;
        float3 bb = dp2p * du1.y + dp1p * du2.y;
        float im = rsqrt(max(max(dot(tt, tt), dot(bb, bb)), 1e-12));
        vn = normalize(nm.x * tt * im + nm.y * bb * im +
                       cn * sqrt(saturate(1.0 - dot(nm, nm))));
      }
      float vndl = saturate(dot(vn, ch_light.xyz));
      float3 rfl = ch_light.xyz - 2.0 * dot(vn, ch_light.xyz) * vn;
      // The sun spec is gated on N.L >= 0 (the ucode multiplies the spec
      // term by an sge result); the cube reflection is not.
      float spec = pow(saturate(dot(vd, -rfl)), max(ch_sh[0].w, 1.0)) *
                   (dot(vn, ch_light.xyz) >= 0.0 ? 1.0 : 0.0);
      float fres = pow(1.0 - saturate(dot(vn, vd)), max(ch_light.w, 1.0));
      float3 cube = float3(0.0, 0.0, 0.0);
      if (overlay.y > 0.5) {
        cube = env_cube.Sample(smp, reflect(-vd, vn)).rgb;
        cube *= cube;  // the PS consumes the cube squared (linear space)
      }
      // Gloss = the diffuse alpha SQUARED: the ucode squares the whole
      // diffuse fetch (linear-space decode), alpha included; raw alpha
      // over-specs ~5x and mottles the body panels.
      float gloss = fam < 6.5 ? albedo.a * albedo.a : 1.0;
      lin = sel * (ch_key.rgb * vndl + ch_amb.rgb) +
            (spec * ch_sh[0].rgb + cube) * fres * gloss;
      out_a = ch_misc.x;
    } else if (fam > 3.5) {
      // Hair (cac_hair / defaulthair): key on a wrapped N.L ramp + flat
      // ambient, fresnel rim tint on a steeper ramp; strand coverage from
      // the mesh's "alpha" channel at the raw second texcoord (bound at t4)
      // - alpha-blended in the sorted sub-pass (hair drawn opaque is the
      // blocky-helmet look).
      float fres = pow(1.0 - saturate(dot(cn, vd)), max(ch_light.w, 1.0));
      float3 hl = ch_key.rgb * (saturate(ndl * 0.75 + 0.25) + ch_amb.w) +
                  ch_tintB.rgb * fres * saturate(ndl * 1.75 + 0.25);
      lin = dlin * hl;
      out_a = saturate(decal_art.Sample(smp, i.uv2).r * ch_tintB.w);
    } else if (fam > 2.5) {
      // livingworld pedestrians: the diffuse is a stamp-mask atlas: red
      // regions recolor with tintA, blue with tintB (judged in linear
      // space; real-color regions have green above the threshold).
      // The game's character PSes multiply the key light by the CSM shadow
      // (tap >= ray = lit); characters are casters themselves, so an extra
      // receiver bias suppresses self-shadow acne while the body-onto-board
      // / body-onto-NPC shading survives (the sun-axis depth gap there is
      // tens of cm). Without this the held skateboard, a big flat surface
      // that is almost always inside the skater's own shadow, renders
      // fully sunlit (near-white) against the emulated dark deck.
      float csm = SampleCsmShadow(i.rpos + cam_pos.xyz, 0.012);
      float3 sel = dlin.g > 0.001225
                       ? dlin
                       : ch_tintA.rgb * dlin.r + ch_tintB.rgb * dlin.b;
      lin = sel * (ch_key.rgb * ndl * csm + ch_amb.rgb);
      // livingworld_stamp_defaultPS ends `max oC0.w, c21.x`: the entity's
      // spawn/distance fade. Only visible when the item is routed to the
      // blended sub-pass (alpha < 1); the opaque pass ignores it.
      out_a = ch_misc.x;
    } else {
      // defaultcharacter / CAC pieces: key light + SH irradiance ambient,
      // key gated by the CSM shadow (see the livingworld comment above).
      float csm = SampleCsmShadow(i.rpos + cam_pos.xyz, 0.012);
      if (ch_tintA.w > 0.0) {
        dlin *= ch_tintA.rgb;
      }
      float3 irr = saturate(
          ch_sh[0].rgb + cn.x * ch_sh[1].rgb + cn.y * ch_sh[2].rgb +
          cn.z * ch_sh[3].rgb + (cn.x * cn.z) * ch_sh[4].rgb +
          (cn.z * cn.y) * ch_sh[5].rgb + (cn.y * cn.x) * ch_sh[6].rgb +
          (cn.z * cn.z) * ch_sh[7].rgb +
          (cn.x * cn.x - cn.y * cn.y) * ch_sh[8].rgb);
      lin = dlin * (ch_key.rgb * ndl * csm + irr * ch_amb.w);
      // defaultcharacter/cacstamp PSes end `max oC0.w, c13.x / c22.x`:
      // the entity's spawn fade (see the livingworld comment above).
      out_a = ch_misc.x;
      // character.alpha accessory (sunglass lens): coverage from the mesh's
      // "alpha" channel at the raw second texcoord, like hair (cac_alphaPS:
      // oC0.w = tf5(uv2).r * c22.x), routed to the blended sub-pass.
      if (ch_misc.z > 0.5) {
        out_a *= saturate(decal_art.Sample(smp, i.uv2).r);
      }
    }
    // Exact tone chain: sqrt(0.5 * (max(x*E/4 + 0.75, 1) - sat(1 - x*E)^2)).
    float E = max(ch_key.w, 0.01);
    float3 t1 = saturate(1.0 - lin * E);
    float3 tm = max(lin * 0.25 * E + 0.75, 1.0) - t1 * t1;
    float3 cc = saturate(sqrt(max(tm * 0.5, 0.0)) * 1.41);
    return float4(cc, out_a);
  }
  // Exact world-material families (cam_pos.w = -family): hand-ported from
  // the game's own pixel shaders and verified per-pixel against them with
  // an offline ucode interpreter. All texture
  // colors linearize IN-SHADER as x^2 (the fetch signs are unsigned on every
  // world texture). Families: 1 baseenvironment, 2 defaultenvironment,
  // 3/4 decalenvironment(_tileable), 5/6 reflective(_simple), 7 alphatest,
  // 8 environmentdiffuse, 9/10 tree(animate), 11 proxyworld,
  // 12 incandescent. v1 runs with NEUTRAL normal/detail maps (kd is the
  // exact flat-map constant 0.39 * 2.39562); spec/reflection masks bind at
  // t4 (overlay.w == 3) on families without decal art.
  if (cam_pos.w < -0.5) {
    float fam = -cam_pos.w;
    float3 dlin = albedo.rgb * albedo.rgb;
    // Global distance fog (VS c5/c6, captured per frame): every world PS
    // ends with col * fog.a + fog.rgb before exposure/tonemap.
    float fdist = length(i.rpos);
    float f1 = saturate(fdist * sh_fogp.x + sh_fogp.y);
    if (sh_fogp.z != 1.0) {
      f1 = pow(max(f1, 1e-6), sh_fogp.z);
    }
    float3 fog_rgb = sh_fogc.rgb * f1;
    float fog_a = 1.0 + sh_fogc.a * f1;
    float expo = sh_sun.w;
    float3 lin;
    float out_a = 1.0;
    bool reduced_tone = false;
    if (fam > 8.5 && fam < 12.5) {
      // tree/treeanimate: D^2 * max(lm^2, floor) * scale [* tint mult];
      // proxyworld/incandescent: D^2 * scale. No shadow receive, no kd, no
      // material multiplier on the fog term.
      if (fam < 10.5) {
        // Console lightmap semantics: bilinear, clamped, mip 0 (see the
        // main env fetch below).
        float3 lmg = lightmap.SampleLevel(smp_clamp, i.uv2, 0.0).rgb;
        lin = dlin * max(lmg * lmg, sh_env.z) * sh_env.y;
        if (fam < 9.5) {
          lin *= sh_env.w;
        }
        out_a = albedo.a;
      } else {
        lin = dlin * (fam < 11.5 ? sh_fogp.w : 1.0);
      }
    } else {
      // Environment families: macro overlay (0.5-neutral, fades under decal
      // art), linear decal composite, lightmap squared and min-clamped
      // against (CSM s + shadow color), kd, phong spec vs the shader's
      // fixed literal light, cube reflection on 5/6.
      float3 ov = float3(1.0, 1.0, 1.0);
      // Fam 13 (transparentenvironmentreflective) carries no macro term.
      if (overlay.z > 0.0 && fam < 12.5) {
        float3 mo = macro.Sample(smp, i.uv * overlay.x).rgb;
        ov = saturate((mo - 0.5) * overlay.y + 0.5);
      }
      if (fam > 2.5 && fam < 4.5 && overlay.w > 0.5) {
        // overlay.w == 0 = art unresolved (white fallback alpha 1 would
        // whitewash the whole surface).
        float4 dk = overlay.w > 1.5 ? decal_art.Sample(smp, i.uv3)
                                    : decal_art.Sample(smp_clamp, i.uv3);
        dlin = lerp(dlin, dk.rgb * dk.rgb, dk.a);
        // The macro weathering overlay applies OVER the composited art:
        // ApplyOverlay(cOverlay, ApplyDecal(...)) in the decalenvironment
        // source. The previous `ov = lerp(1, ov, 1-dk.a)` fade rendered the
        // paint unweathered: on the PCU Library ramp stencils the measured
        // native/emulated brightness error was 1.19x at paint alpha~1,
        // 1.05x at ~0.35 and 1.0x off-patch: exactly 1/ov for ov~0.84,
        // the no-fade model (measured from a matched A/B capture pair).
      }
      float3 dcol = dlin * ov;
      // CSM shadow term s = sat(infront + 1 - coverage) from the native
      // atlas (same cascade select as the legacy receive path).
      float s = 1.0;
      if (sh_misc.y > 0.0) {
        float3 wp = i.rpos + cam_pos.xyz;
        float2 lsv =
            float2(dot(sh_x.xyz, wp) + sh_x.w, dot(sh_y.xyz, wp) + sh_y.w);
        float rd = dot(sh_z.xyz, wp) + sh_z.w - sh_misc.x;
        float2 luv = 0.0;
        float casc = 0.0;
        float2 l2 = lsv * sh_c2.xy + sh_c2.zw;
        if (max(abs(l2.x), abs(l2.y)) < 0.99) { luv = l2; casc = 3.0; }
        float2 l1 = lsv * sh_c1.xy + sh_c1.zw;
        if (max(abs(l1.x), abs(l1.y)) < 0.99) { luv = l1; casc = 2.0; }
        if (max(abs(lsv.x), abs(lsv.y)) < 0.99) { luv = lsv; casc = 1.0; }
        if (casc > 0.0) {
          float2 suv = float2(luv.x / 6.0 + (casc * 2.0 - 1.0) / 6.0,
                              luv.y * -0.5 + 0.5);
          float2 sm2 = shadow_atlas.Sample(smp_clamp, suv);
          s = saturate((sm2.x >= rd ? 1.0 : 0.0) + (1.0 - sm2.y));
        }
      }
      // Lightmap fetch = the console's semantics: BILINEAR, CLAMPED, mip 0.
      // The composed atlas pages are single-level on console and the fetch
      // constants carry clamp_x/clamp_y = 2. Sampling them with the aniso-8
      // WRAP sampler over our generated mip chain averaged NEIGHBORING
      // atlas cells together at grazing angles (deep aniso LOD), and at the
      // page border the wrap pulled in the opposite edge of the page: the
      // reflective_trans canopy slope (cells at v ~ 0.996, white-cliff
      // cells wrapping in from v ~ 0) glowed ~2x bright while the right
      // awning went dark, with the decode, UVs and constants all verified
      // exact (diagnosed via the mode-7 raw-lightmap isolation view).
      float3 lmg = lightmap.SampleLevel(smp_clamp, i.uv2, 0.0).rgb;
      // F12 isolation mode 7: visualize the RAW lightmap sample; mode 8:
      // visualize the lightmap unwrap coordinate (frac(uv2*16) in rg).
      // Debug taps live on fams 5/6/13 only; on fams 1-4 misc.z carries
      // the v2 material bind flags instead (family-gated below).
      if (fam > 4.5 && abs(misc.z - 7.0) < 0.5) {
        return float4(lmg, 1.0);
      }
      if (fam > 4.5 && abs(misc.z - 8.0) < 0.5) {
        return float4(frac(i.uv2 * 16.0), 0.0, 1.0);
      }
      // Mode 9: lightmap-resolve status; RED = a real lightmap is bound
      // (tint.r set by the C++ resolve), BLUE = white fallback in t1.
      if (fam > 4.5 && abs(misc.z - 9.0) < 0.5) {
        return float4(tint.r, 0.0, 1.0 - tint.r, 1.0);
      }
      // tint.r == 0 = the real lightmap has not resolved yet (first-sight
      // decode in flight; t1 = the white fallback). Min-clamping the
      // fallback against the CSM term rendered in-shadow surfaces as BLACK
      // patches for the decode window (the ramp-stencil "black square
      // flash"); serve unshadowed brightness until the real page lands.
      float3 lml = tint.r > 0.0 ? min(lmg * lmg, s + sh_color.rgb) : lmg * lmg;
      // GetTangentLight (world-shading v2). vnd is the tangent-space mapped
      // normal from the material's base normal map (t5) + detail map (t8 at
      // uv * misc.w): xy = 2*base + 2*detail - 2, z = 2*base.z - 1.
      // baseenvironment/decal normalize vnd for the kd dot; default-
      // environment (plain normal map, no detail pair) uses it raw. Fams
      // 1-4 carry bind flags in misc.z (1 = base normal at t5, 2 = detail
      // at t8, 4 = spec2ch at t9); fams 5/6 derive vnd from the reflective
      // path's nt below. Without a normal map the exact flat-map fold
      // 0.39 * 2.39562 = 0.93429 applies (the v1 constant).
      uint v2f = fam < 4.5 ? (uint)(misc.z + 0.5) : 0u;
      float kd = 0.93429;
      float3 wn = dot(i.nrm, i.nrm) > 0.01
                      ? normalize(i.nrm)
                      : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
      float3 vnd_raw = float3(0.0, 0.0, 1.0);  // wn mapping (max-z clamped)
      float3 vnd = float3(0.0, 0.0, 1.0);      // kd dot (family-normalized)
      bool have_vnd = false;
      if ((v2f & 1u) != 0u) {
        float3 nmv = normal_map.Sample(smp, i.uv).rgb;
        float2 dxy = (v2f & 2u) != 0u
                         ? detail_map.Sample(smp, i.uv * misc.w).rg
                         : float2(0.5, 0.5);
        vnd_raw = float3(nmv.xy * 2.0 + dxy * 2.0 - 2.0, nmv.z * 2.0 - 1.0);
        vnd = (fam < 1.5 || fam > 2.5) ? normalize(vnd_raw) : vnd_raw;
        have_vnd = true;
      }
      // Tangent frames shared by kd and the reflective normal mapping.
      // Two variants of the same construction:
      //  - tt_s/bb_s = the RAW screen-space UV-gradient frame. Exact for
      //    the kd SIGN terms under ANY UV orientation; bowl/ramp walls
      //    map their texture rotated relative to world-up, and mirrored
      //    islands flip dP/dU exactly like the stored tangent does.
      //  - tt/bb = analytic world-up axes carrying the screen frame's
      //    signs; degree-accurate where the mapping IS world-up aligned
      //    (building facades, calibrated for the glass reflections).
      //    Transferring signs onto misaligned axes is METASTABLE when the
      //    UV grid sits ~90 deg off world-up: the transfer dots cross zero
      //    along a curved wall and kd banded light/dark around the bowl
      //    transition (seen in a matched F11 capture pair), which is why
      //    kd and the fam 1-4 normal mapping use the screen frame instead.
      float3 tt = float3(1.0, 0.0, 0.0), bb = float3(0.0, 1.0, 0.0);
      float3 tt_s = tt, bb_s = bb;
      if (have_vnd || (overlay.w > 3.5 && abs(misc.z - 3.0) > 0.5)) {
        float3 dp1 = ddx(i.rpos), dp2 = ddy(i.rpos);
        float2 du1 = ddx(i.uv), du2 = ddy(i.uv);
        float3 dp2p = cross(dp2, wn), dp1p = cross(wn, dp1);
        tt = dp2p * du1.x + dp1p * du2.x;
        bb = dp2p * du1.y + dp1p * du2.y;
        tt *= -rsqrt(max(dot(tt, tt), 1e-12));
        bb *= rsqrt(max(dot(bb, bb), 1e-12));
        tt_s = tt;
        bb_s = bb;
        float3 bb2 = float3(0.0, 1.0, 0.0) - wn * wn.y;
        float lb2 = length(bb2);
        if (lb2 > 0.05) {
          bb2 /= lb2;
          float3 tt2 = cross(bb2, wn);
          tt = tt2 * (dot(tt2, tt) >= 0.0 ? 1.0 : -1.0);
          bb = bb2 * (dot(bb2, bb) >= 0.0 ? 1.0 : -1.0);
        }
      }
      // kd axes: prefer the mesh's STORED frame (binormal + handedness in
      // i.tanb, T = cross(B, N) x handedness x calibration polarity);
      // authoring decides per UV island whether the frame follows a
      // mirror, which no derivative frame can know (per-island brightness
      // steps on the wooden ramp panels / faint bowl striping). Screen
      // frame is the fallback for meshes without one.
      float3 kt = tt_s, kb = bb_s;
      if (i.tanb.w > 0.1) {
        kb = normalize(i.tanb.xyz);
        kt = cross(kb, wn) *
             ((i.tanb.w > 0.6 ? 1.0 : -1.0) * sh_v2.x);
      }
      if (have_vnd) {
        // The mapped world normal feeds the spec mirror below; matte spec
        // is broad-lobed, so exact axis precision matters less than the
        // per-island signs (the analytic substitution exists for
        // mirror-sharp glass).
        wn = normalize(vnd_raw.x * kt + vnd_raw.y * kb +
                       wn * max(vnd_raw.z, 0.05));
        kd = (vnd.x * 0.58 * sign(dot(kt, sh_sun.xyz)) +
              vnd.y * 0.62 * sign(dot(kb, sh_sun.xyz)) +
              vnd.z * 0.39) * 2.39562;
      }
      // misc.z = 3 (F12 reflection isolation): force the flat normal.
      // Only the reflective families (5/6/13) ever carry overlay.w == 4;
      // fams 1-4 signal their normal map through the misc.z flags above.
      if (overlay.w > 3.5 && abs(misc.z - 3.0) > 0.5) {
          // Per-pixel normal map (t5, paired descriptor): the real PS
          // (baseenvironmentreflective_defaultPS) reflects off the
          // normal-mapped normal: tangent normal = 2*(n + detail - 1) on
          // xy, 2*n.z - 1 on z (the material's detail map is a constant
          // neutral 16x16, folded here as 0.5). With the FLAT vertex normal
          // every panel of a glass facade reflects the same tiny cube
          // region: one giant magnified smear of the plaza cube's
          // lamp-heads/trees (the "streetlight head" artifact, ucode-traced
          // on a captured pixel; flat N lands on the
          // face-0 tree/lamp texels, the mapped N tilts onto sky). The
          // per-panel tilts break the reflection up exactly like the
          // emulated frame. Screen-space cotangent frame (same shape as
          // the vehicle DXN branch; the world VS carries no tangent
          // attributes we decode).
          // F12 mode 6: the slider drives the normal-map LOD bias live;
          // the console fetches the nm at its 640p gradient LOD (blurrier,
          // weaker bump tilts), so the matching bias is the remaining
          // reflection-rotation candidate. Default stays SHARP (bias 0):
          // an unconditional misc.y bias visibly degraded the glass;
          // tune with the
          // slider first, promote the found value to a default after.
          float nm_bias = misc.z > 5.5 ? misc.w : 0.0;
          float3 nmv = normal_map.SampleBias(smp, i.uv, nm_bias).rgb;
          // Exact composition: xy = 2*normal + 2*detail - 2, z = 2*n.z - 1.
          // The detail map is a CONSTANT 16x16 (0.514, 0.506), NOT the
          // formula's 0.5 neutral, so its fold is a constant tangent tilt.
          // The tilt rides the two F12 trim sliders (packed in misc.x;
          // defaults = the exact fold) so the residual reflection rotation
          // can be dialed live against the emulated frame.
          float trim_yi = floor(misc.x / 1000.0);
          float2 trim = float2(misc.x - trim_yi * 1000.0 - 500.0, trim_yi - 500.0) *
                        0.001;
          float3 nt = float3(nmv.xy * 2.0 - 1.0 + trim, nmv.z * 2.0 - 1.0);
          // (Frame construction hoisted above; the calibration notes ride
          // with it; tt/bb here are the shared axes.)
          wn = normalize(nt.x * tt + nt.y * bb + wn * max(nt.z, 0.05));
          // v2: the reflective families share the kd term; fam 5
          // (reflective) is a base-family material and normalizes vnd for
          // the dot like baseenvironment; fam 6 (reflective_simple) uses
          // it raw like the other _simple family. kt/kb = the shared kd
          // axes computed above (stored frame when the mesh carries one),
          // built from the pre-mapping geometric normal.
          float3 v56 = fam < 5.5 ? normalize(nt) : nt;
          kd = (v56.x * 0.58 * sign(dot(kt, sh_sun.xyz)) +
                v56.y * 0.62 * sign(dot(kb, sh_sun.xyz)) +
                v56.z * 0.39) * 2.39562;
      }
      // Fam 13 has no kd term at all; its body is D^2 * lml * ALPHA,
      // premultiplied once in the shader on top of the a^2 blend factor
      // (verified 0.0-error vs the ucode).
      lin = fam > 12.5 ? lml * dcol * albedo.a : lml * kd * dcol;
      if (overlay.w > 2.5) {
        // spec/ecc/refmask at t4: phong vs the FIXED literal light
        // (-0.14, 0.5, 0.9), power 10 + 290*ecc, tint (2.1, 1.8, 1.5),
        // scaled by the clamped lightmap green and the spec mask.
        float4 masks = decal_art.Sample(smp, i.uv);
        float3 vd = -normalize(i.rpos);
        float3 Ls = float3(-0.14, 0.5, 0.9);
        float3 refl = Ls - 2.0 * wn * dot(wn, Ls);
        float bp = saturate(dot(vd, -refl));
        float ks = pow(max(bp, 1e-6), 10.0 + 290.0 * masks.y);
        // misc.z = 5 (F12 reflection isolation): body only, no spec/cube.
        if (abs(misc.z - 5.0) > 0.5) {
          lin += ks * float3(2.1, 1.8, 1.5) * lml.g * masks.x;
        }
        if (((fam > 4.5 && fam < 6.5) || fam > 12.5) &&
            abs(misc.z - 5.0) > 0.5 && abs(misc.z - 1.0) > 0.5) {
          // Cube reflection: reflect(E, wN) with xy negated (the source's
          // ref_vec.xy *= -1), luminosity lerped toward 1 by
          // 0.3 * sat(4*refmask - 2.6), x refmask x reflection_scale 1.5.
          float3 rv = vd - 2.0 * wn * dot(vd, wn);
          // misc.y = cube LOD bias log2(render_height / 640): the guest
          // computes the cube fetch's gradient LOD at its own 1152x640
          // render; at 4K our per-pixel gradients are ~3.4x smaller, so
          // without the bias baked cube detail (the plaza streetlight
          // heads) survives through mips the console's fetch blurs away.
          // F12 isolation (misc.z): mode 2 samples the ABSOLUTE level in
          // misc.w; other modes add misc.w as extra bias; mode 4 shows the
          // raw cube sample in place of the shaded result.
          float3 dir = float3(-rv.x, -rv.y, rv.z);
          // Mode 6 gives the slider to the NM fetch; the cube keeps just
          // the automatic bias there.
          float cube_extra = misc.z > 5.5 ? 0.0 : misc.w;
          float3 cube = abs(misc.z - 2.0) < 0.5
                            ? env_cube.SampleLevel(smp, dir, misc.w).rgb
                            : env_cube.SampleBias(smp, dir, misc.y + cube_extra).rgb;
          float rl = 0.3 * saturate(4.0 * masks.z - 2.6);
          float lum = lml.g + rl * (1.0 - lml.g);
          lin += cube * lum * masks.z * 1.5;
          if (abs(misc.z - 4.0) < 0.5) {
            lin = cube * cube;
          }
        }
      }
      // v2 decal-family spec (spec/ecc at t9): the same phong-vs-fixed-
      // light term as the base families; the decal art occupies t4 on
      // fams 3/4, so their masks ride the second pair table.
      if ((v2f & 4u) != 0u) {
        float2 m2 = spec2_map.Sample(smp, i.uv).rg;
        float3 vd2 = -normalize(i.rpos);
        float3 Ls2 = float3(-0.14, 0.5, 0.9);
        float3 refl2 = Ls2 - 2.0 * wn * dot(wn, Ls2);
        float ks2 =
            pow(max(saturate(dot(vd2, -refl2)), 1e-6), 10.0 + 290.0 * m2.y);
        lin += ks2 * float3(2.1, 1.8, 1.5) * lml.g * m2.x;
      }
      if (fam > 12.5) {
        // Fam 13 blend factor: the PS outputs a^2 (straight-alpha blend on
        // top of the in-shader premultiplied body; wisps thin as ~a^3,
        // same convention as transparentenvironment).
        out_a = albedo.a * albedo.a;
      } else if (fam > 6.5) {
        out_a = albedo.a;
        reduced_tone = fam > 7.5;  // environmentdiffuse's cheap tonemap
      }
      fog_a *= sh_env.x;  // material multiplier (PS c11.y)
    }
    // Fog -> exposure -> tonemap -> sqrt, then the postfx uber's measured
    // 1.41 scene multiplier (same as the character branch).
    float3 xe = (lin * fog_a + fog_rgb) * expo;
    float3 t1e = saturate(1.0 - xe);
    float3 tme = reduced_tone ? 1.0 - t1e * t1e
                              : max(xe * 0.25 + 0.75, 1.0) - t1e * t1e;
    float3 cce = saturate(sqrt(max(tme * 0.5, 0.0)) * 1.41);
    return float4(cce, out_a);
  }
  // environment.decal surfaces: the paint/graffiti art (t4) is composited
  // over the base diffuse by ITS alpha, opaque output; these meshes ARE
  // the wall/ground there. The art maps with uv3, the second half-pair of
  // the packed half4 first texcoord (validated offline: sampling with the
  // tiling uv0 repeats it: "Stereo Stereo Stereo"; the fmt-26 second
  // element is the lightmap unwrap, not the decal's). Composited BEFORE the
  // macro overlay: the weathering applies over the paint too
  // (ApplyOverlay(cOverlay, ApplyDecal(...)); the old order left the paint
  // unweathered, the too-white ramp stencils).
  if (overlay.w > 0.0) {
    // overlay.w == 2 marks environment.decal_tileable: the art tiles across
    // the surface (rock/cliff faces) and must WRAP; clamp stretched the
    // border texels into giant streaks. Single placements clamp (their
    // transparent border keeps everything outside the placement clear).
    float4 dk = overlay.w > 1.5 ? decal_art.Sample(smp, i.uv3)
                                : decal_art.Sample(smp_clamp, i.uv3);
    albedo.rgb = lerp(albedo.rgb, dk.rgb, dk.a);
  }
  // Macro overlay: large-scale grime/cracks multiplied over the diffuse at
  // uv * macroOverlayUVScale, faded by macroOverlayOpacity: the ground and
  // wall weathering. WHITE is the neutral (materials without weathering
  // bind a 16x16 "default_white"). The game multiplies it ONCE in its
  // linear (squared) color space, so the gamma-space equivalent is
  // sqrt(m); a direct multiply doubles the darkening (harsh black
  // patchwork vs the emulated subtle weathering).
  if (overlay.z > 0.0 && misc.x < 1.5) {  // water reuses overlay.z (ripple map flag)
    float4 m = macro.Sample(smp, i.uv * overlay.x);
    albedo.rgb *= lerp(float3(1.0, 1.0, 1.0), sqrt(m.rgb), overlay.y * m.a);
  }
  // tint.r > 0 marks items with a lightmap bound (2x baked lighting);
  // otherwise fall back to derivative face shading. The lighting term stays
  // separate from the albedo so the CSM receive below can min-clamp IT, the
  // way the game's GetShadowedLightMap clamps the lightmap lighting.
  float3 light;
  if (tint.b > 0.0) {
    light = float3(1.0, 1.0, 1.0);  // unlit (sky dome)
  } else if (tint.r > 0.0) {
    // Console lightmap semantics: bilinear, clamped, mip 0; the composed
    // atlas pages are single-level; mip/wrap sampling bled neighbor cells
    // and page edges (see the exact env fetch).
    light = lightmap.SampleLevel(smp_clamp, i.uv2, 0.0).rgb * 2.0;
  } else {
    // Smooth per-vertex normal when the mesh has one; face normal from
    // position derivatives otherwise.
    float3 n = dot(i.nrm, i.nrm) > 0.01 ? normalize(i.nrm)
                                        : normalize(cross(ddx(i.rpos), ddy(i.rpos)));
    light = abs(dot(n, normalize(float3(0.4, 0.8, 0.3)))) * 0.35 + 0.75;
  }
  // Dynamic CSM shadow receive (world geometry + rigid props; characters
  // need the game's separate PCF/bias variant; skipping them avoids
  // self-shadow acne, and the ground shadow is 95% of the visible effect).
  // Exact receiver math from the baseenvironment PS disassembly: finest
  // cascade whose |ls| < 0.99 wins; shadow = saturate(infront + 1 -
  // coverage), then the game min-clamps the LINEAR lighting term:
  //   light_linear = min(light_linear, s + c8.rgb)
  // Full shadow clamps to the dim bluish c8 ambient, the penumbra saturates
  // wherever the clamp exceeds the lit level (which is what keeps the edge
  // crisp), and surfaces already darker than the clamp, baked shade under
  // bridges/trees, show NO dynamic shadow at all. Our light term is
  // gamma-space (light^2 ~ the game's linear term: the lightmap x2 folds
  // its x4 linear multiplier), so the clamp maps to min(light, sqrt(s+c8))
  // per channel. A fixed-denominator curve here read pitch-black and
  // double-darkened baked shade.
  if (sh_misc.y > 0.0 && tint.g == 0.0 && tint.b == 0.0 && misc.x == 0.0) {
    float3 wp = i.rpos + cam_pos.xyz;
    float2 lsv = float2(dot(sh_x.xyz, wp) + sh_x.w, dot(sh_y.xyz, wp) + sh_y.w);
    float rd = dot(sh_z.xyz, wp) + sh_z.w - sh_misc.x;
    float2 luv = 0.0;
    float casc = 0.0;
    float2 l2 = lsv * sh_c2.xy + sh_c2.zw;
    if (max(abs(l2.x), abs(l2.y)) < 0.99) { luv = l2; casc = 3.0; }
    float2 l1 = lsv * sh_c1.xy + sh_c1.zw;
    if (max(abs(l1.x), abs(l1.y)) < 0.99) { luv = l1; casc = 2.0; }
    if (max(abs(lsv.x), abs(lsv.y)) < 0.99) { luv = lsv; casc = 1.0; }
    if (casc > 0.0) {
      float2 uv = float2(luv.x / 6.0 + (casc * 2.0 - 1.0) / 6.0, luv.y * -0.5 + 0.5);
      float2 m = shadow_atlas.Sample(smp_clamp, uv);
      float s = saturate((m.x >= rd ? 1.0 : 0.0) + (1.0 - m.y));
      light = min(light, sqrt(s + sh_color.rgb));
    }
  }
  float3 lit = albedo.rgb * light;
  if (mat_tint.w > 0.0 && misc.x == 0.0) {
    lit *= mat_tint.rgb;
  }
  if (misc.x > 1.5) {
    // water.* (flowingwater.fx approximation): the real shader is
    // near-black diffuse + dual time-scrolled ripple-normal taps + an
    // environment-cube reflection + sun specular. We have no cube map
    // bound, so the reflection term is the frame fog color (the best
    // single approximation of the surroundings' haze tone) scaled by a
    // fresnel curve; the ripple normal perturbs both the fresnel and a sun
    // sparkle along the captured CSM light axis. The lightmap (x2) keeps
    // the baked bridge/wall shading on the surface. Calibrated against the
    // canal capture (emulated mid-canal mean ~(24,28,32)/255).
    float t = overlay.x;
    float2 rip;
    if (overlay.z > 0.0) {
      // Dual scrolled taps of the material's ripple normal map (macro slot).
      float2 wuv = i.uv * 6.0;
      float3 n1 = macro.Sample(smp, wuv + t * float2(0.11, 0.06)).rgb;
      float3 n2 = macro.Sample(smp, wuv * 1.71 - t * float2(0.07, 0.13)).rgb;
      rip = (n1.xy + n2.xy) - 1.0;
    } else {
      // Normal map unresolved: procedural ripples from world position.
      // Wavelengths ~0.4-1m (emulated ripples are decimeter-scale); low
      // frequencies formed meter-wide chevron interference bands that read
      // as giant arrows on the surface.
      float3 wp = i.rpos + cam_pos.xyz;
      rip = 0.35 * float2(sin(wp.x * 9.7 + wp.z * 6.1 + t * 2.3) +
                              0.6 * sin(wp.x * 17.3 - wp.z * 11.9 + t * 3.4),
                          cos(wp.x * 7.1 - wp.z * 13.7 + t * 2.7) +
                              0.6 * cos(wp.x * 12.9 + wp.z * 18.3 + t * 3.1));
    }
    float3 wn = normalize(float3(rip.x * 0.4, 2.0, rip.y * 0.4));
    float3 vd = -normalize(i.rpos);
    float fres = pow(1.0 - saturate(dot(vd, wn)), 3.0);
    // The flowing-water lightmap unwrap decodes unreliably (bands across
    // atlas gutters), so the water term deliberately ignores it: near-black
    // body + ripple-perturbed cube reflection + sun sparkle.
    // Deep body: the water "diffuse" is a faint STRIPE MASK (max 24/255,
    // WaterFallFoamAlpha, a lookup for the real shader, not a color). The
    // game consumes it in linear space where 0.09^2 vanishes; squaring here
    // likewise kills the visible blue/black banding. overlay.w > 0 = no
    // diffuse channel at all (ocean.default); body is zero there, NOT the
    // white fallback (ocean.fx: diffTerm = (0,0,0), color is all reflection).
    float3 col = overlay.w > 0.5 ? 0.0 : albedo.rgb * albedo.rgb * 0.6;
    // Reflection tint: the environment cube when resolved (t6); otherwise a
    // haze derived from the frame fog color, lifted toward neutral so dark
    // dusk fog doesn't collapse the water to black (fit: emulated canal
    // mean ~(24,28,32)/255 with fog color (0.02,0.07,0.13)).
    float3 renv = overlay.y > 0.0
                      ? env_cube.Sample(smp, reflect(-vd, wn)).rgb
                      : mat_tint.rgb * 0.5 + 0.06;
    col += renv * (0.55 + 0.45 * fres);
    if (sh_misc.y > 0.0) {
      float3 h = normalize(vd + normalize(-sh_z.xyz));
      col += pow(saturate(dot(wn, h)), 90.0) * 0.35;            // sun sparkle
    }
    float fade = saturate(length(i.rpos) * misc.y + misc.z);
    if (misc.w != 1.0) {
      fade = pow(max(fade, 1e-4), misc.w);
    }
    col = sqrt(max(col * col * saturate(1.0 + fade * mat_tint.w) +
                   fade * mat_tint.rgb, 0.0));
    // Opaque: the game's murk hides the canal bed entirely (and our bed
    // shading is untrustworthy under water: striped lightmap unwraps).
    return float4(col, 1.0);
  }
  if (misc.x > 0.0) {
    // transparentenvironment.fx (Skate 2 source; disassembled Skate 3 PS
    // matches): outcolor.rgb = diffTerm * diffuse.rgb * diffuse.a; the rgb
    // is premultiplied by alpha ONCE IN THE SHADER on top of the a^2 blend
    // factor, so wisps thin out as ~a^3. That cubic falloff is most of the
    // emulated "sparse clouds" look. Fog is applied in the game's linear
    // (squared) color space: fade = saturate(dist * ramp.x + ramp.y)^ramp.z
    // toward the fog color, transmittance = 1 + fade * fogcolor.w.
    lit *= albedo.a;
    float fade = saturate(length(i.rpos) * misc.y + misc.z);
    if (misc.w != 1.0) {
      fade = pow(max(fade, 1e-4), misc.w);
    }
    lit = sqrt(max(lit * lit * saturate(1.0 + fade * mat_tint.w) +
                   fade * mat_tint.rgb, 0.0));
    return float4(lit, albedo.a * albedo.a);
  }
  return float4(lit, 1.0);
}
// Shadow caster pass: vs_main runs with mvp = (world *) lightVP built from
// the captured receiver rows, so SV_Position.z IS the light-space depth
// (the height-ramp row; viewport z range 0..1, DepthClip off so casters
// outside the 12 m depth window clamp like the game accepts). MIN blend on
// both channels against a (1, 1) clear: R accumulates the min depth, G
// drops to 0 where any caster drew ("uncoverage"; the blur pass converts
// to the game's coverage convention).
float2 ps_shadow_caster(VSOut i) : SV_Target {
  return float2(i.pos.z, 0.0);
}
