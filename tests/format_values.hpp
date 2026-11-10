// reference values follow D3D11.3 3.1.5/3.1.6, 3.2.3, 19.1.3 and 19.3.2.1. the
// SDK spelling supplies the layout.
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cmath>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace format_values {
using Pixel = std::array<uint32_t, 4>;
enum Kind { Floating, Unsigned, Signed, Unorm, Snorm, Srgb, Shared };
struct Component {
  unsigned channel, bits, offset;
};
struct Format {
  DXGI_FORMAT id;
  std::string name;
  Kind kind;
  std::vector<Component> components;
  unsigned bytes;
  unsigned
  shader_kind() const {
    return kind == Unsigned ? 1 : kind == Signed ? 2 : 0;
  }
};

inline std::vector<Format>
formats() {
  const struct {
    DXGI_FORMAT id;
    const char *name;
  } sdk[] = {DXGI_FORMATS,
             // the Agility SDK's, past the toolchain's dxgiformat.h
             {DXGI_FORMAT(191), "DXGI_FORMAT_A4B4G4R4_UNORM"}};
  std::vector<Format> result;
  for (auto [id, name] : sdk) {
    std::string spelling = name + std::string("DXGI_FORMAT_").size();
    auto suffix = spelling.find('_');
    if (suffix == std::string::npos)
      continue;
    std::string type = spelling.substr(suffix + 1);
    Kind kind;
    if (type == "FLOAT")
      kind = Floating;
    else if (type == "UINT")
      kind = Unsigned;
    else if (type == "SINT")
      kind = Signed;
    else if (type == "UNORM")
      kind = Unorm;
    else if (type == "SNORM")
      kind = Snorm;
    else if (type == "UNORM_SRGB")
      kind = Srgb;
    else if (type == "SHAREDEXP")
      kind = Shared;
    else
      continue;
    Format f{id, name, kind, {}, 0};
    const char *at = spelling.c_str();
    unsigned offset = 0;
    constexpr char channels[] = "RGBAXE";
    while (*at && *at != '_') {
      const char *channel = strchr(channels, *at++);
      if (!channel || !isdigit(static_cast<unsigned char>(*at)))
        break;
      char *end;
      unsigned bits = strtoul(at, &end, 10);
      f.components.push_back({unsigned(channel - channels), bits, offset});
      offset += bits;
      at = end;
    }
    if (*at != '_' || !offset || offset % CHAR_BIT)
      continue;
    f.bytes = offset / CHAR_BIT;
    result.push_back(f);
  }
  return result;
}

inline uint32_t
mask(unsigned bits) {
  return uint32_t((uint64_t(1) << bits) - 1);
}

inline bool
required_vertex(const Format &f) {
  return f.id == DXGI_FORMAT_R10G10B10A2_UNORM ||
         (f.components.front().channel == 0 && f.kind != Srgb && f.kind != Shared &&
          std::all_of(f.components.begin(), f.components.end(), [](auto &c) {
            return c.channel < 4 && c.bits >= CHAR_BIT && std::has_single_bit(c.bits);
          }));
}

inline uint32_t
field(const uint8_t *bytes, const Component &c) {
  uint32_t value = 0;
  for (unsigned b = 0; b < c.bits; b++)
    value |= uint32_t((bytes[(c.offset + b) / CHAR_BIT] >> ((c.offset + b) % CHAR_BIT)) & 1) << b;
  return value;
}

inline float
small_float(uint32_t raw, unsigned bits) {
  unsigned fraction = bits == 16 ? 10 : bits - 5;
  unsigned exponent = raw >> fraction & mask(5), mantissa = raw & mask(fraction);
  float value = exponent == mask(5) ? (mantissa ? NAN : INFINITY)
                : exponent == 0     ? std::ldexp(float(mantissa), 1 - 15 - int(fraction))
                                : std::ldexp(float((1u << fraction) | mantissa), int(exponent) - 15 - int(fraction));
  return bits == 16 && (raw >> (bits - 1)) ? -value : value;
}

inline double
linear(double c) {
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
inline double
srgb(double c) {
  return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

inline Pixel
decode(const Format &f, const uint8_t *bytes) {
  Pixel p{0, 0, 0, f.shader_kind() ? 1u : std::bit_cast<uint32_t>(1.0f)};
  for (auto &c : f.components) {
    if (c.channel >= p.size())
      continue;
    uint32_t raw = field(bytes, c);
    int64_t integer = raw & (1u << (c.bits - 1)) ? int64_t(raw) - (int64_t(1) << c.bits) : raw;
    if (f.kind == Unsigned || f.kind == Signed) {
      p[c.channel] = f.kind == Signed ? uint32_t(integer) : raw;
      continue;
    }
    float value;
    if (f.kind == Shared)
      value = std::ldexp(
          float(raw), int(field(bytes, f.components.back())) - int(mask(f.components.back().bits - 1)) - int(c.bits)
      );
    else if (f.kind == Floating)
      value = c.bits == 32 ? std::bit_cast<float>(raw) : small_float(raw, c.bits);
    else if (f.kind == Snorm)
      value = float(std::max(-1.0, double(integer) / mask(c.bits - 1)));
    else {
      double number = double(raw) / mask(c.bits);
      value = float(f.kind == Srgb && c.channel != 3 ? linear(number) : number);
    }
    p[c.channel] = std::bit_cast<uint32_t>(value);
  }
  return p;
}

inline std::vector<uint8_t>
inputs(const Format &f) {
  unsigned widest = 0;
  for (auto &c : f.components)
    widest = std::max(widest, c.bits);
  std::vector<uint32_t> words;
  if (widest == 32) {
    for (unsigned bit = 0; bit < 32; bit++)
      for (uint32_t v : {(1u << bit) - 1, 1u << bit, (1u << bit) + 1}) {
        words.push_back(v);
        words.push_back(~v);
      }
    if (f.kind == Floating)
      for (unsigned sign = 0; sign <= 1; sign++)
        for (unsigned exponent = 0; exponent <= mask(8); exponent++)
          for (uint32_t mantissa : {0u, 1u, mask(23) / 2, mask(23)})
            words.push_back(sign << 31 | exponent << 23 | mantissa);
    std::sort(words.begin(), words.end());
    words.erase(std::unique(words.begin(), words.end()), words.end());
  }
  const unsigned count = widest == 32                  ? unsigned(words.size())
                         : f.kind == Shared            ? 1u << (f.components.front().bits + f.components.back().bits)
                         : f.bytes == sizeof(uint16_t) ? 1u << (sizeof(uint16_t) * CHAR_BIT)
                                                       : 1u << widest;
  std::vector<uint8_t> bytes(count * f.bytes);
  for (unsigned i = 0; i < count; i++) {
    if (f.bytes == sizeof(uint16_t)) {
      uint16_t raw = uint16_t(i);
      memcpy(bytes.data() + i * f.bytes, &raw, sizeof(raw));
      continue;
    }
    for (unsigned index = 0; index < f.components.size(); index++) {
      auto &c = f.components[index];
      uint32_t raw = widest == 32 ? words[(i + index * (words.size() / f.components.size())) % words.size()]
                     : f.kind == Shared && c.channel == 5 ? i >> f.components.front().bits
                                                          : (i * (2 * index + 1) + index) & mask(c.bits);
      for (unsigned b = 0; b < c.bits; b++)
        bytes[i * f.bytes + (c.offset + b) / CHAR_BIT] |= ((raw >> b) & 1) << ((c.offset + b) % CHAR_BIT);
    }
  }
  return bytes;
}

// zero-weight NaNs and signed zeros may propagate through filtering (7.18.16.2).
inline std::vector<uint8_t>
linear_inputs(const Format &f, const std::vector<uint8_t> &bytes) {
  auto result = bytes;
  if (f.kind == Floating)
    for (size_t i = 0; i < result.size(); i += f.bytes)
      for (const auto &c : f.components) {
        uint32_t raw = field(result.data() + i, c);
        float value = c.bits == 32 ? std::bit_cast<float>(raw) : small_float(raw, c.bits);
        if (std::isnormal(value))
          continue;
        const uint32_t one = c.bits == 32 ? std::bit_cast<uint32_t>(1.0f) : 15u << (c.bits == 16 ? 10 : c.bits - 5);
        for (unsigned b = 0; b < c.bits; b++) {
          auto &byte = result[i + (c.offset + b) / CHAR_BIT];
          byte = (byte & ~(1u << ((c.offset + b) % CHAR_BIT))) | (((one >> b) & 1u) << ((c.offset + b) % CHAR_BIT));
        }
      }
  return result;
}

inline bool
loaded(const Format &f, const uint8_t *bytes, const Pixel &have, bool sampled = false) {
  auto want = decode(f, bytes);
  for (unsigned c = 0; c < have.size(); c++) {
    if (have[c] == want[c])
      continue;
    if (f.shader_kind())
      return false;
    float a = std::bit_cast<float>(have[c]), b = std::bit_cast<float>(want[c]);
    if (std::isnan(a) && std::isnan(b) && (sampled || f.kind != Floating || f.components.front().bits != 32))
      continue;
    // point sampling may flush a 32-bit denormal; ld must preserve it (22.4.15).
    if (sampled && f.kind == Floating && std::fpclassify(b) == FP_SUBNORMAL && a == 0 &&
        std::signbit(a) == std::signbit(b))
      continue;
    if (f.kind == Srgb && c < 3 && a > 0 && a < 1 && b > 0 && b < 1) {
      auto component = std::find_if(f.components.begin(), f.components.end(), [&](auto &v) { return v.channel == c; });
      if (component != f.components.end() &&
          std::fabs(srgb(a) * mask(component->bits) - field(bytes, *component)) <= 0.5)
        continue;
    }
    return false;
  }
  return true;
}

inline bool
inverted(const Format &f, const uint8_t *source, const uint8_t *result, bool sampled) {
  for (const auto &c : f.components) {
    if (c.channel >= 4)
      continue;
    uint32_t from = field(source, c), to = field(result, c);
    if (f.kind == Snorm && from == (1u << (c.bits - 1))) {
      if (to != from + 1)
        return false;
      continue;
    }
    if (from == to)
      continue;
    if (f.kind == Floating) {
      float a = c.bits == 32 ? std::bit_cast<float>(from) : small_float(from, c.bits);
      float b = c.bits == 32 ? std::bit_cast<float>(to) : small_float(to, c.bits);
      if (std::isnan(a) && std::isnan(b))
        continue;
      if (sampled && c.bits == 32 && std::fpclassify(a) == FP_SUBNORMAL && b == 0 && std::signbit(a) == std::signbit(b))
        continue;
    }
    return false;
  }
  return true;
}

// normalized conversion allows 0.6 integer ULP (3.2.3.4/.6/.8), while
// invertibility is a separate requirement.
inline bool
stored(const Format &f, const Pixel &source, const uint8_t *bytes, bool nearest_even = false) {
  for (auto &c : f.components) {
    if (c.channel >= source.size())
      continue;
    uint32_t raw = field(bytes, c), value = source[c.channel];
    if (f.kind == Unsigned || f.kind == Signed) {
      int64_t v = f.kind == Signed ? std::bit_cast<int32_t>(value) : int64_t(value);
      int64_t lo = f.kind == Signed ? -(int64_t(1) << (c.bits - 1)) : 0;
      int64_t hi = f.kind == Signed ? mask(c.bits - 1) : mask(c.bits);
      if (raw != (uint32_t(std::clamp(v, lo, hi)) & mask(c.bits)))
        return false;
    } else if (f.kind == Floating) {
      float have = c.bits == 32 ? std::bit_cast<float>(raw) : small_float(raw, c.bits);
      float want = std::bit_cast<float>(value);
      if (c.bits != 32 && !std::isnan(want)) {
        const unsigned fraction = c.bits == 16 ? 10 : c.bits - 5;
        const bool negative = std::signbit(want);
        double magnitude = std::fabs(double(want));
        if (c.bits != 16 && negative)
          want = 0;
        else if (std::isfinite(want)) {
          double maximum = std::ldexp(double((1u << (fraction + 1)) - 1), 15 - int(fraction));
          magnitude = std::min(magnitude, maximum);
          int exponent;
          std::frexp(magnitude, &exponent);
          int shift = std::max(exponent - 1, 1 - 15) - int(fraction);
          auto scaled = std::ldexp(magnitude, -shift);
          want = float(std::ldexp(nearest_even ? std::nearbyint(scaled) : std::floor(scaled), shift));
          if (negative)
            want = -want;
        }
      }
      if (std::isnan(want) ? !std::isnan(have) : std::bit_cast<uint32_t>(have) != std::bit_cast<uint32_t>(want))
        return false;
    } else {
      double v = std::bit_cast<float>(value);
      if (std::isnan(v))
        v = 0;
      v = std::clamp(v, f.kind == Snorm ? -1.0 : 0.0, 1.0);
      if (f.kind == Srgb && c.channel != 3)
        v = srgb(v);
      v *= mask(c.bits - (f.kind == Snorm));
      int64_t have = f.kind == Snorm && (raw >> (c.bits - 1)) ? int64_t(raw) - (int64_t(1) << c.bits) : raw;
      if (std::fabs(double(have) - v) > 0.6)
        return false;
    }
  }
  return true;
}

// the tolerance alone permits decreasing choices around a midpoint; 3.2.3.4/.6/.8 separately forbid them.
inline bool
ordered(const Format &f, const std::vector<Pixel> &source, const uint8_t *bytes, unsigned width, unsigned pitch) {
  if (f.kind != Unorm && f.kind != Snorm && f.kind != Srgb)
    return true;
  for (const auto &c : f.components) {
    if (c.channel >= 4)
      continue;
    std::vector<std::pair<float, int64_t>> values;
    values.reserve(source.size());
    for (size_t i = 0; i < source.size(); i++) {
      float input = std::bit_cast<float>(source[i][c.channel]);
      if (std::isnan(input))
        continue;
      uint32_t raw = field(bytes + (i / width) * pitch + (i % width) * f.bytes, c);
      int64_t code = f.kind == Snorm && (raw >> (c.bits - 1)) ? int64_t(raw) - (int64_t(1) << c.bits) : raw;
      values.emplace_back(input, code);
    }
    std::sort(values.begin(), values.end());
    for (size_t i = 1; i < values.size(); i++)
      if (values[i].second < values[i - 1].second ||
          (values[i].first == values[i - 1].first && values[i].second != values[i - 1].second))
        return false;
  }
  return true;
}

inline std::vector<Pixel>
sources(const Format &f, const std::vector<uint8_t> &bytes) {
  std::vector<Pixel> result;
  for (size_t i = 0; i < bytes.size(); i += f.bytes)
    result.push_back(decode(f, bytes.data() + i));
  std::vector<uint32_t> edges = {
      0, std::bit_cast<uint32_t>(-0.0f), std::bit_cast<uint32_t>(INFINITY), std::bit_cast<uint32_t>(-INFINITY),
      std::bit_cast<uint32_t>(NAN)
  };
  for (float v :
       {std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::min(), std::numeric_limits<float>::max(),
        -1.0f, 0.0f, 1.0f, 0.0031308f})
    for (float neighbour : {std::nextafter(v, -INFINITY), v, std::nextafter(v, INFINITY)})
      edges.push_back(std::bit_cast<uint32_t>(neighbour));
  for (auto &c : f.components) {
    if (c.channel >= 4)
      continue;
    bool repeated = false;
    for (auto &previous : f.components) {
      if (&previous == &c)
        break;
      repeated |= previous.bits == c.bits && (f.kind != Srgb || (previous.channel == 3) == (c.channel == 3));
    }
    if (repeated)
      continue;
    if (f.shader_kind()) {
      for (uint32_t v :
           {0u, 1u, mask(c.bits - (f.kind == Signed)), mask(c.bits - (f.kind == Signed)) + 1, ~0u, 1u << 31})
        edges.push_back(v);
    } else if (f.kind == Unorm || f.kind == Snorm || f.kind == Srgb) {
      double scale = mask(c.bits - (f.kind == Snorm));
      for (unsigned code = 0; code <= scale; code++) {
        double v = (double(code) + 0.5) / scale;
        if (f.kind == Srgb && c.channel != 3)
          v = linear(v);
        float midpoint = float(v);
        for (float neighbour : {std::nextafter(midpoint, -INFINITY), midpoint, std::nextafter(midpoint, INFINITY)}) {
          edges.push_back(std::bit_cast<uint32_t>(neighbour));
          if (f.kind == Snorm)
            edges.push_back(std::bit_cast<uint32_t>(-neighbour));
        }
      }
    } else if (f.kind == Floating && c.bits < 32) {
      for (unsigned raw = 0; raw <= mask(c.bits); raw++) {
        float v = small_float(raw, c.bits);
        if (!std::isfinite(v))
          continue;
        for (float neighbour : {std::nextafter(v, -INFINITY), v, std::nextafter(v, INFINITY)})
          edges.push_back(std::bit_cast<uint32_t>(neighbour));
      }
    }
  }
  std::sort(edges.begin(), edges.end());
  edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
  for (size_t i = 0; i < edges.size(); i++) {
    Pixel p;
    for (unsigned c = 0; c < p.size(); c++)
      p[c] = edges[(i + c * edges.size() / p.size()) % edges.size()];
    result.push_back(p);
  }
  return result;
}

static const char vertices_hlsl[] = R"hlsl(
uint4 vs(TYPE value : DATA) : VALUE { return asuint(value); }
)hlsl";

static const char hlsl[] = R"hlsl(
cbuffer Shape : register(b0) { uint width; uint count; };
#if MODE == 0 || MODE == 8
Texture2D<TYPE> tex : register(t0);
#elif MODE == 1 || MODE == 9 || MODE == 10
Texture2D<float4> tex : register(t0);
#if MODE == 10
SamplerState nearest : register(s1);
#else
SamplerState nearest : register(s0);
#endif
#elif MODE == 2
Buffer<TYPE> tex : register(t0);
#elif MODE == 3 || MODE == 4
RWTexture2D<TYPE> tex : register(u1);
#elif MODE == 5 || MODE == 6
RWBuffer<TYPE> tex : register(u1);
#endif
StructuredBuffer<uint4> source : register(t1);
RWStructuredBuffer<uint4> result : register(u0);
TYPE unpack(uint4 v) { return UNPACK(v); }
[numthreads(GROUP_SIDE, GROUP_SIDE, 1)] void cs(uint2 p : SV_DispatchThreadID) {
  uint i = p.x + p.y * width;
  if (i >= count) return;
#if MODE == 0
  result[i] = asuint(tex.Load(int3(p,0)));
#elif MODE == 1
  uint high; uint wide; tex.GetDimensions(wide, high);
  result[i] = asuint(tex.SampleLevel(nearest,(float2(p)+0.5)/float2(wide,high),0));
#elif MODE == 2
  result[i] = asuint(tex.Load(i));
#elif MODE == 3
  result[i] = asuint(tex[p]);
#elif MODE == 4
  tex[p] = unpack(source[i]);
#elif MODE == 5
  result[i] = asuint(tex[i]);
#elif MODE == 6
  tex[i] = unpack(source[i]);
#endif
}
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id==2?3:-1,id==1?3:-1,0,1); }
TYPE ps(float4 position : SV_Position) : SV_Target {
#if MODE == 8
  return tex.Load(int3(uint2(position.xy),0));
#elif MODE == 9 || MODE == 10
  uint wide; uint high; tex.GetDimensions(wide,high);
  return tex.SampleLevel(nearest,position.xy/float2(wide,high),0);
#else
  uint i = uint(position.x) + uint(position.y) * width;
  return unpack(source[i]);
#endif
}
)hlsl";

inline std::vector<std::string>
defines(unsigned kind, unsigned mode) {
  return {
      "TYPE=" + std::string(
                    kind == 1   ? "uint4"
                    : kind == 2 ? "int4"
                                : "float4"
                ),
      "UNPACK=" + std::string(
                      kind == 1   ? "uint4"
                      : kind == 2 ? "asint"
                                  : "asfloat"
                  ),
      "MODE=" + std::to_string(mode), "GROUP_SIDE=" + std::to_string(std::tuple_size_v<Pixel>)
  };
}
} // namespace format_values
