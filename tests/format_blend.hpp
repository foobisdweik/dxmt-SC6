// equations and clamping follow D3D11.3 17.1/.2/.5/.6; exact small inputs avoid intermediate-precision choices.
#pragma once
#include <d3d12.h>
#include "format_values.hpp"

namespace format_blend {
using namespace format_values;
using Color = std::array<float, 4>;
struct Source {
  Color value, second;
};
struct Equation {
  unsigned source, destination, alpha_source, alpha_destination, operation, alpha_operation, write_mask;
};
constexpr unsigned input_bits = 6, width = 1u << input_bits;
constexpr unsigned constant_cases = std::tuple_size_v<Color> + 2, phase_count = 2 * constant_cases;

inline Color
constants(unsigned phase) {
  unsigned index = phase % constant_cases;
  unsigned bits = index == constant_cases - 1 ? mask(std::tuple_size_v<Color>) : index ? 1u << (index - 1) : 0;
  Color result;
  for (unsigned c = 0; c < result.size(); c++)
    result[c] = float((bits >> c) & 1);
  return result;
}

inline bool
dual(unsigned factor) {
  return factor >= D3D12_BLEND_SRC1_COLOR && factor <= D3D12_BLEND_INV_SRC1_ALPHA;
}

inline std::vector<Equation>
equations(bool alpha_factor = false) {
  std::vector<unsigned> color = {
      D3D12_BLEND_ZERO,
      D3D12_BLEND_ONE,
      D3D12_BLEND_SRC_COLOR,
      D3D12_BLEND_INV_SRC_COLOR,
      D3D12_BLEND_SRC_ALPHA,
      D3D12_BLEND_INV_SRC_ALPHA,
      D3D12_BLEND_DEST_ALPHA,
      D3D12_BLEND_INV_DEST_ALPHA,
      D3D12_BLEND_DEST_COLOR,
      D3D12_BLEND_INV_DEST_COLOR,
      D3D12_BLEND_SRC_ALPHA_SAT,
      D3D12_BLEND_BLEND_FACTOR,
      D3D12_BLEND_INV_BLEND_FACTOR,
      D3D12_BLEND_SRC1_COLOR,
      D3D12_BLEND_INV_SRC1_COLOR,
      D3D12_BLEND_SRC1_ALPHA,
      D3D12_BLEND_INV_SRC1_ALPHA
  };
  std::vector<unsigned> alpha = {D3D12_BLEND_ZERO,          D3D12_BLEND_ONE,           D3D12_BLEND_SRC_ALPHA,
                                 D3D12_BLEND_INV_SRC_ALPHA, D3D12_BLEND_DEST_ALPHA,    D3D12_BLEND_INV_DEST_ALPHA,
                                 D3D12_BLEND_SRC_ALPHA_SAT, D3D12_BLEND_BLEND_FACTOR,  D3D12_BLEND_INV_BLEND_FACTOR,
                                 D3D12_BLEND_SRC1_ALPHA,    D3D12_BLEND_INV_SRC1_ALPHA};
  if (alpha_factor)
    for (unsigned factor : {D3D12_BLEND_ALPHA_FACTOR, D3D12_BLEND_INV_ALPHA_FACTOR}) {
      color.push_back(factor);
      alpha.push_back(factor);
    }
  std::vector<Equation> result;
  for (unsigned i = 0; i < std::size(color); i++)
    for (unsigned j = 0; j < std::size(color); j++)
      for (unsigned op = D3D12_BLEND_OP_ADD; op <= D3D12_BLEND_OP_MAX; op++) {
        Equation e{color[i], color[j], alpha[i % std::size(alpha)], alpha[j % std::size(alpha)],
                   op,       op,       D3D12_COLOR_WRITE_ENABLE_ALL};
        if (op >= D3D12_BLEND_OP_MIN &&
            (dual(e.source) || dual(e.destination) || dual(e.alpha_source) || dual(e.alpha_destination)))
          continue;
        result.push_back(e);
      }
  for (unsigned mask = 0; mask <= D3D12_COLOR_WRITE_ENABLE_ALL; mask++)
    result.push_back(
        {D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_OP_ADD,
         mask}
    );
  for (unsigned color_op = D3D12_BLEND_OP_ADD; color_op <= D3D12_BLEND_OP_MAX; color_op++)
    for (unsigned alpha_op = D3D12_BLEND_OP_ADD; alpha_op <= D3D12_BLEND_OP_MAX; alpha_op++)
      result.push_back(
          {D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_ONE, D3D12_BLEND_ONE, color_op, alpha_op,
           D3D12_COLOR_WRITE_ENABLE_ALL}
      );
  return result;
}

inline Color
clamped(const Format &f, Color value) {
  if (f.kind != Floating)
    for (auto &v : value)
      v = std::clamp(v, f.kind == Snorm ? -1.0f : 0.0f, 1.0f);
  return value;
}

inline void
encode_input(const Format &f, Color value, uint8_t *bytes) {
  value = clamped(f, value);
  for (const auto &c : f.components) {
    if (c.channel >= value.size())
      continue;
    float v = value[c.channel];
    uint32_t raw;
    if (f.kind == Floating) {
      raw = std::bit_cast<uint32_t>(v);
      if (c.bits != 32) {
        const unsigned sign_bits = c.bits == 16, exponent_bits = 5;
        const unsigned fraction = c.bits - exponent_bits - sign_bits;
        const unsigned bias = mask(exponent_bits - 1);
        const unsigned fp32_fraction = std::numeric_limits<float>::digits - 1;
        const unsigned fp32_bias = std::numeric_limits<float>::max_exponent - 1;
        raw = v == 0 ? 0 : ((raw >> fp32_fraction & mask(8)) - fp32_bias + bias) << fraction;
        if (sign_bits && std::signbit(v))
          raw |= 1u << (c.bits - 1);
      }
    } else if (f.kind == Snorm)
      raw = v < 0 ? uint32_t(-int32_t(mask(c.bits - 1))) & mask(c.bits) : v > 0 ? mask(c.bits - 1) : 0;
    else
      raw = v > 0 ? mask(c.bits) : 0;
    for (unsigned b = 0; b < c.bits; b++)
      bytes[(c.offset + b) / CHAR_BIT] |= ((raw >> b) & 1) << ((c.offset + b) % CHAR_BIT);
  }
}

inline void
inputs(const Format &f, unsigned phase, std::vector<Source> &source, std::vector<uint8_t> &destination) {
  source.resize(width);
  destination.resize(width * f.bytes);
  const bool unsigned_float = f.kind == Floating && f.components.front().bits < 16;
  float low = phase >= constant_cases && !unsigned_float ? -1.0f : 0.0f, high = phase >= constant_cases ? 2.0f : 1.0f;
  auto bit = [&](unsigned value, unsigned index) { return value & (1u << index) ? high : low; };
  const unsigned color_bits[] = {0, 1, 4};
  for (unsigned i = 0; i < width; i++) {
    Color old{};
    for (unsigned c = 0; c < std::size(color_bits); c++) {
      source[i].value[c] = bit(i, color_bits[c]);
      old[c] = bit(i, color_bits[(c + 1) % std::size(color_bits)]);
      source[i].second[c] = bit(i, color_bits[(c + 2) % std::size(color_bits)]);
    }
    source[i].value[3] = bit(i, 2);
    old[3] = bit(i, 3);
    source[i].second[3] = bit(i, 5);
    encode_input(f, old, destination.data() + i * f.bytes);
  }
}

inline float
factor(
    unsigned kind, unsigned channel, const Color &source, const Color &destination, const Color &second,
    const Color &constant
) {
  switch (kind) {
  case D3D12_BLEND_ZERO:
    return 0;
  case D3D12_BLEND_ONE:
    return 1;
  case D3D12_BLEND_SRC_COLOR:
    return source[channel];
  case D3D12_BLEND_INV_SRC_COLOR:
    return 1 - source[channel];
  case D3D12_BLEND_SRC_ALPHA:
    return source[3];
  case D3D12_BLEND_INV_SRC_ALPHA:
    return 1 - source[3];
  case D3D12_BLEND_DEST_COLOR:
    return destination[channel];
  case D3D12_BLEND_INV_DEST_COLOR:
    return 1 - destination[channel];
  case D3D12_BLEND_DEST_ALPHA:
    return destination[3];
  case D3D12_BLEND_INV_DEST_ALPHA:
    return 1 - destination[3];
  case D3D12_BLEND_SRC_ALPHA_SAT:
    return channel == 3 ? 1 : std::min(source[3], 1 - destination[3]);
  case D3D12_BLEND_BLEND_FACTOR:
    return constant[channel];
  case D3D12_BLEND_INV_BLEND_FACTOR:
    return 1 - constant[channel];
  case D3D12_BLEND_ALPHA_FACTOR:
    return constant[3];
  case D3D12_BLEND_INV_ALPHA_FACTOR:
    return 1 - constant[3];
  case D3D12_BLEND_SRC1_COLOR:
    return second[channel];
  case D3D12_BLEND_INV_SRC1_COLOR:
    return 1 - second[channel];
  case D3D12_BLEND_SRC1_ALPHA:
    return second[3];
  case D3D12_BLEND_INV_SRC1_ALPHA:
    return 1 - second[3];
  }
  std::abort();
}

inline bool
matches(
    const Format &f, const Equation &e, unsigned phase, const Source &input, const uint8_t *destination,
    const uint8_t *result
) {
  Color source = clamped(f, input.value), second = clamped(f, input.second), old;
  const auto constant = constants(phase);
  Pixel decoded = decode(f, destination), want;
  for (unsigned c = 0; c < old.size(); c++)
    old[c] = std::bit_cast<float>(decoded[c]);
  for (unsigned c = 0; c < want.size(); c++) {
    unsigned operation = c == 3 ? e.alpha_operation : e.operation;
    float value;
    if (!(e.write_mask & (1u << c)))
      value = old[c];
    else if (operation == D3D12_BLEND_OP_MIN)
      value = std::min(source[c], old[c]);
    else if (operation == D3D12_BLEND_OP_MAX)
      value = std::max(source[c], old[c]);
    else {
      float a = source[c] * factor(c == 3 ? e.alpha_source : e.source, c, source, old, second, constant);
      float b = old[c] * factor(c == 3 ? e.alpha_destination : e.destination, c, source, old, second, constant);
      value = operation == D3D12_BLEND_OP_ADD ? a + b : operation == D3D12_BLEND_OP_SUBTRACT ? a - b : b - a;
    }
    want[c] = std::bit_cast<uint32_t>(value);
  }
  return stored(f, want, result);
}

static const char hlsl[] = R"hlsl(
struct Source { float4 value; float4 second; };
StructuredBuffer<Source> source : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id==2?3:-1,id==1?3:-1,0,1); }
struct Output { float4 value : SV_Target0; float4 second : SV_Target1; };
Output ps(float4 position : SV_Position) {
  Source s = source[uint(position.x)]; Output o; o.value = s.value; o.second = s.second; return o;
}
)hlsl";
} // namespace format_blend
