// token encodings come from Microsoft's header; changed containers keep their original signatures and get a new
// hash. instruction-only fixtures need this for operations and modifiers that HLSL does not expose directly.
#pragma once
#include "dxbc_hash.hpp"
#include "../libs/DXBCParser/d3d12tokenizedprogramformat.hpp"
#include <functional>
#include <string>
#include <vector>

namespace dxbc_tokens
{
using namespace microsoft;
using Words = std::vector<uint32_t>;

inline uint32_t
word(const std::string &s, size_t at)
{
  uint32_t value;
  memcpy(&value, s.data() + at, sizeof(value));
  return value;
}

inline std::string
rewrite(const std::string &base, const std::function<bool(Words &)> &change)
{
  // DXBC container header: magic, hash, version, size, part count, then the offsets (DxilContainer.h).
  constexpr size_t size_at = 24, count_at = 28, offsets_at = 32;
  if (base.size() < offsets_at || base.size() % sizeof(uint32_t))
    return {};
  const uint32_t count = word(base, count_at);
  const size_t header = offsets_at + size_t(count) * sizeof(uint32_t);
  if (header > base.size())
    return {};
  std::string out = base.substr(0, header);
  unsigned changed = 0;
  for (uint32_t i = 0; i < count; i++) {
    size_t at = word(base, offsets_at + i * sizeof(uint32_t));
    if (at < header || at + 2 * sizeof(uint32_t) > base.size())
      return {};
    uint32_t bytes = word(base, at + sizeof(uint32_t)), offset = out.size();
    if (at + 2 * sizeof(uint32_t) + bytes > base.size())
      return {};
    memcpy(out.data() + offsets_at + i * sizeof(uint32_t), &offset, sizeof(offset));
    if (!base.compare(at, 4, "SHDR") || !base.compare(at, 4, "SHEX")) {
      if (bytes % sizeof(uint32_t) || bytes < 2 * sizeof(uint32_t))
        return {};
      Words program(bytes / sizeof(uint32_t));
      memcpy(program.data(), base.data() + at + 2 * sizeof(uint32_t), bytes);
      if (program[1] != program.size() || !change(program))
        return {};
      program[1] = program.size();
      bytes = program.size() * sizeof(uint32_t);
      out.append(base, at, sizeof(uint32_t));
      out.append(reinterpret_cast<const char *>(&bytes), sizeof(bytes));
      out.append(reinterpret_cast<const char *>(program.data()), bytes);
      changed++;
    } else {
      out.append(base, at, 2 * sizeof(uint32_t) + bytes);
    }
  }
  if (changed != 1)
    return {};
  uint32_t size = out.size();
  memcpy(out.data() + size_at, &size, sizeof(size));
  dxbc::sign(out);
  ComPtr<ID3DBlob> assembly;
  HRESULT hr = D3DDisassemble(out.data(), out.size(), 0, nullptr, &assembly);
  if (!expect(SUCCEEDED(hr) && assembly, "generated DXBC disassembles: %08lx", hr))
    return {};
  return out;
}

inline Words
destination(uint32_t index, uint32_t mask = D3D10_SB_OPERAND_4_COMPONENT_MASK_ALL,
            D3D10_SB_OPERAND_TYPE type = D3D10_SB_OPERAND_TYPE_TEMP)
{
  return {ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) |
              ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE) | mask |
              ENCODE_D3D10_SB_OPERAND_TYPE(type) | ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(D3D10_SB_OPERAND_INDEX_1D),
          index};
}

inline Words
source(uint32_t index, uint32_t swizzle = D3D10_SB_OPERAND_4_COMPONENT_NOSWIZZLE)
{
  return {ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) |
              ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE) | swizzle |
              ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_TEMP) |
              ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(D3D10_SB_OPERAND_INDEX_1D),
          index};
}

inline Words
immediate(uint32_t value)
{
  return {ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_1_COMPONENT) |
              ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_IMMEDIATE32),
          value};
}

inline void
instruction(Words &program, D3D10_SB_OPCODE_TYPE opcode, std::initializer_list<Words> operands = {},
            uint32_t controls = 0)
{
  const size_t at = program.size();
  program.push_back(opcode | controls);
  for (auto &operand : operands)
    program.insert(program.end(), operand.begin(), operand.end());
  program[at] |= ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(program.size() - at);
}

inline Words
label(uint32_t index)
{
  return {ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_0_COMPONENT) |
              ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_LABEL) |
              ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(D3D10_SB_OPERAND_INDEX_1D),
          index};
}
inline Words
declarations(const Words &p, uint32_t temporaries)
{
  Words out(p.begin(), p.begin() + 2);
  for (size_t at = 2; at < p.size();) {
    auto op = DECODE_D3D10_SB_OPCODE_TYPE(p[at]);
    unsigned size = DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(p[at]);
    if (!size || at + size > p.size())
      return {};
    if (op < D3D10_SB_OPCODE_DCL_RESOURCE || op > D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS)
      break;
    if (op != D3D10_SB_OPCODE_DCL_TEMPS)
      out.insert(out.end(), p.begin() + at, p.begin() + at + size);
    at += size;
  }
  instruction(out, D3D10_SB_OPCODE_DCL_TEMPS, {{temporaries}});
  return out;
}

inline Words
constant_buffer(bool sm51, uint32_t swizzle = D3D10_SB_OPERAND_4_COMPONENT_NOSWIZZLE)
{
  auto cb = source(0, swizzle);
  cb[0] &= ~(D3D10_SB_OPERAND_TYPE_MASK | D3D10_SB_OPERAND_INDEX_DIMENSION_MASK);
  cb[0] |= ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER) |
           ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(sm51 ? D3D10_SB_OPERAND_INDEX_3D : D3D10_SB_OPERAND_INDEX_2D);
  cb.push_back(0);
  if (sm51)
    cb.push_back(0);
  return cb;
}
} // namespace dxbc_tokens
