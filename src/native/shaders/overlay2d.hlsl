// 2D/APT overlay shader. Verified against a captured HUD draw stream:
// vertices are {float4 pos, float2 uv} in 1280x720
// APT movie space; the game's VS constants apply as clip = ortho * (world *
// pos) with c0..c3 the ortho rows, c4..c7 the element's transform rows and
// c8 the color multiplier, used exactly as staged.
cbuffer C : register(b0) {
  float4 m[10];  // m[0..3] proj rows, m[4..7] world rows, m[8] color,
                 // m[9].x = apply D3D9 half-pixel (2D ortho draws only),
                 // m[9].y = sharp-magnification amount (0 = plain bilinear),
                 // m[9].z = clip-space X extent of the 2D band (0 = full
                 // target; < 1 when a wide output centers the 16:9 stream
                 // in a pillarboxed band, see the emit_draw column-0 scale),
                 // m[9].w = unused (the D3D9 half-pixel shift is derived
                 // in the VS from the draw's own ortho scale)
};
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s1);
struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
  float4 color : COLOR0;
};
VSOut vs_main(float4 p : POSITION, float2 uv : TEXCOORD0, float4 color : COLOR0) {
  float4 wp = float4(dot(p, m[4]), dot(p, m[5]), dot(p, m[6]), dot(p, m[7]));
  VSOut o;
  o.pos = float4(dot(wp, m[0]), dot(wp, m[1]), dot(wp, m[2]), dot(wp, m[3]));
  // D3D9 half-pixel convention: the game targets integer pixel centers
  // and bakes a -0.5px shift into its 2D geometry; host APIs put pixel
  // centers at +0.5. The emulated path reverses this by adding +0.5 GUEST
  // pixel down-right at the viewport transform (rexglue util/draw.cpp,
  // half_pixel_offset cvar); replicate that exactly, deriving the guest
  // pixel size from the draw's own ortho scale (|m[0].x| = 2/W, |m[1].y|
  // = 2/H of the guest target; abs() pins the shift down-right on the
  // render target like the emulated viewport offset). This both aligns
  // pixel centers with texel centers for 1:1 art (without it the
  // clock-face quadrant tiles show their wrapped border rows as dark seam
  // lines) and restores full edge coverage (the previous up-left
  // output-pixel nudge left a see-through sliver along the bottom/right
  // of edge-to-edge loading-screen quads at 2x+ output scales). Must not
  // apply to 3D (world-space SimpleDraw markers), where the projection is
  // perspective.
  if (m[9].x > 0.0) {
    o.pos.x += 0.5 * abs(m[0].x) * o.pos.w;
    o.pos.y -= 0.5 * abs(m[1].y) * o.pos.w;
    // Edge snap: edge-to-edge art routinely lands a hair short of the
    // target boundary (sub-pixel authoring slop; the loading screen's
    // background reaches ~1279.5 of 1280 after the shift above, and the
    // shift itself moves the top/left edges half a pixel inward). At 1x
    // that is an invisible dark fringe; at 2x+ native scale it is a crisp
    // see-through sliver. Pull vertices within 3/4 of a guest pixel of a
    // boundary outward to exactly the boundary; the emulated path ships
    // the same remedy for its scaled resolves
    // (resolve_resolution_scale_fill_half_pixel_offset stretches the
    // first covered pixel over the gap). Distortion is sub-guest-pixel
    // and only for geometry already touching the edge; shared strip
    // vertices snap identically (pure function of position), so no
    // cracks are introduced between adjacent tiles.
    float2 gpx = float2(abs(m[0].x), abs(m[1].y));  // one guest px in NDC
    float2 ndc = o.pos.xy / o.pos.w;
    float2 eps = 0.75 * gpx;
    // The horizontal boundary is the 2D band edge: +-1 normally, +-m[9].z
    // when a wide output pillarboxes the 16:9 stream.
    float edge_x = m[9].z > 0.0 ? m[9].z : 1.0;
    if (ndc.x <= edge_x && ndc.x > edge_x - eps.x) ndc.x = edge_x;
    if (ndc.x >= -edge_x && ndc.x < -edge_x + eps.x) ndc.x = -edge_x;
    if (ndc.y <= 1.0 && ndc.y > 1.0 - eps.y) ndc.y = 1.0;
    if (ndc.y >= -1.0 && ndc.y < -1.0 + eps.y) ndc.y = -1.0;
    o.pos.xy = ndc * o.pos.w;
  }
  o.uv = uv;
  o.color = color;
  return o;
}
// Catmull-Rom reconstruction (9 bilinear fetches). Sharper interpolation
// than plain bilinear for magnified art; the sharpen pass below restores
// edge contrast the source's own 1-texel antialiasing loses under
// magnification.
float4 SampleCR(float2 uv, float2 ts) {
  float2 sp = uv * ts;
  float2 tc = floor(sp - 0.5) + 0.5;
  float2 f = sp - tc;
  float2 f2 = f * f;
  float2 f3 = f2 * f;
  float2 w0 = f2 - 0.5 * (f3 + f);
  float2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
  float2 w3 = 0.5 * (f3 - f2);
  float2 w2 = 1.0 - w0 - w1 - w3;
  float2 w12 = w1 + w2;
  float2 t0 = (tc - 1.0) / ts;
  float2 t3 = (tc + 2.0) / ts;
  float2 t12 = (tc + w2 / w12) / ts;
  float4 c = tex.Sample(smp, float2(t0.x, t0.y)) * (w0.x * w0.y);
  c += tex.Sample(smp, float2(t12.x, t0.y)) * (w12.x * w0.y);
  c += tex.Sample(smp, float2(t3.x, t0.y)) * (w3.x * w0.y);
  c += tex.Sample(smp, float2(t0.x, t12.y)) * (w0.x * w12.y);
  c += tex.Sample(smp, float2(t12.x, t12.y)) * (w12.x * w12.y);
  c += tex.Sample(smp, float2(t3.x, t12.y)) * (w3.x * w12.y);
  c += tex.Sample(smp, float2(t0.x, t3.y)) * (w0.x * w3.y);
  c += tex.Sample(smp, float2(t12.x, t3.y)) * (w12.x * w3.y);
  c += tex.Sample(smp, float2(t3.x, t3.y)) * (w3.x * w3.y);
  return c;
}
// Native FMV: the movie quad (AptMovieIntegration bracket, stride-24
// textured) samples three CPU-filled YUV planes: Y at t0, U at t1, V at
// t4 (the root signature's existing single-SRV tables). BT.601 studio
// range. c8 / vertex color are deliberately ignored: the game stages c8 =
// opaque BLACK on the movie quad (its own movie shader never reads it),
// which is exactly what rendered the video as a black cover through
// ps_main.
Texture2D<float4> tex_u : register(t1);
Texture2D<float4> tex_v : register(t4);
float4 ps_yuv2d(VSOut i) : SV_Target {
  float y = tex.Sample(smp, i.uv).r;
  float u = tex_u.Sample(smp, i.uv).r - 0.5;
  float v = tex_v.Sample(smp, i.uv).r - 0.5;
  y = (y - 16.0 / 255.0) * (255.0 / 219.0);
  float3 c = float3(y + 1.596 * v, y - 0.813 * v - 0.391 * u, y + 2.018 * u);
  return float4(saturate(c), 1.0);
}
float4 ps_main(VSOut i) : SV_Target {
  float4 c = tex.Sample(smp, i.uv);
  // Sharp magnification (m[9].y = skate3_native_render_scene_2d_sharp):
  // much of the HUD samples APT cached-bitmap tiles whose texel count
  // equals their 720p display size; at 2-3x output scales they magnify
  // into a soft blur under plain bilinear while atlas-glyph text (8-10
  // texels per 720p pixel) stays crisp. Where the fetch is MAGNIFIED,
  // reconstruct with Catmull-Rom and apply a locally-CLAMPED unsharp mask
  // (no halos: the result never leaves the neighborhood's value range).
  // Minified/1:1 content (density-rich atlases, replay video at 1x) is
  // untouched by the magnification gate.
  if (m[9].y > 0.0) {
    float2 ts;
    tex.GetDimensions(ts.x, ts.y);
    float2 tpp = fwidth(i.uv) * ts;  // texel footprint per output pixel
    float mag = 1.0 / max(max(tpp.x, tpp.y), 1e-4);
    if (mag > 1.25) {
      float4 cr = SampleCR(i.uv, ts);
      float2 px = 1.0 / ts;
      float4 n0 = tex.Sample(smp, i.uv + float2(-0.5, -0.5) * px);
      float4 n1 = tex.Sample(smp, i.uv + float2(0.5, -0.5) * px);
      float4 n2 = tex.Sample(smp, i.uv + float2(-0.5, 0.5) * px);
      float4 n3 = tex.Sample(smp, i.uv + float2(0.5, 0.5) * px);
      float4 blur = (n0 + n1 + n2 + n3) * 0.25;
      // Ramp in across 1.25x..2x so density-1 tiles at 2-3x output get the
      // full amount while barely-magnified art is barely touched.
      float amt = m[9].y * saturate((mag - 1.25) / 0.75);
      float4 s = cr + (cr - blur) * amt;
      float4 lo = min(min(n0, n1), min(n2, n3));
      float4 hi = max(max(n0, n1), max(n2, n3));
      c = clamp(s, min(lo, cr), max(hi, cr));
    }
  }
  return c * m[8] * i.color;
}
