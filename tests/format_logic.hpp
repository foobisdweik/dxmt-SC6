// D3D11.3 17.1 defines the sixteen Boolean equations; 17.7 restricts operands to UINT.
#pragma once
#include <d3d12.h>
#include "format_values.hpp"

namespace format_logic {
using namespace format_values;
constexpr unsigned operand_pairs = 4;

inline uint32_t
operate(unsigned op, uint32_t source, uint32_t destination) {
  switch (op) {
  case D3D12_LOGIC_OP_CLEAR:
    return 0;
  case D3D12_LOGIC_OP_SET:
    return ~0u;
  case D3D12_LOGIC_OP_COPY:
    return source;
  case D3D12_LOGIC_OP_COPY_INVERTED:
    return ~source;
  case D3D12_LOGIC_OP_NOOP:
    return destination;
  case D3D12_LOGIC_OP_INVERT:
    return ~destination;
  case D3D12_LOGIC_OP_AND:
    return source & destination;
  case D3D12_LOGIC_OP_NAND:
    return ~(source & destination);
  case D3D12_LOGIC_OP_OR:
    return source | destination;
  case D3D12_LOGIC_OP_NOR:
    return ~(source | destination);
  case D3D12_LOGIC_OP_XOR:
    return source ^ destination;
  case D3D12_LOGIC_OP_EQUIV:
    return ~(source ^ destination);
  case D3D12_LOGIC_OP_AND_REVERSE:
    return source & ~destination;
  case D3D12_LOGIC_OP_AND_INVERTED:
    return ~source & destination;
  case D3D12_LOGIC_OP_OR_REVERSE:
    return source | ~destination;
  case D3D12_LOGIC_OP_OR_INVERTED:
    return ~source | destination;
  }
  std::abort();
}

inline unsigned
width(const Format &f) {
  unsigned bits = 0;
  for (const auto &c : f.components)
    bits = std::max(bits, c.bits);
  return operand_pairs * (bits + 1);
}

inline void
inputs(const Format &f, std::vector<Pixel> &source, std::vector<uint8_t> &destination) {
  const unsigned count = width(f);
  source.resize(count);
  destination.resize(count * f.bytes);
  for (unsigned i = 0; i < count; i++)
    for (const auto &c : f.components) {
      unsigned pair = (i + c.channel) % operand_pairs;
      uint32_t bit = i / operand_pairs == count / operand_pairs - 1 ? mask(c.bits)
                                                                    : 1u << ((i / operand_pairs + c.channel) % c.bits);
      source[i][c.channel] = pair & 1 ? bit : 0;
      uint32_t raw = pair & 2 ? bit : 0;
      for (unsigned b = 0; b < c.bits; b++)
        destination[i * f.bytes + (c.offset + b) / CHAR_BIT] |= ((raw >> b) & 1) << ((c.offset + b) % CHAR_BIT);
    }
}

inline bool
matches(
    const Format &f, unsigned op, unsigned write_mask, const Pixel &source, const uint8_t *destination,
    const uint8_t *result
) {
  for (const auto &c : f.components) {
    uint32_t old = field(destination, c);
    uint32_t want = write_mask & (1u << c.channel) ? operate(op, source[c.channel], old) & mask(c.bits) : old;
    if (field(result, c) != want) {
      printf("channel %u: %#x, want %#x\n", c.channel, field(result, c), want);
      return false;
    }
  }
  return true;
}

static const char hlsl[] = R"hlsl(
StructuredBuffer<uint4> source : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position { return float4(id==2?3:-1,id==1?3:-1,0,1); }
uint4 ps(float4 position : SV_Position) : SV_Target { return source[uint(position.x)]; }
)hlsl";
} // namespace format_logic
