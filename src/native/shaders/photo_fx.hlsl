// Photo-editor postfx chain: exact ports of the game's shaders, transcribed
// mechanically from their Xenos ucode (dumped via an F11 capture) and
// validated bit-exact against an offline ucode interpreter before HLSL
// conversion. Pass order
// mirrors the game's chain while a photo-mission photo editor is up:
//   visualfx   (full res)  scene+accum color grade / vignette / depth->CoC
//   dof_down   (576x320)   4-tap alpha(CoC)-weighted downsample
//   dof_mb     (576x320)   9-tap motion-blur+DOF pass (motion from depth)
//   dof        (576x320)   9-tap variable-radius CoC blur
//   uber       (full res)  temporal-jitter combine + grade LUT + grain
//   fisheye    (full res)  lens warp + vignette gradient + tint -> screen
// Constants come LIVE from the game's PS/VS shadow banks captured at the
// SetPending_AluConstants hook per pass (rows c0..c31 + VS rows in c240..247);
// rows c250..c255 are the shaders' LITERAL tables, loaded by the game via
// PM4 LOAD_ALU_CONSTANT from the shader asset footers and baked here by the
// app layer (values read from a live capture).
//
// Register indices are kept IDENTICAL to the ucode. Do not simplify.

cbuffer Consts : register(b0) { float4 c[256]; };
// c[240..247] = the pass's VS c0..c7 (uvOffset/uvScale/oC1 rows).
// c[248].x    = output width, .y = output height (depth pack / debug).

Texture2D    t0 : register(t0);
Texture2D    t1 : register(t1);
Texture2D    t2 : register(t2);
Texture2D    t3 : register(t3);
Texture3D    t4 : register(t4);
Texture2D    t6 : register(t6);
Texture2D    t7 : register(t7);
SamplerState s_lin  : register(s0);  // linear clamp (color/gradient fetches)
SamplerState s_pt   : register(s1);  // point clamp (packed depth)
SamplerState s_wrap : register(s2);  // linear wrap (grain)

struct VsOut {
  float4 pos : SV_Position;
  float4 r0 : TEXCOORD0;
  float4 r1 : TEXCOORD1;
};

// Fullscreen triangle; per-pass interpolators mirror the game quad VSs
// (uv attr = screen uv; ucode: oC0 = (uv + c3.xy, uv), oC1 = uv.x*c0 +
// uv.y*c1 + c2; the offset/scale rows are the captured VS constants).
VsOut vs_offset(uint vid : SV_VertexID) {
  VsOut o;
  float2 t = float2((vid << 1) & 2, vid & 2);
  o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
  o.r0 = float4(t + c[243].xy, t);
  o.r1 = t.x * c[240] + t.y * c[241] + c[242];
  return o;
}

// uber: oC0 = raw uv4.
VsOut vs_raw(uint vid : SV_VertexID) {
  VsOut o;
  float2 t = float2((vid << 1) & 2, vid & 2);
  o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
  o.r0 = float4(t, t);
  o.r1 = 0;
  return o;
}

// fisheye: oC0.xy = uv*c3.xy + c2.xy (uvScale/uvTrans), oC0.zw = raw uv.
VsOut vs_scaled(uint vid : SV_VertexID) {
  VsOut o;
  float2 t = float2((vid << 1) & 2, vid & 2);
  o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
  o.r0 = float4(t * c[243].xy + c[242].xy, t);
  o.r1 = 0;
  return o;
}

// ---------------------------------------------------------------------------
// Depth pack: native D32 MSAA depth (sample 0) -> the console D24S8-as-8888
// texel layout the ported shaders reconstruct: sample.x = mid byte,
// sample.y = low byte, sample.z = stencil (0 natively), sample.w = high
// byte. dp3 with the baked weights (255/16776960, 255/256, 255/65536) then
// yields the exact 24-bit depth (visualfx c254.xyz, dof_mb c255.xyz).
#ifdef PFX_MSAA
Texture2DMS<float> depth_ms : register(t5);
#else
Texture2D<float> depth_1x : register(t5);
#endif

float4 ps_depthpack(VsOut i) : SV_Target {
#ifdef PFX_MSAA
  float d = depth_ms.Load(int2(i.pos.xy), 0);
#else
  float d = depth_1x.Load(int3(int2(i.pos.xy), 0));
#endif
  float D = min(floor(d * 16777216.0), 16777215.0);
  float hi = floor(D / 65536.0);
  float mid = floor((D - hi * 65536.0) / 256.0);
  float lo = D - hi * 65536.0 - mid * 256.0;
  return float4(mid, lo, 0.0, hi) * (1.0 / 255.0);
}

// Plain bilinear blit: the quarter-res accumulation update (the game feeds
// visualfx tf3 from a motion-accumulation buffer; natively it is approximated
// per skate3_native_render_scene_photo_native_accum).
float4 ps_blit(VsOut i) : SV_Target {
  return t0.SampleLevel(s_lin, i.r0.zw, 0);
}

// Debug visualization (photo_native_debug): c[249].x selects
//  1 = t0 alpha (the visualfx CoC) as grayscale
//  2 = packed-depth reconstruction from t1 (r = (1-d)*100 sat, g/b = frac
//      ramps of d for band inspection)
float4 ps_pfx_debug(VsOut i) : SV_Target {
  if (c[249].x < 1.5) {
    float a = t0.SampleLevel(s_lin, i.r0.zw, 0).w;
    return float4(a, a, a, 1);
  }
  float4 s = t1.SampleLevel(s_pt, i.r0.zw, 0);
  float d = s.y * 1.51991853e-05 + s.w * 0.99609381 + s.x * 0.00389099144;
  return float4(saturate((1.0 - d) * 100.0), frac(d * 256.0), frac(d * 4096.0), 1);
}

// ---------------------------------------------------------------------------
// postfx_visualfxPS (bit-exact transcription). t0 = scene,
// t1 = packed depth, t3 = quarter-res accumulation buffer.
float4 ps_visualfx(VsOut i) : SV_Target {
  float2 uv = i.r0.xy;
  float4 r0 = i.r0;
  float4 r1 = i.r1;
  float4 r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0;
  float4 oC0 = 0;
  float nx, ny, nz, nw, dw, rx, old, r1x_old;

  r2 = t1.SampleLevel(s_pt, uv, 0);            // tfetch2D r2, r0.xy, tf1
  r4.xyz = t0.SampleLevel(s_lin, uv, 0).xyz;   // tfetch2D r4.xyz_, r0.xy, tf0
  r3.xyz = t3.SampleLevel(s_lin, uv, 0).xyz;   // tfetch2D r3.xyz_, r0.xy, tf3

  // 7: mad r3.xyz_, r4.zxyy, c253.zzzz, r3.zxyy
  nx = r4.z * c[253].z + r3.z;
  ny = r4.x * c[253].z + r3.x;
  nz = r4.y * c[253].z + r3.y;
  r3.x = nx; r3.y = ny; r3.z = nz;
  // 8: dp3 r0._y__, r2.ywxx, c254.xyzz  (24-bit depth reconstruction)
  r0.y = r2.y * c[254].x + r2.w * c[254].y + r2.x * c[254].z;
  // 9: add r6.__zw, c2.yyyx, -c1.yyyx ; + subsc r3.___w, -c4.x, -r0.y
  r6.z = c[2].y - c[1].y;
  r6.w = c[2].x - c[1].x;
  r3.w = r0.y - c[4].x;
  // 10: mad r1.xyz_, r0.yyyy, c6.xyzz, r1.xyzz
  r1.x = r0.y * c[6].x + r1.x;
  r1.y = r0.y * c[6].y + r1.y;
  r1.z = r0.y * c[6].z + r1.z;
  // 11: mad r0._yz_, r1.zzzz, r0.zzww, r1.xxyy
  ny = r1.z * r0.z + r1.x;
  nz = r1.z * r0.w + r1.y;
  r0.y = ny; r0.z = nz;
  // 12: dp3 r0.___w, r3.xyzz, c255.xyzz ; + rcp r0.x___, r3.w
  dw = r3.x * c[255].x + r3.y * c[255].y + r3.z * c[255].z;
  rx = 1.0 / r3.w;
  r0.w = dw;
  r0.x = rx;
  // 13: mad r1.xyz_, r0.wwww, c0.zyxx, -r3.xzyy
  r1.x = r0.w * c[0].z + (-r3.x);
  r1.y = r0.w * c[0].y + (-r3.z);
  r1.z = r0.w * c[0].x + (-r3.y);
  // 14: mad r4.xyz_, r1.xyzz, c0.wwww, r3.xzyy
  r4.x = r1.x * c[0].w + r3.x;
  r4.y = r1.y * c[0].w + r3.z;
  r4.z = r1.z * c[0].w + r3.y;
  // 15: mul r0.x__w, r0.xxxx, c253.xyyy
  old = r0.x;
  r0.x = old * c[253].x;
  r0.w = old * c[253].y;
  // 16: mul r0.x__w, r0.xwww, c3.xxxx ; + maxs r4.___w, c255.ww
  nx = r0.x * c[3].x;
  nw = r0.w * c[3].x;
  r0.x = nx; r0.w = nw;
  r4.w = c[255].w;
  // 17: mad_sat r5.xy__, r0.wwww, c13.xyyy, c14.xyyy
  r5.x = saturate(r0.w * c[13].x + c[14].x);
  r5.y = saturate(r0.w * c[13].y + c[14].y);

#define DP4PERM(CR) ((CR).z * r4.x + (CR).x * r4.z + (CR).y * r4.y + (CR).w * r4.w)
  // 18-24: the FX color matrix + gates
  r3.x = DP4PERM(c[7]);
  r3.y = DP4PERM(c[8]);
  r2.x = saturate(r2.z);
  r3.z = DP4PERM(c[9]);
  r2.y = r2.x - c[255].w;
  r3.w = DP4PERM(c[10]);
  r1.y = c[5].x * r0.y;
  r1.x = (r0.w >= c[1].y) ? 1.0 : 0.0;
  r0.y = c[5].x * r0.z;
  r6.x = DP4PERM(c[12]);
  r2.z = r1.y * r1.y;
  r6.y = DP4PERM(c[11]);
  r2.w = r0.y * r0.y;
#undef DP4PERM
  // 25: add_sat r1._y_w, r2.xxyy, r2.xxxx
  r1.y = saturate(r2.x + r2.x);
  r1.w = saturate(r2.y + r2.x);
  // 26: mad r1.x_z_, r6.zwww, r1.xxxx, c1.yxxx
  r1x_old = r1.x;
  r1.x = r6.z * r1x_old + c[1].y;
  r1.z = r6.w * r1x_old + c[1].x;
  // 27: mul r3, r3, r5.xxxy ; + addsc r0.__z_, c253.w, r0.x
  r3.x *= r5.x;
  r3.y *= r5.x;
  r3.z *= r5.x;
  r3.w *= r5.y;
  r0.z = c[253].w + r0.x;
  // 28: mad r3._yz_, r6.xxyy, r5.yyyy, r3.zzyy
  ny = r6.x * r5.y + r3.z;
  nz = r6.y * r5.y + r3.y;
  r3.y = ny; r3.z = nz;
  // 29: add r0._y__, r1.xxxx, -r0.wwww ; + adds r0.___w, r2.zw
  r0.y = r1.x + (-r0.w);
  r0.w = r2.z + r2.w;
  // 30: mul_sat r0.xy__, r0.yzzz, r1.zwww ; + adds r3.x___, r3.xw
  nx = saturate(r0.y * r1.z);
  ny = saturate(r0.z * r1.w);
  r0.x = nx; r0.y = ny;
  r3.x = r3.x + r3.w;
  // 31: add r4.xyz_, r4.zxyy, -r3.xyzz ; + adds r0.__z_, r2.xx
  nx = r4.z - r3.x;
  ny = r4.x - r3.y;
  nz = r4.y - r3.z;
  r4.x = nx; r4.y = ny; r4.z = nz;
  r0.z = r2.x + r2.x;
  // 32: add r4.___w, -r0.yyyy, c255.wwww ; + mulsc r0._y__, c16.x, r1.y
  r4.w = (-r0.y) + c[255].w;
  r0.y = c[16].x * r1.y;
  // 33: mul r1, r0.yyyw, r4.xzyw ; + subsc_sat r0.__z_, c255.w, r0.z
  r1.x = r0.y * r4.x;
  r1.y = r0.y * r4.z;
  r1.z = r0.y * r4.y;
  r1.w = r0.w * r4.w;
  r0.z = saturate(c[255].w - r0.z);
  // 34: mul_sat r0.__z_, r1.wwww, r0.zzzz ; + mulsc r0._y__, c255.z, r0.x
  r0.z = saturate(r1.w * r0.z);
  r0.y = c[255].z * r0.x;
  // 35: add oC0.xyz_, r3.xzyy, r1.xyzz
  oC0.x = r3.x + r1.x;
  oC0.y = r3.z + r1.y;
  oC0.z = r3.y + r1.z;
  // 36: max r0.__z_, r0.zzzz, c254.wwww ; + mulsc r0.x___, c15.x, r0.y
  r0.z = max(r0.z, c[254].w);
  r0.x = c[15].x * r0.y;
  // 37: max_sat oC0.___w, r0.zzzz, r0.xxxx
  oC0.w = saturate(max(r0.z, r0.x));
  return oC0;
}

// ---------------------------------------------------------------------------
// bloom_dof_motionblur_dof_dowsamplePS (bit-exact): 4-tap
// alpha-weighted downsample of the visualfx output into the DOF half buffer.
float4 ps_dof_down(VsOut i) : SV_Target {
  float4 r0 = i.r0;
  float4 r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0;
  float4 c255 = c[255];

  r1.y = r0.y;
  r1.z = r0.x;
  r1.x = r0.x + c255.x;
  r1.w = r0.y + c255.y;
  r4 = t0.SampleLevel(s_lin, float2(r1.x, r1.y), 0);
  r5 = t0.SampleLevel(s_lin, float2(r1.z, r1.w), 0);
  r1 = t0.SampleLevel(s_lin, float2(r1.x, r1.w), 0);
  r2 = t0.SampleLevel(s_lin, float2(r0.x, r0.y), 0);
  r3.x = r2.w + c255.w;
  r0.x = r1.x * r1.w;
  r0.y = r1.z * r1.w;
  r0.z = r1.y * r1.w;
  {
    float ny = r5.x * r5.w + r0.x;
    float nz = r5.y * r5.w + r0.z;
    float nw = r5.z * r5.w + r0.y;
    r0.y = ny; r0.z = nz; r0.w = nw;
  }
  r0.x = r3.x + r4.w;
  r0.x = r0.x + r5.w;
  {
    float ny = r4.x * r4.w + r0.y;
    float nz = r4.z * r4.w + r0.w;
    float nw = r4.y * r4.w + r0.z;
    r0.y = ny; r0.z = nz; r0.w = nw;
  }
  r1.x = r3.x * r2.x + r0.y;
  r1.y = r3.x * r2.y + r0.w;
  r1.z = r3.x * r2.z + r0.z;
  r0.x = r0.x + r1.w;
  r0.y = 1.0 / r0.x;
  float4 oC0;
  oC0.x = r1.x * r0.y;
  oC0.y = r1.y * r0.y;
  oC0.z = r1.z * r0.y;
  oC0.w = c255.z * r0.x;
  return oC0;
}

// ---------------------------------------------------------------------------
// bloom_dof_tap9dofMotionBlurPS (bit-exact): 9-tap motion-blur +
// DOF pass. t0 = DOF half buffer, t1 = packed depth (motion reconstructed
// from depth + camera rows in c1).
float4 ps_dof_mb(VsOut i) : SV_Target {
  float4 r0 = i.r0;
  float4 r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r11 = 0;
  float ps = 0;
  float4 c254 = c[254], c255 = c[255];

  // /*6*/
  r2.x = r0.x + c[7].x;  r2.y = r0.y + c[7].y;
  // /*7*/ tfetch2D r1.___w, r2.xy, tf0
  r1.w = t0.SampleLevel(s_lin, r2.xy, 0).w;
  // /*8*/ tfetch2D r2.xyw_, r0.xy, tf1  (dest xyw_ -> r2.x,r2.y,r2.z <- s.x,s.y,s.w)
  { float4 vel = t1.SampleLevel(s_pt, r0.xy, 0); r2.x = vel.x; r2.y = vel.y; r2.z = vel.w; }
  // /*9*/ dp3 r2._y__, r2.yzxx, c255.xyzz  +  mulsc r2.x, c2.x, r1.w
  { float dp = r2.y * c255.x + r2.z * c255.y + r2.x * c255.z; r2.y = dp; }
  r2.x = c[2].x * r1.w;
  // /*10*/
  r2.x = max(r2.x, c254.y);
  // /*11*/ mad r1.xyz_, r2.yyyy, c1.xzyy, r1.xzyy   (r1.xyz==0)
  { float nx = r2.y * c[1].x + r1.x, ny = r2.y * c[1].z + r1.z, nz = r2.y * c[1].y + r1.y; r1.x = nx; r1.y = ny; r1.z = nz; }
  // /*12*/ mad r1.xy__, r1.yyyy, r0.zwww, r1.xzzz
  { float nx = r1.y * r0.z + r1.x, ny = r1.y * r0.w + r1.z; r1.x = nx; r1.y = ny; }
  // /*13*/ mul r1._yz_, r1.xxyy, c0.xxxx
  { float ny = r1.x * c[0].x, nz = r1.y * c[0].x; r1.y = ny; r1.z = nz; }
  // /*14*/ dp2add r0.___w, r1.yzzz, r1.yzzz, c255.wwww
  r0.w = r1.y * r1.y + r1.z * r1.z + c255.w;
  // /*15*/ add r1.x, r1.y, -r2.x  +  sqrt_sat r0.w, |r0.w|
  r1.x = r1.y - r2.x;
  r0.w = saturate(sqrt(abs(r0.w)));
  // /*16*/ mul r1._yz_, r1.zzxx, r0.wwww  +  addsc r0.w, c254.z, r0.w
  { float ny = r1.z * r0.w, nz = r1.x * r0.w; r1.y = ny; r1.z = nz; }
  r0.w = c254.z + r0.w;
  // /*17*/
  r1.x = r1.z + r2.x;

  // /*18-27*/ tap centers
  r2.x = r1.x * c[11].w + r0.x;   r2.y = r1.y * c[11].w + r0.y;
  r4.xy = r1.xy * c[9].w;
  r6.xy = r1.xy * c[7].w;
  r5.xy = r1.xy * c[5].w;        r4.z = c[10].w * r1.x;
  r3.xy = r1.xy * c[3].w;        r6.z = c[8].w * r1.x;
  r3.z = r1.x * c[4].w; r3.w = r1.y * c[4].w;   r5.z = c[6].w * r1.x;
  r3 += r0.xyxy;                 r5.w = c[6].w * r1.y;
  r5 += r0.xyxy;                 r6.w = c[8].w * r1.y;
  r6 += r0.xyxy;                 r4.w = c[10].w * r1.y;
  r1 = r4 + r0.xyxy;

  // /*28-37*/ taps
  r4 = t0.SampleLevel(s_lin, r1.zw, 0);
  r1 = t0.SampleLevel(s_lin, r1.xy, 0);
  float4 r12 = t0.SampleLevel(s_lin, r6.zw, 0);
  float4 r10 = t0.SampleLevel(s_lin, r6.xy, 0);
  float4 r9  = t0.SampleLevel(s_lin, r5.zw, 0);
  float4 r8  = t0.SampleLevel(s_lin, r5.xy, 0);
  float4 r7  = t0.SampleLevel(s_lin, r3.zw, 0);
  r5         = t0.SampleLevel(s_lin, r3.xy, 0);
  r2         = t0.SampleLevel(s_lin, r2.xy, 0);
  r0.x       = t0.SampleLevel(s_lin, r0.xy, 0).w;   // /*37*/ r0.w___ -> r0.x <- s.w

  // /*38-49*/ coc weights (r0.w = base blur radius term)
  r3.x = saturate(r0.w + c[9].z);    r0.y = saturate(c[10].z + r0.w);
  r3.y = saturate(r0.w + c[11].z);   r0.z = saturate(c[3].z + r0.w);
  r3.w = saturate(r3.y * r2.w + r0.w);
  r6.x = r0.z * r5.w;                 r0.z = saturate(c[4].z + r0.w);
  r6.y = r0.z * r7.w;                 r0.z = saturate(c[5].z + r0.w);
  r6.z = r0.z * r8.w;                 r0.z = saturate(c[6].z + r0.w);
  r6.w = r0.z * r9.w;                 r0.z = saturate(c[7].z + r0.w);
  r11.x = r0.z * r10.w;               r0.z = saturate(c[8].z + r0.w);
  r11.z = r3.x * r1.w;                ps = r0.z;                 // maxs r0._,r0.zz
  r11.w = r0.y * r4.w;                r11.y = r12.w * ps; ps = r12.w * ps;
  r11 = saturate(r0.w + r11);         ps = r0.x;                 // maxs r0.x,r0.xx
  r6  = saturate(r0.w + r6);          ps = r11.w;                // maxs r0._,r11.ww

  // /*50-62*/ weighted color accumulation
  r5.w = (r6.w + r6.z + r6.x + r6.y) * c254.x;   r3.x = r4.x * ps; ps = r4.x * ps;
  { float nx = r11.z * r1.x, ny = r11.z * r1.y, nz = r11.z * r1.z; r1.x = nx; r1.y = ny; r1.z = nz; } ps = r11.w;
  r1.w = (r11.w + r11.z + r11.y + r11.x) * c254.x;  r3.y = r4.y * ps; ps = r4.y * ps;
  r0.y = r11.y * r12.x; r0.z = r11.y * r12.y; r0.w = r11.y * r12.z;   ps = r11.w;
  // /*54*/ mad r0._yzw, r11.xxxx, r10.xxzy, r0.yywz
  { float ny = r11.x * r10.x + r0.y, nz = r11.x * r10.z + r0.w, nw = r11.x * r10.y + r0.z; r0.y = ny; r0.z = nz; r0.w = nw; }
  // /*55*/ mad r0._yzw, r6.wwww, r9.xxyz, r0.yywz
  { float ny = r6.w * r9.x + r0.y, nz = r6.w * r9.y + r0.w, nw = r6.w * r9.z + r0.z; r0.y = ny; r0.z = nz; r0.w = nw; }
  // /*56*/ mad r0._yzw, r6.zzzz, r8.xxzy, r0.yywz
  { float ny = r6.z * r8.x + r0.y, nz = r6.z * r8.z + r0.w, nw = r6.z * r8.y + r0.z; r0.y = ny; r0.z = nz; r0.w = nw; }
  // /*57*/ mad r0._yzw, r6.yyyy, r7.xxyz, r0.yywz
  { float ny = r6.y * r7.x + r0.y, nz = r6.y * r7.y + r0.w, nw = r6.y * r7.z + r0.z; r0.y = ny; r0.z = nz; r0.w = nw; }
  // /*58*/ mad r5.xyz_, r6.xxxx, r5.xzyy, r0.ywzz
  { float nx = r6.x * r5.x + r0.y, ny = r6.x * r5.z + r0.w, nz = r6.x * r5.y + r0.z; r5.x = nx; r5.y = ny; r5.z = nz; }
  // /*59*/ add r1, r5.xzyw, r1  +  muls_prev r3.z, r4.z
  r1 = float4(r5.x + r1.x, r5.z + r1.y, r5.y + r1.z, r5.w + r1.w);
  r3.z = r4.z * ps; ps = r4.z * ps;
  // /*60*/ add r1, r1.xzyw, r3.xzyw  +  maxs r0.y, c254.ww
  r1 = float4(r1.x + r3.x, r1.z + r3.z, r1.y + r3.y, r1.w + r3.w);
  r0.y = c254.w; ps = c254.w;
  // /*61*/ rcp r0.z, r1.w
  r0.z = 1.0 / r1.w;
  // /*62*/ mad r1.xyz_, r3.wwww, r2.xyzz, r1.xzyy
  { float nx = r3.w * r2.x + r1.x, ny = r3.w * r2.y + r1.z, nz = r3.w * r2.z + r1.y; r1.x = nx; r1.y = ny; r1.z = nz; }
  // /*63*/ mul oC0.xyz_, r1.xyzz, r0.zzzz  +  maxs oC0.w, r0.xy
  float4 oC0;
  oC0.x = r1.x * r0.z;
  oC0.y = r1.y * r0.z;
  oC0.z = r1.z * r0.z;
  oC0.w = max(r0.x, r0.y);
  return oC0;
}

// ---------------------------------------------------------------------------
// bloom_dof_tap9dofPS (bit-exact): 9-tap variable-radius CoC blur.
float4 ps_dof(VsOut i) : SV_Target {
  float4 r0 = i.r0;
  float4 r2 = 0, r3 = 0, r4 = 0, r5 = 0;

  float4 r1 = t0.SampleLevel(s_lin, float2(r0.x, r0.y), 0);
  r0.z = c[0].x * r1.w;
  r0.z = max(r0.z, c[255].x);
  float rz = r0.z;

  r5.xy = rz * c[8].xy;
  r4.xy = rz * c[6].xy;
  r3.xy = rz * c[3].xy;   r5.z = c[9].x * rz;
  r2.xy = rz * c[1].xy;   r4.z = c[7].x * rz;
  r2.z = rz * c[2].x;     r2.w = rz * c[2].y;   r3.z = c[4].x * rz;
  r2 += r0.xyxy;          r3.w = c[4].y * rz;
  r3 += r0.xyxy;          r4.w = c[7].y * rz;
  r4 += r0.xyxy;          r5.w = c[9].y * rz;
  r0 = r5 + r0.xyxy;

  float4 r12 = t0.SampleLevel(s_lin, r0.zw, 0);
  float4 r11 = t0.SampleLevel(s_lin, r0.xy, 0);
  float4 r10 = t0.SampleLevel(s_lin, r4.zw, 0);
  float4 r9 = t0.SampleLevel(s_lin, r4.xy, 0);
  float4 r8 = t0.SampleLevel(s_lin, r3.zw, 0);
  float4 r7 = t0.SampleLevel(s_lin, r3.xy, 0);
  float4 r6 = t0.SampleLevel(s_lin, r2.zw, 0);
  r5 = t0.SampleLevel(s_lin, r2.xy, 0);

  r3.y = r8.w * c[4].z;   r0.x = r7.w;
  r3.x = r1.w * c[5].z;   r3.z = c[3].z * r0.x;
  r2.z = r10.w * c[7].z;  r0.y = r9.w;
  r2.x = r12.w * c[9].z;  r0.x = r11.w;
  r4.x = r2.x * r12.x; r4.y = r2.x * r12.y; r4.z = r2.x * r12.z;
  r2.y = c[8].z * r0.x;
  {
    float nx = r2.y * r11.x, nz = r2.y * r11.y, nw = r2.y * r11.z;
    r0.x = nx; r0.z = nz; r0.w = nw;
    r2.w = c[6].z * r0.y;
  }
  { float nx = r2.z * r10.x + r0.x, ny = r2.z * r10.z + r0.w, nz = r2.z * r10.y + r0.z; r0.x = nx; r0.y = ny; r0.z = nz; }
  { float nx = r2.w * r9.x + r0.x, ny = r2.w * r9.y + r0.z, nz = r2.w * r9.z + r0.y; r0.x = nx; r0.y = ny; r0.z = nz; }
  { float nx = r3.x * r1.x + r0.x, ny = r3.x * r1.z + r0.z, nz = r3.x * r1.y + r0.y; r0.x = nx; r0.y = ny; r0.z = nz; }
  { float nx = r3.y * r8.x + r0.x, ny = r3.y * r8.y + r0.z, nz = r3.y * r8.z + r0.y; r0.x = nx; r0.y = ny; r0.z = nz; }
  { float nx = r3.z * r7.x + r0.x, ny = r3.z * r7.z + r0.z, nz = r3.z * r7.y + r0.y; r0.x = nx; r0.y = ny; r0.z = nz; }
  r4.w = r6.w * c[2].z;   r5.w = c[1].z * r5.w;
  { float nx = r4.w * r6.x + r0.x, ny = r4.w * r6.y + r0.z, nz = r4.w * r6.z + r0.y; r0.x = nx; r0.y = ny; r0.z = nz; }
  { float nx = r5.w * r5.x + r0.x, ny = r5.w * r5.z + r0.z, nz = r5.w * r5.y + r0.y; r5.x = nx; r5.y = ny; r5.z = nz; }
  r0 = float4(r5.w + r4.w, r5.x + r4.x, r5.z + r4.y, r5.y + r4.z);
  r3.z = r0.x + r3.z;
  r3.y = r3.z + r3.y;
  r3.x = r3.y + r3.x;
  r2.w = r3.x + r2.w;
  r2.z = r2.w + r2.z;
  r2.y = r2.z + r2.y;
  r2.x = r2.y + r2.x;
  r0.x = 1.0 / r2.x;
  float4 oC0;
  oC0.x = r0.y * r0.x;
  oC0.y = r0.z * r0.x;
  oC0.z = r0.w * r0.x;
  oC0.w = r1.w;
  return oC0;
}

// ---------------------------------------------------------------------------
// postfx_uberPS (bit-exact): temporal-jitter combine + 3D grade
// LUT + grain + border mask. t0 = graded sharp scene, t1 = packed depth,
// t4 = 3D LUT (identity volume natively unless the game's is resolvable),
// t6 = grain, t7 = DOF-blurred half buffer.
float4 ps_uber(VsOut i) : SV_Target {
  float4 r0 = i.r0;
  float4 r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0;

  r2.x = r0.x * c[253].x + c[253].z;
  r1.x = c[2].x + r0.y;
  r4.y = frac(r1.x);
  r2.y = r4.y * c[253].x + c[253].z;
  r1.z = r2.x * c[0].y - r2.x;
  r1.w = r2.y * c[0].y - r2.y;
  r1.x = abs(r2.x) - c[1].w;
  r1.y = abs(r2.y) - c[1].w;
  r1.x = max(r1.x, c[253].w);
  r1.y = max(r1.y, c[253].w);
  {
    float2 nr = float2(abs(r2.x) - c[0].z, abs(r2.y) - c[0].z);
    r2.x = nr.x; r2.y = nr.y;
    r2.z = r1.x + r1.y;
  }
  r2.x = r2.x * c[0].z;
  r2.y = r2.y * c[0].z;
  r2.z = r2.z * c[0].w;
  r1.x = max(r2.x, c[253].w);
  r1.y = max(r2.y, c[253].w);
  r4.z = r1.z * r1.x;
  r4.w = r1.w * r1.y;
  r4.x = r0.x;
  r3 = t7.SampleLevel(s_lin, r4.xy, 0);
  r1 = t0.SampleLevel(s_lin, r4.xy, 0);
  r5 = r3 - r1;
  r3 = r5 * r3.w + r1;
  {
    float3 t19;
    t19.x = r3.y * c[254].z + c[254].x;
    t19.y = r3.x * c[254].w + c[254].y;
    t19.z = r3.z * c[254].z + c[254].x;
    r1.xyz = t19;
  }
  r5.y = r4.w + r4.y;
  r5.x = r0.x + r0.x;
  {
    float2 t21 = float2(r5.x + r4.z, r5.y + r4.y);
    r0.x = t21.x; r0.y = t21.y;
  }
  r0.x = r0.x * c[253].y;
  r0.y = r0.y * c[253].y;
  r0 = t0.SampleLevel(s_lin, r0.xy, 0);
  r5 = t4.SampleLevel(s_lin, r1.xyz, 0);
  r1 = t6.SampleLevel(s_wrap, r4.xy, 0);
  r2.w = t1.SampleLevel(s_pt, r4.xy, 0).z;
  r4 = r2.w * c[4];
  r1 = r1 * c[253].x + c[253].z;
  r1 = r1 * c[4] + c[255].x;
  r5 = r5 - r3;
  r5 = r5 * c[3].x + r3;
  r1 = r5 * r1;
  r3 = r3 - r1;
  r1 = r4 * r3 + r1;
  r3 = r1 - r0;
  r0 = r3 * c[0].x + r0;
  r1 = r1 + r0;
  r0 = r1 * c[253].y;
  r3.x = c[1].x * c[253].y;
  r3.y = c[1].y * c[253].y;
  r3.z = c[1].z * c[253].y;
  r1.w = 0.0;
  r1.x = r3.x * r1.x - r0.x;
  r1.y = r3.y * r1.y - r0.y;
  r1.z = r3.z * r1.z - r0.z;
  r0 = r2.z * r1 + r0;
  return r0 * c[5];
}

// ---------------------------------------------------------------------------
// postfx_basictex_fisheye_opaquePS (bit-exact): lens warp +
// vignette gradient + tint. t0 = uber output, t2 = the 512x2 vignette
// gradient texture (resolved from the game's own asset via fetch words).
float4 ps_fisheye(VsOut i) : SV_Target {
  float4 r0 = i.r0;
  float4 r1 = 0;

  r1.y = max(c[0].y, c[0].y);
  {
    float r0x_old = r0.x;
    float3 nr;
    nr.x = r0x_old * c[255].w + c[255].y;
    nr.y = r0.y * c[255].z + c[255].x;
    nr.z = r0x_old * c[255].z + c[255].x;
    r0.xyz = nr;
  }
  r1.x = r0.x * r0.x;
  r1.z = r0.y * r0.y;
  r1.w = r0.z * r0.z;
  float ps = c[0].x;
  {
    float r0_x = r1.x + r1.z;
    float r0_w = r1.w + r1.z;
    r0.x = r0_x; r0.w = r0_w;
  }
  r1.x = c[255].z + ps;
  r1.z = r0.w * c[1].x;
  r1.w = sqrt(abs(r0.x));
  r1.x = r1.w * (-c[1].z) + r1.x;
  r0.x = r1.z * r0.w + c[1].y;
  {
    float r0x_new = r0.x;
    float rX = r0.z * r0x_new + c[254].y;
    float rY = r0.y * r0x_new + c[254].y;
    r0.x = rX; r0.y = rY;
  }
  r0.xyz = t0.SampleLevel(s_lin, r0.xy, 0).xyz;
  r1.xyz = t2.SampleLevel(s_lin, r1.xy, 0).xyz;
  r1.xyz = saturate(r1.xyz + c[0].w);
  r1.xyz = r1.xyz + c[254].x;
  r1.xyz = r1.xyz * c[2].y + c[255].z;
  float4 oC0;
  oC0.xyz = r1.xyz * r0.xyz;
  oC0.w = 1.0;
  return oC0;
}
