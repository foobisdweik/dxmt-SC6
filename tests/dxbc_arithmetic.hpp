// contract: bitfields use "The LSB 5 bits" for width and offset (D3D11.3 22.11.2, 22.11.6, 22.11.11), with
// width zero returning zero and extraction extending to the end of the word when width+offset >=32. "Divide by
// zero returns 0xffffffff for both quotient and remainder." (22.12.9). multiply returns the full64-bit result
// (22.12.6/13); carry/borrow are0 or1 (22.12.8/14). no result is combined with another before checking it.
// "Round-to-zero must be used during conversion to another float format." (3.2.2). half conversion checks every
// encoding and neighbouring float32 values; NaNs are checked by class because their payload is unspecified.
#pragma once
#include "dxbc_tokens.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace dxbc_arithmetic
{
using Value = std::array<uint32_t, 4>;
enum Subject { fields, products, division, carries, shifts, firstbits, conversions, float_specials };
constexpr const char *names[] = {"bitfields",        "products",         "division",    "carry/borrow",
                                 "shifts/countbits", "firstbit/reverse", "conversions", "float specials"};
static const char hlsl[] = R"hlsl(
cbuffer Inputs : register(b0) { uint4 values[ROWS]; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
}
uint4 ps(float4 pos : SV_Position) : SV_Target { return values[uint(pos.y) * SIDE + uint(pos.x)]; }
)hlsl";

inline std::string
program(const std::string &base, Subject subject, uint32_t side)
{
  using namespace dxbc_tokens;
  return rewrite(base, [&](Words &p) {
    auto out = declarations(p, 2);
    if (out.empty())
      return false;
    const bool sm51 = DECODE_D3D10_SB_TOKENIZED_PROGRAM_MAJOR_VERSION(p[0]) == 5 &&
                      DECODE_D3D10_SB_TOKENIZED_PROGRAM_MINOR_VERSION(p[0]) == 1;
    auto pos = source(0);
    pos[0] &= ~D3D10_SB_OPERAND_TYPE_MASK;
    pos[0] |= ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_INPUT);
    instruction(out, D3D10_SB_OPCODE_FTOU,
                {destination(1, D3D10_SB_OPERAND_4_COMPONENT_MASK_X | D3D10_SB_OPERAND_4_COMPONENT_MASK_Y), pos});
    instruction(out, D3D10_SB_OPCODE_IMAD,
                {destination(1, D3D10_SB_OPERAND_4_COMPONENT_MASK_X),
                 source(1, D3D10_SB_OPERAND_4_COMPONENT_REPLICATEY), immediate(side),
                 source(1, D3D10_SB_OPERAND_4_COMPONENT_REPLICATEX)});
    auto cb = constant_buffer(sm51);
    const unsigned row_dimension = sm51 ? 2 : 1;
    cb[0] |=
        ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(row_dimension, D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE);
    auto index = source(1);
    index[0] &= ~(D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE_MASK | D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MASK);
    index[0] |= ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE);
    cb.insert(cb.end(), index.begin(), index.end());
    instruction(out, D3D10_SB_OPCODE_MOV, {destination(1), cb});
    instruction(out, D3D10_SB_OPCODE_MOV, {destination(0), source(1)});
    const uint32_t masks[] = {D3D10_SB_OPERAND_4_COMPONENT_MASK_X, D3D10_SB_OPERAND_4_COMPONENT_MASK_Y,
                              D3D10_SB_OPERAND_4_COMPONENT_MASK_Z, D3D10_SB_OPERAND_4_COMPONENT_MASK_W};
    const uint32_t swizzles[] = {D3D10_SB_OPERAND_4_COMPONENT_REPLICATEX, D3D10_SB_OPERAND_4_COMPONENT_REPLICATEY,
                                 D3D10_SB_OPERAND_4_COMPONENT_REPLICATEZ, D3D10_SB_OPERAND_4_COMPONENT_REPLICATEW};
    auto dst = [&](unsigned c) { return destination(0, masks[c]); };
    auto src = [&](unsigned c) { return source(1, swizzles[c]); };
    switch (subject) {
    case fields:
      instruction(out, D3D11_SB_OPCODE_UBFE, {dst(0), src(0), src(1), src(2)});
      instruction(out, D3D11_SB_OPCODE_IBFE, {dst(1), src(0), src(1), src(2)});
      instruction(out, D3D11_SB_OPCODE_BFI, {dst(2), src(0), src(1), src(2), src(3)});
      break;
    case products:
      instruction(out, D3D10_SB_OPCODE_IMUL, {dst(0), dst(1), src(0), src(1)});
      instruction(out, D3D10_SB_OPCODE_UMUL, {dst(2), dst(3), src(0), src(1)});
      break;
    case division:
      instruction(out, D3D10_SB_OPCODE_UDIV, {dst(0), dst(1), src(0), src(1)});
      break;
    case carries:
      instruction(out, D3D11_SB_OPCODE_UADDC, {dst(0), dst(1), src(0), src(1)});
      instruction(out, D3D11_SB_OPCODE_USUBB, {dst(2), dst(3), src(0), src(1)});
      break;
    case shifts:
      instruction(out, D3D10_SB_OPCODE_ISHL, {dst(0), src(0), src(1)});
      instruction(out, D3D10_SB_OPCODE_ISHR, {dst(1), src(0), src(1)});
      instruction(out, D3D10_SB_OPCODE_USHR, {dst(2), src(0), src(1)});
      instruction(out, D3D11_SB_OPCODE_COUNTBITS, {dst(3), src(0)});
      break;
    case firstbits:
      instruction(out, D3D11_SB_OPCODE_FIRSTBIT_HI, {dst(0), src(0)});
      instruction(out, D3D11_SB_OPCODE_FIRSTBIT_LO, {dst(1), src(0)});
      instruction(out, D3D11_SB_OPCODE_FIRSTBIT_SHI, {dst(2), src(0)});
      instruction(out, D3D11_SB_OPCODE_BFREV, {dst(3), src(0)});
      break;
    case conversions:
      instruction(out, D3D10_SB_OPCODE_FTOI, {dst(0), src(0)});
      instruction(out, D3D10_SB_OPCODE_FTOU, {dst(1), src(0)});
      instruction(out, D3D11_SB_OPCODE_F32TOF16, {dst(2), src(0)});
      instruction(out, D3D11_SB_OPCODE_F16TOF32, {dst(3), src(1)});
      break;
    case float_specials:
      instruction(out, D3D10_SB_OPCODE_ADD, {dst(0), src(0), src(1)});
      instruction(out, D3D10_SB_OPCODE_MUL, {dst(1), src(0), src(1)});
      instruction(out, D3D10_SB_OPCODE_MIN, {dst(2), src(0), src(1)});
      instruction(out, D3D10_SB_OPCODE_MAX, {dst(3), src(0), src(1)});
      break;
    }
    instruction(out, D3D10_SB_OPCODE_MOV,
                {destination(0, D3D10_SB_OPERAND_4_COMPONENT_MASK_ALL, D3D10_SB_OPERAND_TYPE_OUTPUT), source(0)});
    instruction(out, D3D10_SB_OPCODE_RET);
    p = std::move(out);
    return true;
  });
}

constexpr uint32_t word_bits = std::numeric_limits<uint32_t>::digits;
constexpr uint32_t half_bits = 16, half_fraction = 10, float_fraction = 23, half_bias = 15, float_bias = 127;
constexpr uint32_t half_sign = 1u << (half_bits - 1), half_infinity = 31u << half_fraction;

inline uint32_t
half_to_float(uint32_t h)
{
  h &= (1u << half_bits) - 1;
  const uint32_t sign = (h & half_sign) << half_bits, exponent = h >> half_fraction & 31;
  const uint32_t fraction = h & ((1u << half_fraction) - 1);
  if (exponent == 31)
    return sign | 0x7f800000u | (fraction << (float_fraction - half_fraction));
  float magnitude = std::ldexp(float(exponent ? (1u << half_fraction) + fraction : fraction),
                               int(exponent ? exponent : 1) - int(half_bias + half_fraction));
  return sign | std::bit_cast<uint32_t>(magnitude);
}

inline uint32_t
float_to_half(uint32_t bits)
{
  const uint32_t sign = bits >> half_bits & half_sign, magnitude = bits & 0x7fffffffu;
  if (magnitude >= 0x7f800000u)
    return sign | half_infinity | (magnitude == 0x7f800000u ? 0 : 1);
  const int exponent = int(magnitude >> float_fraction) - int(float_bias) + int(half_bias);
  if (exponent >= 31)
    return sign | (half_infinity - 1);
  if (exponent > 0)
    return sign | uint32_t(exponent) << half_fraction | (magnitude >> (float_fraction - half_fraction) & 1023u);
  if (exponent < -int(half_fraction))
    return sign;
  return sign | ((magnitude & ((1u << float_fraction) - 1)) | (1u << float_fraction)) >>
                    (float_fraction - half_fraction + 1 - exponent);
}

inline std::vector<uint32_t>
integers()
{
  std::vector<uint32_t> out{0, UINT32_MAX, 0x55555555u, 0xaaaaaaaau};
  for (unsigned i = 0; i < word_bits; i++)
    out.insert(out.end(), {uint32_t(1) << i, ~(uint32_t(1) << i)});
  return out;
}

inline std::vector<Value>
inputs(Subject subject)
{
  std::vector<Value> out;
  auto values = integers();
  if (subject == fields) {
    for (uint32_t width = 0; width < word_bits; width++)
      for (uint32_t offset = 0; offset < word_bits; offset++)
        for (uint32_t high_width : {0u, word_bits, ~uint32_t(word_bits - 1)})
          for (uint32_t high_offset : {0u, word_bits, ~uint32_t(word_bits - 1)})
            for (uint32_t value : values)
              out.push_back({width | high_width, offset | high_offset, value, ~value});
  } else if (subject == conversions) {
    const uint32_t float_sign = 1u << (word_bits - 1);
    for (uint32_t half = 0; half < (1u << half_bits); half++) {
      uint32_t exact = half_to_float(half);
      for (uint32_t bits : {exact, exact ? exact - 1 : float_sign | 1, exact + 1})
        for (uint32_t high : {0u, 1u << half_bits, UINT32_MAX << half_bits})
          out.push_back({bits, half | high, 0, 0});
    }
    for (float value : {float(INT32_MIN), float(INT32_MAX), float(UINT32_MAX), 1.0f, -1.0f, 0.5f, -0.5f})
      for (float adjacent : {std::nextafter(value, -INFINITY), value, std::nextafter(value, INFINITY)})
        out.push_back({std::bit_cast<uint32_t>(adjacent), 0, 0, 0});
  } else if (subject == float_specials) {
    std::vector<uint32_t> floats{0,           1,           0x007fffffu, 0x00800000u, 0x00800001u, 0x3f000000u,
                                 0x3f800000u, 0x40000000u, 0x7f7fffffu, 0x7f800000u, 0x7fc00000u, 0x7f800001u};
    auto positive = floats;
    for (auto v : positive)
      floats.push_back(v | (1u << (word_bits - 1)));
    for (auto a : floats)
      for (auto b : floats)
        out.push_back({a, b, 0, 0});
  } else {
    for (auto a : values)
      for (auto b : values)
        out.push_back({a, b, ~a, ~b});
    if (subject == shifts)
      for (auto a : values)
        for (uint32_t b = 0; b <= 2 * word_bits; b++)
          out.push_back({a, b, 0, 0});
  }
  return out;
}

inline uint32_t
arithmetic_shift(uint32_t value, uint32_t count)
{
  count %= word_bits;
  return !count ? value : value >> count | (value >> (word_bits - 1) ? UINT32_MAX << (word_bits - count) : 0);
}

inline float
flushed(uint32_t bits)
{
  float value = std::bit_cast<float>(bits);
  return std::fpclassify(value) == FP_SUBNORMAL ? std::copysign(0.0f, value) : value;
}

inline Value
reference(Subject subject, Value v)
{
  uint32_t a = v[0], b = v[1];
  switch (subject) {
  case fields: {
    unsigned width = a % word_bits, offset = b % word_bits, kept = std::min(width, word_bits - offset);
    uint32_t low = uint32_t((uint64_t(1) << kept) - 1), field = v[2] >> offset & low;
    uint32_t signed_field = kept && (field & uint32_t(1) << (kept - 1)) ? field | ~low : field;
    uint32_t mask = uint32_t(((uint64_t(1) << width) - 1) << offset);
    return {field, signed_field, (v[2] << offset & mask) | (v[3] & ~mask), v[3]};
  }
  case products: {
    uint64_t s = uint64_t(int64_t(std::bit_cast<int32_t>(a)) * int64_t(std::bit_cast<int32_t>(b)));
    uint64_t u = uint64_t(a) * b;
    return {uint32_t(s >> word_bits), uint32_t(s), uint32_t(u >> word_bits), uint32_t(u)};
  }
  case division:
    return {b ? a / b : UINT32_MAX, b ? a % b : UINT32_MAX, v[2], v[3]};
  case carries:
    return {a + b, uint32_t((uint64_t(a) + b) >> word_bits), a - b, uint32_t(a < b)};
  case shifts:
    return {a << (b % word_bits), arithmetic_shift(a, b), a >> (b % word_bits), uint32_t(std::popcount(a))};
  case firstbits: {
    uint32_t signed_bits = a >> (word_bits - 1) ? ~a : a, reversed = 0;
    for (unsigned bit = 0; bit < word_bits; bit++)
      reversed |= (a >> bit & 1) << (word_bits - 1 - bit);
    return {a ? uint32_t(std::countl_zero(a)) : UINT32_MAX, a ? uint32_t(std::countr_zero(a)) : UINT32_MAX,
            signed_bits ? uint32_t(std::countl_zero(signed_bits)) : UINT32_MAX, reversed};
  }
  case conversions: {
    double value = std::bit_cast<float>(a);
    auto signed_value = value != value       ? 0
                        : value <= INT32_MIN ? INT32_MIN
                        : value >= INT32_MAX ? INT32_MAX
                                             : int32_t(value);
    uint32_t unsigned_value = value != value || value <= 0 ? 0 : value >= UINT32_MAX ? UINT32_MAX : uint32_t(value);
    return {uint32_t(signed_value), unsigned_value, float_to_half(a), half_to_float(b)};
  }
  case float_specials: {
    float x = flushed(a), y = flushed(b);
    float low = std::isnan(x) ? y : std::isnan(y) ? x : x < y ? x : y;
    float high = std::isnan(x) ? y : std::isnan(y) ? x : x >= y ? x : y;
    return {std::bit_cast<uint32_t>(flushed(std::bit_cast<uint32_t>(x + y))),
            std::bit_cast<uint32_t>(flushed(std::bit_cast<uint32_t>(x * y))), std::bit_cast<uint32_t>(low),
            std::bit_cast<uint32_t>(high)};
  }
  }
  return {};
}

inline bool
allowed(Subject subject, unsigned c, uint32_t got, uint32_t want, Value input)
{
  if (got == want)
    return true;
  if (subject == conversions && c == 2)
    return (want & half_infinity) == half_infinity && (want & ((1u << half_fraction) - 1)) != 0 &&
           !(got >> half_bits) && (got & half_infinity) == half_infinity && (got & ((1u << half_fraction) - 1));
  if ((subject == conversions && c == 3) || subject == float_specials) {
    float g = std::bit_cast<float>(got), w = std::bit_cast<float>(want);
    if (std::isnan(w))
      return std::isnan(g);
    if (subject == float_specials && c >= 2) {
      if (g == 0 && w == 0)
        return true;
      // min/max may return an unflushed input, but must compare after flushing (22.10.10-11).
      return std::fpclassify(g) == FP_SUBNORMAL && flushed(got) == w && (got == input[0] || got == input[1]);
    }
  }
  return false;
}
} // namespace dxbc_arithmetic
