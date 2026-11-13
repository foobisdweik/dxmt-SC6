/*
 * SC6 fork: a layered geometry shader, run inside the vertex function with instancing.
 *
 * Mullion runs a geometry shader (GS) on Metal's object and mesh stages (dxbc_converter_gs.cpp). Each GS
 * invocation, which is one input triangle, gets a mesh threadgroup of its own, so most of the GPU's SIMD lanes stay
 * idle there. Cube-map shadow passes (for example Unreal Engine 4's one-pass point light shadows) spend most of
 * their GPU time in that stage.
 *
 * Many such geometry shaders have this shape, fully unrolled:
 *
 *   for each face f:
 *     if (a uniform condition, for example "the mesh is visible in face f")
 *       compute the clip positions of the 3 input vertices for face f
 *       if (a per-triangle frustum test passes)
 *         emit vertices 0, 1 and 2 with SV_RenderTargetArrayIndex = f; cut
 *
 * Each emitted vertex depends only on its own input vertex and on constants, so the same work can run once per
 * vertex: the draw has N times the instances, Metal instance i draws face (i % N) of D3D instance (i / N), and the
 * vertex function writes the face to [[render_target_array_index]].
 *
 * The rewrite is exact only where the analysis below proves these facts:
 * 1. The control flow has no loops, switches or calls, and it uses no resources other than constant buffers.
 * 2. Each face's vertices are one strip of 3 emits and a cut, and the faces are 0 to N-1, each once.
 * 3. Emit j of a strip reads input vertex j only, and the 3 emits compute their outputs with the same operations
 *    (the same value numbers, Value::number). So emit 0's formula, applied to any vertex, gives that vertex's output.
 * 4. A branch whose condition depends only on constants is the same for the 3 vertices of a triangle, and it runs as
 *    written.
 * 5. A branch whose condition depends on vertex data (the per-triangle test) cannot run per vertex, so it always
 *    goes to its true target. This is right only for a culling test: the rasterizer clips what the test would have
 *    removed. The analysis takes such a branch only without an else part, so taking it can add a triangle but never
 *    change one.
 * Anything the analysis does not understand leaves the shader on the general path.
 */

#include "dxbc_converter.hpp"
#include "airconv_error.hpp"
#include "nt/air_builder.hpp"
#include "llvm/IR/Constants.h"
#include "llvm/Support/raw_ostream.h"
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace mullion::dxbc {

using namespace microsoft;

namespace {

template <class... Ts> struct layered_visitor : Ts... {
  using Ts::operator()...;
};
template <class... Ts> layered_visitor(Ts...) -> layered_visitor<Ts...>;

// component c of a swizzle (Swizzle's operator[] is not const)
uint32_t
lane(const Swizzle &swizzle, unsigned c) {
  return std::array<int, 4>(swizzle)[c];
}

// a number for each instruction type, without RTTI: the address of a variable of the type's own
template <typename T> struct TypeTag {
  static constexpr char id = 0;
};

// combines value numbers (a 64-bit hash: equal numbers mean the same computation)
constexpr uint64_t
mix(uint64_t h, uint64_t v) {
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  h ^= h >> 31;
  h *= 0xbf58476d1ce4e5b9ull;
  return h ^ (h >> 29);
}

enum Tag : uint64_t {
  kTagUndefined = 1,
  kTagImmediate,
  kTagInput,
  kTagConstantBuffer,
  kTagImmediateConstantBuffer,
  kTagModifier,
  kTagMerge,
  kTagInstruction,
};

// what one register component holds
struct Value {
  // bit j: the value depends on input vertex j of the triangle
  uint8_t inputs = 0;
  // bit r: the value was written inside the body of forced branch r
  uint64_t regions = 0;
  // the computation that makes the value, where reading input vertex j counts the same for every j: two emits whose
  // outputs have equal numbers compute them alike from their own vertices
  uint64_t number = 0;
};

using Value4 = std::array<Value, 4>;
using Literal4 = std::array<std::optional<uint32_t>, 4>;

struct State {
  std::vector<Value4> temps;
  std::vector<Value4> outputs;
  // a known constant value of an output component (only through `mov o, l(x)`)
  std::vector<Literal4> output_literals;

  // the state where control flow from `o` joins this state's at block `join`
  void
  merge(const State &o, const BasicBlock *join) {
    auto merge_value = [&](Value &a, const Value &b, uint64_t where) {
      if (a.number != b.number)
        a.number = mix(mix(mix(kTagMerge, (uint64_t)(uintptr_t)join), where), mix(a.number, b.number));
      a.inputs |= b.inputs;
      a.regions |= b.regions;
    };
    for (size_t i = 0; i < temps.size(); i++)
      for (unsigned c = 0; c < 4; c++)
        merge_value(temps[i][c], o.temps[i][c], i * 4 + c);
    for (size_t i = 0; i < outputs.size(); i++)
      for (unsigned c = 0; c < 4; c++) {
        merge_value(outputs[i][c], o.outputs[i][c], 0x10000 + i * 4 + c);
        if (output_literals[i][c] != o.output_literals[i][c])
          output_literals[i][c] = std::nullopt;
      }
  }
};

class LayeredAnalysis {
public:
  LayeredAnalysis(const SM50ShaderInternal *gs, uint32_t rtai_reg, uint32_t rtai_comp, uint32_t compared_registers) :
      gs_(gs),
      rtai_reg_(rtai_reg),
      rtai_comp_(rtai_comp),
      compared_registers_(compared_registers) {}

  // the face count, or 0 when the shader does not qualify
  uint32_t
  run() {
    BasicBlock *entry = gs_->entry();

    // the reachable blocks, and how many edges come into each
    std::vector<BasicBlock *> stack{entry};
    std::unordered_set<BasicBlock *> reachable{entry};
    std::unordered_map<BasicBlock *, uint32_t> indegree;
    while (!stack.empty()) {
      auto bb = stack.back();
      stack.pop_back();
      auto succs = successors(bb);
      if (!succs)
        return 0;
      for (auto succ : *succs) {
        if (!succ)
          return 0;
        indegree[succ]++;
        if (reachable.insert(succ).second)
          stack.push_back(succ);
      }
    }

    State initial;
    initial.temps.resize(gs_->shader_info.tempRegisterCount);
    initial.outputs.resize(gs_->max_output_register);
    initial.output_literals.resize(gs_->max_output_register);
    for (size_t i = 0; i < initial.temps.size(); i++)
      for (unsigned c = 0; c < 4; c++)
        initial.temps[i][c].number = mix(mix(kTagUndefined, i), c);
    for (size_t i = 0; i < initial.outputs.size(); i++)
      for (unsigned c = 0; c < 4; c++)
        initial.outputs[i][c].number = mix(mix(kTagUndefined, 0x10000 + i), c);
    in_state_[entry] = initial;

    // the blocks in topological order: a cycle (a loop) leaves blocks unvisited, and the shader does not qualify
    std::queue<BasicBlock *> ready;
    ready.push(entry);
    size_t visited = 0;
    while (!ready.empty()) {
      auto bb = ready.front();
      ready.pop();
      visited++;
      State state = in_state_[bb];
      if (!visit_block(bb, state))
        return 0;
      auto succs = *successors(bb);
      for (auto succ : succs) {
        if (auto it = in_state_.find(succ); it != in_state_.end())
          it->second.merge(state, succ);
        else
          in_state_.emplace(succ, state);
        if (--indegree[succ] == 0)
          ready.push(succ);
      }
    }
    if (visited != reachable.size())
      return 0;

    // the faces are exactly 0, 1, ..., N-1, each emitted once
    uint32_t faces = faces_.size();
    if (faces == 0 || faces > 8)
      return 0;
    for (uint32_t f = 0; f < faces; f++)
      if (!faces_.contains(f))
        return 0;
    return faces;
  }

  std::unordered_set<const BasicBlock *> forced;

private:
  const SM50ShaderInternal *gs_;
  uint32_t rtai_reg_;
  uint32_t rtai_comp_;
  // the output registers whose values the rasterizer takes, as a mask: emits must agree on these
  uint32_t compared_registers_;
  std::unordered_map<BasicBlock *, State> in_state_;
  // the forced branch regions that enclose each block
  std::unordered_map<const BasicBlock *, uint64_t> enclosing_;
  std::set<uint32_t> faces_;

  // the emit state of the current block
  uint32_t strip_vertices_ = 0;
  uint32_t strip_face_ = 0;
  std::vector<Value4> strip_outputs_;
  uint64_t current_regions_ = 0;

  // the blocks a block goes on to, or nothing for a terminator the analysis does not take
  static std::optional<std::vector<BasicBlock *>>
  successors(BasicBlock *bb) {
    return std::visit(
        layered_visitor{
            [](const BasicBlockUnconditionalBranch &b) -> std::optional<std::vector<BasicBlock *>> {
              return std::vector<BasicBlock *>{b.target};
            },
            [](const BasicBlockConditionalBranch &b) -> std::optional<std::vector<BasicBlock *>> {
              return std::vector<BasicBlock *>{b.true_branch, b.false_branch};
            },
            [](const BasicBlockReturn &) -> std::optional<std::vector<BasicBlock *>> {
              return std::vector<BasicBlock *>{};
            },
            [](const auto &) -> std::optional<std::vector<BasicBlock *>> { return std::nullopt; },
        },
        bb->target
    );
  }

  // the blocks reachable from `from` without passing through `stop`
  static std::unordered_set<BasicBlock *>
  region_body(BasicBlock *from, BasicBlock *stop, bool &reaches_stop) {
    std::unordered_set<BasicBlock *> body;
    std::vector<BasicBlock *> stack{from};
    reaches_stop = false;
    while (!stack.empty()) {
      auto bb = stack.back();
      stack.pop_back();
      if (bb == stop) {
        reaches_stop = true;
        continue;
      }
      if (!body.insert(bb).second)
        continue;
      auto succs = *successors(bb);
      for (auto succ : succs)
        stack.push_back(succ);
    }
    return body;
  }

  static std::optional<uint32_t>
  constant_index(const OperandIndex &index) {
    if (auto value = std::get_if<uint32_t>(&index))
      return *value;
    return std::nullopt;
  }

  // the values of a source operand's components, after its swizzle and modifiers
  std::optional<Value4>
  read(const SrcOperand &src, const State &state, Literal4 *literal = nullptr) {
    Value4 result{};
    if (literal)
      *literal = Literal4{};
    const SrcOperandCommon *common = nullptr;
    bool ok = std::visit(
        layered_visitor{
            [&](const SrcOperandImmediate32 &imm) {
              common = &imm._;
              for (unsigned c = 0; c < 4; c++) {
                result[c].number = mix(kTagImmediate, imm.uvalue[lane(imm._.swizzle, c)]);
                if (literal && !imm._.abs && !imm._.neg)
                  (*literal)[c] = imm.uvalue[lane(imm._.swizzle, c)];
              }
              return true;
            },
            [&](const SrcOperandTemp &temp) {
              if (temp.regid >= state.temps.size())
                return false;
              common = &temp._;
              for (unsigned c = 0; c < 4; c++)
                result[c] = state.temps[temp.regid][lane(temp._.swizzle, c)];
              return true;
            },
            [&](const SrcOperandInputICP &input) {
              auto cpid = constant_index(input.cpid);
              auto regid = constant_index(input.regid);
              if (!cpid || !regid || *cpid >= 3)
                return false;
              common = &input._;
              // the number leaves the vertex out: emit j reads vertex j
              for (unsigned c = 0; c < 4; c++)
                result[c] = {uint8_t(1u << *cpid), 0, mix(mix(kTagInput, *regid), lane(input._.swizzle, c))};
              return true;
            },
            [&](const SrcOperandConstantBuffer &cb) {
              auto range = constant_index(cb.rangeindex);
              auto reg = constant_index(cb.regindex);
              if (!range || !reg)
                return false;
              common = &cb._;
              for (unsigned c = 0; c < 4; c++)
                result[c].number =
                    mix(mix(mix(mix(kTagConstantBuffer, cb.rangeid), *range), *reg), lane(cb._.swizzle, c));
              return true;
            },
            [&](const SrcOperandImmediateConstantBuffer &icb) {
              auto reg = constant_index(icb.regindex);
              if (!reg)
                return false;
              common = &icb._;
              for (unsigned c = 0; c < 4; c++)
                result[c].number = mix(mix(kTagImmediateConstantBuffer, *reg), lane(icb._.swizzle, c));
              return true;
            },
            // indexable temps, other inputs and system values
            [](const auto &) { return false; },
        },
        src
    );
    if (!ok)
      return std::nullopt;
    uint64_t modifiers = (common->abs ? 1 : 0) | (common->neg ? 2 : 0) | (uint64_t(common->read_type) << 2);
    if (modifiers)
      for (auto &value : result)
        value.number = mix(mix(kTagModifier, modifiers), value.number);
    return result;
  }

  bool
  write(const DstOperand &dst, const Value4 &value, State &state, const Literal4 *literal = nullptr) {
    return std::visit(
        layered_visitor{
            [](const DstOperandNull &) { return true; },
            [&](const DstOperandTemp &temp) {
              if (temp.regid >= state.temps.size())
                return false;
              for (unsigned c = 0; c < 4; c++)
                if (temp._.mask & (1 << c)) {
                  state.temps[temp.regid][c] = value[c];
                  state.temps[temp.regid][c].regions |= current_regions_;
                }
              return true;
            },
            [&](const DstOperandOutput &out) {
              if (out.regid >= state.outputs.size())
                return false;
              for (unsigned c = 0; c < 4; c++)
                if (out._.mask & (1 << c)) {
                  state.outputs[out.regid][c] = value[c];
                  state.outputs[out.regid][c].regions |= current_regions_;
                  state.output_literals[out.regid][c] = literal ? (*literal)[c] : std::nullopt;
                }
              return true;
            },
            [](const auto &) { return false; },
        },
        dst
    );
  }

  // the number of an instruction's result: its kind, the fields that choose its operation, and `which` result
  template <typename T>
  static uint64_t
  operation(const T &inst, uint64_t which) {
    uint64_t h = mix(mix(kTagInstruction, (uint64_t)(uintptr_t)&TypeTag<T>::id), which);
    if constexpr (requires { inst.op; })
      h = mix(h, uint64_t(inst.op));
    if constexpr (requires { inst.cmp; })
      h = mix(h, uint64_t(inst.cmp));
    if constexpr (requires { inst.is_signed; })
      h = mix(h, uint64_t(inst.is_signed));
    if constexpr (requires { inst.dimension; })
      h = mix(h, uint64_t(inst.dimension));
    if constexpr (requires { inst._.saturate; })
      h = mix(h, uint64_t(inst._.saturate));
    return h;
  }

  // an operation on component c of each source, for each c
  template <typename T, typename... Srcs>
  std::optional<Value4>
  compute(const T &inst, uint64_t which, const State &state, const Srcs &...srcs) {
    Value4 result{};
    for (unsigned c = 0; c < 4; c++)
      result[c].number = operation(inst, which);
    for (const SrcOperand *src : {&srcs...}) {
      auto value = read(*src, state);
      if (!value)
        return std::nullopt;
      for (unsigned c = 0; c < 4; c++) {
        result[c].inputs |= (*value)[c].inputs;
        result[c].regions |= (*value)[c].regions;
        result[c].number = mix(result[c].number, (*value)[c].number);
      }
    }
    return result;
  }

  template <typename T, typename... Srcs>
  bool
  componentwise(const T &inst, State &state, const DstOperand &dst, const Srcs &...srcs) {
    auto result = compute(inst, 0, state, srcs...);
    return result && write(dst, *result, state);
  }

  // an instruction with two results, both made from the sources before either is written
  template <typename T, typename... Srcs>
  bool
  two_results(const T &inst, State &state, const DstOperand &dst0, const DstOperand &dst1, const Srcs &...srcs) {
    auto first = compute(inst, 0, state, srcs...);
    auto second = compute(inst, 1, state, srcs...);
    return first && second && write(dst0, *first, state) && write(dst1, *second, state);
  }

  bool
  emit(const InstEmit &inst, State &state) {
    uint32_t corner = strip_vertices_;
    if (inst.stream != 0 || corner >= 3)
      return false;
    // each output depends only on input vertex `corner`, and on work that runs whenever this emit runs
    for (auto &reg : state.outputs)
      for (auto &value : reg) {
        if (value.inputs & ~(1u << corner))
          return false;
        if (value.regions & ~current_regions_)
          return false;
      }
    auto face = state.output_literals[rtai_reg_][rtai_comp_];
    if (!face)
      return false;
    if (corner == 0) {
      strip_face_ = *face;
      strip_outputs_ = state.outputs;
    } else {
      if (strip_face_ != *face)
        return false;
      // and is computed as emit 0's is
      for (uint32_t reg = 0; reg < state.outputs.size(); reg++)
        if (compared_registers_ >> reg & 1)
          for (unsigned c = 0; c < 4; c++)
            if (state.outputs[reg][c].number != strip_outputs_[reg][c].number)
              return false;
    }
    strip_vertices_++;
    return true;
  }

  bool
  cut(const InstCut &inst) {
    if (inst.stream != 0)
      return false;
    if (strip_vertices_ == 0)
      return true;
    if (strip_vertices_ != 3)
      return false;
    if (!faces_.insert(strip_face_).second)
      return false;
    strip_vertices_ = 0;
    return true;
  }

  template <typename T>
  bool
  visit_instruction(const T &inst, State &state) {
    if constexpr (std::is_same_v<T, InstNop>) {
      return true;
    } else if constexpr (std::is_same_v<T, InstEmit>) {
      return emit(inst, state);
    } else if constexpr (std::is_same_v<T, InstCut>) {
      return cut(inst);
    } else if constexpr (std::is_same_v<T, InstMov>) {
      Literal4 literal;
      auto value = read(inst.src, state, &literal);
      if (!value)
        return false;
      if (inst._.saturate)
        for (auto &v : *value)
          v.number = mix(operation(inst, 0), v.number);
      return write(inst.dst, *value, state, inst._.saturate ? nullptr : &literal);
    } else if constexpr (std::is_same_v<T, InstMovConditional>) {
      return componentwise(inst, state, inst.dst, inst.src_cond, inst.src0, inst.src1);
    } else if constexpr (std::is_same_v<T, InstDotProduct>) {
      auto a = read(inst.src0, state);
      auto b = read(inst.src1, state);
      if (!a || !b || inst.dimension > 4)
        return false;
      Value all{0, 0, operation(inst, 0)};
      for (unsigned c = 0; c < inst.dimension; c++)
        for (auto &v : {(*a)[c], (*b)[c]}) {
          all.inputs |= v.inputs;
          all.regions |= v.regions;
          all.number = mix(all.number, v.number);
        }
      return write(inst.dst, Value4{all, all, all, all}, state);
    } else if constexpr (std::is_same_v<T, InstSinCos>) {
      return two_results(inst, state, inst.dst_sin, inst.dst_cos, inst.src);
    } else if constexpr (std::is_same_v<T, InstConvert> || std::is_same_v<T, InstFloatUnaryOp> ||
                         std::is_same_v<T, InstIntegerUnaryOp>) {
      return componentwise(inst, state, inst.dst, inst.src);
    } else if constexpr (std::is_same_v<T, InstIntegerCompare> || std::is_same_v<T, InstFloatCompare> ||
                         std::is_same_v<T, InstFloatBinaryOp> || std::is_same_v<T, InstIntegerBinaryOp>) {
      return componentwise(inst, state, inst.dst, inst.src0, inst.src1);
    } else if constexpr (std::is_same_v<T, InstIntegerBinaryOpWithTwoDst>) {
      return two_results(inst, state, inst.dst_hi, inst.dst_low, inst.src0, inst.src1);
    } else if constexpr (std::is_same_v<T, InstFloatMAD> || std::is_same_v<T, InstIntegerMAD> ||
                         std::is_same_v<T, InstMaskedSumOfAbsDiff> || std::is_same_v<T, InstExtractBits>) {
      return componentwise(inst, state, inst.dst, inst.src0, inst.src1, inst.src2);
    } else if constexpr (std::is_same_v<T, InstBitFiledInsert>) {
      return componentwise(inst, state, inst.dst, inst.src0, inst.src1, inst.src2, inst.src3);
    } else {
      // resource access, derivatives, atomics, swapc, ...
      return false;
    }
  }

  bool
  visit_block(BasicBlock *bb, State &state) {
    current_regions_ = enclosing_[bb];
    strip_vertices_ = 0;

    bool ok = true;
    bb->instructions.for_each([&](const auto &inst) {
      if (ok)
        ok = visit_instruction(inst, state);
    });
    if (!ok)
      return false;
    // every strip ends with a cut in its block
    if (strip_vertices_ != 0)
      return false;

    return std::visit(
        layered_visitor{
            // a return inside a forced body would skip later faces
            [&](const BasicBlockReturn &) { return current_regions_ == 0; },
            [](const BasicBlockUnconditionalBranch &) { return true; },
            [&](const BasicBlockConditionalBranch &branch) {
              auto value = read(branch.cond.operand, state);
              if (!value)
                return false;
              Value cond = (*value)[0];
              // the condition may not use work that can be skipped here
              if (cond.regions & ~current_regions_)
                return false;
              // the same for all vertices: the branch stays
              if (cond.inputs == 0)
                return true;
              // per vertex: the branch always goes to its true target. only an `if` without an `else`, whose false
              // target is where the true body joins
              if (branch.true_branch == branch.false_branch)
                return false;
              bool reaches_join = false;
              auto body = region_body(branch.true_branch, branch.false_branch, reaches_join);
              if (!reaches_join)
                return false;
              uint32_t region = forced.size();
              if (region >= 64)
                return false;
              for (auto body_bb : body)
                enclosing_[body_bb] |= uint64_t(1) << region;
              forced.insert(bb);
              return true;
            },
            [](const auto &) { return false; },
        },
        bb->target
    );
  }
};

} // namespace

void
analyze_dxbc_layered_geometry_shader(SM50ShaderInternal *gs) {
  gs->gs_layered_faces = 0;
  gs->gs_layered_forced_branches.clear();

  if (!gs->dxil.empty() || gs->bbs.empty())
    return;
  if (gs->gs_input_primitive != D3D10_SB_PRIMITIVE_TRIANGLE)
    return;
  if (gs->gs_output_topology != D3D10_SB_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP)
    return;
  if (gs->gs_instance_count > 1)
    return;
  auto &info = gs->shader_info;
  if (!info.srvMap.empty() || !info.uavMap.empty() || !info.samplerMap.empty() || !info.tgsmMap.empty() ||
      !info.indexableTempRegisterCounts.empty())
    return;
  if (!gs->clip_distance_scalars.empty() || !gs->cull_distance_scalars.empty())
    return;

  uint32_t rtai_reg = ~0u, rtai_comp = 0, compared = 0;
  bool has_position = false;
  for (auto &element : gs->output_signature) {
    if (element.stream() != 0 || element.reg() >= 32)
      return;
    switch (element.systemValue()) {
    case D3D10_SB_NAME_UNDEFINED:
      compared |= 1u << element.reg();
      break;
    case D3D10_SB_NAME_POSITION:
      has_position = true;
      compared |= 1u << element.reg();
      break;
    case D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX:
      if (rtai_reg != ~0u || std::popcount(element.mask() & 0xFu) != 1)
        return;
      rtai_reg = element.reg();
      rtai_comp = std::countr_zero(element.mask() & 0xFu);
      break;
    default:
      return;
    }
  }
  if (!has_position || rtai_reg == ~0u || rtai_reg >= gs->max_output_register)
    return;

  LayeredAnalysis analysis(gs, rtai_reg, rtai_comp, compared);
  uint32_t faces = analysis.run();
  if (!faces)
    return;
  gs->gs_layered_faces = faces;
  gs->gs_layered_forced_branches = std::move(analysis.forced);
}

llvm::Error
convert_dxbc_vertex_for_layered_geometry_shader(
    const SM50ShaderInternal *pVertexStage, const char *name, const SM50ShaderInternal *pGeometryStage,
    llvm::LLVMContext &context, llvm::Module &module, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  const uint32_t faces = pGeometryStage ? pGeometryStage->gs_layered_faces : 0;
  if (!faces)
    return llvm::make_error<UnsupportedFeature>("the geometry shader is not layered");

  SM50_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout = nullptr;
  args_get_data<SM50_SHADER_IA_INPUT_LAYOUT, SM50_SHADER_IA_INPUT_LAYOUT_DATA>(pArgs, &ia_layout);
  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_FLAG shader_flags = {};
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common)) {
    metal_version = sm50_common->metal_version;
    shader_flags = sm50_common->flags;
  }
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  if (args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig))
    return llvm::make_error<UnsupportedFeature>("a layered geometry shader with a root signature");

  auto vs_info = &pVertexStage->shader_info;
  auto gs_info = &pGeometryStage->shader_info;
  const uint32_t vs_max_input = pVertexStage->max_input_register;
  const uint32_t vs_max_output = pVertexStage->max_output_register;
  const uint32_t gs_max_output = pGeometryStage->max_output_register;

  // the function takes the vertex shader's inputs and gives the geometry shader's outputs
  auto func_signature = pVertexStage->func_signature; // copy
  func_signature.ClearOutputs();

  IREffect prologue([](auto) { return std::monostate(); });
  IRValue vs_epilogue([](struct context) -> pvalue { return nullptr; });
  IRValue epilogue([](struct context ctx) -> pvalue {
    return llvm::ConstantAggregateZero::get(ctx.function->getReturnType());
  });
  io_binding_map res_vs, res_gs, res_out;
  air::AirType types(context);

  {
    SignatureContext sig_ctx(prologue, vs_epilogue, func_signature, res_vs);
    sig_ctx.ia_layout = ia_layout;
    sig_ctx.skip_vertex_output = true;
    for (auto &p : pVertexStage->signature_handlers)
      p(sig_ctx);
  }

  // the outputs, as a vertex shader's are (handle_signature_vs): the position, the layer, and each register with
  // varyings as all its components, typed as the geometry shader's mesh outputs are
  uint32_t position_reg = ~0u, declared = 0;
  for (auto &element : pGeometryStage->output_signature) {
    auto reg = element.reg();
    auto mask = element.mask() & 0xFu;
    switch (element.systemValue()) {
    case D3D10_SB_NAME_POSITION: {
      position_reg = reg;
      auto index = func_signature.DefineOutput(air::OutputPosition{.type = air::msl_float4});
      epilogue >> pop_output_reg_sanitize_pos(reg, mask, index);
      break;
    }
    case D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX: {
      auto index = func_signature.DefineOutput(air::OutputRenderTargetArrayIndex{});
      epilogue >> pop_output_reg(reg, mask, index);
      break;
    }
    case D3D10_SB_NAME_UNDEFINED: {
      if (std::exchange(declared, declared | 1u << reg) >> reg & 1)
        break;
      auto component_types = pGeometryStage->register_types(reg, element.componentType());
      for (unsigned c = 0; c < 4; c++) {
        auto index = func_signature.DefineOutput(
            air::OutputVertex{.user = varying_name(reg, c), .type = varying_type(component_types[c])}
        );
        epilogue >> pop_output_reg(reg, 1 << c, index);
      }
      break;
    }
    default:
      return llvm::make_error<UnsupportedFeature>("an unexpected system value in a layered geometry shader");
    }
  }
  if (position_reg == ~0u)
    return llvm::make_error<UnsupportedFeature>("a layered geometry shader without a position");

  uint32_t vertex_idx = func_signature.DefineInput(air::InputVertexID{});
  uint32_t base_vertex_idx = func_signature.DefineInput(air::InputBaseVertex{});
  uint32_t instance_idx = func_signature.DefineInput(air::InputInstanceID{});
  uint32_t base_instance_idx = func_signature.DefineInput(air::InputBaseInstance{});

  // the vertex shader's resources take the vertex stage's usual bindings, the geometry shader's the second ones
  auto vs_binding = setup_binding_table2(vs_info, func_signature, module);
  auto gs_binding = setup_binding_table2(
      gs_info, func_signature, module, SM50_BINDING_INDEX_CONSTANT_BUFFER2, SM50_BINDING_INDEX_ARGUMENT_TABLE2
  );
  if (!vs_binding || !gs_binding)
    return llvm::make_error<UnsupportedFeature>("missing resource binding");

  auto raster_scale = DefineRasterScale(func_signature);
  auto [function, function_metadata] = func_signature.CreateFunction(name, context, module, 0, false);

  auto entry_bb = llvm::BasicBlock::Create(context, "entry", function);
  auto vs_end_bb = llvm::BasicBlock::Create(context, "vs_end", function);
  auto gs_end_bb = llvm::BasicBlock::Create(context, "gs_end", function);
  llvm::IRBuilder<> builder(entry_bb);
  llvm::raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(
      {
          .sampleNaNToZero = bool(shader_flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO),
          .defuseFma = bool(shader_flags & SM50_SHADER_FLAG_DEFUSE_FMA),
          .noAtomicMinMax64 = bool(shader_flags & SM50_SHADER_FLAG_NO_ATOMIC_MINMAX64),
          .noCustomBorder = bool(shader_flags & SM50_SHADER_FLAG_NO_CUSTOM_BORDER),
          .noMinMaxFilter = bool(shader_flags & SM50_SHADER_FLAG_NO_MINMAX_FILTER),
      },
      builder, nulldbg
  );
  air.RasterScale = function->getArg(raster_scale);
  setup_metal_version(module, metal_version);

  // Metal instance i is face (i % faces) of D3D instance (i / faces): the draw multiplies its instance count and its
  // base instance by `faces`
  auto faces_const = builder.getInt32(faces);
  auto metal_instance = function->getArg(instance_idx);
  auto metal_base_instance = function->getArg(base_instance_idx);
  auto local_instance = builder.CreateSub(metal_instance, metal_base_instance);
  auto face = builder.CreateURem(local_instance, faces_const);

  res_vs.vertex_id_with_base = function->getArg(vertex_idx);
  res_vs.base_vertex_id = function->getArg(base_vertex_idx);
  res_vs.vertex_id = builder.CreateSub(res_vs.vertex_id_with_base, res_vs.base_vertex_id);
  res_vs.instance_id = builder.CreateUDiv(local_instance, faces_const);
  res_vs.base_instance_id = builder.CreateUDiv(metal_base_instance, faces_const);
  res_vs.instance_id_with_base = builder.CreateAdd(res_vs.instance_id, res_vs.base_instance_id);

  // every register file is an alloca in the entry block, which LLVM promotes. the outputs start as 0, as a vertex
  // shader's do (convert_dxbc_vertex_shader): Metal's behaviour is undefined for an INF or a NaN in an interpolant
  auto make_file = [&](register_file &file, uint32_t count, bool zero) {
    auto int_type = llvm::ArrayType::get(types._int4, count);
    file.ptr_int4 = builder.CreateAlloca(int_type);
    if (zero)
      builder.CreateStore(llvm::ConstantAggregateZero::get(int_type), file.ptr_int4);
    file.ptr_float4 =
        builder.CreateBitCast(file.ptr_int4, llvm::ArrayType::get(types._float4, count)->getPointerTo());
  };
  make_file(res_vs.input, vs_max_input, false);
  res_vs.input_element_count = vs_max_input;
  make_file(res_vs.output, vs_max_output, true);
  res_vs.output_element_count = vs_max_output;

  // the geometry shader's input: 3 vertices, which all hold this one (point 3 of the file comment)
  auto vs_output_type = llvm::ArrayType::get(types._int4, vs_max_output);
  auto gs_input_type = llvm::ArrayType::get(vs_output_type, 3);
  res_gs.input.ptr_int4 = builder.CreateAlloca(gs_input_type);
  res_gs.input.ptr_float4 = builder.CreateBitCast(
      res_gs.input.ptr_int4,
      llvm::ArrayType::get(llvm::ArrayType::get(types._float4, vs_max_output), 3)->getPointerTo()
  );
  res_gs.input_element_count = vs_max_output;
  make_file(res_gs.output, gs_max_output, true);
  res_gs.output_element_count = gs_max_output;
  res_gs.gs_instance_id = builder.getInt32(0);

  // the outputs of the face's first emit. a face that emits nothing (its uniform condition is false) leaves its
  // position outside the clip volume, so the rasterizer culls the triangle
  auto gs_output_type = llvm::ArrayType::get(types._int4, gs_max_output);
  make_file(res_out.output, gs_max_output, true);
  res_out.output_element_count = gs_max_output;
  builder.CreateStore(
      llvm::ConstantVector::get({air.getFloat(2.0f), air.getFloat(2.0f), air.getFloat(2.0f), air.getFloat(1.0f)}),
      builder.CreateConstInBoundsGEP2_32(
          llvm::ArrayType::get(types._float4, gs_max_output), res_out.output.ptr_float4, 0, position_reg
      )
  );
  auto captured = builder.CreateAlloca(types._int, nullptr, "captured");
  builder.CreateStore(builder.getInt32(0), captured);

  setup_temp_register(vs_info, res_vs, types, module, builder);
  setup_immediate_constant_buffer(vs_info, res_vs, types, module, builder);
  setup_temp_register(gs_info, res_gs, types, module, builder);
  setup_immediate_constant_buffer(gs_info, res_gs, types, module, builder);

  struct context ctx_vs{
      .builder = builder,
      .air = air,
      .binding = *vs_binding,
      .llvm = context,
      .module = module,
      .function = function,
      .resource = res_vs,
      .types = types,
      .pso_sample_mask = 0xffffffff,
      .shader_type = pVertexStage->shader_type,
      .metal_version = metal_version,
      .simd_width = sm50_common ? sm50_common->simd_width : 0,
  };

  if (auto err = prologue.build(ctx_vs).takeError())
    return err;

  // 1. the vertex shader
  auto vs_entry = convert_basicblocks(pVertexStage->entry(), ctx_vs, vs_end_bb);
  if (auto err = vs_entry.takeError())
    return err;
  builder.CreateBr(vs_entry.get());

  // 2. its outputs into the geometry shader's 3 input vertices
  builder.SetInsertPoint(vs_end_bb);
  for (uint32_t reg = 0; reg < vs_max_output; reg++) {
    auto value = builder.CreateLoad(
        types._int4, builder.CreateConstInBoundsGEP2_32(vs_output_type, res_vs.output.ptr_int4, 0, reg)
    );
    for (uint32_t slot = 0; slot < 3; slot++)
      builder.CreateStore(
          value, builder.CreateGEP(
                     gs_input_type, res_gs.input.ptr_int4,
                     {builder.getInt32(0), builder.getInt32(slot), builder.getInt32(reg)}
                 )
      );
  }

  // 3. the geometry shader, one copy for each face: copy k keeps the first emit whose layer is k, so LLVM can remove
  // the work of the other faces
  uint32_t copy_face = 0;
  uint32_t rtai_reg = ~0u, rtai_comp = 0;
  for (auto &element : pGeometryStage->output_signature)
    if (element.systemValue() == D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX) {
      rtai_reg = element.reg();
      rtai_comp = std::countr_zero(element.mask() & 0xFu);
    }
  if (rtai_reg == ~0u)
    return llvm::make_error<UnsupportedFeature>("a layered geometry shader without a layer");
  res_gs.call_emit = [&](uint32_t) -> IREffect {
    auto layer = builder.CreateExtractElement(
        builder.CreateLoad(
            types._int4, builder.CreateConstInBoundsGEP2_32(gs_output_type, res_gs.output.ptr_int4, 0, rtai_reg)
        ),
        rtai_comp
    );
    auto take = builder.CreateAnd(
        builder.CreateICmpEQ(layer, builder.getInt32(copy_face)),
        builder.CreateICmpEQ(builder.CreateLoad(types._int, captured), builder.getInt32(0))
    );
    for (uint32_t reg = 0; reg < gs_max_output; reg++) {
      auto src = builder.CreateConstInBoundsGEP2_32(gs_output_type, res_gs.output.ptr_int4, 0, reg);
      auto dst = builder.CreateConstInBoundsGEP2_32(gs_output_type, res_out.output.ptr_int4, 0, reg);
      builder.CreateStore(
          builder.CreateSelect(take, builder.CreateLoad(types._int4, src), builder.CreateLoad(types._int4, dst)), dst
      );
    }
    builder.CreateStore(
        builder.CreateOr(builder.CreateLoad(types._int, captured), builder.CreateZExt(take, types._int)), captured
    );
    co_return {};
  };
  res_gs.call_cut = [](uint32_t) -> IREffect { co_return {}; };

  struct context ctx_gs{
      .builder = builder,
      .air = air,
      .binding = *gs_binding,
      .llvm = context,
      .module = module,
      .function = function,
      .resource = res_gs,
      .types = types,
      .pso_sample_mask = 0xffffffff,
      .shader_type = pGeometryStage->shader_type,
      .metal_version = metal_version,
      .own_signature = false,
      .forced_true_branches = &pGeometryStage->gs_layered_forced_branches,
  };

  auto face_switch = builder.CreateSwitch(face, gs_end_bb, faces);
  for (uint32_t k = 0; k < faces; k++) {
    auto face_bb = llvm::BasicBlock::Create(context, "face", function);
    face_switch->addCase(builder.getInt32(k), face_bb);
    builder.SetInsertPoint(face_bb);
    copy_face = k;
    auto gs_entry = convert_basicblocks(pGeometryStage->entry(), ctx_gs, gs_end_bb);
    if (auto err = gs_entry.takeError())
      return err;
    builder.CreateBr(gs_entry.get());
  }

  // 4. the outputs the face's emit kept
  builder.SetInsertPoint(gs_end_bb);
  struct context ctx_out{
      .builder = builder,
      .air = air,
      .binding = *gs_binding,
      .llvm = context,
      .module = module,
      .function = function,
      .resource = res_out,
      .types = types,
      .pso_sample_mask = 0xffffffff,
      .shader_type = pVertexStage->shader_type,
      .metal_version = metal_version,
      .own_signature = false,
  };
  auto result = epilogue.build(ctx_out);
  if (auto err = result.takeError())
    return err;
  builder.CreateRet(result.get());

  module.getOrInsertNamedMetadata("air.vertex")->addOperand(function_metadata);
  return llvm::Error::success();
}

} // namespace mullion::dxbc
