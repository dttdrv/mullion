#pragma once
#include "llvm/IR/PassManager.h"

/**
 * Moves each load of device and constant memory, with the instructions that compute its address, up to a window of
 * loads ahead of where it was, so that the GPU waits for several at once. DXC places each load right before what uses
 * it, and Metal's compiler keeps that order: a shader that reads rows of a buffer then waits for every row on its own
 * (FXC hoists them itself, so the same HLSL as DXBC does not).
 */

namespace llvm::air {
class HoistLoadsPass : public PassInfoMixin<HoistLoadsPass> {
public:
  explicit HoistLoadsPass(unsigned Window) : Window(Window) {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

private:
  unsigned Window;
};
} // namespace llvm::air
