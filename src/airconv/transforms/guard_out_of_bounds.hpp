#pragma once
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

// a buffer access outside its view goes through `select(inside, pointer, null)` (CreateGEPInt32WithBoundCheck,
// unit_ptr). D3D writes nothing there (D3D11.3 22.17, "Out of bounds addressing on u# causes nothing to be written to
// memory"), and Metal leaves a null device pointer undefined, so each such access runs under `if (inside)`, reading 0
// outside. it runs after the converters, whose blocks a branch would split
namespace llvm::air {
class GuardOutOfBoundsPass : public PassInfoMixin<GuardOutOfBoundsPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};
} // namespace llvm::air
