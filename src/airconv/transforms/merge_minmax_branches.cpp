#include "merge_minmax_branches.hpp"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/Local.h"
#include <optional>

namespace llvm::air {

namespace {

// a sample's branch: Head branches to Reduce (the emulation) or straight to Join, which Reduce goes to as well
struct Diamond {
  BranchInst *Branch;
  BasicBlock *Head, *Reduce, *Join;
};

std::optional<Diamond>
Match(BasicBlock *Head, unsigned Kind) {
  auto Branch = dyn_cast_or_null<BranchInst>(Head->getTerminator());
  if (!Branch || !Branch->isConditional() || !Branch->getMetadata(Kind))
    return {};
  auto Reduce = Branch->getSuccessor(0), Join = Branch->getSuccessor(1);
  if (Reduce == Join || Reduce->getSinglePredecessor() != Head || Reduce->getSingleSuccessor() != Join ||
      !Join->hasNPredecessors(2))
    return {};
  return Diamond{Branch, Head, Reduce, Join};
}

// a derivative has no memory effects, though its declaration does not say so (AIRBuilder::CreateDerivative)
bool
Derivative(const Instruction &I) {
  auto Call = dyn_cast<CallInst>(&I);
  auto Callee = Call ? Call->getCalledFunction() : nullptr;
  return Callee && (Callee->getName().startswith("air.dfdx") || Callee->getName().startswith("air.dfdy"));
}

// whether `A` and `B` are the same value wherever both are: the same operations on the same values, reading only
// memory a shader cannot write (address space 2: argument buffers and descriptor heaps). two loads of one sampler's
// metadata are two instructions until something removes the second
struct Equal {
  DenseMap<std::pair<Value *, Value *>, bool> Known;

  bool
  operator()(Value *A, Value *B, unsigned Depth = 16) {
    if (A == B)
      return true;
    auto I = dyn_cast<Instruction>(A), J = dyn_cast<Instruction>(B);
    if (!I || !J || !Depth || isa<PHINode>(I) || !I->isSameOperationAs(J) ||
        I->getNumOperands() != J->getNumOperands() || I->mayWriteToMemory())
      return false;
    if (I->mayReadFromMemory()) {
      auto Load = dyn_cast<LoadInst>(I);
      if (!Load || Load->isVolatile() || Load->getPointerAddressSpace() != 2)
        return false;
    }
    if (auto Found = Known.find({A, B}); Found != Known.end())
      return Found->second;
    bool Same = true;
    for (unsigned i = 0; Same && i < I->getNumOperands(); i++)
      Same = (*this)(I->getOperand(i), J->getOperand(i), Depth - 1);
    return Known[{A, B}] = Same;
  }
};

// joins Second, whose head is First's join, to First
bool
Join(Diamond &First, const Diamond &Second) {
  auto Between = First.Join;
  if (!Equal()(First.Branch->getCondition(), Second.Branch->getCondition()))
    return false;
  // what the first sample's value feeds stays after it; the rest moves before the first branch. the emulation writes
  // no memory, so nothing moved past it reads otherwise
  SmallPtrSet<Instruction *, 16> After;
  SmallVector<Instruction *, 16> Before, Later;
  for (auto &Phi : Between->phis())
    After.insert(&Phi);
  auto Feeds = [&](const Instruction &I) {
    return any_of(I.operands(), [&](Value *V) {
      auto D = dyn_cast<Instruction>(V);
      return D && (After.count(D) || D->getParent() == First.Reduce);
    });
  };
  for (auto &I : *Between) {
    if (isa<PHINode>(I) || I.isTerminator())
      continue;
    if (I.mayHaveSideEffects() && !Derivative(I))
      return false;
    if (Feeds(I))
      After.insert(&I), Later.push_back(&I);
    else
      Before.push_back(&I);
  }
  // the second sample and its emulation do not read the first. the second branch's condition is the first's value
  // (Equal), which nothing after the first sample computes
  for (auto &I : *Second.Reduce)
    if (Feeds(I))
      return false;
  for (auto &Phi : Second.Join->phis())
    if (Feeds(Phi))
      return false;
  if (auto Condition = dyn_cast<Instruction>(Second.Branch->getCondition()); Condition && After.count(Condition))
    return false;

  for (auto I : Before)
    I->moveBefore(First.Branch);
  // in their order, ahead of what followed the second sample
  auto At = Second.Join->getFirstNonPHI();
  for (auto &Phi : make_early_inc_range(Between->phis()))
    Phi.moveBefore(At);
  for (auto I : Later)
    I->moveBefore(At);
  // the first emulation runs on into the second, and both arms of the first branch go to the second join
  First.Reduce->getTerminator()->eraseFromParent();
  First.Reduce->getInstList().splice(First.Reduce->end(), Second.Reduce->getInstList());
  Second.Join->replacePhiUsesWith(Second.Reduce, First.Reduce);
  Second.Join->replacePhiUsesWith(Between, First.Head);
  First.Branch->setSuccessor(1, Second.Join);
  auto Condition = Second.Branch->getCondition();
  Second.Branch->eraseFromParent();
  Between->eraseFromParent();
  Second.Reduce->eraseFromParent();
  RecursivelyDeleteTriviallyDeadInstructions(Condition);
  First.Join = Second.Join;
  return true;
}

} // namespace

PreservedAnalyses
MergeMinMaxBranchesPass::run(Function &F, FunctionAnalysisManager &AM) {
  auto Kind = F.getContext().getMDKindID(MinMaxBranchMetadata);
  SmallVector<BasicBlock *, 16> Heads;
  for (auto &BB : F)
    if (auto Branch = dyn_cast_or_null<BranchInst>(BB.getTerminator()); Branch && Branch->getMetadata(Kind))
      Heads.push_back(&BB);
  bool Changed = false;
  if (Merge) {
    SmallPtrSet<BasicBlock *, 16> Gone;
    for (auto Head : Heads) {
      if (Gone.count(Head))
        continue;
      auto First = Match(Head, Kind);
      while (First) {
        auto Second = Match(First->Join, Kind);
        auto Between = First->Join;
        if (!Second || !Join(*First, *Second))
          break;
        Gone.insert(Between);
        Changed = true;
      }
    }
  }
  for (auto &BB : F)
    if (auto Branch = dyn_cast_or_null<BranchInst>(BB.getTerminator()); Branch && Branch->getMetadata(Kind))
      Branch->setMetadata(Kind, nullptr), Changed = true;
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace llvm::air
