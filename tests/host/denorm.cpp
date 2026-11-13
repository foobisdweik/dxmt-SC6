// contract: the preserve-mode helpers implement IEEE 754 round to nearest, ties to even, including subnormal
// inputs and outputs (4.3.1; DXC Denorm Mode: "denorm input/output should be preserved"). the host's binary32
// arithmetic is an independent oracle for the same source linked into shaders. each failure gives its seed,
// operation and operand bits; all exponent ranges and generated mantissas include cases no fixture names.
#include <bit>
#include <cfloat>
#include <climits>
#include <cstdio>
#include <limits>
#include <random>
#include "metal/metal_stdlib"
using ulong = uint64_t;
inline uint clz(uint x) { return x ? __builtin_clz(x) : sizeof(x) * CHAR_BIT; }
inline uint clz(ulong x) { return x ? __builtin_clzll(x) : sizeof(x) * CHAR_BIT; }
#include "../../src/airconv/shaders/air_denorm.metal"

int
main() {
  const uint32_t seed = 1;
  std::mt19937 next(seed);
  unsigned failures = 0;
  auto check = [&](uint a, uint b) {
    float x = std::bit_cast<float>(a), y = std::bit_cast<float>(b);
    double dx = x, dy = y;
    uint got[] = {denorm_add(a, b), denorm_sub(a, b), denorm_mul(a, b), denorm_div(a, b)};
    double want[] = {dx + dy, dx - dy, dx * dy, dx / dy};
    for (unsigned op = 0; op < std::size(got); op++) {
      auto expected = (float)want[op];
      if (got[op] != std::bit_cast<uint>(expected) &&
          !(std::isnan(std::bit_cast<float>(got[op])) && std::isnan(expected))) {
        if (failures++ < 16)
          printf("seed=%u op=%u a=%#x b=%#x got=%#x want=%#x\n", seed, op, a, b,
                 got[op], std::bit_cast<uint>(expected));
      }
    }
  };
  uint edges[] = {0, 1, hidden_bit - 1, hidden_bit, hidden_bit + 1, infinity_bits - 1,
                  infinity_bits, infinity_bits | (hidden_bit >> 1), std::bit_cast<uint>(0.5f),
                  std::bit_cast<uint>(1.0f), std::bit_cast<uint>(2.0f),
                  std::bit_cast<uint>(std::ldexp(1.0f, -__FLT_MANT_DIG__)),
                  std::bit_cast<uint>(std::nextafter(1.0f, 2.0f))};
  for (auto a : edges)
    for (auto b : edges)
      for (uint sa : {0u, sign_bit})
        for (uint sb : {0u, sign_bit}) check(a | sa, b | sb);
  for (uint a = 0; a < infinity_bits / hidden_bit; a++)
    for (uint b = 0; b < infinity_bits / hidden_bit; b++) {
      uint x = a * hidden_bit | (next() & (hidden_bit - 1)), y = b * hidden_bit | (next() & (hidden_bit - 1));
      check(x, y); check(x | sign_bit, y); check(x, y | sign_bit); check(x | sign_bit, y | sign_bit);
    }
  for (uint i = 0; i < hidden_bit; i++) {
    uint a = next(), b = next();
    check(a, b); check(a, a); check(a, a ^ sign_bit);
  }
  printf("%s: %u wrong results\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
