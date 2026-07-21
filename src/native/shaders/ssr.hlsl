// Screen-space reflections over the pre-tonemap HDR scene plane.
//
// Inputs come from two producers:
//  - scene.hlsl ps_refl_gbuf: a half-res reflection G-buffer re-rendered
//    from the frame's reflective draw items only (env families 5/6/13 +
//    water). RG = octahedral-encoded WORLD-space normal, B = linear view
//    depth, A = reflectivity weight, NEGATED for surfaces that never write
//    scene depth (water / blended fam-13 glass); those use an in-front
//    visibility rule instead of the exact depth match.
//  - a full-res linear view-Z plane (the SSAO linearize output when AO ran
//    this frame, else ps_linearize below).
//
// Pass chain (own binding layout: b0 root constants + t0/t1/t2 single-
// texture tables + point/linear clamp samplers):
//   ps_linearize  scene depth -> view Z, R32_FLOAT full res (only when the
//                 SSAO pass did not already produce the plane this frame)
//   ps_march      half res: reconstruct the view-space ray, reflect about
//                 the G-buffer normal, march the full-res depth plane and
//                 sample the HDR scene at the hit. rgb = reflected color,
//                 a = ray confidence (facing / edge / precision fades).
//   ps_composite  full res onto the HDR scene plane (src-alpha blend):
//                 a = confidence x reflectivity x intensity. The material's
//                 static cube-reflection term stays in the scene as the
//                 fallback; SSR blends over it where a ray hit.
//
// Spaces: the march runs in the SSAO passes' "AO view space", positions
// from ViewPos(uv, z) built with |m00|/|m11|, i.e. the real view space with
// each axis mirrored by its projection sign (the guest projection has
// NEGATIVE m00). Reflection directions are mirror-SENSITIVE, so vrow0..2
// fold the same axis signs into the world->view normal rotation; ray
// geometry and screen projection then stay consistent by construction.
cbuffer C : register(b0) {
  float4 size;   // xy = dest size in px, zw = 1 / size
  float4 proj;   // x = |m00|, y = |m11|, z = m22, w = m32 (row-vector proj)
  float4 vrow0;  // world->AO-view rotation rows: n_view = n.x * vrow0.xyz
  float4 vrow1;  //   + n.y * vrow1.xyz + n.z * vrow2.xyz
  float4 vrow2;
  float4 p0;     // x = march steps, y = thickness (view units),
                 // z = composite intensity, w = debug mode
  float4 p1;     // x = max ray distance (view units)
  float4 p2;     // graphics build-up showcase: x = split position in output
                 // pixels, y/z = SSR visibility (0/1) left/right of the
                 // split (both 1 when the showcase is off)
};
// Per-pass t0/t1/t2 roles (each entry uses one of the overlapping
// declarations; the rest drop out at compile):
//   ps_linearize  t0 = scene depth (2D or 2DMS under SSR_MSAA)
//   ps_march      t0 = G-buffer, t1 = linear depth, t2 = HDR scene
//   ps_composite  t0 = SSR result, t1 = G-buffer, t2 = linear depth
#ifdef SSR_MSAA
Texture2DMS<float> depth_ms : register(t0);
#endif
Texture2D<float> tex0d : register(t0);
Texture2D<float4> tex0 : register(t0);
Texture2D<float> tex1 : register(t1);
Texture2D<float4> tex1c : register(t1);
Texture2D<float4> tex2 : register(t2);
Texture2D<float> tex2d : register(t2);
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

// Device depth -> view-space Z. Row-vector projection with m23 = 1:
// ndc_z = m22 + m32 / z_view.
float LinearZ(float d) {
  return clamp(proj.w / min(d - proj.z, -1e-6), 0.05f, 4e5f);
}

float ps_linearize(VSOut i) : SV_Target {
#ifdef SSR_MSAA
  float d = depth_ms.Load(int2(i.pos.xy), 0);
#else
  float d = tex0d.Load(int3(int2(i.pos.xy), 0));
#endif
  return LinearZ(d);
}

// AO-view-space position from UV + linear Z (the |m00|/|m11| mirrored view
// space every SSR quantity lives in; see the header note).
float3 ViewPos(float2 uv, float z) {
  float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
  return float3(ndc.x * z / proj.x, ndc.y * z / proj.y, z);
}

// AO-view-space position -> screen UV (the exact ViewPos inverse).
float2 ViewUv(float3 q) {
  float2 ndc = float2(q.x * proj.x / q.z, q.y * proj.y / q.z);
  return float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
}

// Octahedral normal decode (ps_refl_gbuf encodes; unit vectors only).
float3 OctDecode(float2 e) {
  float3 n = float3(e.x, e.y, 1.0f - abs(e.x) - abs(e.y));
  if (n.z < 0.0f) {
    n.xy = float2((1.0f - abs(e.y)) * (e.x >= 0.0f ? 1.0f : -1.0f),
                  (1.0f - abs(e.x)) * (e.y >= 0.0f ? 1.0f : -1.0f));
  }
  return normalize(n);
}

// G-buffer visibility vs the scene depth: opaque reflective surfaces
// (signed_w > 0) wrote the depth buffer, so their stored depth must MATCH
// it; a mismatch means another surface occludes this G-buffer texel.
// Water / blended glass (signed_w < 0) never write depth; they are visible
// exactly when they sit IN FRONT of the opaque depth.
// grazing = 1 - |N.V|: on surfaces seen edge-on the view depth changes by
// meters across one half-res G-buffer texel, so the match window must widen
// with slope; a fixed window rejects valid texels in a dither pattern
// (stippled reflection dropout concentrated on grazing panes).
float GbufVisible(float gz, float sz, float signed_w, float grazing) {
  float eps = max(gz * 0.04f, 0.3f) * (1.0f + 8.0f * grazing * grazing);
  if (signed_w < 0.0f) {
    return gz < sz + eps ? 1.0f : 0.0f;
  }
  return abs(gz - sz) < eps ? 1.0f : 0.0f;
}

float4 ps_march(VSOut i) : SV_Target {
  // Debug 4 (ray autopsy): color-code where each ray terminates;
  // green = G-buffer visibility rejected, blue = degenerate direction,
  // magenta = crossed the near plane, cyan = left the screen,
  // yellow = ran out of steps with no crossing, red = crossing found but
  // thicker than the window every time, white x conf = accepted hit.
  const bool autopsy = p0.w > 3.5f;
  float4 g = tex0.SampleLevel(smp_point, i.uv, 0);
  float wrefl = abs(g.a);
  float gz = g.b;
  if (wrefl < 0.004f) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  float3 P = ViewPos(i.uv, gz);
  float3 nw = OctDecode(g.rg);
  float3 N = normalize(nw.x * vrow0.xyz + nw.y * vrow1.xyz +
                       nw.z * vrow2.xyz);
  float3 V = normalize(P);
  float grazing = 1.0f - saturate(abs(dot(N, V)));
  float sz = tex1.SampleLevel(smp_point, i.uv, 0);
  if (GbufVisible(gz, sz, g.a, grazing) < 0.5f) {
    return autopsy ? float4(0.0f, 1.0f, 0.0f, 1.0f)
                   : float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  float3 R = reflect(V, N);
  // Toward-camera rays are allowed: a mirror facing the viewer bounces
  // exactly that way, and the march still resolves hits against the depth
  // buffer; the reflected subject is painted from its camera-facing
  // texels (the standard screen-space approximation; this is what puts the
  // skater in the glass). Only rays aimed at the camera POSITION itself
  // are degenerate (they reflect the viewpoint, which has no on-screen
  // content); fade those out instead of popping.
  // Keep this gate TIGHT: the self-reflection zone (skater in the glass)
  // has ray directions within ~25 degrees of the to-camera axis, and a wide
  // fade here erases it wholesale (the autopsy's blue disc). Only rays
  // within a few degrees of the camera point itself are unresolvable.
  float degen = 1.0f - smoothstep(0.95f, 0.995f, dot(R, -V));
  if (degen <= 0.0f) {
    return autopsy ? float4(0.0f, 0.0f, 1.0f, 1.0f)
                   : float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  // Lift the origin off the surface (depth-proportional) so grazing rays
  // don't re-hit their own G-buffer texel as instant self-intersections.
  P += N * (0.02f + gz * 0.004f);

  // ---- Screen-space DDA march ----
  // The ray is clipped in VIEW space so its far endpoint stays in front of
  // the near plane, then its SCREEN projection is walked with UNIFORM
  // screen-space steps. 1/z is exactly linear along the projection of a 3D
  // line, so the ray depth at every tap interpolates perspective-correctly
  // - unlike view-space stepping, no stride can overshoot a depth feature
  // wider than one screen step (the failure mode that made hits sparse and
  // demanded thickness/jitter crutches).
  float t_far = p1.x;
  if (R.z < 0.0f) {
    t_far = min(t_far, (P.z - 0.2f) / -R.z);  // stop before the near plane
  }
  t_far = max(t_far, 0.05f);
  float3 Q1 = P + R * t_far;
  float2 uv0 = ViewUv(P);
  float2 uv1 = ViewUv(Q1);
  float2 dseg = uv1 - uv0;
  // Clip the screen segment to the viewport rectangle.
  float s_max = 1.0f;
  if (dseg.x > 1e-6f) {
    s_max = min(s_max, (1.0f - uv0.x) / dseg.x);
  } else if (dseg.x < -1e-6f) {
    s_max = min(s_max, -uv0.x / dseg.x);
  }
  if (dseg.y > 1e-6f) {
    s_max = min(s_max, (1.0f - uv0.y) / dseg.y);
  } else if (dseg.y < -1e-6f) {
    s_max = min(s_max, -uv0.y / dseg.y);
  }
  if (s_max <= 0.0f || dot(dseg, dseg) < 1e-10f) {
    return autopsy ? float4(0.0f, 1.0f, 1.0f, 1.0f)
                   : float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  float w0 = 1.0f / P.z;
  float w1 = 1.0f / Q1.z;
  float steps = max(p0.x, 8.0f);
  // Small phase jitter; the composite's confidence-weighted blur and the
  // bilinear upsample integrate it. Kept small: per-pixel phase error in
  // the HIT POSITION runs along the ray's screen direction and reads as
  // diagonal smearing of the reflected image.
  float jitter = 0.5f * frac(52.9829189f *
                             frac(dot(i.pos.xy,
                                      float2(0.06711056f, 0.00583715f))));
  float s_prev = 0.0f;
  float hit_s = -1.0f;
  float hit_z = 0.0f;
  float hit_dz = 0.0f;
  float2 hit_uv = float2(0.0f, 0.0f);
  float3 death = float3(1.0f, 1.0f, 0.0f);  // autopsy: default = out of steps
  [loop] for (int k = 0; k < 64; ++k) {
    if (k >= (int)steps) {
      break;
    }
    float s = (float(k) + 0.5f + jitter) / steps * s_max;
    float2 uv = uv0 + dseg * s;
    float z_ray = 1.0f / lerp(w0, w1, s);
    float zs = tex1.SampleLevel(smp_point, uv, 0);
    float dz = z_ray - zs;
    if (dz > 0.0f) {
      // Depth crossing: refine in s (same perspective-correct interp),
      // then judge the refined gap against the thickness window.
      hit_s = s;
      hit_uv = uv;
      hit_z = z_ray;
      hit_dz = dz;
      float lo = s_prev;
      float hi = s;
      // 6 bisections: a full-screen segment at 48 taps has ~40 half-res px
      // strides; /64 refinement pins the hit to sub-pixel so the sampled
      // reflection doesn't wander along the ray (diagonal smear).
      [unroll] for (int r = 0; r < 6; ++r) {
        float sm = 0.5f * (lo + hi);
        float2 um = uv0 + dseg * sm;
        float zr = 1.0f / lerp(w0, w1, sm);
        float zm = tex1.SampleLevel(smp_point, um, 0);
        float dm = zr - zm;
        if (dm > 0.0f) {
          hi = sm;
          hit_s = sm;
          hit_uv = um;
          hit_z = zr;
          hit_dz = dm;
        } else {
          lo = sm;
        }
      }
      float thick = p0.y * (1.0f + hit_z * 0.03f);
      if (hit_dz < thick) {
        break;  // real surface hit
      }
      hit_s = -1.0f;  // passed behind a silhouette: keep marching
      death = float3(1.0f, 0.0f, 0.0f);
    }
    s_prev = s;
  }
  if (hit_s < 0.0f) {
    return autopsy ? float4(death, 1.0f) : float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
  // Confidence: the tight degenerate fade and a screen-edge fade at the hit
  // (content leaving the screen hands off to the cube term smoothly). The
  // DDA needs no distance or precision shaping; a hit is a hit.
  float2 eb = min(hit_uv, 1.0f - hit_uv);
  float edge = saturate(min(eb.x, eb.y) * 12.0f);
  float conf = degen * edge;
  if (autopsy) {
    return float4(conf, conf, conf, 1.0f);  // accepted hit: white x conf
  }
  // Firefly clamp: the scene plane is pre-tonemap HDR; reflected sun-lit
  // texels above the tone curve's range sparkle hard at half res.
  float3 col = min(tex2.SampleLevel(smp_linear, hit_uv, 0).rgb, 3.0f);
  return float4(col, conf);
}

float4 ps_composite(VSOut i) : SV_Target {
#ifndef SHOWCASE
#define SHOWCASE 0
#endif
#if SHOWCASE
  // Showcase gate: alpha 0 leaves the scene plane untouched on a side
  // whose build-up stage has not revealed reflections yet. Compiled in
  // only for the SHOWCASE=1 variant swapped in during a run.
  if ((i.pos.x < p2.x ? p2.y : p2.z) < 0.5f) {
    return float4(0.0f, 0.0f, 0.0f, 0.0f);
  }
#endif
  float4 g = tex1c.SampleLevel(smp_point, i.uv, 0);
  float wrefl = abs(g.a);
  float dbg = p0.w;
  if (dbg > 1.5f && dbg < 2.5f) {
    // Debug 2: G-buffer world normals (black where no reflective surface).
    float3 nw = wrefl < 0.004f ? float3(-1.0f, -1.0f, -1.0f) : OctDecode(g.rg);
    return float4(nw * 0.5f + 0.5f, 1.0f);
  }
  if (dbg > 3.5f) {
    // Debug 4: the march's ray-autopsy plane, raw.
    return float4(tex0.SampleLevel(smp_point, i.uv, 0).rgb, 1.0f);
  }
  float sz = tex2d.SampleLevel(smp_point, i.uv, 0);
  float vis = 0.0f;
  if (wrefl >= 0.004f) {
    float3 n = normalize(OctDecode(g.rg).x * vrow0.xyz +
                         OctDecode(g.rg).y * vrow1.xyz +
                         OctDecode(g.rg).z * vrow2.xyz);
    float3 v = normalize(ViewPos(i.uv, g.b));
    vis = GbufVisible(g.b, sz, g.a, 1.0f - saturate(abs(dot(n, v))));
  }
  if (dbg > 2.5f) {
    // Debug 3: reflectivity (red) x depth visibility (green).
    return float4(wrefl, vis, 0.0f, 1.0f);
  }
  // Confidence-weighted 5-tap cross over the half-res SSR plane: the
  // jittered march resolves per-pixel hit phase noise (reflected thin
  // geometry over bright content sparkles otherwise); weighting by
  // confidence keeps misses from bleeding black into hits.
  float2 hs = size.zw;  // one texel of the half-res SSR plane
  float4 s0 = tex0.SampleLevel(smp_linear, i.uv, 0);
  float4 s1 = tex0.SampleLevel(smp_linear, i.uv + float2(hs.x, 0.0f), 0);
  float4 s2 = tex0.SampleLevel(smp_linear, i.uv - float2(hs.x, 0.0f), 0);
  float4 s3 = tex0.SampleLevel(smp_linear, i.uv + float2(0.0f, hs.y), 0);
  float4 s4 = tex0.SampleLevel(smp_linear, i.uv - float2(0.0f, hs.y), 0);
  float wsum = s0.a * 6.0f + s1.a + s2.a + s3.a + s4.a;
  float4 ssr;
  ssr.rgb = wsum > 1e-4f
                ? (s0.rgb * s0.a * 6.0f + s1.rgb * s1.a + s2.rgb * s2.a +
                   s3.rgb * s3.a + s4.rgb * s4.a) / wsum
                : s0.rgb;
  ssr.a = wsum * (1.0f / 10.0f);
  if (dbg > 0.5f) {
    // Debug 1: raw SSR color, confidence as a blue lift where colorless.
    return float4(ssr.rgb + float3(0.0f, 0.0f, 0.15f * ssr.a), 1.0f);
  }
  return float4(ssr.rgb, saturate(ssr.a * wrefl * vis * p0.z));
}
