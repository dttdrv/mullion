#pragma once
#include "llvm/IR/Module.h"
#include "llvm/Passes/OptimizationLevel.h"

namespace dxmt {

void initializeModule(llvm::Module &M);

// false, with the verifier's findings in Error, for a module that is not well formed
bool runOptimizationPasses(llvm::Module &M, llvm::raw_ostream &Error);

void linkMSAD(llvm::Module &M);

void linkSamplePos(llvm::Module &M);

void linkTessellation(llvm::Module &M);

} // namespace dxmt