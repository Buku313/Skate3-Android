// Fullscreen MSAA resolve: the deferred command list has no
// ResolveSubresource, so average the samples in a pixel shader (SAMPLES is
// injected as a compile-time macro).
Texture2DMS<float4> src : register(t0);
float4 vs_main(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps_main(float4 pos : SV_Position) : SV_Target {
  int2 p = int2(pos.xy);
  float4 c = 0;
  [unroll] for (int k = 0; k < SAMPLES; ++k) {
    c += src.Load(p, k);
  }
  return c / SAMPLES;
}
