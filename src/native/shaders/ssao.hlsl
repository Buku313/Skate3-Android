// Screen-space ambient occlusion (GTAO) over the resolved native scene.
// Ground-truth ambient occlusion after Jimenez et al.: per pixel, a few
// screen-space slices each march the depth buffer both ways to find the
// horizon angles, and the cosine-weighted arc between them integrates to a
// visibility term. Normals are reconstructed from depth (no G-buffer), so
// the pass needs only the scene depth + the projection constants.
//
// Pass chain (all fullscreen triangles, own binding layout):
//   ps_linearize  scene depth (sample 0 when MSAA; AO_MSAA=1 variant)
//                 -> view-space Z, R32_FLOAT, full res
//   ps_luma       scene color -> luminance, R8_UNORM, AO raster (the
//                 sun-lit protection mask input)
//   ps_gtao       linear depth + luma -> the final gamma-encoded AO
//                 multiplier, R8_UNORM (fully-protected pixels skip the
//                 march, most of a daytime frame)
//   ps_blur       depth-aware separable gaussian (2 passes), R8_UNORM
//   ps_composite  AO onto the guest output via dst.rgb * src.a blending
//                 (RGB write mask; the output has no usable alpha anyway)
//   ps_debug      AO greyscale replace, for tuning
//
// Color space note: the scene target is gamma-encoded (the material shaders
// linearize by squaring and return via sqrt), so the composite applies
// sqrt(ao_linear): a linear-space multiply expressed in gamma space.
cbuffer C : register(b0) {
  float4 size;  // xy = output size in px, zw = 1 / size
  float4 proj;  // x = |m00|, y = |m11|, z = m22, w = m32 (row-vector proj)
  float4 p0;    // x = radius (view units), y = intensity,
                // z = fade start (view Z), w = 1 / fade range
  float4 p1;    // xy = blur axis in px, z = sun-lit luma protection scale
};
// t0 is the per-pass primary input: the scene depth for ps_linearize, the
// linear-depth / AO intermediates for the march/blur, the scene color for
// copy/composite. Each entry references exactly one of the overlapping t0
// declarations; the others drop out at compile.
#ifdef AO_MSAA
Texture2DMS<float> depth_ms : register(t0);
#endif
Texture2D<float> tex0 : register(t0);
Texture2D<float4> tex0c : register(t0);  // ps_luma scene color
Texture2D<float> tex1 : register(t1);    // gtao: luma; blur: linear depth
SamplerState smp_point : register(s0);
SamplerState smp_linear : register(s1);

static const float kPi = 3.14159265f;
// 3 slices x 3 steps/side = 18 taps: the reference GTAO "high" budget. The
// per-pixel rotation + depth-aware blur integrate the dither; the nonlinear
// step distribution below spends the taps near the contact zone where the
// occlusion signal lives. (6 steps was ~2x the cost for a difference the
// blur erased; at night, where no pixel early-outs, that doubled the whole
// pass.)
#define AO_SLICES 3
#define AO_STEPS 3

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
#ifdef AO_MSAA
  float d = depth_ms.Load(int2(i.pos.xy), 0);
#else
  float d = tex0.Load(int3(int2(i.pos.xy), 0));
#endif
  return LinearZ(d);
}

// View-space position from UV + linear Z. |m00|/|m11| make the space a
// (possibly mirrored) view space; every AO quantity is invariant under a
// mirrored axis, so the projection's negative x scale needs no special
// handling.
float3 ViewPos(float2 uv, float z) {
  float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
  return float3(ndc.x * z / proj.x, ndc.y * z / proj.y, z);
}

float SampleZ(float2 uv) {
  return tex0.SampleLevel(smp_point, uv, 0);
}

// Normal from depth: for each axis take the neighbor with the smaller depth
// delta (avoids smeared normals across silhouette edges), cross the two
// tangents, and face the camera.
float3 ReconstructNormal(float2 uv, float3 P) {
  float2 dx = float2(size.z, 0.0f);
  float2 dy = float2(0.0f, size.w);
  float zl = SampleZ(uv - dx), zr = SampleZ(uv + dx);
  float zu = SampleZ(uv - dy), zd = SampleZ(uv + dy);
  float3 th = (abs(zr - P.z) < abs(zl - P.z))
                  ? (ViewPos(uv + dx, zr) - P)
                  : (P - ViewPos(uv - dx, zl));
  float3 tv = (abs(zd - P.z) < abs(zu - P.z))
                  ? (ViewPos(uv + dy, zd) - P)
                  : (P - ViewPos(uv - dy, zu));
  float3 n = normalize(cross(tv, th));
  return (dot(n, -P) < 0.0f) ? -n : n;
}

// Spatial rotation/offset noise (interleaved gradient noise); the blur
// integrates the resulting high-frequency dither.
float Ign(float2 p) {
  return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

// Half-res scene luminance for the sun-lit protection mask (low-frequency
// by nature, so the AO raster resolution is plenty; bilinear at the 2x
// reduction is an exact 2x2 box). Under HDR=1 the scene plane holds the
// pre-tonemap linear value; apply the scene tone chain first so the
// protection thresholds keep their display-space meaning.
float ps_luma(VSOut i) : SV_Target {
  float3 scene = tex0c.SampleLevel(smp_linear, i.uv, 0).rgb;
#ifdef HDR
  float3 x = max(scene, 0.0);
  float3 t1 = saturate(1.0 - x);
  float3 tm = max(x * 0.25 + 0.75, 1.0) - t1 * t1;
  scene = saturate(sqrt(max(tm * 0.5, 0.0)) * 1.41);
#endif
  return dot(scene, float3(0.299f, 0.587f, 0.114f));
}

float ps_gtao(VSOut i) : SV_Target {
  float zc = SampleZ(i.uv);
  float fade = saturate((zc - p0.z) * p0.w);
  // Sun-lit protection: real AO attenuates only ambient light; direct sun
  // is unaffected, but this chain runs post-tonemap where the two are
  // already summed. Approximate the split by luminance: bright pixels are
  // sun-lit and resist the darkening (keeping characters from dragging a
  // gray pool across lit pavement), dim pixels are ambient/shade-lit and
  // keep it. Fully-protected or fully-faded pixels skip the march.
  float luma = tex1.SampleLevel(smp_point, i.uv, 0);
  float protect = saturate(luma * p1.z);
  protect *= protect;
  // Skip fully-protected (sun-lit), fully-faded, and near-black pixels; a
  // multiplier on near-zero color is invisible, so night skies and deep
  // shadow don't pay for the march either.
  if (fade >= 1.0f || protect >= 0.999f || luma < 0.015f) {
    return 1.0f;
  }
  float3 P = ViewPos(i.uv, zc);
  float3 N = ReconstructNormal(i.uv, P);
  float3 V = -normalize(P);

  // World-space radius projected to pixels at this depth (vertical FOV
  // scale), bounded so near surfaces don't walk the whole screen (large
  // scattered footprints also thrash the texture cache; the dominant cost
  // of this pass is tap locality, not ALU).
  float radius_px = clamp(p0.x * proj.y / zc * size.y * 0.5f, 2.0f, 64.0f);
  float noise_dir = Ign(i.pos.xy);
  float noise_off = Ign(i.pos.xy + 41.13f);
  float inv_r = 1.0f / max(p0.x, 1e-4f);

  float vis = 0.0f;
  [unroll] for (int s = 0; s < AO_SLICES; ++s) {
    float phi = (float(s) + noise_dir) * (kPi / float(AO_SLICES));
    float2 sdir = float2(cos(phi), sin(phi));
    // The view-space direction a +sdir pixel step moves the surface point
    // (screen y runs down, view y up; aspect via the per-axis pixel sizes).
    float3 D = normalize(float3(sdir.x * size.z / proj.x,
                                -sdir.y * size.w / proj.y, 0.0f));
    float3 axis = normalize(cross(D, V));
    float3 ortho = normalize(D - dot(D, V) * V);
    // Normal projected into the slice plane; its signed angle from V decides
    // the hemisphere the horizon arc is clamped to.
    float3 Np = N - axis * dot(N, axis);
    float npl = length(Np);
    if (npl < 1e-4f) {
      vis += 1.0f;
      continue;
    }
    float cos_n = clamp(dot(Np, V) / npl, -1.0f, 1.0f);
    float n = sign(dot(Np, ortho)) * acos(cos_n);

    // March both sides, tracking the max cos(angle to V) with a smooth
    // range falloff so geometry near the radius edge fades instead of pops.
    float2 hcos = float2(-1.0f, -1.0f);
    [unroll] for (int side = 0; side < 2; ++side) {
      float sgn = side == 0 ? -1.0f : 1.0f;
      [loop] for (int j = 0; j < AO_STEPS; ++j) {
        // Nonlinear step distribution: taps cluster toward the contact
        // zone; the falloff discounts the far ones anyway.
        float fj = (float(j) + noise_off) / float(AO_STEPS);
        float t = radius_px * fj * sqrt(fj) + 1.0f;
        float2 suv = i.uv + sgn * sdir * t * size.zw;
        float3 Ps = ViewPos(suv, SampleZ(suv));
        float3 d3 = Ps - P;
        float dist2 = max(dot(d3, d3), 1e-8f);
        float rinv = rsqrt(dist2);
        float c = dot(d3, V) * rinv;
        float dist = dist2 * rinv;
        c = lerp(c, -1.0f, saturate(dist * inv_r * 3.333f - 2.333f));
        hcos[side] = max(hcos[side], c);
      }
    }
    float h1 = n + max(-acos(clamp(hcos.x, -1.0f, 1.0f)) - n, -0.5f * kPi);
    float h2 = n + min(acos(clamp(hcos.y, -1.0f, 1.0f)) - n, 0.5f * kPi);
    // Cosine-weighted arc integral between the two horizons (GTAO).
    float a =
        0.25f * ((-cos(2.0f * h1 - n) + cos_n + 2.0f * h1 * sin(n)) +
                 (-cos(2.0f * h2 - n) + cos_n + 2.0f * h2 * sin(n)));
    vis += npl * a;
  }
  vis = saturate(vis / float(AO_SLICES));
  // Bake the full multiplier here so the blur smooths the finished term and
  // the composite is a plain blend: intensity exponent on the linear
  // visibility, then the protection and distance fades toward 1. The
  // classic target is gamma-2-encoded, so the exponent halves to express a
  // linear-space multiply there; the HDR target is pre-tonemap linear
  // (near-proportional to display-linear below the tonemapper's knee) and
  // takes the exponent directly.
#ifdef HDR
  float a = pow(vis, p0.y);
#else
  float a = pow(vis, p0.y * 0.5f);
#endif
  a = lerp(a, 1.0f, protect);
  return lerp(a, 1.0f, fade);
}

// Depth-aware separable gaussian (sigma 2, radius 4): integrates the slice
// dither without bleeding AO across depth edges.
float ps_blur(VSOut i) : SV_Target {
  const float w[5] = {1.0f, 0.8825f, 0.6065f, 0.3247f, 0.1353f};
  float zc = tex1.SampleLevel(smp_point, i.uv, 0);
  float acc = tex0.SampleLevel(smp_point, i.uv, 0);
  float wsum = 1.0f;
  [unroll] for (int k = 1; k <= 4; ++k) {
    [unroll] for (int s = 0; s < 2; ++s) {
      float2 suv = i.uv + (s == 0 ? -1.0f : 1.0f) * p1.xy * float(k) * size.zw;
      float zs = tex1.SampleLevel(smp_point, suv, 0);
      float wz = saturate(1.0f - abs(zs - zc) / (zc * 0.05f + 0.05f));
      float wk = w[k] * wz;
      acc += tex0.SampleLevel(smp_point, suv, 0) * wk;
      wsum += wk;
    }
  }
  return acc / wsum;
}

// dst.rgb * src.a blend (RGB write mask): the AO plane already carries the
// finished gamma-encoded, luma-protected, distance-faded multiplier.
float4 ps_composite(VSOut i) : SV_Target {
  return float4(0.0f, 0.0f, 0.0f, tex0.SampleLevel(smp_linear, i.uv, 0));
}

float4 ps_debug(VSOut i) : SV_Target {
  float ao = tex0.SampleLevel(smp_linear, i.uv, 0);
  return float4(ao, ao, ao, 1.0f);
}

// Tile MAX-reduce of the linear-depth plane (t0) into the small occlusion-
// attribution grid the per-item profiler reads back to the CPU: a scene
// item whose nearest bbox corner is farther than the FARTHEST depth in
// every grid tile it covers cannot contribute pixels. b0 is repurposed for
// this pass: size.xy = linear-depth dims, size.zw = grid dims. 8x8 point
// taps spread across each tile's footprint (a ~24x24-texel tile at 4K is
// sampled at stride ~3, so a sub-3-px background hole can be missed; the
// consumer is telemetry, not culling, and errs toward "occluded" only
// through such holes).
float ps_depth_max(VSOut i) : SV_Target {
  float2 half_tile = 0.5f / size.zw;
  float m = 0.0f;
  [unroll] for (int ty = 0; ty < 8; ++ty) {
    [unroll] for (int tx = 0; tx < 8; ++tx) {
      float2 f = (float2(float(tx), float(ty)) + 0.5f) / 8.0f;
      float2 uv = i.uv + (f * 2.0f - 1.0f) * half_tile;
      m = max(m, tex0.SampleLevel(smp_point, uv, 0));
    }
  }
  return m;
}
