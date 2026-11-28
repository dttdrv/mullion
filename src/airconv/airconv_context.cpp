#include <mutex>
#include <set>
#include <cstdlib>
#include <cstring>
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/VersionTuple.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/Transforms/Scalar/LowerExpectIntrinsic.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/LICM.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/IPO/DeadArgumentElimination.h"
#include "llvm/Transforms/IPO/GlobalOpt.h"
#include "llvm/Transforms/IPO/SCCP.h"
#include "llvm/Transforms/IPO/Annotation2Metadata.h"
#include "llvm/Transforms/IPO/ForceFunctionAttrs.h"
#include "llvm/Transforms/IPO/InferFunctionAttrs.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"

#include "airconv_context.hpp"

#include "air_msad.h"
#include "air_denorm.h"
#include "air_samplepos.h"
#include "air_tessellation.h"

#include "transforms/guard_out_of_bounds.hpp"
#include "transforms/lower_16bit_texread.hpp"
#include "transforms/merge_sample_branches.hpp"
#include "transforms/merge_minmax_branches.hpp"
#include "transforms/simdgroup_implicit_membarrier.hpp"
#include "transforms/hoist_loads.hpp"

// dxbc_converter.hpp
namespace mullion::dxbc {
void FinishFeedbackLanes(llvm::Module &module);
}

using namespace llvm;

namespace mullion {

void initializeModule(llvm::Module &M) {
  M.setSourceFileName("airconv_generated.metal");
  M.setTargetTriple("air64-apple-macosx14.0.0");
  M.setDataLayout(
    "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:"
    "64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-"
    "v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:"
    "16:32"
  );
  M.setSDKVersion(VersionTuple(15, 0));
  M.addModuleFlag(Module::ModFlagBehavior::Error, "wchar_size", 4);
  M.addModuleFlag(Module::ModFlagBehavior::Max, "frame-pointer", 2);
  M.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_device_buffers", 31);
  M.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_constant_buffers", 31);
  M.addModuleFlag(
    Module::ModFlagBehavior::Max, "air.max_threadgroup_buffers", 31
  );
  M.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_textures", 128);
  M.addModuleFlag(
    Module::ModFlagBehavior::Max, "air.max_read_write_textures", 8
  );
  M.addModuleFlag(Module::ModFlagBehavior::Max, "air.max_samplers", 16);

};

static std::once_flag llvm_overwrite;

// Metal's compiler counts the leading zeros and the ones of a 32-bit value it can tell is all ones or zero (a
// sign-extended bool, or what it folds to one) as if the value were 16 bits wide: clz(c == 0 ? ~0u : 0u) is 16 for
// c = 0, and popcount of it 16, not 0 and 32, for scalars and vectors alike (an M6, macOS 27.3; tests/fuzz.py found
// it in a firstbithigh and a countbits, and plain Metal source does the same). what the converter's passes leave is
// no guide: Metal's own folding makes the pattern after them (a remainder by a divisor it proves is one, then a
// select). its trailing zeros and bit reversal are right, so every 32-bit count is of the reversed bits, which
// Metal does not fold back: clz(x) is ctz(reverse_bits(x)), and popcount(x) popcount(reverse_bits(x)), an operation
// more each. MULLION_AIRCONV_SKIP=sign-bit-counts leaves the counts as they are
static void
AvoidNarrowedSignBitCounts(llvm::Module &M) {
  if (auto skip = getenv("MULLION_AIRCONV_SKIP"); skip && strstr(skip, "sign-bit-counts"))
    return;
  std::vector<CallInst *> counts;
  for (auto &F : M)
    for (auto &I : instructions(F))
      if (auto Call = dyn_cast<CallInst>(&I))
        if (auto Callee = Call->getCalledFunction();
            Callee && Call->arg_size() >= 1 && Call->getType()->getScalarSizeInBits() == 32 &&
            (Callee->getName().startswith("air.clz.") || Callee->getName().startswith("air.popcount.")))
          counts.push_back(Call);
  for (auto Call : counts) {
    auto Name = Call->getCalledFunction()->getName();
    bool Leading = Name.startswith("air.clz.");
    auto Suffix = Name.substr(Leading ? strlen("air.clz") : strlen("air.popcount")); // ".i32", ".v2i32"...
    auto Ty = Call->getType();
    auto Attrs = Call->getCalledFunction()->getAttributes();
    IRBuilder<> B(Call);
    auto Reverse = M.getOrInsertFunction(("air.reverse_bits" + Suffix).str(), FunctionType::get(Ty, {Ty}, false), Attrs);
    auto Reversed = B.CreateCall(Reverse, {Call->getArgOperand(0)});
    if (!Leading) {
      Call->setArgOperand(0, Reversed);
      continue;
    }
    SmallVector<Value *, 2> Args{Reversed};
    SmallVector<Type *, 2> Types{Ty};
    for (unsigned i = 1; i < Call->arg_size(); i++) {
      Args.push_back(Call->getArgOperand(i));
      Types.push_back(Call->getArgOperand(i)->getType());
    }
    auto Trailing = M.getOrInsertFunction(("air.ctz" + Suffix).str(), FunctionType::get(Ty, Types, false), Attrs);
    auto Count = B.CreateCall(Trailing, Args);
    auto Callee = Call->getCalledFunction();
    Call->replaceAllUsesWith(Count);
    Call->eraseFromParent();
    if (Callee->use_empty())
      Callee->eraseFromParent();
  }
}

// Metal's compiler divides a constant by a variable wrongly for some divisors, how many depending on the program
// around it: over a million divisors 0xfffffffe / y was wrong for 12% of them in one kernel and 77 in another, and
// 0xfffffffe / 3 gives 0x55555555 (an M6, macOS 27.3; tools/metal-bench/integer_miscompiles.swift), the remainder
// with it. a division of two variables is right, and so is one whose dividend Metal cannot see is constant.
// tests/fuzz.py found it in a ~uint(b == b) / a. so a 32-bit dividend that is a constant is passed through
// reverse_bits twice, which Metal does not fold. MULLION_AIRCONV_SKIP=constant-dividends leaves it
static void
AvoidConstantDividends(llvm::Module &M) {
  if (auto skip = getenv("MULLION_AIRCONV_SKIP"); skip && strstr(skip, "constant-dividends"))
    return;
  std::vector<BinaryOperator *> divisions;
  for (auto &F : M)
    for (auto &I : instructions(F))
      if (auto Op = dyn_cast<BinaryOperator>(&I))
        switch (Op->getOpcode()) {
        case Instruction::UDiv:
        case Instruction::URem:
        case Instruction::SDiv:
        case Instruction::SRem:
          if (isa<Constant>(Op->getOperand(0)) && !isa<UndefValue>(Op->getOperand(0)) &&
              !isa<Constant>(Op->getOperand(1)) && Op->getType()->getScalarSizeInBits() == 32)
            divisions.push_back(Op);
          break;
        default:
          break;
        }
  for (auto Op : divisions) {
    auto Ty = Op->getType();
    std::string Suffix = ".i32";
    if (auto Vector = dyn_cast<FixedVectorType>(Ty))
      Suffix = ".v" + std::to_string(Vector->getNumElements()) + "i32";
    auto &Context = M.getContext();
    auto Attrs = AttributeList::get(
        Context, {{~0U, Attribute::get(Context, Attribute::AttrKind::NoUnwind)},
                  {~0U, Attribute::get(Context, Attribute::AttrKind::WillReturn)},
                  {~0U, Attribute::get(Context, Attribute::AttrKind::ReadNone)}}
    );
    auto Reverse = M.getOrInsertFunction("air.reverse_bits" + Suffix, FunctionType::get(Ty, {Ty}, false), Attrs);
    IRBuilder<> B(Op);
    Op->setOperand(0, B.CreateCall(Reverse, {B.CreateCall(Reverse, {Op->getOperand(0)})}));
  }
}

// a varying as the stages link registers (dxbc::varying_name): reg<register>_<component>
static bool
IsRegisterVarying(const MDNode *Output) {
  for (unsigned i = 0; Output && i + 1 < Output->getNumOperands(); i++)
    if (auto Key = dyn_cast_or_null<MDString>(Output->getOperand(i).get()); Key && Key->getString() == "air.arg_name") {
      auto Name = dyn_cast_or_null<MDString>(Output->getOperand(i + 1).get());
      if (!Name)
        return false;
      StringRef Rest = Name->getString(), Reg, Component;
      if (!Rest.consume_front("reg"))
        return false;
      std::tie(Reg, Component) = Rest.split('_');
      return !Reg.empty() && !Component.empty() && Reg.find_first_not_of("0123456789") == StringRef::npos &&
             Component.find_first_not_of("0123456789") == StringRef::npos;
    }
  return false;
}

// the indices of a mesh function's vertex or primitive data (`Kind`: air.mesh_vertex_data, air.mesh_primitive_data)
// that are register varyings
static void
MeshRegisterVaryings(const MDNode *Node, StringRef Kind, std::set<uint64_t> &Indices, unsigned Depth = 0) {
  if (!Node || Depth > 8)
    return;
  if (Node->getNumOperands() > 1)
    if (auto Name = dyn_cast_or_null<MDString>(Node->getOperand(0).get()); Name && Name->getString() == Kind) {
      if (auto Index = mdconst::dyn_extract_or_null<ConstantInt>(Node->getOperand(1)); Index && IsRegisterVarying(Node))
        Indices.insert(Index->getZExtValue());
      return;
    }
  for (auto &Operand : Node->operands())
    MeshRegisterVaryings(dyn_cast_or_null<MDNode>(Operand.get()), Kind, Indices, Depth + 1);
}

// Direct3D draws a depth prepass and the pass after it with different shaders that compute the position with the same
// expression, and expects the same depths (an EQUAL depth test), and the clip and cull distances to agree with it:
// the front ends keep each operation of a stage before the rasterizer as written for that (Converter::UseFastMath,
// read_dxil), and so Metal contracts none of them. what nothing of that depends on may: in each vertex and mesh
// function, an operation the front end left free (it has afn, as every one that was not precise does) on which no
// output but a register varying (dxbc::varying_name) and no branch depends gets contract, reassoc and arcp, as a
// pixel shader's operations have. what the position depends on stays as written, so any two shaders that compute it
// the same way still give the same: a value it takes from memory that the function also writes could depend on
// anything, so such a function stays as it is. object and kernel functions, a later stage's inputs, stay as written.
// MULLION_AIRCONV_SKIP=position-contraction keeps every operation of those stages as written
static void
ContractOutsidePosition(llvm::Module &M) {
  if (auto skip = getenv("MULLION_AIRCONV_SKIP"); skip && strstr(skip, "position-contraction"))
    return;
  for (auto Kind : {"air.vertex", "air.mesh"}) {
    auto Functions = M.getNamedMetadata(Kind);
    for (unsigned f = 0; Functions && f < Functions->getNumOperands(); f++) {
      auto Node = Functions->getOperand(f);
      auto F = Node->getNumOperands() ? mdconst::dyn_extract_or_null<Function>(Node->getOperand(0)) : nullptr;
      if (!F || F->isDeclaration())
        continue;
      bool Vertex = Kind == StringRef("air.vertex");
      // a vertex function's outputs are its return value's fields, in its first list's order
      std::set<unsigned> FreeFields;
      auto Outputs = Vertex && Node->getNumOperands() > 1 ? dyn_cast_or_null<MDNode>(Node->getOperand(1).get()) : nullptr;
      for (unsigned i = 0; Outputs && i < Outputs->getNumOperands(); i++)
        if (auto Output = dyn_cast_or_null<MDNode>(Outputs->getOperand(i).get()); Output && Output->getNumOperands() &&
            isa<MDString>(Output->getOperand(0)) && cast<MDString>(Output->getOperand(0))->getString() == "air.vertex_output" &&
            IsRegisterVarying(Output))
          FreeFields.insert(i);
      std::set<uint64_t> FreeVertexData, FreePrimitiveData;
      MeshRegisterVaryings(Node, "air.mesh_vertex_data", FreeVertexData);
      MeshRegisterVaryings(Node, "air.mesh_primitive_data", FreePrimitiveData);

      SmallVector<Value *, 32> Roots;
      bool Writes = false;
      for (auto &I : instructions(*F)) {
        if (auto Branch = dyn_cast<BranchInst>(&I); Branch && Branch->isConditional())
          Roots.push_back(Branch->getCondition());
        else if (auto Switch = dyn_cast<SwitchInst>(&I))
          Roots.push_back(Switch->getCondition());
        else if (auto Return = dyn_cast<ReturnInst>(&I); Return && Return->getReturnValue()) {
          // each field the last insertvalue into it gives, of a whole struct an earlier one is not seen through
          Value *Aggregate = Return->getReturnValue();
          std::set<unsigned> Given;
          while (auto Insert = dyn_cast<InsertValueInst>(Aggregate)) {
            unsigned Field = Insert->getIndices()[0];
            if (!Given.count(Field) && !FreeFields.count(Field))
              Roots.push_back(Insert->getInsertedValueOperand());
            if (Insert->getNumIndices() == 1)
              Given.insert(Field);
            Aggregate = Insert->getAggregateOperand();
          }
          Roots.push_back(Aggregate);
        } else if (auto Call = dyn_cast<CallInst>(&I)) {
          auto Callee = Call->getCalledFunction();
          auto Name = Callee ? Callee->getName() : StringRef();
          if (Name.startswith("air.set_") && Name.contains("_mesh")) {
            auto Index = Call->arg_size() > 1 ? dyn_cast<ConstantInt>(Call->getArgOperand(1)) : nullptr;
            bool Free = Index && ((Name.startswith("air.set_vertex_data_mesh") && FreeVertexData.count(Index->getZExtValue())) ||
                                  (Name.startswith("air.set_primitive_data_mesh") &&
                                   FreePrimitiveData.count(Index->getZExtValue())));
            if (!Free)
              for (auto &Arg : Call->args())
                Roots.push_back(Arg);
            continue;
          }
          Writes |= Call->mayWriteToMemory();
        } else {
          Writes |= I.mayWriteToMemory();
        }
      }

      SmallPtrSet<Instruction *, 32> Slice;
      SmallVector<Instruction *, 64> Work;
      auto Add = [&](Value *V) {
        if (auto I = dyn_cast<Instruction>(V); I && Slice.insert(I).second)
          Work.push_back(I);
      };
      for (auto Root : Roots)
        Add(Root);
      bool Reads = false;
      while (!Work.empty()) {
        auto I = Work.pop_back_val();
        // the constant address space is read only
        auto Load = dyn_cast<LoadInst>(I);
        Reads |= I->mayReadFromMemory() && !(Load && Load->getPointerAddressSpace() == 2);
        for (auto &Operand : I->operands())
          Add(Operand);
      }
      if (Reads && Writes)
        continue;
      for (auto &I : instructions(*F))
        if (isa<FPMathOperator>(&I) && !isa<FCmpInst>(&I) && I.hasApproxFunc() && !Slice.count(&I)) {
          I.setHasAllowContract(true);
          I.setHasAllowReassoc(true);
          I.setHasAllowReciprocal(true);
        }
    }
  }
}

bool
runOptimizationPasses(llvm::Module &M, llvm::raw_ostream &Error) {
  // a pixel shader that was not shaded coarsely keeps every lane's writes
  mullion::dxbc::FinishFeedbackLanes(M);
  // Metal says of ill-formed IR only "Failed to materializeAll"
  if (verifyModule(M, &Error))
    return false;

  // once, before any compilation on any thread runs InstCombine, which reads it
  std::call_once(llvm_overwrite, [] {
    auto Map = cl::getRegisteredOptions();
    auto InfiniteLoopThreshold = Map["instcombine-infinite-loop-threshold"];
    if (InfiniteLoopThreshold) {
      reinterpret_cast<cl::opt<unsigned> *>(InfiniteLoopThreshold)->setValue(1000);
    }
  });

  // Following optimization passes are picked from default LLVM pipelines
  // An unoptimized shader can make PSO compilation take a very long time
  // But a full optimization pipeline is also too heavy
  // So we choose some passes that really matter to run

  // Create the analysis managers.
  // These must be declared in this order so that they are destroyed in the
  // correct order due to inter-analysis-manager references.
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;

  // Create the new pass manager builder.
  // Take a look at the PassBuilder constructor parameters for more
  // customization, e.g. specifying a TargetMachine or various debugging
  // options.
  llvm::PassInstrumentationCallbacks PIC;
  PassBuilder PB(nullptr, PipelineTuningOptions(), {}, &PIC);

  // Register all the basic analyses with the managers.
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  // llvm::StandardInstrumentations SI(true);
  // SI.registerCallbacks(PIC, &FAM);

  ModulePassManager MPM;

  // Convert @llvm.global.annotations to !annotation metadata.
  MPM.addPass(Annotation2MetadataPass());

  // Force any function attributes we want the rest of the pipeline to observe.
  MPM.addPass(ForceFunctionAttrsPass());
  // Do basic inference of function attributes from known properties of system
  // libraries and other oracles.
  MPM.addPass(InferFunctionAttrsPass());

  {
    // Create an early function pass manager to cleanup the output of the
    // frontend.
    FunctionPassManager EarlyFPM;
    // Lower llvm.expect to metadata before attempting transforms.
    // Compare/branch metadata may alter the behavior of passes like SimplifyCFG.
    EarlyFPM.addPass(LowerExpectIntrinsicPass());
    EarlyFPM.addPass(SimplifyCFGPass());
    EarlyFPM.addPass(SROAPass());
    EarlyFPM.addPass(EarlyCSEPass());
    MPM.addPass(createModuleToFunctionPassAdaptor(std::move(EarlyFPM), true /* ? */));
  }

  MPM.addPass(AlwaysInlinerPass());
  MPM.addPass(IPSCCPPass());
  MPM.addPass(GlobalOptPass());
  MPM.addPass(createModuleToFunctionPassAdaptor(PromotePass()));
  MPM.addPass(DeadArgumentEliminationPass());

  {
    // Create a small function pass pipeline to cleanup after all the global
    // optimizations.
    FunctionPassManager GlobalCleanupPM;
    // MULLION_AIRCONV_SKIP=sample-branch-merge leaves a branch per sample (SampleImplicit)
    if (auto skip = (getenv("MULLION_AIRCONV_SKIP") ?: getenv("DXMT_AIRCONV_SKIP")); !skip || !strstr(skip, "sample-branch-merge"))
      GlobalCleanupPM.addPass(air::MergeSampleBranchesPass());
    else
      GlobalCleanupPM.addPass(air::MergeSampleBranchesPass(false));
    // MULLION_AIRCONV_SKIP=minmax-branch-merge leaves a minimum and maximum filter branch per sample (MinMaxFilter)
    {
      auto skip = getenv("MULLION_AIRCONV_SKIP") ?: getenv("DXMT_AIRCONV_SKIP");
      GlobalCleanupPM.addPass(air::MergeMinMaxBranchesPass(!skip || !strstr(skip, "minmax-branch-merge")));
    }
    GlobalCleanupPM.addPass(InstCombinePass());

    GlobalCleanupPM.addPass(SimplifyCFGPass(SimplifyCFGOptions().convertSwitchRangeToICmp(true)));

    GlobalCleanupPM.addPass(air::Lower16BitTexReadPass());
    // MULLION_AIRCONV_SKIP names what the converter leaves out, to find the one a shader goes wrong with
    auto skip = getenv("MULLION_AIRCONV_SKIP") ?: getenv("DXMT_AIRCONV_SKIP");
    if (!skip || !strstr(skip, "simdgroup-barrier"))
      GlobalCleanupPM.addPass(air::SimdgroupImplicitMemBarrierPass());
    GlobalCleanupPM.addPass(air::GuardOutOfBoundsPass());
    GlobalCleanupPM.addPass(createFunctionToLoopPassAdaptor(LICMPass(LICMOptions()), /*UseMemorySSA=*/true));
    GlobalCleanupPM.addPass(EarlyCSEPass(/*UseMemorySSA=*/true));
    GlobalCleanupPM.addPass(ADCEPass());
    // loads move ahead of what uses them, at most 32 registers of values ahead (MULLION_AIRCONV_HOIST_LOADS, which
    // the shader cache's key has, for another window): more, and Metal's compiler spills them
    if (!skip || !strstr(skip, "hoist-loads")) {
      auto window = getenv("MULLION_AIRCONV_HOIST_LOADS");
      GlobalCleanupPM.addPass(air::HoistLoadsPass(window ? atoi(window) : 32));
    }

    MPM.addPass(createModuleToFunctionPassAdaptor(std::move(GlobalCleanupPM), true /* ? */));
  }

  if (auto temp = getenv("DXMT_TEMP_O2"))
    MPM.addPass(createModuleToFunctionPassAdaptor(PB.buildFunctionSimplificationPipeline(
        temp[0] == '3' ? OptimizationLevel::O3 : OptimizationLevel::O2, ThinOrFullLTOPhase::None
    )));

  // MPM.addPass(VerifierPass());

  // Optimize the IR!
  MPM.run(M, MAM);
  ContractOutsidePosition(M);
  AvoidNarrowedSignBitCounts(M);
  AvoidConstantDividends(M);
  return true;
}

void
removeNamedMetadata(llvm::Module &M, StringRef Name) {
  if (auto MD = M.getNamedMetadata(Name)) {
    M.eraseNamedMetadata(MD);
  }
}

template<size_t N>
void linkShader(llvm::Module &M, const unsigned char (&bitcode)[N], bool inline_all = false) {
  // read in place, and lazily: the linker materializes only the functions the shader calls, and what they need
  Expected<std::unique_ptr<Module>> modOrErr =
      getLazyBitcodeModule(MemoryBufferRef(StringRef((const char *)bitcode, N), "air"), M.getContext());

  if (!modOrErr) {
    // not expected to see this unless something really bad happened in compile time
    errs() << "Failed to parse air bitcode\n";
    return;
  }

  auto module = std::move(modOrErr.get());

  removeNamedMetadata(*module, "air.compile_options");
  removeNamedMetadata(*module, "air.version");
  removeNamedMetadata(*module, "air.language_version");
  removeNamedMetadata(*module, "air.source_file_name");
  removeNamedMetadata(*module, "llvm.ident");
  removeNamedMetadata(*module, "llvm.module.flags");

  std::vector<std::string> defined;
  if (inline_all)
    for (auto &F : *module)
      if (!F.isDeclaration() || F.isMaterializable())
        defined.push_back(F.getName().str());
  llvm::Linker::linkModules(M, std::move(module), Linker::LinkOnlyNeeded);
  // the library's functions inline into the shader's, as the pipeline's optimization inlines only those that must be
  // (AlwaysInlinerPass): left as calls, Metal keeps what they are given by pointer on the stack
  for (auto &name : defined)
    if (auto F = M.getFunction(name); F && !F->isDeclaration()) {
      F->removeFnAttr(Attribute::NoInline);
      F->addFnAttr(Attribute::AlwaysInline);
    }
};

void
linkMSAD(llvm::Module &M) {
  linkShader(M, air_msad);
}

void
linkDenorm(llvm::Module &M) {
  linkShader(M, air_denorm);
}

void
linkSamplePos(llvm::Module &M) {
  linkShader(M, air_samplepos);
}

void
linkTessellation(llvm::Module &M) {
  linkShader(M, air_tessellation, true);
}

} // namespace mullion