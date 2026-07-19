// Character families: the game's own lighting in LINEAR space (diffuse is
// gamma -> square it), then the exact tone chain from the disassembly and
// the postfx uber's 1.41 scene multiplier (which the empirical world
// shading already folds into its constants; without it characters sit
// ~30% darker than their surroundings, measured on an F11 A/B pair).
float4 ShadeCharacter(VSOut i, float4 albedo) {
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
    // livingworld pedestrians: the diffuse is a stamp-mask atlas; red
    // regions recolor with tintA, blue with tintB (judged in linear
    // space; real-color regions have green above the threshold).
    // The game's character PSes multiply the key light by the CSM shadow
    // (tap >= ray = lit); characters are casters themselves, so an extra
    // receiver bias suppresses self-shadow acne while the body-onto-board
    // / body-onto-NPC shading survives (the sun-axis depth gap there is
    // tens of cm). Without this the held skateboard, a big flat surface
    // that is almost always inside the skater's own shadow, renders
    // fully sunlit (near-white) against the emulated dark deck.
    float csm = min(SampleCsmShadowSoft(i.rpos + cam_pos.xyz, 0.012, cn, i.pos.xy),
                    SampleStaticSun(i.rpos + cam_pos.xyz, cn, i.pos.xy));
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
    float csm = min(SampleCsmShadowSoft(i.rpos + cam_pos.xyz, 0.012, cn, i.pos.xy),
                    SampleStaticSun(i.rpos + cam_pos.xyz, cn, i.pos.xy));
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
  return ToneOut(lin * E, out_a, false);

}
