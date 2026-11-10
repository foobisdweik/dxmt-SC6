// contract: "When a "ret" is encountered, return execution to the instruction after this call." (D3D11.3
// 22.7.10-11). callc/retc test bits, not floating values. "If there are already 32 entries on the return address
// stack and a "call" is issued, the call is skipped over." (22.7.10). the generated bodies keep FXC's declarations
// and signatures, using Microsoft's token encodings for conditional calls that HLSL cannot request directly.
#pragma once
#include "dxbc_tokens.hpp"
#include <algorithm>
#include <array>
#include <climits>

namespace dxbc_calls
{
constexpr uint32_t call_depth_limit = 32, after_call = 17, in_call = 3, after_retc = 5;
using Value = std::array<uint32_t, 4>;
struct Case {
  std::string name, code;
  Value input, want;
};
static const char hlsl[] = R"hlsl(
cbuffer Inputs : register(b0) { uint4 value; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
}
uint4 ps() : SV_Target { return value; }
)hlsl";

inline std::string
program(const std::string &base, unsigned mode, unsigned depth)
{
  using namespace dxbc_tokens;
  return rewrite(base, [&](Words &p) {
    auto out = declarations(p, 1);
    if (out.empty())
      return false;
    const bool sm51 = DECODE_D3D10_SB_TOKENIZED_PROGRAM_MAJOR_VERSION(p[0]) == 5 &&
                      DECODE_D3D10_SB_TOKENIZED_PROGRAM_MINOR_VERSION(p[0]) == 1;
    auto cb = constant_buffer(sm51);
    auto condition = cb;
    condition[0] &= ~(D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE_MASK | D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MASK);
    condition[0] |= ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE) |
                    ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECT_1(D3D10_SB_4_COMPONENT_X);
    const auto test = ENCODE_D3D10_SB_INSTRUCTION_TEST_BOOLEAN(mode % 2 ? D3D10_SB_INSTRUCTION_TEST_NONZERO
                                                                        : D3D10_SB_INSTRUCTION_TEST_ZERO);
    instruction(out, D3D10_SB_OPCODE_MOV, {destination(0), cb});
    if (mode < 2)
      instruction(out, D3D10_SB_OPCODE_CALLC, {condition, label(0)}, test);
    else
      instruction(out, D3D10_SB_OPCODE_CALL, {label(0)});
    instruction(out, D3D10_SB_OPCODE_IADD, {destination(0), source(0), immediate(after_call)});
    instruction(out, D3D10_SB_OPCODE_MOV,
                {destination(0, D3D10_SB_OPERAND_4_COMPONENT_MASK_ALL, D3D10_SB_OPERAND_TYPE_OUTPUT), source(0)});
    instruction(out, D3D10_SB_OPCODE_RET);
    for (unsigned i = 0; i < depth; i++) {
      instruction(out, D3D10_SB_OPCODE_LABEL, {label(i)});
      instruction(out, D3D10_SB_OPCODE_IADD, {destination(0), source(0), immediate(in_call)});
      if (mode == 2 || mode == 3) {
        instruction(out, D3D10_SB_OPCODE_RETC, {condition}, test);
        instruction(out, D3D10_SB_OPCODE_IADD, {destination(0), source(0), immediate(after_retc)});
      }
      if (i + 1 < depth)
        instruction(out, D3D10_SB_OPCODE_CALL, {label(i + 1)});
      instruction(out, D3D10_SB_OPCODE_RET);
    }
    p = std::move(out);
    return true;
  });
}

inline std::vector<Case>
cases(const std::string &base)
{
  std::vector<Case> out;
  std::vector<uint32_t> conditions{0, UINT32_MAX};
  for (unsigned bit = 0; bit < sizeof(uint32_t) * CHAR_BIT; bit++)
    conditions.push_back(uint32_t(1) << bit);
  const char *names[] = {"callc_z", "callc_nz", "retc_z", "retc_nz"};
  for (unsigned mode = 0; mode < std::size(names); mode++) {
    auto code = program(base, mode, 1);
    if (code.empty())
      return {};
    for (uint32_t condition : conditions) {
      Value input{condition, 7, 23, UINT32_MAX}, want = input;
      bool taken = (condition != 0) == (mode % 2 != 0);
      uint32_t add = after_call + (mode < 2 ? (taken ? in_call : 0) : in_call + (taken ? 0 : after_retc));
      for (auto &v : want)
        v += add;
      out.push_back({names[mode], code, input, want});
    }
  }
  for (unsigned depth : {1u, call_depth_limit - 1, call_depth_limit, call_depth_limit + 1}) {
    auto code = program(base, 4, depth);
    if (code.empty())
      return {};
    Value input{1, 7, 23, UINT32_MAX}, want = input;
    for (auto &v : want)
      v += after_call + std::min(depth, call_depth_limit) * in_call;
    out.push_back({"call depth " + std::to_string(depth), code, input, want});
  }
  return out;
}

inline void
check(const uint32_t *got, const Case &c)
{
  for (unsigned i = 0; i < c.want.size(); i++)
    expect(got[i] == c.want[i], "%s condition %#x component %u: %#x, want %#x", c.name.c_str(), c.input[0], i, got[i],
           c.want[i]);
}
} // namespace dxbc_calls
