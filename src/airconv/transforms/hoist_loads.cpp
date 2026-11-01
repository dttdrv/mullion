#include "hoist_loads.hpp"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Instructions.h"

namespace llvm::air {

// whether a load of device memory reads an address only its own bounds check chose: a raw buffer's units each are
// (LoadBufferValue, CreateGEPInt32WithBoundCheck), as a pointer select or, once InstCombine has folded it, a selected
// index of their address alone. moved ahead, each keeps its check's result live until the value is used, and they ran
// slower for it (raw_rows 81 -> 85 ms; with the value's select moved too, 549). a structured row's check selects the
// index that all its units' addresses share, and constant buffer rows read whole
static bool
OwnCheck(const LoadInst &Load) {
  if (Load.getPointerAddressSpace() != 1)
    return false;
  auto *Pointer = Load.getPointerOperand();
  if (isa<SelectInst>(Pointer))
    return true;
  auto *Element = dyn_cast<GetElementPtrInst>(Pointer);
  auto *Index = Element && Element->getNumIndices() == 1 ? dyn_cast<SelectInst>(Element->getOperand(1)) : nullptr;
  return Index && Index->hasOneUse();
}

// a load the pass moves: of device or constant memory, neither volatile nor atomic, nor chosen by its own bounds check
static bool
Movable(const Instruction &I) {
  auto Load = dyn_cast<LoadInst>(&I);
  return Load && Load->isSimple() && (Load->getPointerAddressSpace() == 1 || Load->getPointerAddressSpace() == 2) &&
         !OwnCheck(*Load);
}

// an instruction of the address a load reads, which moves with it: one with no effect beyond its value
static bool
Pure(const Instruction &I) {
  return !isa<PHINode>(I) && !I.mayReadOrWriteMemory() && !I.mayHaveSideEffects() && !I.isTerminator() &&
         !isa<AllocaInst>(I);
}

// the 32-bit registers a load's value takes
static unsigned
Size(const Instruction &Load) {
  return std::max<unsigned>(1, (Load.getModule()->getDataLayout().getTypeSizeInBits(Load.getType()) + 31) / 32);
}

// the first instruction in the block that does more with a load's value than gather it into a vector, select it or
// take it apart again: where the value is first waited for
static Instruction *
FirstConsumer(Instruction *Load) {
  Instruction *First = nullptr;
  SmallVector<Instruction *, 8> Work{Load};
  SmallPtrSet<Instruction *, 16> Seen;
  while (!Work.empty()) {
    auto *Cur = Work.pop_back_val();
    for (auto *User : Cur->users()) {
      auto *U = dyn_cast<Instruction>(User);
      if (!U || U->getParent() != Load->getParent() || !Seen.insert(U).second)
        continue;
      if (isa<InsertElementInst, ExtractElementInst, ShuffleVectorInst, BitCastInst, SelectInst, FreezeInst>(U))
        Work.push_back(U);
      else if (!First || U->comesBefore(First))
        First = U;
    }
  }
  return First;
}

PreservedAnalyses
HoistLoadsPass::run(Function &F, FunctionAnalysisManager &) {
  if (!Window)
    return PreservedAnalyses::all();
  bool Changed = false;
  for (auto &BB : F) {
    // the latest instruction no load may move above: one that writes memory or has effects; and the loads so far,
    // in their new order. a load moves no earlier than the first use of the one a window of 32-bit registers before
    // it, so that at most that many registers of loaded values wait ahead of what uses them: past that, Metal's
    // compiler spills them
    Instruction *Fence = nullptr;
    SmallVector<Instruction *, 64> Loads;
    auto Later = [](Instruction *A, Instruction *B) { return !A ? B : !B ? A : A->comesBefore(B) ? B : A; };
    for (auto &I : make_early_inc_range(BB)) {
      if (!Movable(I)) {
        if (I.mayWriteToMemory() || I.mayHaveSideEffects() || isa<PHINode>(I) || (I.mayReadFromMemory() && !Pure(I)))
          Fence = Later(Fence, &I);
        continue;
      }
      // the address's instructions in this block that move with the load, and the latest of what they need
      SmallVector<Instruction *, 16> Slice;
      SmallPtrSet<Instruction *, 16> InSlice;
      Instruction *Anchor = Fence;
      SmallVector<Instruction *, 16> Work{&I};
      while (!Work.empty()) {
        auto *Cur = Work.pop_back_val();
        for (auto &Op : Cur->operands()) {
          auto *Def = dyn_cast<Instruction>(Op.get());
          if (!Def || Def->getParent() != &BB || InSlice.contains(Def))
            continue;
          if (Pure(*Def) && !(Anchor && Def->comesBefore(Anchor))) {
            InSlice.insert(Def);
            Slice.push_back(Def);
            Work.push_back(Def);
          } else {
            Anchor = Later(Anchor, Def);
          }
        }
      }
      Instruction *To = Anchor ? Anchor->getNextNode() : &*BB.getFirstInsertionPt();
      unsigned Registers = Size(I);
      for (auto Earlier = Loads.rbegin(); Earlier != Loads.rend(); ++Earlier)
        if ((Registers += Size(**Earlier)) > Window) {
          if (auto *Use = FirstConsumer(*Earlier); Use && To->comesBefore(Use))
            To = Use;
          break;
        }
      // slice instructions at the insertion point stay there, before it
      while (InSlice.contains(To))
        To = To->getNextNode();
      if (To == &I || I.comesBefore(To)) {
        Loads.push_back(&I);
        continue;
      }
      // the slice's instructions after the insertion point move before it, in their order: their operands in the block
      // are slice instructions moved before them, or come before the insertion point
      llvm::sort(Slice, [](Instruction *A, Instruction *B) { return A->comesBefore(B); });
      for (auto *S : Slice)
        if (To->comesBefore(S))
          S->moveBefore(To);
      I.moveBefore(To);
      // a move within one block leaves its instructions' order numbers as they were, which comesBefore reads
      BB.renumberInstructions();
      Changed = true;
      Loads.push_back(&I);
    }
  }
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace llvm::air
