// D3D11.3 17.1 gives the 16 bitwise truth tables; 17.7.1: "Logic ops are
// supported only on renderable UINT formats." 17.7.2 requires one operation
// from slot 0, IndependentBlendEnable=false. input bits above each component's
// width are zero because 17.7.1 allows either clamping or truncation there.
// masks only gate writes (17.15).
#pragma once
#include <d3d11_1.h>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <climits>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include "../depth_formats.hpp"

namespace om {
struct LogicFormat {
  DXGI_FORMAT value;
  std::array<UINT, 4> bits{};
  UINT channels = 0, stride = 0;
};
inline std::vector<LogicFormat> logic_formats() {
  const std::pair<DXGI_FORMAT, const char *> values[] = {DXGI_FORMATS};
  std::vector<LogicFormat> result;
  for (auto &[format, spelling] : values) {
    const char *p = spelling + strlen("DXGI_FORMAT_");
    if (*p != 'R' || strstr(p, "TYPELESS") ||
        std::string(p).ends_with("_UINT") == false)
      continue;
    LogicFormat f{format};
    UINT total = 0;
    while (*p && *p != '_' && f.channels < f.bits.size()) {
      if (!strchr("RGBA", *p++))
        break;
      char *end;
      unsigned long bits = strtoul(p, &end, 10);
      if (end == p || !bits || bits > sizeof(UINT) * CHAR_BIT)
        break;
      f.bits[f.channels++] = UINT(bits);
      total += UINT(bits);
      p = end;
    }
    if (!strcmp(p, "_UINT") && total % CHAR_BIT == 0) {
      f.stride = total / CHAR_BIT;
      result.push_back(f);
    }
  }
  return result;
}
inline UINT component_mask(UINT bits) {
  return UINT((uint64_t(1) << bits) - 1);
}
inline UINT logic(D3D11_LOGIC_OP op, UINT s, UINT d) {
  switch (op) {
  case D3D11_LOGIC_OP_CLEAR:
    return 0;
  case D3D11_LOGIC_OP_SET:
    return ~0u;
  case D3D11_LOGIC_OP_COPY:
    return s;
  case D3D11_LOGIC_OP_COPY_INVERTED:
    return ~s;
  case D3D11_LOGIC_OP_NOOP:
    return d;
  case D3D11_LOGIC_OP_INVERT:
    return ~d;
  case D3D11_LOGIC_OP_AND:
    return s & d;
  case D3D11_LOGIC_OP_NAND:
    return ~(s & d);
  case D3D11_LOGIC_OP_OR:
    return s | d;
  case D3D11_LOGIC_OP_NOR:
    return ~(s | d);
  case D3D11_LOGIC_OP_XOR:
    return s ^ d;
  case D3D11_LOGIC_OP_EQUIV:
    return ~(s ^ d);
  case D3D11_LOGIC_OP_AND_REVERSE:
    return s & ~d;
  case D3D11_LOGIC_OP_AND_INVERTED:
    return ~s & d;
  case D3D11_LOGIC_OP_OR_REVERSE:
    return s | ~d;
  case D3D11_LOGIC_OP_OR_INVERTED:
    return ~s | d;
  }
  std::abort();
}
inline constexpr UINT logic_patterns[] = {0,   ~0u,        0,
                                          ~0u, 0x55555555, 0xaaaaaaaa};
inline UINT logic_source(LogicFormat f, UINT row, UINT c) {
  return logic_patterns[row] & component_mask(f.bits[c]);
}
inline UINT logic_destination(LogicFormat f, UINT row, UINT slot, UINT c) {
  const UINT values[] = {0, 0, ~0u, ~0u, 0xaaaaaaaa, 0x55555555};
  return values[(row + slot + c) % std::size(values)] &
         component_mask(f.bits[c]);
}
inline void pack(LogicFormat f, const std::array<UINT, 4> &values,
                 unsigned char *p) {
  memset(p, 0, f.stride);
  UINT offset = 0;
  for (UINT c = 0; c < f.channels; c++)
    for (UINT bit = 0; bit < f.bits[c]; bit++, offset++)
      p[offset / CHAR_BIT] |= ((values[c] >> bit) & 1u) << (offset % CHAR_BIT);
}
inline UINT unpack(LogicFormat f, const unsigned char *p, UINT channel) {
  UINT offset = 0, value = 0;
  for (UINT c = 0; c < channel; c++)
    offset += f.bits[c];
  for (UINT bit = 0; bit < f.bits[channel]; bit++, offset++)
    value |= UINT((p[offset / CHAR_BIT] >> (offset % CHAR_BIT)) & 1u) << bit;
  return value;
}
inline constexpr UINT logic_masks = D3D11_COLOR_WRITE_ENABLE_ALL + 1;
inline constexpr UINT logic_width =
    (D3D11_LOGIC_OP_OR_INVERTED + 1) * logic_masks + 2;
inline std::vector<unsigned char> logic_seed(LogicFormat f, UINT slot) {
  std::vector<unsigned char> bytes(logic_width * std::size(logic_patterns) *
                                   f.stride);
  for (UINT y = 0; y < std::size(logic_patterns); y++)
    for (UINT x = 0; x < logic_width; x++) {
      std::array<UINT, 4> values{};
      for (UINT c = 0; c < f.channels; c++)
        values[c] = logic_destination(f, y, slot, c);
      pack(f, values, bytes.data() + (y * logic_width + x) * f.stride);
    }
  return bytes;
}
inline bool check_logic(LogicFormat f, UINT slot, const unsigned char *bytes,
                        UINT pitch) {
  for (UINT y = 0; y < std::size(logic_patterns); y++)
    for (UINT x = 0; x < logic_width; x++)
      for (UINT c = 0; c < f.channels; c++) {
        UINT want = logic_destination(f, y, slot, c);
        if (x && x + 1 < logic_width) {
          UINT index = x - 1, mask = index % logic_masks;
          if (mask & (1u << c))
            want = logic(D3D11_LOGIC_OP(index / logic_masks),
                         logic_source(f, y, c), want) &
                   component_mask(f.bits[c]);
        }
        UINT got = unpack(f, bytes + y * pitch + x * f.stride, c);
        if (!expect(got == want,
                    "slot %u, pixel %u,%u, channel %u: %#x, want %#x", slot, x,
                    y, c, got, want))
          return false;
      }
  return true;
}
inline constexpr char logic_hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint4 value; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv=float2((id << 1)&2, id&2); return float4(uv*float2(2,-2)+float2(-1,1),0,1);
}
struct Mrt { uint4 a:SV_Target0; uint4 b:SV_Target1; uint4 c:SV_Target2; uint4 d:SV_Target3;
             uint4 e:SV_Target4; uint4 f:SV_Target5; uint4 g:SV_Target6; uint4 h:SV_Target7; };
Mrt ps() { Mrt o; o.a=o.b=o.c=o.d=o.e=o.f=o.g=o.h=value; return o; }
)hlsl";
} // namespace om
