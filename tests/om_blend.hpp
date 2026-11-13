// the CPU oracle follows D3D11.3 17.1: "The min and max blend ops ignore
// D3D11_BLEND modes"; 17.15: "The masks simply limit writes." 3.2.3.5-8 define
// normalized and sRGB conversion. source:
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
#pragma once
#include <d3d11_1.h>
#include <d3d12.h>
#include <d3d10.h>
#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <climits>
#include <cstring>
#include <limits>
#include <vector>

namespace om {
using Color = std::array<float, 4>;
struct BlendCase {
  D3D11_BLEND_DESC state{};
  Color source{0.125f, 0.75f, 0.25f, 0.5f},
      secondary{0.625f, 0.125f, 0.375f, 0.75f};
  Color destination{0.75f, 0.25f, 0.5f, 0.125f},
      factor{0.25f, 0.5f, 0.75f, 0.125f};
  bool dual = false, null_factor = false, special = false;
};
inline constexpr D3D11_BLEND factors[] = {D3D11_BLEND_ZERO,
                                          D3D11_BLEND_ONE,
                                          D3D11_BLEND_SRC_COLOR,
                                          D3D11_BLEND_INV_SRC_COLOR,
                                          D3D11_BLEND_SRC_ALPHA,
                                          D3D11_BLEND_INV_SRC_ALPHA,
                                          D3D11_BLEND_DEST_ALPHA,
                                          D3D11_BLEND_INV_DEST_ALPHA,
                                          D3D11_BLEND_DEST_COLOR,
                                          D3D11_BLEND_INV_DEST_COLOR,
                                          D3D11_BLEND_SRC_ALPHA_SAT,
                                          D3D11_BLEND_BLEND_FACTOR,
                                          D3D11_BLEND_INV_BLEND_FACTOR,
                                          D3D11_BLEND_SRC1_COLOR,
                                          D3D11_BLEND_INV_SRC1_COLOR,
                                          D3D11_BLEND_SRC1_ALPHA,
                                          D3D11_BLEND_INV_SRC1_ALPHA};
inline bool alpha_factor(D3D11_BLEND f) {
  return f != D3D11_BLEND_SRC_COLOR && f != D3D11_BLEND_INV_SRC_COLOR &&
         f != D3D11_BLEND_DEST_COLOR && f != D3D11_BLEND_INV_DEST_COLOR &&
         f != D3D11_BLEND_SRC1_COLOR && f != D3D11_BLEND_INV_SRC1_COLOR;
}
inline BlendCase baseline() {
  BlendCase c;
  for (auto &r : c.state.RenderTarget)
    r = {TRUE,
         D3D11_BLEND_ONE,
         D3D11_BLEND_ONE,
         D3D11_BLEND_OP_ADD,
         D3D11_BLEND_ONE,
         D3D11_BLEND_ONE,
         D3D11_BLEND_OP_ADD,
         D3D11_COLOR_WRITE_ENABLE_ALL};
  return c;
}
inline std::vector<BlendCase> blend_cases() {
  std::vector<BlendCase> cases;
  for (auto f : factors)
    for (UINT role = 0; role < 4; role++) {
      if (role >= 2 && !alpha_factor(f))
        continue;
      bool dual = f >= D3D11_BLEND_SRC1_COLOR;
      for (UINT op = D3D11_BLEND_OP_ADD;
           op <= UINT(dual ? D3D11_BLEND_OP_REV_SUBTRACT : D3D11_BLEND_OP_MAX);
           op++) {
        auto c = baseline();
        c.dual = dual;
        auto &r = c.state.RenderTarget[0];
        if (role == 0)
          r.SrcBlend = f;
        if (role == 1)
          r.DestBlend = f;
        if (role == 2)
          r.SrcBlendAlpha = f;
        if (role == 3)
          r.DestBlendAlpha = f;
        r.BlendOp = D3D11_BLEND_OP(op);
        r.BlendOpAlpha = D3D11_BLEND_OP(
            dual ? op : D3D11_BLEND_OP_MAX + D3D11_BLEND_OP_ADD - op);
        cases.push_back(c);
      }
    }
  for (bool alpha : {false, true})
    for (auto src : factors)
      for (auto dst : factors) {
        if (alpha && (!alpha_factor(src) || !alpha_factor(dst)))
          continue;
        bool dual =
            src >= D3D11_BLEND_SRC1_COLOR || dst >= D3D11_BLEND_SRC1_COLOR;
        for (UINT op = D3D11_BLEND_OP_ADD;
             op <=
             UINT(dual ? D3D11_BLEND_OP_REV_SUBTRACT : D3D11_BLEND_OP_MAX);
             op++) {
          auto c = baseline();
          c.dual = dual;
          auto &r = c.state.RenderTarget[0];
          if (alpha) {
            r.SrcBlendAlpha = src;
            r.DestBlendAlpha = dst;
            r.BlendOpAlpha = D3D11_BLEND_OP(op);
          } else {
            r.SrcBlend = src;
            r.DestBlend = dst;
            r.BlendOp = D3D11_BLEND_OP(op);
          }
          cases.push_back(c);
        }
      }
  for (bool independent : {false, true})
    for (UINT mask = 0; mask <= D3D11_COLOR_WRITE_ENABLE_ALL; mask++) {
      auto c = baseline();
      c.state.IndependentBlendEnable = independent;
      for (UINT slot = 0; slot < std::size(c.state.RenderTarget); slot++) {
        auto &r = c.state.RenderTarget[slot];
        r.RenderTargetWriteMask =
            UINT8((mask + slot) & D3D11_COLOR_WRITE_ENABLE_ALL);
        r.BlendEnable = slot % 2 == 0;
        r.SrcBlend =
            slot % 4 < 2 ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
        r.DestBlend =
            slot % 4 < 2 ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_BLEND_FACTOR;
        r.SrcBlendAlpha =
            slot % 4 < 2 ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_SRC_ALPHA;
        r.DestBlendAlpha = slot % 4 < 2 ? D3D11_BLEND_INV_DEST_ALPHA
                                        : D3D11_BLEND_INV_BLEND_FACTOR;
        r.BlendOp =
            D3D11_BLEND_OP(D3D11_BLEND_OP_ADD + slot % D3D11_BLEND_OP_MAX);
        r.BlendOpAlpha =
            D3D11_BLEND_OP(D3D11_BLEND_OP_MAX - slot % D3D11_BLEND_OP_MAX);
      }
      cases.push_back(c);
    }
  for (bool inverse : {false, true})
    for (bool null_factor : {false, true}) {
      auto c = baseline();
      c.null_factor = null_factor;
      auto &r = c.state.RenderTarget[0];
      r.SrcBlend = r.SrcBlendAlpha =
          inverse ? D3D11_BLEND_INV_BLEND_FACTOR : D3D11_BLEND_BLEND_FACTOR;
      r.DestBlend = r.DestBlendAlpha = D3D11_BLEND_ZERO;
      cases.push_back(c);
    }
  for (auto f : {D3D11_BLEND_INV_SRC_COLOR, D3D11_BLEND_BLEND_FACTOR,
                 D3D11_BLEND_INV_BLEND_FACTOR}) {
    auto c = baseline();
    c.source = {-1, 2, -0.5f, 1.5f};
    c.factor = {-2, 3, -1, 2};
    c.state.RenderTarget[0].SrcBlend = f;
    cases.push_back(c);
  }
  for (float v : {std::numeric_limits<float>::quiet_NaN(),
                  std::numeric_limits<float>::infinity(),
                  -std::numeric_limits<float>::infinity(),
                  std::numeric_limits<float>::denorm_min(),
                  -std::numeric_limits<float>::denorm_min(), -0.0f}) {
    auto c = baseline();
    c.special = true;
    c.source.fill(v);
    c.destination.fill(-0.0f);
    c.state.RenderTarget[0].SrcBlend = c.state.RenderTarget[0].SrcBlendAlpha =
        D3D11_BLEND_ZERO;
    cases.push_back(c);
  }
  return cases;
}
inline float weight(D3D11_BLEND f, UINT channel, const Color &s, const Color &d,
                    const Color &s1, const Color &b) {
  switch (UINT(f)) {
  case D3D11_BLEND_ZERO:
    return 0;
  case D3D11_BLEND_ONE:
    return 1;
  case D3D11_BLEND_SRC_COLOR:
    return s[channel];
  case D3D11_BLEND_INV_SRC_COLOR:
    return 1 - s[channel];
  case D3D11_BLEND_SRC_ALPHA:
    return s[3];
  case D3D11_BLEND_INV_SRC_ALPHA:
    return 1 - s[3];
  case D3D11_BLEND_DEST_COLOR:
    return d[channel];
  case D3D11_BLEND_INV_DEST_COLOR:
    return 1 - d[channel];
  case D3D11_BLEND_DEST_ALPHA:
    return d[3];
  case D3D11_BLEND_INV_DEST_ALPHA:
    return 1 - d[3];
  case D3D11_BLEND_SRC_ALPHA_SAT:
    return channel == 3 ? 1 : std::min(s[3], 1 - d[3]);
  case D3D11_BLEND_BLEND_FACTOR:
    return b[channel];
  case D3D11_BLEND_INV_BLEND_FACTOR:
    return 1 - b[channel];
  case D3D11_BLEND(D3D12_BLEND_ALPHA_FACTOR):
    return b[3];
  case D3D11_BLEND(D3D12_BLEND_INV_ALPHA_FACTOR):
    return 1 - b[3];
  case D3D11_BLEND_SRC1_COLOR:
    return s1[channel];
  case D3D11_BLEND_INV_SRC1_COLOR:
    return 1 - s1[channel];
  case D3D11_BLEND_SRC1_ALPHA:
    return s1[3];
  case D3D11_BLEND_INV_SRC1_ALPHA:
    return 1 - s1[3];
  }
  std::abort();
}
inline Color blend(const D3D11_RENDER_TARGET_BLEND_DESC &r, Color s, Color d,
                   Color s1, Color b) {
  if (!r.BlendEnable)
    return s;
  Color result{};
  for (UINT c = 0; c < result.size(); c++) {
    auto op = c == 3 ? r.BlendOpAlpha : r.BlendOp;
    if (op == D3D11_BLEND_OP_MIN)
      result[c] = std::fmin(s[c], d[c]);
    else if (op == D3D11_BLEND_OP_MAX)
      result[c] = std::fmax(s[c], d[c]);
    else {
      float a =
          s[c] * weight(c == 3 ? r.SrcBlendAlpha : r.SrcBlend, c, s, d, s1, b);
      float z = d[c] *
                weight(c == 3 ? r.DestBlendAlpha : r.DestBlend, c, s, d, s1, b);
      result[c] = op == D3D11_BLEND_OP_ADD        ? a + z
                  : op == D3D11_BLEND_OP_SUBTRACT ? a - z
                                                  : z - a;
    }
  }
  return result;
}
inline float srgb_decode(float c) {
  return c <= D3D11_SRGB_TO_FLOAT_THRESHOLD
             ? c / D3D11_SRGB_TO_FLOAT_DENOMINATOR_1
             : std::pow((c + D3D11_SRGB_TO_FLOAT_OFFSET) /
                            D3D11_SRGB_TO_FLOAT_DENOMINATOR_2,
                        D3D11_SRGB_TO_FLOAT_EXPONENT);
}
inline float srgb_encode(float c) {
  return c <= D3D10_FLOAT_TO_SRGB_THRESHOLD
             ? D3D10_FLOAT_TO_SRGB_SCALE_1 * c
             : D3D10_FLOAT_TO_SRGB_SCALE_2 *
                       std::pow(c,
                                D3D10_FLOAT_TO_SRGB_EXPONENT_NUMERATOR /
                                    D3D10_FLOAT_TO_SRGB_EXPONENT_DENOMINATOR) -
                   D3D10_FLOAT_TO_SRGB_OFFSET;
}
// float16 fields are defined by D3D11.3 3.1.5; the test's seed values are
// exactly representable.
inline constexpr unsigned fraction_bits = 10, exponent_bits = 5,
                          bias = (1u << (exponent_bits - 1)) - 1;
inline float half(uint16_t bits) {
  unsigned fraction = bits & ((1u << fraction_bits) - 1),
           exponent = (bits >> fraction_bits) & ((1u << exponent_bits) - 1);
  float v =
      exponent == (1u << exponent_bits) - 1
          ? (fraction ? NAN : INFINITY)
          : std::ldexp(float(fraction + (exponent ? 1u << fraction_bits : 0)),
                       int(exponent ? exponent : 1) - int(bias) -
                           int(fraction_bits));
  return bits >> (fraction_bits + exponent_bits) ? -v : v;
}
inline uint16_t half_bits(float value) {
  unsigned sign =
      std::signbit(value) ? 1u << (fraction_bits + exponent_bits) : 0;
  if (value == 0)
    return sign;
  int exponent;
  float mantissa = std::frexp(std::abs(value), &exponent);
  return uint16_t(sign | ((exponent - 1 + bias) << fraction_bits) |
                  unsigned((mantissa * 2 - 1) * (1u << fraction_bits)));
}
struct Format {
  DXGI_FORMAT value;
  bool normalized, signed_normalized, srgb;
  UINT bits;
};
inline constexpr Format formats[] = {
    {DXGI_FORMAT_R32G32B32A32_FLOAT, false, false, false, 32},
    {DXGI_FORMAT_R16G16B16A16_FLOAT, false, false, false, 16},
    {DXGI_FORMAT_R8G8B8A8_UNORM, true, false, false, 8},
    {DXGI_FORMAT_R8G8B8A8_SNORM, true, true, false, 8},
    {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, true, false, true, 8}};
inline float clamp(Format f, float v) {
  if (!f.normalized)
    return std::fpclassify(v) == FP_SUBNORMAL ? std::copysign(0.0f, v) : v;
  return std::isnan(v)
             ? 0
             : std::clamp(v, f.signed_normalized ? -1.0f : 0.0f, 1.0f);
}
inline Color source(const BlendCase &k, UINT slot) {
  Color s = k.source;
  if (!k.special)
    for (auto &v : s)
      v *= float(slot + 1) / D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
  return s;
}
inline void seed(Format f, const Color &d, unsigned char *p) {
  for (UINT c = 0; c < d.size(); c++) {
    float v = clamp(f, d[c]);
    if (!f.normalized && f.bits == 32)
      memcpy(p + c * sizeof(v), &v, sizeof(v));
    else if (!f.normalized) {
      auto h = half_bits(v);
      memcpy(p + c * sizeof(h), &h, sizeof(h));
    } else {
      if (f.srgb && c != 3)
        v = srgb_encode(v);
      p[c] = uint8_t(
          int(std::round(v * (f.signed_normalized ? INT8_MAX : UINT8_MAX))));
    }
  }
}
inline Color decode(Format f, const unsigned char *p) {
  Color d{};
  for (UINT c = 0; c < d.size(); c++) {
    if (!f.normalized && f.bits == 32)
      memcpy(&d[c], p + c * sizeof(float), sizeof(float));
    else if (!f.normalized) {
      uint16_t h;
      memcpy(&h, p + c * sizeof(h), sizeof(h));
      d[c] = half(h);
    } else {
      d[c] = f.signed_normalized
                 ? std::max(-1.0f, float(int8_t(p[c])) / INT8_MAX)
                 : float(p[c]) / UINT8_MAX;
      if (f.srgb && c != 3)
        d[c] = srgb_decode(d[c]);
    }
  }
  return d;
}
inline bool compare(Format f, const BlendCase &k, UINT slot,
                    const unsigned char *initial, const unsigned char *got) {
  Color s = source(k, slot), d = decode(f, initial), s1 = k.secondary,
        b = k.factor;
  if (k.null_factor)
    b.fill(1);
  for (auto *v : {&s, &d, &s1, &b})
    for (auto &c : *v)
      c = clamp(f, c);
  const auto &r =
      k.state.RenderTarget[k.state.IndependentBlendEnable ? slot : 0];
  Color want = blend(r, s, d, s1, b), actual = decode(f, got);
  bool ok = true;
  for (UINT c = 0; c < want.size(); c++) {
    if (!(r.RenderTargetWriteMask & (1u << c))) {
      ok &= expect(!memcmp(initial + c * f.bits / CHAR_BIT,
                           got + c * f.bits / CHAR_BIT, f.bits / CHAR_BIT),
                   "slot %u channel %u changed under a disabled write mask",
                   slot, c);
      continue;
    }
    if (!f.normalized) {
      bool equal =
          std::isnan(want[c]) ? std::isnan(actual[c]) : actual[c] == want[c];
      // finite fused float16 arithmetic may keep extra precision (17.5); seed
      // arithmetic is dyadic.
      if (f.bits == 16 && std::isfinite(want[c]) && std::isfinite(actual[c])) {
        int exponent;
        std::frexp(std::abs(want[c]), &exponent);
        float ulp =
            std::ldexp(1.0f, std::max(1 - int(bias) - int(fraction_bits),
                                      exponent - 1 - int(fraction_bits)));
        equal = std::abs(actual[c] - want[c]) <=
                3 * D3D10_FLOAT16_FUSED_TOLERANCE_IN_ULP * ulp;
      }
      if (want[c] == 0 && actual[c] == 0)
        equal &= std::signbit(want[c]) == std::signbit(actual[c]);
      ok &= expect(equal, "slot %u channel %u got %.9g, want %.9g", slot, c,
                   actual[c], want[c]);
    } else {
      float v = clamp(f, want[c]);
      if (f.srgb && c != 3)
        v = srgb_encode(v);
      float scaled = v * (f.signed_normalized ? INT8_MAX : UINT8_MAX);
      int value = f.signed_normalized ? int(int8_t(got[c])) : got[c];
      // 17.5 permits fixed-point blend precision; three arithmetic terms plus
      // output conversion.
      float tolerance =
          3 * D3D10_FLOAT32_TO_INTEGER_TOLERANCE_IN_ULP +
          (f.srgb && c != 3 ? D3D11_SRGB_TO_FLOAT_TOLERANCE_IN_ULP : 0);
      ok &= expect(std::abs(value - scaled) <= tolerance,
                   "slot %u channel %u got %d, want %.9g", slot, c, value,
                   scaled);
    }
  }
  return ok;
}
inline constexpr char blend_hlsl[] = R"hlsl(
cbuffer C : register(b0) { float4 primary; float4 secondary; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
struct Mrt { float4 a : SV_Target0; float4 b : SV_Target1; float4 c : SV_Target2; float4 d : SV_Target3;
             float4 e : SV_Target4; float4 f : SV_Target5; float4 g : SV_Target6; float4 h : SV_Target7; };
Mrt ps() {
  Mrt o;
  o.a=primary*(1.0/TARGETS); o.b=primary*(2.0/TARGETS); o.c=primary*(3.0/TARGETS); o.d=primary*(4.0/TARGETS);
  o.e=primary*(5.0/TARGETS); o.f=primary*(6.0/TARGETS); o.g=primary*(7.0/TARGETS); o.h=primary;
  return o;
}
struct Dual { float4 a : SV_Target0; float4 b : SV_Target1; };
Dual ps_dual() { Dual o; o.a=primary/TARGETS; o.b=secondary; return o; }
Mrt ps_special() { Mrt o; o.a=o.b=o.c=o.d=o.e=o.f=o.g=o.h=primary; return o; }
)hlsl";
} // namespace om
