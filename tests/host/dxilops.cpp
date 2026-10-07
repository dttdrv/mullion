// prints which DXIL operations shaders use: for each DXIL container named, a line of its shader kind and, for every
// call of a dx.op function, the operation's number (the call's first argument: DXIL.rst, "Operations") with the
// function called, which carries the overload, and how many such calls the shader has. tests/coverage.py sets these
// against the operations the converter lowers. with DXILOPS_IR set, each shader's DXIL is also written as LLVM
// assembly beside it (<file>.dxil.ll), for reading what a shader does
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

int
main(int argc, char **argv) {
  llvm::LLVMContext context;
  for (int i = 1; i < argc; i++) {
    auto file = llvm::MemoryBuffer::getFile(argv[i]);
    if (!file)
      continue;
    auto data = (*file)->getBuffer();
    auto word = [&](size_t at) {
      uint32_t value = 0;
      if (at + sizeof(value) <= data.size())
        memcpy(&value, data.data() + at, sizeof(value));
      return value;
    };
    // DxilContainerHeader's parts; the DXIL one is a DxilProgramHeader, whose bitcode is at an offset from its
    // DxilBitcodeHeader (DxilContainer.h)
    for (uint32_t part = 0; data.startswith("DXBC") && part < word(28); part++) {
      size_t at = word(32 + 4 * part), body = at + 8, header = body + 8;
      if (data.substr(at, 4) != "DXIL")
        continue;
      auto module = llvm::parseBitcodeFile(
          llvm::MemoryBufferRef(data.substr(header + word(header + 8), word(header + 12)), argv[i]), context
      );
      if (!module) {
        llvm::consumeError(module.takeError());
        printf("%s unreadable\n", argv[i]);
        continue;
      }
      if (getenv("DXILOPS_IR")) {
        std::error_code failed;
        llvm::raw_fd_ostream out(std::string(argv[i]) + ".dxil.ll", failed);
        (*module)->print(out, nullptr);
      }
      std::map<std::pair<uint64_t, std::string>, unsigned> calls;
      for (auto &function : **module)
        for (auto &instruction : llvm::instructions(function))
          if (auto call = llvm::dyn_cast<llvm::CallInst>(&instruction))
            if (auto callee = call->getCalledFunction(); callee && callee->getName().startswith("dx.op.") && call->arg_size())
              if (auto op = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(0)))
                calls[{op->getZExtValue(), callee->getName().str()}]++;
      printf("%s kind %u", argv[i], word(body) >> 16);
      for (auto &[call, count] : calls)
        printf(" %llu:%s:%u", (unsigned long long)call.first, call.second.c_str(), count);
      printf("\n");
    }
  }
}
