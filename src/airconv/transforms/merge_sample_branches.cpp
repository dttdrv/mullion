#include "merge_sample_branches.hpp"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Function.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"
#include <optional>

namespace llvm::air {

namespace {

// a sample's branch: Head branches to Own (the implicit sample) or Scaled (the gradients), which both go to Join. an
// arm is one block, or a block that branches to two that meet in its last (the footprint's check before the gradients)
struct Arm {
  BasicBlock *First, *Last;
};
struct Diamond {
  BranchInst *Branch;
  Arm Own, Scaled;
  BasicBlock *Join;
};

std::optional<Arm>
Reach(BasicBlock *First, BasicBlock *Head, BasicBlock *Join) {
  if (First->getSinglePredecessor() != Head)
    return {};
  if (First->getSingleSuccessor() == Join)
    return Arm{First, First};
  auto Branch = dyn_cast_or_null<BranchInst>(First->getTerminator());
  if (!Branch || !Branch->isConditional())
    return {};
  auto A = Branch->getSuccessor(0), B = Branch->getSuccessor(1), Last = A->getSingleSuccessor();
  if (A == B || !Last || B->getSingleSuccessor() != Last || A->getSinglePredecessor() != First ||
      B->getSinglePredecessor() != First || !Last->hasNPredecessors(2) || Last->getSingleSuccessor() != Join)
    return {};
  return Arm{First, Last};
}

std::optional<Diamond>
Match(BasicBlock *Head, unsigned Kind) {
  auto Branch = dyn_cast_or_null<BranchInst>(Head->getTerminator());
  if (!Branch || !Branch->isConditional() || !Branch->getMetadata(Kind))
    return {};
  auto Own = Branch->getSuccessor(0), Scaled = Branch->getSuccessor(1);
  auto Join = Own->getSingleSuccessor();
  auto Check = Join && Own != Scaled && Own->getSinglePredecessor() == Head ? Reach(Scaled, Head, Join) : std::nullopt;
  if (!Check || !Join->hasNPredecessors(2))
    return {};
  return Diamond{Branch, {Own, Own}, *Check, Join};
}

// the blocks of an arm
SmallVector<BasicBlock *, 4>
Blocks(const Arm &A) {
  SmallVector<BasicBlock *, 4> Blocks{A.First};
  if (A.Last != A.First) {
    append_range(Blocks, successors(A.First));
    Blocks.push_back(A.Last);
  }
  return Blocks;
}

// the early SimplifyCFG folds the footprint check's join, which holds only phis, into the sample's: the check's arms
// then go to the sample's join themselves. it is split off again, so the gradients arm ends in one block
void
Unfold(BasicBlock *Head) {
  auto Branch = cast<BranchInst>(Head->getTerminator());
  if (!Branch->isConditional())
    return;
  auto Join = Branch->getSuccessor(0)->getSingleSuccessor();
  auto Scaled = Branch->getSuccessor(1);
  auto Check = dyn_cast<BranchInst>(Scaled->getTerminator());
  if (!Join || !Check || !Check->isConditional())
    return;
  auto A = Check->getSuccessor(0), B = Check->getSuccessor(1);
  if (A != B && A->getSinglePredecessor() == Scaled && B->getSinglePredecessor() == Scaled &&
      A->getSingleSuccessor() == Join && B->getSingleSuccessor() == Join)
    SplitBlockPredecessors(Join, {A, B}, ".footprint");
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
  // what the first sample's value feeds stays after it; the rest moves before the first branch, which nothing in the
  // arms writes memory past
  SmallPtrSet<Instruction *, 16> After;
  SmallVector<Instruction *, 16> Before, Later;
  for (auto &Phi : Between->phis())
    After.insert(&Phi);
  auto Feeds = [&](const Instruction &I) {
    return any_of(I.operands(), [&](Value *V) {
      auto D = dyn_cast<Instruction>(V);
      return D && After.count(D);
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
  // the second sample does not read the first. its branch's condition is the first's value (Equal), which nothing
  // after the first sample computes
  for (auto &Arm : {Second.Own, Second.Scaled})
    for (auto Block : Blocks(Arm))
      for (auto &I : *Block)
        if (Feeds(I))
          return false;
  for (auto &Phi : Second.Join->phis())
    if (Feeds(Phi))
      return false;

  for (auto I : Before)
    I->moveBefore(First.Branch);
  // in their order, ahead of what followed the second sample
  auto At = Second.Join->getFirstNonPHI();
  SmallVector<PHINode *, 4> Moved;
  for (auto &Phi : make_early_inc_range(Between->phis()))
    Phi.moveBefore(At), Moved.push_back(&Phi);
  for (auto I : Later)
    I->moveBefore(At);
  // each of the second's arms goes on at the end of the first's, which then ends where the second's did
  for (auto [From, To] : {std::pair{Second.Own, &First.Own}, std::pair{Second.Scaled, &First.Scaled}}) {
    auto Into = To->Last;
    Into->getTerminator()->eraseFromParent();
    Into->getInstList().splice(Into->end(), From.First->getInstList());
    for (auto Next : successors(Into))
      Next->replacePhiUsesWith(From.First, Into);
    To->Last = From.Last == From.First ? Into : From.Last;
    for (auto Phi : Moved)
      Phi->replaceIncomingBlockWith(Into, To->Last);
  }
  auto Condition = Second.Branch->getCondition();
  Second.Branch->eraseFromParent();
  Between->eraseFromParent();
  Second.Own.First->eraseFromParent();
  Second.Scaled.First->eraseFromParent();
  RecursivelyDeleteTriviallyDeadInstructions(Condition);
  First.Join = Second.Join;
  return true;
}

} // namespace

PreservedAnalyses
MergeSampleBranchesPass::run(Function &F, FunctionAnalysisManager &AM) {
  auto Kind = F.getContext().getMDKindID(SampleBranchMetadata);
  SmallVector<BasicBlock *, 16> Heads;
  for (auto &BB : F)
    if (auto Branch = dyn_cast_or_null<BranchInst>(BB.getTerminator()); Branch && Branch->getMetadata(Kind))
      Heads.push_back(&BB);
  bool Changed = false;
  if (Merge) {
    for (auto Head : Heads)
      Unfold(Head);
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
