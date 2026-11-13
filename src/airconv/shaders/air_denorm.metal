#include <metal_stdlib>
using namespace metal;

// IEEE 754 binary32 fields, derived from the compiler's float format
constant constexpr uint fraction_bits = __FLT_MANT_DIG__ - 1;
constant constexpr uint hidden_bit = 1u << fraction_bits;
constant constexpr int exponent_bias = __FLT_MAX_EXP__ - 1;
constant constexpr uint infinity_bits = (2 * exponent_bias + 1) << fraction_bits;
constant constexpr uint sign_bit = 1u << (sizeof(uint) * __CHAR_BIT__ - 1);
constant constexpr uint wide_bits = sizeof(ulong) * __CHAR_BIT__;
constant constexpr uint guard_bits = wide_bits - __FLT_MANT_DIG__ - 1;

// integer rounding keeps an underflowing result from being flushed by floating point arithmetic
static uint
denorm_pack(ulong value, int exponent, uint sign) {
  if (!value) return sign;
  int shift = int(wide_bits - 1 - clz(value)) - int(fraction_bits);
  exponent += shift;
  if (exponent <= 0) {
    shift += 1 - exponent;
    exponent = 0;
  }
  if (shift > int(wide_bits)) return sign;
  if (shift > 0) {
    ulong halfway = 1ul << (shift - 1);
    ulong remainder = value & (halfway | (halfway - 1));
    value = shift == int(wide_bits) ? 0 : value >> shift;
    value += remainder > halfway || (remainder == halfway && (value & 1));
  } else {
    value <<= -shift;
  }
  if (exponent >= 2 * exponent_bias + 1) return sign | infinity_bits;
  return sign | (((uint(max(exponent, 1)) - 1) << fraction_bits) + uint(value));
}

enum class denorm_op { add, sub, mul, div };

template <denorm_op op>
static uint
denorm_binary(uint a, uint b) {
  uint ma = a & ~sign_bit, mb = b & ~sign_bit;
  if (ma > infinity_bits || mb > infinity_bits)
    return (ma > infinity_bits ? a : b) | (hidden_bit >> 1);
  if (op == denorm_op::sub) b ^= sign_bit;
  uint sign = (a ^ b) & sign_bit;
  if (op == denorm_op::add || op == denorm_op::sub) {
    if (ma == infinity_bits || mb == infinity_bits)
      return ma == mb && sign ? infinity_bits | (hidden_bit >> 1) : ma == infinity_bits ? a : b;
    if (ma < mb) {
      uint tmp = a; a = b; b = tmp;
      tmp = ma; ma = mb; mb = tmp;
    }
  } else {
    if (op == denorm_op::mul) {
      if ((ma == infinity_bits && !mb) || (mb == infinity_bits && !ma))
        return infinity_bits | (hidden_bit >> 1);
      if (ma == infinity_bits || mb == infinity_bits) return sign | infinity_bits;
    } else {
      if ((!ma && !mb) || (ma == infinity_bits && mb == infinity_bits))
        return infinity_bits | (hidden_bit >> 1);
      if (ma == infinity_bits || !mb) return sign | infinity_bits;
      if (!ma || mb == infinity_bits) return sign;
    }
  }
  int ea = max(int(ma >> fraction_bits), 1), eb = max(int(mb >> fraction_bits), 1);
  ma = (ma & (hidden_bit - 1)) | (ma >= hidden_bit ? hidden_bit : 0);
  mb = (mb & (hidden_bit - 1)) | (mb >= hidden_bit ? hidden_bit : 0);
  ulong value;
  int exponent;
  if (op == denorm_op::add || op == denorm_op::sub) {
    ulong lhs = ulong(ma) << guard_bits, rhs = ulong(mb) << guard_bits;
    uint distance = ea - eb;
    // a sticky bit retains all discarded bits for round to nearest, ties to even (IEEE 754 4.3.1)
    if (distance)
      rhs = distance >= wide_bits ? ulong(rhs != 0) : (rhs >> distance) | ulong((rhs << (wide_bits - distance)) != 0);
    value = sign ? lhs - rhs : lhs + rhs;
    sign = value ? a & sign_bit : a & b & sign_bit;
    exponent = ea - int(guard_bits);
  } else if (op == denorm_op::mul) {
    value = ulong(ma) * mb;
    exponent = ea + eb - exponent_bias - int(fraction_bits);
  } else {
    int sa = clz(ma) - (sizeof(uint) * __CHAR_BIT__ - __FLT_MANT_DIG__);
    int sb = clz(mb) - (sizeof(uint) * __CHAR_BIT__ - __FLT_MANT_DIG__);
    ma <<= sa; mb <<= sb;
    ulong numerator = ulong(ma) << guard_bits;
    value = (numerator / mb) | ulong(numerator % mb != 0);
    exponent = ea - sa - eb + sb + exponent_bias + int(fraction_bits) - int(guard_bits);
  }
  return denorm_pack(value, exponent, sign);
}

uint denorm_add(uint x, uint y) asm("mullion.denorm.fadd");
uint denorm_sub(uint x, uint y) asm("mullion.denorm.fsub");
uint denorm_mul(uint x, uint y) asm("mullion.denorm.fmul");
uint denorm_div(uint x, uint y) asm("mullion.denorm.fdiv");
uint denorm_add(uint x, uint y) { return denorm_binary<denorm_op::add>(x, y); }
uint denorm_sub(uint x, uint y) { return denorm_binary<denorm_op::sub>(x, y); }
uint denorm_mul(uint x, uint y) { return denorm_binary<denorm_op::mul>(x, y); }
uint denorm_div(uint x, uint y) { return denorm_binary<denorm_op::div>(x, y); }
