// D3D11.3 16.12.1: "nothing downstream further impacts the count."
// 16.12.2 requires UAV shaders to run before the default depth/stencil test.
// 16.13: "After the discard is issued, further operations on UAVs do not
// change the UAV memory". the two counters straddle discard; depth, stencil,
// target bytes and occlusion distinguish early tests from late tests.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
#pragma once
#include <d3d11.h>
#include <array>
#include <bit>
#include <vector>

namespace om {
inline constexpr UINT order_width = 6, order_color = 19, order_stencil = 3,
                      order_reference = 7;
inline constexpr float order_stored_depth = 0.5f;
struct OrderCase {
  UINT shader, discard;
  float raster_depth;
  bool depth_enabled, depth_write, stencil_fail;
  bool early() const { return shader & 1; }
  bool output_depth() const { return shader & 2; }
};
inline constexpr const char *order_entries[] = {"ps", "ps_early", "ps_depth",
                                                "ps_early_depth"};
inline std::vector<OrderCase> order_cases() {
  std::vector<OrderCase> cases;
  for (UINT shader = 0; shader < std::size(order_entries); shader++)
    for (UINT discard = 0; discard < 3; discard++)
      for (float depth : {0.25f, 0.75f})
        for (bool enable : {false, true})
          for (bool write : {false, true})
            for (bool stencil_fail : {false, true})
              cases.push_back(
                  {shader, discard, depth, enable, write, stencil_fail});
  return cases;
}
inline D3D11_DEPTH_STENCIL_DESC order_state(const OrderCase &k) {
  D3D11_DEPTH_STENCIL_DESC s{k.depth_enabled,
                             k.depth_write ? D3D11_DEPTH_WRITE_MASK_ALL
                                           : D3D11_DEPTH_WRITE_MASK_ZERO,
                             D3D11_COMPARISON_LESS,
                             TRUE,
                             D3D11_DEFAULT_STENCIL_READ_MASK,
                             D3D11_DEFAULT_STENCIL_WRITE_MASK};
  s.FrontFace = s.BackFace = {
      D3D11_STENCIL_OP_INCR, D3D11_STENCIL_OP_DECR, D3D11_STENCIL_OP_REPLACE,
      k.stencil_fail ? D3D11_COMPARISON_NEVER : D3D11_COMPARISON_ALWAYS};
  return s;
}
struct OrderResult {
  UINT color = 0, before = 0, after = 0, occlusion = 0;
  float depth = order_stored_depth;
  UINT8 stencil = order_stencil;
};
inline OrderResult order_result(const OrderCase &k, UINT x) {
  OrderResult r;
  if (!x || x + 1 == order_width)
    return r;
  bool discarded = k.discard == 2 || (k.discard == 1 && (x & 1));
  float depth =
      k.output_depth() && !k.early() ? 1 - k.raster_depth : k.raster_depth;
  bool depth_pass = !k.depth_enabled || depth < order_stored_depth;
  bool passed = !k.stencil_fail && depth_pass;
  r.before = !k.early() || passed;
  r.after = r.before && !discarded;
  r.occlusion = passed && (k.early() || !discarded);
  r.color = passed && !discarded ? order_color : 0;
  if (k.early() || !discarded) {
    r.stencil = k.stencil_fail ? order_stencil + 1
                : !depth_pass  ? order_stencil - 1
                               : order_reference;
    if (passed && k.depth_enabled && k.depth_write)
      r.depth = depth;
  }
  return r;
}
static const char order_hlsl[] = R"hlsl(
cbuffer C : register(b0) { float raster_depth; float output_depth; uint discard_mode; uint width; };
RWStructuredBuffer<uint> counters : register(u1);
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), raster_depth, 1);
}
uint shade(float4 pos) {
  uint index = 2 * (uint(pos.y) * width + uint(pos.x));
  InterlockedAdd(counters[index], 1);
  if (discard_mode == 2 || (discard_mode == 1 && (uint(pos.x) & 1))) discard;
  InterlockedAdd(counters[index + 1], 1);
  return COLOR_VALUE;
}
uint ps(float4 pos : SV_Position) : SV_Target { return shade(pos); }
[earlydepthstencil] uint ps_early(float4 pos : SV_Position) : SV_Target { return shade(pos); }
uint ps_depth(float4 pos : SV_Position, out float z : SV_Depth) : SV_Target {
  z = output_depth; return shade(pos);
}
[earlydepthstencil] uint ps_early_depth(float4 pos : SV_Position, out float z : SV_Depth) : SV_Target {
  z = output_depth; return shade(pos);
}
)hlsl";
} // namespace om
