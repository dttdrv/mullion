#pragma once
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

// a pixel shader's implicit sample through a sampler known only when the shader runs branches on the sampler
// (Converter::SampleImplicit): `br (quad_all isotropic), implicit, gradients`, marked SampleBranchMetadata. a branch
// between samples keeps the GPU from issuing them together (#32: a branch per sample cost 3.47 ms, one around all of
// a sampler's samples no more than the 2.23 of the implicit samples alone), so a branch whose join holds the next one,
// on the same sampler, takes in the second sample: what the second sample reads moves before the first branch, what
// the first sample's value feeds after the second's join. `Merge` false leaves a branch per sample. the mark is removed
namespace llvm::air {
constexpr const char *SampleBranchMetadata = "mullion.sample_branch";

class MergeSampleBranchesPass : public PassInfoMixin<MergeSampleBranchesPass> {
public:
  explicit MergeSampleBranchesPass(bool Merge = true) : Merge(Merge) {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

private:
  bool Merge;
};
} // namespace llvm::air
