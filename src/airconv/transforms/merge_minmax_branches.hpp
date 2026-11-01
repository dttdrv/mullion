#pragma once
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

// a sample through a sampler that may be a minimum or maximum filter branches after it to the emulation
// (Converter::MinMaxFilter): `br reduces, sample_minmax, join`, marked MinMaxBranchMetadata. a branch between two
// samples keeps the GPU from issuing them together (dttdrv/mullion#32 measured a branch per sample at 3.47 ms against
// 2.23 for one around all of a sampler's samples), so a branch whose join holds the next one, on the same condition,
// takes in the second: what the second sample reads moves before the first branch, what the first sample's value
// feeds after the second's join. `Merge` false leaves a branch per sample. the mark is removed
namespace llvm::air {
constexpr const char *MinMaxBranchMetadata = "mullion.minmax_branch";

class MergeMinMaxBranchesPass : public PassInfoMixin<MergeMinMaxBranchesPass> {
public:
  explicit MergeMinMaxBranchesPass(bool Merge = true) : Merge(Merge) {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

private:
  bool Merge;
};
} // namespace llvm::air
