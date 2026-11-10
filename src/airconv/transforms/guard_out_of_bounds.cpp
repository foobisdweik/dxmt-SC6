#include "guard_out_of_bounds.hpp"
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

namespace llvm::air {

// the select a bounds check made, which InstCombine may have turned to `select(outside, null, pointer)`
static SelectInst *
Bounded(Value *Pointer) {
  auto Select = dyn_cast_or_null<SelectInst>(Pointer);
  return Select && (isa<ConstantPointerNull>(Select->getFalseValue()) || isa<ConstantPointerNull>(Select->getTrueValue()))
             ? Select
             : nullptr;
}

PreservedAnalyses
GuardOutOfBoundsPass::run(Function &F, FunctionAnalysisManager &AM) {
  SmallVector<std::pair<Instruction *, SelectInst *>> Accesses;
  SmallSetVector<SelectInst *, 8> Selects;
  for (auto &I : instructions(F)) {
    SelectInst *Select = nullptr;
    if (auto Pointer = getLoadStorePointerOperand(&I))
      Select = Bounded(Pointer);
    else if (auto RMW = dyn_cast<AtomicRMWInst>(&I))
      Select = Bounded(RMW->getPointerOperand());
    else if (auto Exchange = dyn_cast<AtomicCmpXchgInst>(&I))
      Select = Bounded(Exchange->getPointerOperand());
    else if (auto Call = dyn_cast<CallInst>(&I))
      for (auto &Arg : Call->args())
        if ((Select = Bounded(Arg)))
          break;
    if (Select)
      Accesses.push_back({&I, Select}), Selects.insert(Select);
  }
  for (auto [I, Select] : Accesses) {
    auto Head = I->getParent();
    bool Inverted = isa<ConstantPointerNull>(Select->getTrueValue());
    auto Inside = Inverted ? BinaryOperator::CreateNot(Select->getCondition(), "", I) : Select->getCondition();
    auto Then = SplitBlockAndInsertIfThen(Inside, I, false);
    auto Tail = I->getParent();
    I->moveBefore(Then);
    I->replaceUsesOfWith(Select, Inverted ? Select->getFalseValue() : Select->getTrueValue());
    if (!I->getType()->isVoidTy()) {
      auto Read = PHINode::Create(I->getType(), 2, "", &Tail->front());
      I->replaceAllUsesWith(Read);
      Read->addIncoming(I, Then->getParent());
      Read->addIncoming(Constant::getNullValue(I->getType()), Head);
    }
  }
  for (auto Select : Selects)
    if (Select->use_empty())
      Select->eraseFromParent();
  return Accesses.empty() ? PreservedAnalyses::all() : PreservedAnalyses::none();
}

} // namespace llvm::air
