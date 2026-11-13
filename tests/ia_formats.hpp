// contract: D3D11.3 8.20: "If the source data contains 32-bit per-component float, UINT or SINT data, it is read
// without modifying the bits at all (no conversion)." 3.2.3 defines normalized and integer conversions; 3.2.3.2
// requires "1/2 ULP" precision. 3.2.2 preserves signs and widens denormals; 19.1.3.3 supplies missing channels.
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
// format widths and required/optional IA support are from Microsoft's feature-level 10.0 and 11.0 tables:
// https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/format-support-for-direct3d-feature-level-10-0-hardware
// https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/format-support-for-direct3d-11-0-feature-level-hardware
// enumerate every sub-32-bit component encoding; 32-bit inputs include field boundaries, infinities, NaNs, denormals
// and signed zero. odd permutations give every component each encoding without making the channels equal.
#pragma once
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <vector>

namespace ia_formats {
enum Kind { Unorm, Snorm, Uint, Sint, Float, PackedFloat };
struct Format {
  const char *name;
  DXGI_FORMAT format;
  Kind kind;
  uint32_t bits[4];
  bool bgra = false;
  uint32_t padding = 0;
  bool required = true;
};
inline const Format formats[] = {
      {"R8G8B8A8_UNORM", DXGI_FORMAT_R8G8B8A8_UNORM, Unorm, {8, 8, 8, 8}},
      {"B8G8R8A8_UNORM", DXGI_FORMAT_B8G8R8A8_UNORM, Unorm, {8, 8, 8, 8}, true},
      {"R8G8B8A8_SNORM", DXGI_FORMAT_R8G8B8A8_SNORM, Snorm, {8, 8, 8, 8}},
      {"R8G8B8A8_UINT", DXGI_FORMAT_R8G8B8A8_UINT, Uint, {8, 8, 8, 8}},
      {"R8G8B8A8_SINT", DXGI_FORMAT_R8G8B8A8_SINT, Sint, {8, 8, 8, 8}},
      {"R8G8_UNORM", DXGI_FORMAT_R8G8_UNORM, Unorm, {8, 8}},
      {"R8G8_SNORM", DXGI_FORMAT_R8G8_SNORM, Snorm, {8, 8}},
      {"R8G8_UINT", DXGI_FORMAT_R8G8_UINT, Uint, {8, 8}},
      {"R8G8_SINT", DXGI_FORMAT_R8G8_SINT, Sint, {8, 8}},
      {"R8_UNORM", DXGI_FORMAT_R8_UNORM, Unorm, {8}},
      {"R8_SNORM", DXGI_FORMAT_R8_SNORM, Snorm, {8}},
      {"R8_UINT", DXGI_FORMAT_R8_UINT, Uint, {8}},
      {"R8_SINT", DXGI_FORMAT_R8_SINT, Sint, {8}},
      {"R16G16B16A16_UNORM", DXGI_FORMAT_R16G16B16A16_UNORM, Unorm, {16, 16, 16, 16}},
      {"R16G16B16A16_SNORM", DXGI_FORMAT_R16G16B16A16_SNORM, Snorm, {16, 16, 16, 16}},
      {"R16G16B16A16_UINT", DXGI_FORMAT_R16G16B16A16_UINT, Uint, {16, 16, 16, 16}},
      {"R16G16B16A16_SINT", DXGI_FORMAT_R16G16B16A16_SINT, Sint, {16, 16, 16, 16}},
      {"R16G16B16A16_FLOAT", DXGI_FORMAT_R16G16B16A16_FLOAT, Float, {16, 16, 16, 16}},
      {"R16G16_UNORM", DXGI_FORMAT_R16G16_UNORM, Unorm, {16, 16}},
      {"R16G16_SNORM", DXGI_FORMAT_R16G16_SNORM, Snorm, {16, 16}},
      {"R16G16_UINT", DXGI_FORMAT_R16G16_UINT, Uint, {16, 16}},
      {"R16G16_SINT", DXGI_FORMAT_R16G16_SINT, Sint, {16, 16}},
      {"R16G16_FLOAT", DXGI_FORMAT_R16G16_FLOAT, Float, {16, 16}},
      {"R16_UNORM", DXGI_FORMAT_R16_UNORM, Unorm, {16}},
      {"R16_SNORM", DXGI_FORMAT_R16_SNORM, Snorm, {16}},
      {"R16_UINT", DXGI_FORMAT_R16_UINT, Uint, {16}},
      {"R16_SINT", DXGI_FORMAT_R16_SINT, Sint, {16}},
      {"R16_FLOAT", DXGI_FORMAT_R16_FLOAT, Float, {16}},
      {"R32G32B32A32_FLOAT", DXGI_FORMAT_R32G32B32A32_FLOAT, Float, {32, 32, 32, 32}},
      {"R32G32B32A32_UINT", DXGI_FORMAT_R32G32B32A32_UINT, Uint, {32, 32, 32, 32}},
      {"R32G32B32A32_SINT", DXGI_FORMAT_R32G32B32A32_SINT, Sint, {32, 32, 32, 32}},
      {"R32G32B32_FLOAT", DXGI_FORMAT_R32G32B32_FLOAT, Float, {32, 32, 32}},
      {"R32G32B32_UINT", DXGI_FORMAT_R32G32B32_UINT, Uint, {32, 32, 32}},
      {"R32G32B32_SINT", DXGI_FORMAT_R32G32B32_SINT, Sint, {32, 32, 32}},
      {"R32G32_FLOAT", DXGI_FORMAT_R32G32_FLOAT, Float, {32, 32}},
      {"R32G32_UINT", DXGI_FORMAT_R32G32_UINT, Uint, {32, 32}},
      {"R32G32_SINT", DXGI_FORMAT_R32G32_SINT, Sint, {32, 32}},
      {"R32_FLOAT", DXGI_FORMAT_R32_FLOAT, Float, {32}},
      {"R32_UINT", DXGI_FORMAT_R32_UINT, Uint, {32}},
      {"R32_SINT", DXGI_FORMAT_R32_SINT, Sint, {32}},
      {"R10G10B10A2_UNORM", DXGI_FORMAT_R10G10B10A2_UNORM, Unorm, {10, 10, 10, 2}},
      {"R10G10B10A2_UINT", DXGI_FORMAT_R10G10B10A2_UINT, Uint, {10, 10, 10, 2}},
      {"R11G11B10_FLOAT", DXGI_FORMAT_R11G11B10_FLOAT, PackedFloat, {11, 11, 10}},
      {"B8G8R8X8_UNORM", DXGI_FORMAT_B8G8R8X8_UNORM, Unorm, {8, 8, 8}, true, 8},
      {"B5G6R5_UNORM", DXGI_FORMAT_B5G6R5_UNORM, Unorm, {5, 6, 5}, true, 0, false},
      {"B5G5R5A1_UNORM", DXGI_FORMAT_B5G5R5A1_UNORM, Unorm, {5, 5, 5, 1}, true, 0, false},
      {"B4G4R4A4_UNORM", DXGI_FORMAT_B4G4R4A4_UNORM, Unorm, {4, 4, 4, 4}, true, 0, false},
};
inline const char hlsl[] = R"hlsl(
struct F { float4 value : VALUE; };
struct U { uint4 value : VALUE; };
struct I { int4 value : VALUE; };
F as_float(float4 data : DATA) { F v; v.value = data; return v; }
U as_uint(uint4 data : DATA) { U v; v.value = data; return v; }
I as_int(int4 data : DATA) { I v; v.value = data; return v; }
)hlsl";

inline uint32_t mask(uint32_t bits) {
  return bits == std::numeric_limits<uint32_t>::digits ? UINT32_MAX : (uint32_t(1) << bits) - 1;
}
inline uint32_t stride(const Format &f) {
  uint32_t bits = f.padding;
  for (auto b : f.bits) bits += b;
  return bits / CHAR_BIT;
}
struct Layout {
  struct Element { const char *name; DXGI_FORMAT format; uint32_t slot, offset; };
  const char *name;
  std::vector<Element> elements;
  uint32_t slot, element_offset, binding_offset;
};
// 4.4.6: alignment is "the nearest power of 2 greater or equal to the width of the Element's format" capped at 4.
// APPEND_ALIGNED_ELEMENT places the element "directly after the previous one, including any packing if necessary".
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_input_element_desc
inline std::vector<Layout> layouts(const Format &f, uint32_t slots, uint32_t append, uint32_t maximum_alignment) {
  uint32_t alignment = 1;
  while (alignment < stride(f) && alignment < maximum_alignment) alignment *= 2;
  const auto aligned = [=](uint32_t value) { return (value + alignment - 1) / alignment * alignment; };
  const uint32_t last = slots - 1, explicit_offset = 2 * maximum_alignment;
  return {
      {"explicit zero", {{"DATA", f.format, 0, 0}}, 0, 0, 0},
      {"first append", {{"DATA", f.format, last, append}}, last, 0, maximum_alignment},
      {"mixed append", {{"UNUSED", DXGI_FORMAT_R8_UINT, last, append},
                        {"OTHER", DXGI_FORMAT_R32G32B32A32_UINT, 0, append},
                        {"DATA", f.format, last, append}}, last, aligned(sizeof(uint8_t)), maximum_alignment},
      {"explicit then append", {{"UNUSED", DXGI_FORMAT_R8_UINT, last, explicit_offset},
                                {"OTHER", DXGI_FORMAT_R32G32B32A32_UINT, 0, append},
                                {"DATA", f.format, last, append}},
                                last, aligned(explicit_offset + sizeof(uint8_t)), maximum_alignment},
      {"full overlap", {{"UNUSED", f.format, last, 0},
                        {"DATA", f.format, last, 0}}, last, 0, maximum_alignment},
      {"last byte overlap", {{"UNUSED", DXGI_FORMAT_R8_UINT, last, stride(f) - uint32_t(sizeof(uint8_t))},
                             {"DATA", f.format, last, 0}}, last, 0, maximum_alignment}};
}
inline std::vector<DXGI_FORMAT> other_formats() {
  std::vector<DXGI_FORMAT> out;
  for (uint32_t value = DXGI_FORMAT_UNKNOWN; value <= DXGI_FORMAT_B4G4R4A4_UNORM; value++)
    if (std::none_of(std::begin(formats), std::end(formats), [=](auto &f) { return uint32_t(f.format) == value; }))
      out.push_back(DXGI_FORMAT(value));
  out.insert(out.end(), {DXGI_FORMAT_P208, DXGI_FORMAT_V208, DXGI_FORMAT_V408, DXGI_FORMAT_FORCE_UINT});
  return out;
}
inline std::vector<uint32_t> values32() {
  std::vector<uint32_t> out{0, UINT32_MAX};
  for (uint32_t bit = 0; bit < std::numeric_limits<uint32_t>::digits; bit++) {
    uint32_t value = uint32_t(1) << bit;
    out.insert(out.end(), {value - 1, value, value + 1});
  }
  // IEEE binary32 fields (D3D11.3 3.1.3).
  const uint32_t sign = uint32_t(1) << 31, fraction = mask(23), exponent = mask(8) << 23;
  for (uint32_t value : {fraction, fraction + 1, exponent - 1, exponent, exponent + 1, exponent | (uint32_t(1) << 22)})
    out.insert(out.end(), {value, sign | value});
  return out;
}
inline uint32_t enumerated_count(const Format &f) {
  const uint32_t bits = *std::max_element(std::begin(f.bits), std::end(f.bits));
  return bits == 32 ? uint32_t(values32().size()) : uint32_t(1) << bits;
}
inline constexpr uint32_t legacy_vertices = 12;
inline uint32_t count(const Format &f) { return enumerated_count(f) + legacy_vertices; }
inline uint32_t raw(const Format &f, uint32_t vertex, uint32_t component) {
  const auto bits = f.bits[component], enumerated = enumerated_count(f);
  // retain the original twelve mixed-byte vectors alongside the enumerated component ranges.
  if (vertex >= enumerated) {
    const uint8_t ends[] = {0x00, 0xff, 0x80, 0x7f, 0x81, 0x01};
    uint32_t first_bit = 0, value = 0;
    for (uint32_t c = 0; c < component; c++) first_bit += f.bits[c];
    for (uint32_t bit = 0; bit < bits; bit++) {
      const uint32_t at = (vertex - enumerated) * stride(f) + (first_bit + bit) / CHAR_BIT;
      const uint8_t byte = at % 5 == 0 ? ends[at / 5 % std::size(ends)] : uint8_t(at * 37 + 11);
      value |= uint32_t((byte >> ((first_bit + bit) % CHAR_BIT)) & 1) << bit;
    }
    return value;
  }
  if (bits == 32) {
    const auto values = values32();
    return values[(vertex + component * (values.size() / 4 + 1)) % values.size()];
  }
  return (vertex * (2 * component + 1) + component) & mask(bits);
}
inline std::vector<uint8_t> data(const Format &f) {
  std::vector<uint8_t> bytes(count(f) * stride(f), 0);
  for (uint32_t vertex = 0; vertex < count(f); vertex++) {
    uint32_t at = 0;
    for (uint32_t c = 0; c < 4; c++) {
      const uint32_t value = raw(f, vertex, c);
      for (uint32_t bit = 0; bit < f.bits[c]; bit++, at++)
        bytes[vertex * stride(f) + at / CHAR_BIT] |= ((value >> bit) & 1) << (at % CHAR_BIT);
    }
    for (uint32_t bit = 0; bit < f.padding; bit++, at++)
      bytes[vertex * stride(f) + at / CHAR_BIT] |= ((vertex >> bit) & 1) << (at % CHAR_BIT);
  }
  return bytes;
}
inline uint32_t expected(const Format &f, uint32_t vertex, uint32_t component) {
  const uint32_t bits = f.bits[component], value = raw(f, vertex, component);
  if (f.kind == Uint || f.kind == Sint) {
    if (!bits) return component == 3;
    if (f.kind == Uint || bits == 32) return value;
    const uint32_t sign = uint32_t(1) << (bits - 1);
    return value & sign ? value | ~mask(bits) : value;
  }
  if (f.kind == Float && bits == 32) return value;
  float number = component == 3 ? 1 : 0;
  if (bits) {
    if (f.kind == Unorm) number = float(double(value) / mask(bits));
    else if (f.kind == Snorm) {
      const int64_t integer = value & (uint32_t(1) << (bits - 1)) ? int64_t(value) - (int64_t(1) << bits) : value;
      number = float(std::max(double(integer) / mask(bits - 1), -1.0));
    } else {
      // binary16 and unsigned binary11/10 use a five-bit exponent with bias 15 (3.1.5-6).
      const uint32_t mantissa_bits = f.kind == PackedFloat ? bits - 5 : 10;
      const uint32_t fraction = value & mask(mantissa_bits), exponent = value >> mantissa_bits & mask(5);
      number = exponent == 0 ? std::ldexp(float(fraction), 1 - 15 - int(mantissa_bits))
               : exponent == mask(5) ? (fraction ? NAN : INFINITY)
               : std::ldexp(float((uint32_t(1) << mantissa_bits) + fraction), int(exponent) - 15 - int(mantissa_bits));
      if (f.kind == Float && value >> (bits - 1)) number = -number;
    }
  }
  uint32_t out;
  memcpy(&out, &number, sizeof(out));
  return out;
}
inline bool matches(const Format &f, uint32_t vertex, uint32_t component, uint32_t got) {
  const uint32_t want = expected(f, vertex, component);
  if (want == got) return true;
  if (f.kind == Float && f.bits[component] == 32) return false;
  if (f.kind == Uint || f.kind == Sint) return false;
  float a, b;
  memcpy(&a, &want, sizeof(a));
  memcpy(&b, &got, sizeof(b));
  return std::isnan(a) && std::isnan(b);
}
} // namespace ia_formats
