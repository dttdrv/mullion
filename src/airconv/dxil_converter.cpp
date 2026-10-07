/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

// DXIL is LLVM bitcode: LLVM reads it, and its dx.op calls are lowered in place inside the AIR function
// the DXBC path builds. Declarations go through the same handlers DXBC uses.

#include "airconv_error.hpp"
#include "dxbc_converter.hpp"
#include "nt/dxbc_converter_base.hpp"
#include "airconv_ray.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/LowerAtomic.h"
#include <format>
#include <map>
#include <set>

namespace dxmt::dxbc {

using namespace microsoft;
using namespace llvm;
using llvm::air::Texture;

namespace {

// DxilConstants.h OpCode, the ones lowered here
enum Op : uint32_t {
  LoadInput = 4,
  StoreOutput = 5,
  FAbs = 6,
  Saturate = 7,
  IsNaN = 8,
  IsInf = 9,
  IsFinite = 10,
  Cos = 12,
  Sin = 13,
  Tan = 14,
  Acos = 15,
  Asin = 16,
  Atan = 17,
  Hcos = 18,
  Hsin = 19,
  Htan = 20,
  Exp = 21,
  Frc = 22,
  Log = 23,
  Sqrt = 24,
  Rsqrt = 25,
  Round_ne = 26,
  Round_ni = 27,
  Round_pi = 28,
  Round_z = 29,
  Bfrev = 30,
  Countbits = 31,
  FirstbitLo = 32,
  FirstbitHi = 33,
  FirstbitSHi = 34,
  FMax = 35,
  FMin = 36,
  IMax = 37,
  IMin = 38,
  UMax = 39,
  UMin = 40,
  FMad = 46,
  Fma = 47,
  IMad = 48,
  UMad = 49,
  Msad = 50,
  Ibfe = 51,
  Ubfe = 52,
  Bfi = 53,
  Dot2 = 54,
  Dot3 = 55,
  Dot4 = 56,
  CreateHandle = 57,
  CBufferLoadLegacy = 59,
  Sample = 60,
  SampleBias = 61,
  SampleLevel = 62,
  SampleGrad = 63,
  SampleCmp = 64,
  SampleCmpLevelZero = 65,
  TextureLoad = 66,
  TextureStore = 67,
  BufferLoad = 68,
  BufferStore = 69,
  BufferUpdateCounter = 70,
  CheckAccessFullyMapped = 71,
  GetDimensions = 72,
  TextureGather = 73,
  TextureGatherCmp = 74,
  Texture2DMSGetSamplePosition = 75,
  RenderTargetGetSamplePosition = 76,
  RenderTargetGetSampleCount = 77,
  AtomicBinOp = 78,
  AtomicCompareExchange = 79,
  Barrier = 80,
  CalculateLOD = 81,
  Discard = 82,
  DerivCoarseX = 83,
  DerivCoarseY = 84,
  DerivFineX = 85,
  DerivFineY = 86,
  EvalSnapped = 87,
  EvalSampleIndex = 88,
  EvalCentroid = 89,
  SampleIndex = 90,
  Coverage = 91,
  ThreadId = 93,
  GroupId = 94,
  ThreadIdInGroup = 95,
  FlattenedThreadIdInGroup = 96,
  EmitStream = 97,
  CutStream = 98,
  EmitThenCutStream = 99,
  GSInstanceID = 100,
  LoadOutputControlPoint = 103,
  LoadPatchConstant = 104,
  DomainLocation = 105,
  StorePatchConstant = 106,
  OutputControlPointID = 107,
  PrimitiveID = 108,
  WaveIsFirstLane = 110,
  WaveGetLaneIndex = 111,
  WaveGetLaneCount = 112,
  WaveAnyTrue = 113,
  WaveAllTrue = 114,
  WaveActiveAllEqual = 115,
  WaveActiveBallot = 116,
  WaveReadLaneAt = 117,
  WaveReadLaneFirst = 118,
  WaveActiveOp = 119,
  WaveActiveBit = 120,
  WavePrefixOp = 121,
  QuadReadLaneAt = 122,
  QuadOp = 123,
  LegacyF32ToF16 = 130,
  LegacyF16ToF32 = 131,
  WaveAllBitCount = 135,
  WavePrefixBitCount = 136,
  ViewID = 138,
  RawBufferLoad = 139,
  RawBufferStore = 140,
  SetMeshOutputCounts = 168,
  EmitIndices = 169,
  GetMeshPayload = 170,
  StoreVertexOutput = 171,
  StorePrimitiveOutput = 172,
  DispatchMesh = 173,
  WriteSamplerFeedback = 174,
  WriteSamplerFeedbackBias = 175,
  WriteSamplerFeedbackLevel = 176,
  WriteSamplerFeedbackGrad = 177,
  HitInstanceID = 141,
  HitInstanceIndex = 142,
  HitKind = 143,
  RayFlags = 144,
  DispatchRaysIndex = 145,
  DispatchRaysDimensions = 146,
  WorldRayOrigin = 147,
  WorldRayDirection = 148,
  ObjectRayOrigin = 149,
  ObjectRayDirection = 150,
  ObjectToWorld = 151,
  WorldToObject = 152,
  RayTMin = 153,
  RayTCurrent = 154,
  IgnoreHit = 155,
  AcceptHitAndEndSearch = 156,
  TraceRay = 157,
  ReportHit = 158,
  CallShader = 159,
  CreateHandleForLib = 160,
  HitPrimitiveIndex = 161,
  HitGeometryIndex = 213,
  AllocateRayQuery = 178,
  RayQuery_TraceRayInline = 179,
  RayQuery_Proceed = 180,
  RayQuery_Abort = 181,
  RayQuery_CommitNonOpaqueTriangleHit = 182,
  RayQuery_CommitProceduralPrimitiveHit = 183,
  RayQuery_CandidateType = 185,
  RayQuery_RayFlags = 195,
  RayQuery_CandidateInstanceContributionToHitGroupIndex = 214,
  RayQuery_CommittedInstanceContributionToHitGroupIndex = 215,
  AllocateRayQuery2 = 258,
  Dot2AddHalf = 162,
  Dot4AddI8Packed = 163,
  Dot4AddU8Packed = 164,
  WaveMatch = 165,
  WaveMultiPrefixOp = 166,
  WaveMultiPrefixBitCount = 167,
  AnnotateHandle = 216,
  Unpack4x8 = 219,
  Pack4x8 = 220,
  IsHelperLane = 221,
};

// DxilConstants.h SemanticKind values that have their own storage in the DXBC path, or in a Metal mesh
enum Semantic : uint32_t {
  SemanticPosition = 3,
  SemanticRenderTargetArrayIndex = 4,
  SemanticViewportArrayIndex = 5,
  SemanticClipDistance = 6,
  SemanticCullDistance = 7,
  SemanticPrimitiveID = 10,
  CoverageOutput = 14,
  Depth = 17,
  DepthLessEqual,
  DepthGreaterEqual,
  StencilRef,
  SemanticShadingRate = 29,
  SemanticCullPrimitive = 30,
};

// DxilConstants.h ResourceClass (createHandle's first operand, and the dx.resources list order)
enum ResourceClass : uint32_t { SRV, UAV, CBV, SamplerClass };

// DxilConstants.h ResourceKind, the ones that are not a texture or typed buffer
enum ResourceKind : uint32_t {
  ResourceKind_RawBuffer = 11,
  ResourceKind_StructuredBuffer,
  ResourceKind_CBuffer,
  ResourceKind_Sampler,
  ResourceKind_FeedbackTexture2D = 17,
  ResourceKind_FeedbackTexture2DArray
};

struct Element {
  uint32_t row, col, rows, cols, semantic, interpolation, component;
};

// DxilConstants.h ComponentType of a signed 16-bit value
constexpr uint32_t kComponentI16 = 2;

struct Entry {
  Function *function;
  std::vector<Element> input, output, patch; // patch: the patch constants a hull shader writes or a domain shader reads
  MDNode *properties;
};

uint64_t
md_uint(const MDOperand &op) {
  return mdconst::extract<ConstantInt>(op)->getZExtValue();
}

std::vector<Element>
read_elements(const MDOperand &list) {
  std::vector<Element> elements;
  if (auto node = dyn_cast_or_null<MDNode>(list.get()))
    for (auto &op : node->operands()) {
      auto e = cast<MDNode>(op.get());
      elements.resize(std::max<size_t>(elements.size(), md_uint(e->getOperand(0)) + 1));
      elements[md_uint(e->getOperand(0))] = {
          (uint32_t)md_uint(e->getOperand(8)), (uint32_t)md_uint(e->getOperand(9)), (uint32_t)md_uint(e->getOperand(6)),
          (uint32_t)md_uint(e->getOperand(7)), (uint32_t)md_uint(e->getOperand(3)), (uint32_t)md_uint(e->getOperand(5)),
          (uint32_t)md_uint(e->getOperand(2)),
      };
    }
  return elements;
}

Entry
read_entry(Module &M, StringRef function = {}) {
  // a library's entry points are its shaders, after one without a function that holds its resources
  auto entries = M.getNamedMetadata("dx.entryPoints");
  auto entry = entries->getOperand(0);
  for (auto candidate : entries->operands())
    if (auto f = mdconst::extract_or_null<Function>(candidate->getOperand(0)); f && f->getName() == function)
      entry = candidate;
  auto signatures = dyn_cast_or_null<MDNode>(entry->getOperand(2).get());
  return {
      mdconst::extract_or_null<Function>(entry->getOperand(0)),
      signatures ? read_elements(signatures->getOperand(0)) : std::vector<Element>{},
      signatures ? read_elements(signatures->getOperand(1)) : std::vector<Element>{},
      signatures ? read_elements(signatures->getOperand(2)) : std::vector<Element>{},
      dyn_cast_or_null<MDNode>(entry->getOperand(4).get()),
  };
}

// value of a tag in a DXIL tag/value list, or null
const MDOperand *
tag(const MDNode *list, uint64_t tag) {
  if (list)
    for (unsigned i = 0; i + 1 < list->getNumOperands(); i += 2)
      if (md_uint(list->getOperand(i)) == tag)
        return &list->getOperand(i + 1);
  return nullptr;
}

uint32_t
opcode(const CallInst *call) {
  return cast<ConstantInt>(call->getArgOperand(0))->getZExtValue();
}

bool
is_dx_op(const Value *value) {
  auto call = dyn_cast<CallInst>(value);
  return call && call->getCalledFunction() && call->getCalledFunction()->getName().startswith("dx.op.");
}

// SM 6.6 annotates every handle with its resource's properties, which the handle's range says already: what uses the
// annotation uses the handle
void
drop_annotations(CallInst *call, Value *handle) {
  for (auto use : make_early_inc_range(call->users()))
    if (auto annotate = dyn_cast<CallInst>(use);
        annotate && is_dx_op(annotate) && opcode(annotate) == AnnotateHandle) {
      annotate->replaceAllUsesWith(handle);
      annotate->eraseFromParent();
    }
}

// SM 6.6 names a bound resource by its binding (createHandleFromBinding) and types the handle (annotateHandle). Bound
// ranges are still listed in dx.resources with their types, so each becomes the SM 6.0 createHandle of its range, and
// the annotation of a bound handle adds nothing. The index stays the absolute register, in both forms.
FunctionCallee
create_handle(Module &M, Type *handle) {
  auto &ctx = M.getContext();
  return M.getOrInsertFunction(
      "dx.op.createHandle", FunctionType::get(
                                handle,
                                {Type::getInt32Ty(ctx), Type::getInt8Ty(ctx), Type::getInt32Ty(ctx),
                                 Type::getInt32Ty(ctx), Type::getInt1Ty(ctx)},
                                false
                            )
  );
}

void
lower_bound_handles(Module &M) {
  auto from_binding = M.getFunction("dx.op.createHandleFromBinding");
  if (!from_binding)
    return;
  auto i32 = Type::getInt32Ty(M.getContext()), i8 = Type::getInt8Ty(M.getContext());
  auto create = create_handle(M, from_binding->getReturnType());
  auto lists = M.getNamedMetadata("dx.resources")->getOperand(0);
  for (auto user : make_early_inc_range(from_binding->users())) {
    auto call = cast<CallInst>(user);
    // %dx.types.ResBind: lower bound, upper bound, space, class
    auto bind = cast<Constant>(call->getArgOperand(1));
    auto field = [&](unsigned i) { return cast<ConstantInt>(bind->getAggregateElement(i))->getZExtValue(); };
    auto cls = field(3);
    // the range is the record of that class starting at that register in that space
    uint64_t id = ~0ull;
    if (auto list = dyn_cast_or_null<MDNode>(lists->getOperand(cls).get()))
      for (auto &op : list->operands()) {
        auto r = cast<MDNode>(op.get());
        if (md_uint(r->getOperand(3)) == field(2) && md_uint(r->getOperand(4)) == field(0))
          id = md_uint(r->getOperand(0));
      }
    auto handle = CallInst::Create(
        create,
        {ConstantInt::get(i32, CreateHandle), ConstantInt::get(i8, cls), ConstantInt::get(i32, id),
         call->getArgOperand(2), call->getArgOperand(3)},
        "", call
    );
    drop_annotations(call, handle);
    call->replaceAllUsesWith(handle);
    call->eraseFromParent();
  }
}

// SM 6.6 dynamic resources index the bound descriptor heaps (createHandleFromHeap), typed by annotateHandle's
// DxilResourceProperties. Each distinct type becomes a dx.resources range in kDescriptorHeapSpace that covers the heap
// from register 0, so the heap index is the register, and the root signature binds the space to the heap
void
lower_heap_handles(Module &M) {
  auto from_heap = M.getFunction("dx.op.createHandleFromHeap");
  if (!from_heap)
    return;
  auto &ctx = M.getContext();
  auto i32 = Type::getInt32Ty(ctx), i8 = Type::getInt8Ty(ctx);
  auto create = create_handle(M, from_heap->getReturnType());
  auto md = [&](uint64_t v) -> Metadata * { return ConstantAsMetadata::get(ConstantInt::get(i32, v)); };
  auto resources = M.getOrInsertNamedMetadata("dx.resources");
  auto old = resources->getNumOperands() ? resources->getOperand(0) : nullptr;

  // the existing records by class, and the next free range id of each
  std::array<std::vector<Metadata *>, 4> lists;
  std::array<uint64_t, 4> next{};
  for (unsigned cls = SRV; cls <= SamplerClass; cls++)
    if (auto list = old ? dyn_cast_or_null<MDNode>(old->getOperand(cls).get()) : nullptr)
      for (auto &op : list->operands()) {
        lists[cls].push_back(op.get());
        next[cls] = std::max(next[cls], md_uint(cast<MDNode>(op.get())->getOperand(0)) + 1);
      }

  std::map<std::tuple<uint32_t, uint64_t, uint64_t>, uint64_t> ranges;
  for (auto user : make_early_inc_range(from_heap->users())) {
    auto call = cast<CallInst>(user);
    for (auto use : make_early_inc_range(call->users())) {
      auto annotate = dyn_cast<CallInst>(use);
      if (!annotate || !is_dx_op(annotate) || opcode(annotate) != AnnotateHandle)
        continue;
      // DxilResourceProperties: dword 0 has the kind (byte 0), then after 4 bits of alignment UAV, ROV, globally
      // coherent and comparison sampler or counter (bits 12 to 15); dword 1 the component type and sample count
      // (typed), stride (structured) or size (CBV)
      auto props = cast<Constant>(annotate->getArgOperand(2));
      auto dword0 = cast<ConstantInt>(props->getAggregateElement(0u))->getZExtValue();
      auto dword1 = cast<ConstantInt>(props->getAggregateElement(1u))->getZExtValue();
      auto kind = dword0 & 0xff;
      auto bit = [&](unsigned b) { return dword0 >> b & 1; };
      uint32_t cls = kind == ResourceKind_Sampler   ? SamplerClass
                     : kind == ResourceKind_CBuffer ? CBV
                     : bit(12)                      ? UAV
                                                    : SRV;
      auto [range, added] = ranges.try_emplace({cls, dword0, dword1}, next[cls]);
      if (added) {
        next[cls]++;
        std::vector<Metadata *> r = {
            md(range->second),
            ConstantAsMetadata::get(UndefValue::get(i32)),
            MDString::get(ctx, ""),
            md(kDescriptorHeapSpace),
            md(0),
            md(~0u)
        };
        auto tags = kind == ResourceKind_StructuredBuffer ? MDNode::get(ctx, {md(1), md(dword1)})
                    : kind == ResourceKind_RawBuffer      ? nullptr
                                                          : MDNode::get(ctx, {md(0), md(dword1 & 0xff)});
        if (cls == SRV)
          r.insert(r.end(), {md(kind), md(dword1 >> 16 & 0xff), tags});
        else if (cls == UAV)
          r.insert(r.end(), {md(kind), md(bit(14)), md(bit(15)), md(bit(13)), tags});
        else if (cls == CBV)
          r.insert(r.end(), {md(dword1), nullptr});
        else
          r.insert(r.end(), {md(bit(15)), nullptr});
        lists[cls].push_back(MDNode::get(ctx, r));
      }
      auto handle = CallInst::Create(
          create,
          {ConstantInt::get(i32, CreateHandle), ConstantInt::get(i8, cls), ConstantInt::get(i32, range->second),
           call->getArgOperand(1), call->getArgOperand(3)},
          "", annotate
      );
      annotate->replaceAllUsesWith(handle);
      annotate->eraseFromParent();
    }
    if (call->use_empty())
      call->eraseFromParent();
  }

  std::vector<Metadata *> classes;
  for (auto &list : lists)
    classes.push_back(list.empty() ? nullptr : MDNode::get(ctx, list));
  auto tuple = MDNode::get(ctx, classes);
  if (resources->getNumOperands())
    resources->setOperand(0, tuple);
  else
    resources->addOperand(tuple);
}

// the constant expressions using `value` as instructions at each of their uses
void
expand_constant_uses(Value *value) {
  for (auto user : make_early_inc_range(value->users()))
    if (auto expr = dyn_cast<ConstantExpr>(user)) {
      expand_constant_uses(expr);
      for (auto &use : make_early_inc_range(expr->uses()))
        use.set(expr->getAsInstruction(cast<llvm::Instruction>(use.getUser())));
      expr->destroyConstant();
    }
}

// rebuilds the accesses through `from` on `to`, a pointer to the same type in another address space
Error
retarget(Value *from, Value *to) {
  expand_constant_uses(from);
  for (auto user : make_early_inc_range(from->users())) {
    auto inst = cast<llvm::Instruction>(user);
    IRBuilder<> ir(inst);
    if (auto gep = dyn_cast<GetElementPtrInst>(inst)) {
      SmallVector<Value *> indices(gep->indices());
      if (auto err = retarget(gep, ir.CreateGEP(gep->getSourceElementType(), to, indices, "", gep->isInBounds())))
        return err;
    } else if (auto bitcast = dyn_cast<BitCastInst>(inst)) {
      auto pointee = bitcast->getDestTy()->getNonOpaquePointerElementType();
      if (auto err =
              retarget(bitcast, ir.CreateBitCast(to, pointee->getPointerTo(to->getType()->getPointerAddressSpace()))))
        return err;
    } else if (auto load = dyn_cast<LoadInst>(inst)) {
      load->replaceAllUsesWith(ir.CreateAlignedLoad(load->getType(), to, load->getAlign()));
    } else if (auto store = dyn_cast<StoreInst>(inst); store && store->getPointerOperand() == from) {
      ir.CreateAlignedStore(store->getValueOperand(), to, store->getAlign());
    } else if (auto transfer = dyn_cast<MemTransferInst>(inst)) {
      // DXC copies a static array into a local one this way
      auto dst = transfer->getRawDest() == from ? to : transfer->getRawDest();
      auto src = transfer->getRawSource() == from ? to : transfer->getRawSource();
      if (isa<MemMoveInst>(transfer))
        ir.CreateMemMove(
            dst, transfer->getDestAlign(), src, transfer->getSourceAlign(), transfer->getLength(),
            transfer->isVolatile()
        );
      else
        ir.CreateMemCpy(
            dst, transfer->getDestAlign(), src, transfer->getSourceAlign(), transfer->getLength(),
            transfer->isVolatile()
        );
    } else if (auto set = dyn_cast<MemSetInst>(inst)) {
      ir.CreateMemSet(to, set->getValue(), set->getLength(), set->getDestAlign(), set->isVolatile());
    } else if (auto intrinsic = dyn_cast<IntrinsicInst>(inst); intrinsic && intrinsic->isLifetimeStartOrEnd()) {
      // the variable lives as long as the function
    } else {
      return make_error<UnsupportedFeature>(std::format("DXIL module variable used by {}", inst->getOpcodeName()));
    }
    inst->eraseFromParent();
  }
  return Error::success();
}

// a library names a resource by its variable: createHandleForLib takes the variable's value, or for an array of
// resources an element's. dx.resources lists each variable with its range, so the handle becomes the SM 6.0
// createHandle of that range, at its first register plus the element
void
lower_lib_handles(Module &M) {
  auto resources = M.getNamedMetadata("dx.resources");
  if (!resources)
    return;
  auto i32 = Type::getInt32Ty(M.getContext()), i8 = Type::getInt8Ty(M.getContext());
  auto lists = resources->getOperand(0);
  for (auto &F : make_early_inc_range(M)) {
    if (!F.getName().startswith("dx.op.createHandleForLib"))
      continue;
    auto create = create_handle(M, F.getReturnType());
    for (auto user : make_early_inc_range(F.users())) {
      auto call = cast<CallInst>(user);
      auto load = dyn_cast<LoadInst>(call->getArgOperand(1));
      if (!load)
        continue;
      Value *variable = load->getPointerOperand(), *element = ConstantInt::get(i32, 0);
      if (auto gep = dyn_cast<GEPOperator>(variable)) {
        variable = gep->getPointerOperand();
        element = gep->getOperand(gep->getNumOperands() - 1);
      }
      variable = variable->stripPointerCasts();
      for (unsigned cls = SRV; cls <= SamplerClass; cls++)
        if (auto list = dyn_cast_or_null<MDNode>(lists->getOperand(cls).get()))
          for (auto &op : list->operands()) {
            auto r = cast<MDNode>(op.get());
            auto symbol = mdconst::dyn_extract_or_null<Constant>(r->getOperand(1));
            if (!symbol || symbol->stripPointerCasts() != variable)
              continue;
            IRBuilder<> ir(call);
            auto index = ir.CreateAdd(
                ConstantInt::get(i32, md_uint(r->getOperand(4))), ir.CreateZExtOrTrunc(element, i32)
            );
            auto handle = ir.CreateCall(
                create, {ConstantInt::get(i32, CreateHandle), ConstantInt::get(i8, cls),
                         ConstantInt::get(i32, md_uint(r->getOperand(0))), index, ir.getFalse()}
            );
            drop_annotations(call, handle);
            call->replaceAllUsesWith(handle);
          }
      if (call->use_empty()) {
        call->eraseFromParent();
        if (load->use_empty())
          load->eraseFromParent();
      }
    }
  }
}

Expected<std::unique_ptr<Module>>
parse(const std::string &bitcode, LLVMContext &context) {
  auto module = parseBitcodeFile(MemoryBufferRef(bitcode, "dxil"), context);
  if (module) {
    lower_lib_handles(**module);
    lower_bound_handles(**module);
    lower_heap_handles(**module);
  }
  return module;
}

// a ray tracing shader's context, which its visible function takes: DXMTRayContext in dxmt_command.metal, field for
// field
enum RayContextField : unsigned {
  RayShaders,
  RayDispatch,
  RayRootArguments,
  RayStaticSamplers,
  RayLocalArguments,
  RayPayload,
  RayIndex,
  RayFlagsField,
  RayWorldOrigin,
  RayTMinField,
  RayWorldDirection,
  RayTCurrentField,
  RayObjectOrigin,
  RayInstanceIndex,
  RayObjectDirection,
  RayInstanceID,
  RayObjectToWorld,
  RayWorldToObject,
  RayGeometryIndex,
  RayPrimitiveIndex,
  RayHitKind,
  RayVerdict,
  RayReportedT,
  RayReportedKind,
  RayReportedSize,
  RayAccepted,
  RayAnyHit,
  RayEnded,
  RayCommitted,
  RayCallable,
  RayAttributes,
};

StructType *
ray_context_type(LLVMContext &ctx) {
  if (auto ty = StructType::getTypeByName(ctx, "dxmt.ray_context"))
    return ty;
  auto table = StructType::getTypeByName(ctx, "struct._visible_function_table_t");
  if (!table)
    table = StructType::create(ctx, "struct._visible_function_table_t");
  Type *i32 = Type::getInt32Ty(ctx), *i64 = Type::getInt64Ty(ctx), *f32 = Type::getFloatTy(ctx);
  auto vector = ArrayType::get(f32, 3), matrix = ArrayType::get(f32, 12);
  return StructType::create(
      ctx,
      {table->getPointerTo(1), i32->getPointerTo(2), i64->getPointerTo(2), i64->getPointerTo(2), i64,
       Type::getInt8PtrTy(ctx), ArrayType::get(i32, 3), i32, vector, f32, vector, f32, vector, i32, vector, i32,
       matrix, matrix, i32, i32, i32, i32, f32, i32, i32, i32, i32, i32, i32, i32, ArrayType::get(i32, 8)},
      "dxmt.ray_context"
  );
}

// DxilConstants.h ResourceKind
shader::common::ResourceType
resource_type(uint64_t kind) {
  using shader::common::ResourceType;
  constexpr ResourceType types[] = {
      ResourceType::NonApplicable,         ResourceType::Texture1D,      ResourceType::Texture2D,
      ResourceType::Texture2DMultisampled, ResourceType::Texture3D,      ResourceType::TextureCube,
      ResourceType::Texture1DArray,        ResourceType::Texture2DArray, ResourceType::Texture2DMultisampledArray,
      ResourceType::TextureCubeArray,      ResourceType::TextureBuffer,
  };
  return kind < std::size(types) ? types[kind] : ResourceType::NonApplicable;
}

// DxilConstants.h ComponentType of a typed resource element
shader::common::ScalerDataType
scaler_type(uint64_t component) {
  using shader::common::ScalerDataType;
  switch (component) {
  case 2:
  case 4:
    return ScalerDataType::Int; // I16, I32
  case 3:
  case 5:
    return ScalerDataType::Uint; // U16, U32
  case 10:
    return ScalerDataType::Double;
  default:
    return ScalerDataType::Float;
  }
}

// a module's resources: their ranges, from dx.resources, and how its functions use them, from the operations that
// consume each handle, as DXBC decoding does
void
read_resources(Module &M, ShaderInfo &info) {
if (auto resources = M.getNamedMetadata("dx.resources")) {
  auto lists = resources->getOperand(0);
  for (unsigned cls = SRV; cls <= SamplerClass; cls++) {
    auto list = dyn_cast_or_null<MDNode>(lists->getOperand(cls).get());
    for (auto &op : list ? list->operands() : ArrayRef<MDOperand>{}) {
      auto r = cast<MDNode>(op.get());
      auto id = (uint32_t)md_uint(r->getOperand(0));
      ResourceRange range = {
          .range_id = id,
          .lower_bound = (uint32_t)md_uint(r->getOperand(4)),
          .size = (uint32_t)md_uint(r->getOperand(5)),
          .space = (uint32_t)md_uint(r->getOperand(3)),
      };
      auto kind = cls <= UAV ? md_uint(r->getOperand(6)) : 0;
      auto tags = cls <= UAV ? dyn_cast_or_null<MDNode>(r->getOperand(cls == SRV ? 8 : 10).get()) : nullptr;
      auto element = tag(tags, 0);
      auto stride = tag(tags, 1);
      auto type = element ? scaler_type(md_uint(*element)) : shader::common::ScalerDataType::Uint;
      bool feedback = kind == ResourceKind_FeedbackTexture2D || kind == ResourceKind_FeedbackTexture2DArray;
      switch (cls) {
      case SRV:
        info.srvMap[id] = {
            .range = range,
            .scaler_type = type,
            .resource_type = resource_type(kind),
            .structure_stride = stride ? (uint32_t)md_uint(*stride) : 0,
        };
        break;
      case UAV:
        // a feedback map is an array of region grids of bits, whatever its paired texture is
        info.uavMap[id] = {
            .range = range,
            .scaler_type = feedback ? shader::common::ScalerDataType::Uint : type,
            .resource_type = feedback ? shader::common::ResourceType::Texture2DArray : resource_type(kind),
            .global_coherent = md_uint(r->getOperand(7)) != 0,
            .rasterizer_order = md_uint(r->getOperand(9)) != 0,
            .with_counter = md_uint(r->getOperand(8)) != 0,
            .structure_stride = stride ? (uint32_t)md_uint(*stride) : 0,
        };
        break;
      case CBV:
        info.cbufferMap[id] = {.range = range, .size_in_vec4 = (uint32_t)(md_uint(r->getOperand(6)) + 15) / 16};
        break;
      default:
        info.samplerMap[id] = {.range = range};
        break;
      }
    }
  }
}


  auto handle_of = [](Value *v, uint32_t cls) -> CallInst * {
    auto call = is_dx_op(v) ? cast<CallInst>(v) : nullptr;
    return call && opcode(call) == CreateHandle && cast<ConstantInt>(call->getArgOperand(1))->getZExtValue() == cls
               ? call
               : nullptr;
  };
  for (auto &F : M) {
    if (!F.getName().startswith("dx.op."))
      continue;
    for (auto user : F.users()) {
      auto call = cast<CallInst>(user);
      if (opcode(call) != CreateHandle)
        continue;
      auto cls = cast<ConstantInt>(call->getArgOperand(1))->getZExtValue();
      auto id = (uint32_t)cast<ConstantInt>(call->getArgOperand(2))->getZExtValue();
      for (auto use : call->users()) {
        auto consumer = cast<CallInst>(use);
        // a resource consumed together with a sampler is sampled, or compared by a comparing operation, as DXBC's
        // sample_c marks it
        auto how = opcode(consumer);
        bool compared = how == SampleCmp || how == SampleCmpLevelZero || how == TextureGatherCmp;
        bool sampled = llvm::any_of(consumer->args(), [&](auto &arg) { return handle_of(arg, SamplerClass); });
        // feedback is written with atomics, which read
        // and an acceleration structure is read by the query or the ray it starts
        bool read = !consumer->getType()->isVoidTy() ||
                    (how >= WriteSamplerFeedback && how <= WriteSamplerFeedbackGrad) ||
                    how == RayQuery_TraceRayInline || how == TraceRay;
        bool written = !consumer->onlyReadsMemory();
        if (cls == SRV) {
          info.srvMap[id].sampled |= sampled && !compared;
          info.srvMap[id].compared |= compared;
          info.srvMap[id].read |= read && !sampled;
        } else if (cls == UAV) {
          info.uavMap[id].read |= read;
          info.uavMap[id].written |= written;
        }
      }
    }
  }
}

// links into a library's module the functions it calls that another library of its state object defines, with their
// resources: a library numbers its resource ranges from 0, so the other library's follow the module's own
Error
link_libraries(Module &M, ArrayRef<SM50ShaderInternal *> libraries, bool &linked) {
  auto &context = M.getContext();
  auto i32 = Type::getInt32Ty(context);
  auto missing = [&]() -> Function * {
    for (auto &F : M)
      if (F.isDeclaration() && !F.use_empty() && !F.isIntrinsic() && !F.getName().startswith("dx.op."))
        return &F;
    return nullptr;
  };
  // a module's lists of resources by class, of which some may be null
  auto lists_of = [](Module &of) -> MDNode * {
    auto resources = of.getNamedMetadata("dx.resources");
    return resources && resources->getNumOperands() ? resources->getOperand(0) : nullptr;
  };
  auto list_of = [](MDNode *lists, unsigned cls) {
    return lists ? dyn_cast_or_null<MDNode>(lists->getOperand(cls).get()) : nullptr;
  };
  while (auto needed = missing()) {
    std::unique_ptr<Module> definition;
    for (auto library : libraries) {
      auto other = parse(library->dxil, context);
      if (!other)
        return other.takeError();
      auto defined = (*other)->getFunction(needed->getName());
      if (defined && !defined->isDeclaration()) {
        definition = std::move(*other);
        break;
      }
    }
    if (!definition)
      return make_error<UnsupportedFeature>(std::format("no library defines {}", needed->getName().str()));
    auto lists = lists_of(M), other_lists = lists_of(*definition);
    std::vector<Metadata *> merged;
    for (unsigned cls = SRV; cls <= SamplerClass; cls++) {
      auto list = list_of(lists, cls), other_list = list_of(other_lists, cls);
      std::vector<Metadata *> records;
      if (list)
        records.assign(list->op_begin(), list->op_end());
      uint32_t first = records.size();
      for (auto &op : other_list ? other_list->operands() : ArrayRef<MDOperand>{}) {
        auto record = cast<MDNode>(op.get());
        std::vector<Metadata *> fields(record->op_begin(), record->op_end());
        fields[0] = ConstantAsMetadata::get(ConstantInt::get(i32, first + md_uint(record->getOperand(0))));
        // its variable stays in its library: the handles of it are already of its range
        fields[1] = nullptr;
        records.push_back(MDNode::get(context, fields));
      }
      merged.push_back(records.empty() ? nullptr : MDNode::get(context, records));
      for (auto &F : *definition)
        if (F.getName().startswith("dx.op.createHandle"))
          for (auto user : F.users())
            if (auto call = cast<CallInst>(user); cast<ConstantInt>(call->getArgOperand(1))->getZExtValue() == cls)
              call->setArgOperand(2, ConstantInt::get(i32, first + cast<ConstantInt>(call->getArgOperand(2))->getZExtValue()));
    }
    auto resources = M.getOrInsertNamedMetadata("dx.resources");
    if (resources->getNumOperands())
      resources->setOperand(0, MDNode::get(context, merged));
    else
      resources->addOperand(MDNode::get(context, merged));
    for (auto &md : make_early_inc_range(definition->named_metadata()))
      definition->eraseNamedMetadata(&md);
    if (Linker::linkModules(M, std::move(definition), Linker::LinkOnlyNeeded))
      return make_error<UnsupportedFeature>("failed to link a library's function");
    linked = true;
  }
  return Error::success();
}

} // namespace

llvm::Error
read_dxil(
    const void *part, SM50ShaderInternal *shader, CSignatureParser &inputParser, CSignatureParser5 &outputParser,
    CSignatureParser &patchParser
) {
  // DxilProgramHeader: version, size, then DxilBitcodeHeader: magic, version, offset, size
  auto header = (const uint32_t *)part;
  shader->shader_type = (D3D10_SB_TOKENIZED_PROGRAM_TYPE)(header[0] >> 16);
  // a mesh or amplification shader has no DXBC counterpart: its stage is built here (convert_dxil_mesh_stage)
  bool mesh_stage =
      shader->shader_type == D3D12_SB_MESH_SHADER || shader->shader_type == D3D12_SB_AMPLIFICATION_SHADER;
  // a library's functions are ray tracing shaders, each compiled on its own (convert_dxil_ray_shader)
  bool library = shader->shader_type == kShaderLibrary;
  if (!library && shader->shader_type != D3D10_SB_PIXEL_SHADER && shader->shader_type != D3D10_SB_VERTEX_SHADER &&
      shader->shader_type != D3D11_SB_HULL_SHADER && shader->shader_type != D3D11_SB_DOMAIN_SHADER &&
      shader->shader_type != D3D10_SB_GEOMETRY_SHADER && shader->shader_type != D3D11_SB_COMPUTE_SHADER && !mesh_stage)
    return make_error<UnsupportedFeature>(std::format("DXIL shader kind {} is not supported yet", header[0] >> 16));
  shader->dxil.assign((const char *)&header[2] + header[4], header[5]);

  auto &info = shader->shader_info;
  LLVMContext context;
  context.setOpaquePointers(false);
  auto module = parse(shader->dxil, context);
  if (!module)
    return module.takeError();
  auto &M = **module;
  auto entry = read_entry(M);

  read_resources(M, info);

  // usage, from the operations that consume each handle, as DXBC decoding does
  // inputs the system provides without a signature element, by the operation that reads them
  constexpr std::pair<uint32_t, D3D10_SB_OPERAND_TYPE> attribute_inputs[] = {
      {ThreadId, D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID},
      {GroupId, D3D11_SB_OPERAND_TYPE_INPUT_THREAD_GROUP_ID},
      {ThreadIdInGroup, D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP},
      {FlattenedThreadIdInGroup, D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP_FLATTENED},
      {Coverage, D3D11_SB_OPERAND_TYPE_INPUT_COVERAGE_MASK},
  };
  std::set<uint32_t> used_ops;
  for (auto &F : M) {
    if (!F.getName().startswith("dx.op."))
      continue;
    for (auto user : F.users()) {
      auto call = cast<CallInst>(user);
      auto op = opcode(call);
      used_ops.insert(op);
      // evaluated attributes are interpolated in the shader, as DXBC's eval instructions mark them
      if ((op == EvalSnapped || op == EvalSampleIndex || op == EvalCentroid) &&
          isa<ConstantInt>(call->getArgOperand(2)))
        info.pull_mode_reg_mask |= 1u
                                   << (entry.input[cast<ConstantInt>(call->getArgOperand(1))->getZExtValue()].row +
                                       cast<ConstantInt>(call->getArgOperand(2))->getZExtValue());
    }
  }

  if (library) {
    // each shader's entry point has its kind (tag 8) and the sizes of its payload (6) and attributes (7)
    for (auto candidate : M.getNamedMetadata("dx.entryPoints")->operands()) {
      auto f = mdconst::extract_or_null<Function>(candidate->getOperand(0));
      auto properties = dyn_cast_or_null<MDNode>(candidate->getOperand(4).get());
      auto kind = tag(properties, 8);
      if (!f || !kind)
        continue;
      auto size = [&](uint64_t t) { return tag(properties, t) ? (uint32_t)md_uint(*tag(properties, t)) : 0; };
      shader->ray_shaders.push_back({f->getName().str(), {(uint32_t)md_uint(*kind), size(6), size(7)}});
    }
    return Error::success();
  }

  // declarations, synthesized from the signature elements and fed to the DXBC handlers
  auto declare = [&](D3D10_SB_OPCODE_TYPE opcode, D3D10_SB_OPERAND_TYPE type, uint32_t reg, uint32_t mask,
                     D3D10_SB_NAME name, uint32_t interpolation, uint32_t phase) {
    D3D10ShaderBinary::CInstruction inst(opcode);
    inst.m_NumOperands = 1;
    inst.m_Operands[0].m_Type = type;
    inst.m_Operands[0].m_IndexDimension = D3D10_SB_OPERAND_INDEX_1D;
    inst.m_Operands[0].m_Index[0].m_RegIndex = reg;
    inst.m_Operands[0].m_WriteMask = mask << 4;
    switch (opcode) {
    case D3D10_SB_OPCODE_DCL_INPUT_PS:
      inst.m_InputPSDecl.InterpolationMode = (D3D10_SB_INTERPOLATION_MODE)interpolation;
      break;
    case D3D10_SB_OPCODE_DCL_INPUT_PS_SIV:
      inst.m_InputPSDeclSIV = {name, (D3D10_SB_INTERPOLATION_MODE)interpolation};
      break;
    case D3D10_SB_OPCODE_DCL_INPUT_PS_SGV:
      inst.m_InputPSDeclSGV = {name, (D3D10_SB_INTERPOLATION_MODE)interpolation};
      break;
    case D3D10_SB_OPCODE_DCL_INPUT_SGV:
      inst.m_InputDeclSGV.Name = name;
      break;
    case D3D10_SB_OPCODE_DCL_INPUT_SIV:
      inst.m_InputDeclSIV.Name = name;
      break;
    case D3D10_SB_OPCODE_DCL_OUTPUT_SIV:
      inst.m_OutputDeclSIV.Name = name;
      break;
    default:
      break;
    }
    handle_signature(inputParser, outputParser, inst, shader, phase);
  };
  // the container's own signature names each register's system value
  auto system_value = [](auto &parser, uint32_t reg, uint32_t mask) {
    const D3D11_SIGNATURE_PARAMETER *params;
    parser.GetParameters(&params);
    for (unsigned i = 0; i < parser.GetNumParameters(); i++)
      if (params[i].Register == reg && (params[i].Mask & mask))
        return params[i].SystemValue;
    return D3D10_SB_NAME_UNDEFINED;
  };
  // values the system generates rather than a previous stage produces
  auto generated = [](D3D10_SB_NAME name) {
    return name == D3D10_SB_NAME_VERTEX_ID || name == D3D10_SB_NAME_INSTANCE_ID || name == D3D10_SB_NAME_PRIMITIVE_ID ||
           name == D3D10_SB_NAME_IS_FRONT_FACE || name == D3D10_SB_NAME_SAMPLE_INDEX;
  };
  bool pixel = shader->shader_type == D3D10_SB_PIXEL_SHADER, hull = shader->shader_type == D3D11_SB_HULL_SHADER,
       domain = shader->shader_type == D3D11_SB_DOMAIN_SHADER, geometry = shader->shader_type == D3D10_SB_GEOMETRY_SHADER;
  // one declaration per register and system value, as in DXBC: clip and cull distances packed into one register
  // share it. user values keep one declaration per element, since the handlers name them by register and mask.
  // key: opcode, operand type, register, system value, element id (user values only), interpolation, phase (a hull
  // shader's patch constants are declared in its patch-constant phase)
  using Key =
      std::tuple<D3D10_SB_OPCODE_TYPE, D3D10_SB_OPERAND_TYPE, uint32_t, D3D10_SB_NAME, uint32_t, uint32_t, uint32_t>;
  std::map<Key, uint32_t> declarations;
  auto rows = [&](auto &elements, auto &parser, auto opcode_of, D3D10_SB_OPERAND_TYPE type, uint32_t phase = ~0u) {
    for (unsigned id = 0; id < elements.size(); id++) {
      auto &e = elements[id];
      for (uint32_t row = e.row; row < e.row + e.rows; row++) {
        uint32_t mask = ((1u << e.cols) - 1) << e.col;
        auto name = system_value(parser, row, mask);
        declarations[{
            opcode_of(name), type, row, name, name == D3D10_SB_NAME_UNDEFINED ? id : ~0u, e.interpolation, phase
        }] |= mask;
      }
    }
  };
  // control points and a geometry shader's vertices need no declaration: their stages size them from the one before
  if (!hull && !domain && !geometry && !mesh_stage)
    rows(
        entry.input, inputParser,
        [&](D3D10_SB_NAME name) {
          return name == D3D10_SB_NAME_UNDEFINED ? (pixel ? D3D10_SB_OPCODE_DCL_INPUT_PS : D3D10_SB_OPCODE_DCL_INPUT)
                 : generated(name) ? (pixel ? D3D10_SB_OPCODE_DCL_INPUT_PS_SGV : D3D10_SB_OPCODE_DCL_INPUT_SGV)
                                   : (pixel ? D3D10_SB_OPCODE_DCL_INPUT_PS_SIV : D3D10_SB_OPCODE_DCL_INPUT_SIV);
        },
        D3D10_SB_OPERAND_TYPE_INPUT
    );
  // pixel outputs other than targets are separate operand types, not registers
  constexpr std::pair<uint32_t, D3D10_SB_OPERAND_TYPE> special[] = {
      {CoverageOutput, D3D10_SB_OPERAND_TYPE_OUTPUT_COVERAGE_MASK},
      {Depth, D3D10_SB_OPERAND_TYPE_OUTPUT_DEPTH},
      {DepthLessEqual, D3D11_SB_OPERAND_TYPE_OUTPUT_DEPTH_LESS_EQUAL},
      {DepthGreaterEqual, D3D11_SB_OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL},
      {StencilRef, D3D11_SB_OPERAND_TYPE_OUTPUT_STENCIL_REF},
  };
  std::vector<Element> registers;
  for (auto &e : mesh_stage ? std::vector<Element>{} : entry.output) {
    auto s = std::find_if(std::begin(special), std::end(special), [&](auto &s) { return s.first == e.semantic; });
    if (s != std::end(special))
      declarations[{D3D10_SB_OPCODE_DCL_OUTPUT, s->second, 0, D3D10_SB_NAME_UNDEFINED, ~0u, 0, ~0u}] = 1;
    registers.push_back(s != std::end(special) ? Element{} : e); // keeps element ids, with no rows for specials
  }
  rows(
      registers, *outputParser.Signature(0),
      // a hull shader's control points are plain registers, as in DXBC
      [&](D3D10_SB_NAME name) {
        return name == D3D10_SB_NAME_UNDEFINED || hull ? D3D10_SB_OPCODE_DCL_OUTPUT : D3D10_SB_OPCODE_DCL_OUTPUT_SIV;
      },
      D3D10_SB_OPERAND_TYPE_OUTPUT
  );
  if (hull)
    rows(
        entry.patch, patchParser,
        [](D3D10_SB_NAME name) {
          return name == D3D10_SB_NAME_UNDEFINED ? D3D10_SB_OPCODE_DCL_OUTPUT : D3D10_SB_OPCODE_DCL_OUTPUT_SIV;
        },
        D3D10_SB_OPERAND_TYPE_OUTPUT, 0
    );
  if (domain)
    rows(
        entry.patch, patchParser, [](D3D10_SB_NAME) { return D3D10_SB_OPCODE_DCL_INPUT; },
        D3D11_SB_OPERAND_TYPE_INPUT_PATCH_CONSTANT
    );
  for (auto &[key, mask] : declarations) {
    auto [opcode, type, row, name, id, interpolation, phase] = key;
    declare(opcode, type, row, mask, name, interpolation, phase);
  }
  // groupshared 64-bit atomics hash their locks with the threadgroup's position (`locked`)
  for (auto &F : M)
    for (auto &BB : F)
      for (auto &I : BB)
        if (auto rmw = dyn_cast<AtomicRMWInst>(&I); rmw && rmw->getType()->isIntegerTy(64))
          used_ops.insert(GroupId);
        else if (auto cx = dyn_cast<AtomicCmpXchgInst>(&I); cx && cx->getCompareOperand()->getType()->isIntegerTy(64))
          used_ops.insert(GroupId);
  // threadgroups work together when one waits for what another does: a loop that reads globally coherent memory
  auto coherent = [&](Value *handle) {
    auto made = dyn_cast<CallInst>(handle);
    if (!made || !is_dx_op(made))
      return false;
    // DxilResourceProperties: bit 14 of the first word is IsGloballyCoherent
    if (opcode(made) == AnnotateHandle)
      return (cast<ConstantInt>(cast<Constant>(made->getArgOperand(2))->getAggregateElement(0u))->getZExtValue() >> 14 & 1) != 0;
    if (opcode(made) != CreateHandle || cast<ConstantInt>(made->getArgOperand(1))->getZExtValue() != UAV)
      return false;
    auto uav = info.uavMap.find(cast<ConstantInt>(made->getArgOperand(2))->getZExtValue());
    return uav != info.uavMap.end() && uav->second.global_coherent;
  };
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    DominatorTree dominators(F);
    LoopInfo loops(dominators);
    for (auto &BB : F)
      if (loops.getLoopFor(&BB))
        for (auto &I : BB)
          if (auto call = dyn_cast<CallInst>(&I); call && is_dx_op(call))
            switch (opcode(call)) {
            case BufferLoad:
            case RawBufferLoad:
            case TextureLoad:
            case AtomicBinOp:
            case AtomicCompareExchange:
              shader->groups_work_together |= coherent(call->getArgOperand(1));
            }
  }
  for (auto [op, type] : attribute_inputs)
    if (used_ops.count(op) && !mesh_stage)
      declare(D3D10_SB_OPCODE_DCL_INPUT, type, 0, 0, D3D10_SB_NAME_UNDEFINED, 0, ~0u);
  if (used_ops.count(SampleIndex))
    shader->func_signature.DefineInput(air::InputSampleIndex{});
  // a SIMD group's lane and width are inputs of every Metal stage but vertex, for the operations that read them
  auto used = [&](std::initializer_list<uint32_t> ops) {
    return llvm::any_of(ops, [&](auto op) { return used_ops.count(op); });
  };
  bool compute = shader->shader_type == D3D11_SB_COMPUTE_SHADER;
  if (shader->shader_type != D3D10_SB_VERTEX_SHADER &&
      (used({WaveGetLaneIndex, WaveGetLaneCount, WavePrefixOp, WaveMultiPrefixOp, WaveMultiPrefixBitCount}) ||
       (compute && used({DerivFineX, DerivFineY})))) {
    shader->func_signature.DefineInput(air::InputThreadIndexInSIMDGroup{});
    shader->func_signature.DefineInput(air::InputThreadsPerSIMDGroup{});
  }
  info.use_msad = used_ops.count(Msad);
  info.use_samplepos |= used({Texture2DMSGetSamplePosition, RenderTargetGetSamplePosition});

  // kDxilShaderFlagsTag, whose bit 3 is ForceEarlyDepthStencil
  if (auto flags = tag(entry.properties, 0); flags && (md_uint(*flags) & (1u << 3)))
    shader->func_signature.UseEarlyFragmentTests();
  // kDxilNumThreadsTag
  if (auto threads = tag(entry.properties, 4)) {
    auto xyz = cast<MDNode>(threads->get());
    for (unsigned i = 0; i < 3; i++)
      shader->threadgroup_size[i] = md_uint(xyz->getOperand(i));
    shader->func_signature.UseMaxWorkgroupSize(
        shader->threadgroup_size[0] * shader->threadgroup_size[1] * shader->threadgroup_size[2]
    );
  }
  // kDxilMSStateTag: threads, vertex and primitive limits, topology (1 lines, 2 triangles), payload bytes.
  // kDxilASStateTag: threads, payload bytes
  auto mesh_threads = [&](const MDOperand &xyz) {
    for (unsigned i = 0; i < 3; i++)
      shader->threadgroup_size[i] = md_uint(cast<MDNode>(xyz.get())->getOperand(i));
  };
  if (auto state = tag(entry.properties, 9)) {
    auto ms = cast<MDNode>(state->get());
    mesh_threads(ms->getOperand(0));
    shader->mesh_max_vertices = md_uint(ms->getOperand(1));
    shader->mesh_max_primitives = md_uint(ms->getOperand(2));
    shader->mesh_lines = md_uint(ms->getOperand(3)) == 1;
    shader->payload_size = md_uint(ms->getOperand(4));
  }
  if (auto state = tag(entry.properties, 10)) {
    auto as = cast<MDNode>(state->get());
    mesh_threads(as->getOperand(0));
    shader->payload_size = md_uint(as->getOperand(1));
  }
  if (shader->shader_type == D3D12_SB_MESH_SHADER) {
    // each element with the semantic the container's signature gives its register
    auto outputs = [&](auto &elements, const CSignatureParser &parser, auto &to) {
      const D3D11_SIGNATURE_PARAMETER *params;
      auto count = parser.GetParameters(&params);
      for (auto &e : elements) {
        SM50ShaderInternal::MeshOutput output{e.semantic, e.row, e.rows, e.col, e.cols, RegisterComponentType::Unknown, {}, 0};
        for (unsigned i = 0; i < count; i++)
          if (params[i].Register == e.row && (params[i].Mask >> e.col & 1)) {
            output.type = widened((RegisterComponentType)params[i].ComponentType);
            output.name = params[i].SemanticName;
            output.index = params[i].SemanticIndex;
          }
        to.push_back(output);
      }
    };
    outputs(entry.output, *outputParser.Signature(0), shader->mesh_outputs[0]);
    outputs(entry.patch, patchParser, shader->mesh_outputs[1]);
  }
  // kDxilWaveSizeTag, a number or (DXIL 1.8) a tuple starting with it
  if (auto size = tag(entry.properties, 11)) {
    auto tuple = dyn_cast<MDNode>(size->get());
    info.wave_size = md_uint(tuple ? tuple->getOperand(0) : *size);
  }
  // SM 6.6 quads: in groups with even X and Y, 2x2 blocks of thread IDs. Metal's are four consecutive threads, which
  // other groups' quads already are, so these groups' IDs are derived from the flattened one and the group's
  auto &n = shader->threadgroup_size;
  info.quads_2d = compute && n[0] % 2 == 0 && n[1] % 2 == 0 &&
                  used(
                      {DerivCoarseX, DerivCoarseY, DerivFineX, DerivFineY, Sample, SampleBias, SampleCmp, CalculateLOD,
                       QuadOp, QuadReadLaneAt}
                  );
  if (info.quads_2d)
    for (auto type :
         {D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP_FLATTENED, D3D11_SB_OPERAND_TYPE_INPUT_THREAD_GROUP_ID})
      declare(D3D10_SB_OPCODE_DCL_INPUT, type, 0, 0, D3D10_SB_NAME_UNDEFINED, 0, ~0u);

  // kDxilGSStateTag: input primitive, max vertices, active streams, output topology, instances. the enums are DXBC's
  if (auto state = tag(entry.properties, 1)) {
    auto gs = cast<MDNode>(state->get());
    shader->gs_input_primitive = (D3D10_SB_PRIMITIVE)md_uint(gs->getOperand(0));
    shader->gs_max_vertex_output = md_uint(gs->getOperand(1));
    shader->gs_output_topology = (D3D10_SB_PRIMITIVE_TOPOLOGY)md_uint(gs->getOperand(3));
    shader->gs_instance_count = md_uint(gs->getOperand(4));
  }
  // kDxilDSStateTag: domain, input control points. kDxilHSStateTag: patch-constant function, input and output
  // control points, domain, partitioning, output primitive, max factor. the enums are DXBC's
  if (auto state = tag(entry.properties, 2)) {
    auto ds = cast<MDNode>(state->get());
    shader->tessellation_domain = (D3D11_SB_TESSELLATOR_DOMAIN)md_uint(ds->getOperand(0));
    shader->input_control_point_count = md_uint(ds->getOperand(1));
  }
  Function *patch_constant = nullptr;
  if (auto state = tag(entry.properties, 3)) {
    auto hs = cast<MDNode>(state->get());
    patch_constant = mdconst::extract<Function>(hs->getOperand(0));
    shader->input_control_point_count = md_uint(hs->getOperand(1));
    shader->output_control_point_count = md_uint(hs->getOperand(2));
    shader->tessellation_domain = (D3D11_SB_TESSELLATOR_DOMAIN)md_uint(hs->getOperand(3));
    shader->tessellation_partition = (D3D11_SB_TESSELLATOR_PARTITIONING)md_uint(hs->getOperand(4));
    shader->tessellator_output_primitive = (D3D11_SB_TESSELLATOR_OUTPUT_PRIMITIVE)md_uint(hs->getOperand(5));
    shader->max_tesselation_factor = mdconst::extract<ConstantFP>(hs->getOperand(6))->getValueAPF().convertToFloat();
    // the control-point function runs once per output point, the patch-constant function once
    shader->hull_maximum_threads_per_patch =
        std::max({shader->input_control_point_count, shader->output_control_point_count, 1u});
    info.no_control_point_phase_passthrough = true;
    info.output_control_point_read = used_ops.count(LoadOutputControlPoint);
  }

  // the stage runs its entry function where the DXBC path runs its instructions. a hull shader runs it as DXBC's
  // control-point phase, then its patch-constant function as one fork phase, each ending in a threadgroup barrier
  auto block = [&](const char *name) { return shader->bbs.emplace_back(std::make_unique<BasicBlock>(name)).get(); };
  auto phase = [&](BasicBlock *from, uint32_t instances, Function *function) {
    auto active = block("dxil_phase"), sync = block("dxil_phase_end");
    from->target = BasicBlockInstanceBarrier{instances, active, sync};
    active->target = BasicBlockDXIL{shader, function->getName().str(), sync};
    sync->instructions.push_back(
        InstSync{
            .uav_boundary = InstSync::UAVBoundary::none,
            .tgsm_memory_barrier = true,
            .tgsm_execution_barrier = true,
        }
    );
    return sync;
  };
  auto start = block("dxil_start"), end = block("dxil_end");
  end->target = BasicBlockReturn{};
  if (!patch_constant) {
    start->target = BasicBlockDXIL{shader, entry.function->getName().str(), end};
    return Error::success();
  }
  auto last = phase(phase(start, shader->output_control_point_count, entry.function), 1, patch_constant);
  // control points the patch-constant function reads were kept in threadgroup memory; they are copied out last
  if (info.output_control_point_read)
    last->target = BasicBlockHullShaderWriteOutput{shader->output_control_point_count, end};
  else
    last->target = BasicBlockUnconditionalBranch{end};
  return Error::success();
}

llvm::Expected<llvm::BasicBlock *>
convert_dxil(
    SM50ShaderInternal *shader, context &ctx, const std::string &function, llvm::BasicBlock *epilogue,
    std::unique_ptr<llvm::Module> linked, ShaderInfo *linked_info
) {
  // a required wave size runs only on a GPU whose SIMD groups have it (SM 6.6)
  auto wave_size = shader->shader_info.wave_size;
  if (wave_size && ctx.simd_width && wave_size != ctx.simd_width)
    return make_error<UnsupportedFeature>(
        std::format("the shader needs waves of {} lanes, and this GPU's have {}", wave_size, ctx.simd_width)
    );
  auto module = linked ? Expected<std::unique_ptr<Module>>(std::move(linked)) : parse(shader->dxil, ctx.llvm);
  if (!module)
    return module.takeError();
  auto entry = read_entry(**module, function); // the signatures, which outlive the module
  // a stage is one function of Metal: what the function calls of its module is inlined
  if (auto start = (*module)->getFunction(function))
    for (bool inlined = true; inlined;) {
      inlined = false;
      for (auto &I : make_early_inc_range(instructions(*start)))
        if (auto call = dyn_cast<CallInst>(&I);
            call && call->getCalledFunction() && !call->getCalledFunction()->isDeclaration()) {
          InlineFunctionInfo inlining;
          if (!InlineFunction(*call, inlining).isSuccess())
            return make_error<UnsupportedFeature>(
                std::format("a call of {} cannot be inlined", call->getCalledFunction()->getName().str())
            );
          inlined = true;
        }
    }
  // one Metal function can run several DXIL functions (a hull shader's phases, the stages tessellation fuses), each
  // linked in on its own: only this one keeps its body, under a name no other takes, and module variables stay private
  // to it
  auto name = "dxil." + std::to_string(ctx.module.getFunctionList().size()) + "." + function;
  for (auto &G : (*module)->functions())
    if (G.getName() == function)
      G.setName(name);
    else if (!G.isDeclaration())
      G.deleteBody();
  // module variables in address space 0 belong to each thread, and AIR has none: constants move to the constant
  // address space, the others become variables of the function, set to their initial value each time it runs
  auto kept = (*module)->getFunction(name);
  for (auto &G : make_early_inc_range((*module)->globals())) {
    if (G.isDeclaration() || G.getAddressSpace() || !kept)
      continue;
    Value *to;
    if (G.isConstant()) {
      to = new GlobalVariable(
          **module, G.getValueType(), true, GlobalValue::InternalLinkage, G.getInitializer(), G.getName(), nullptr,
          GlobalValue::NotThreadLocal, (unsigned)air::AddressSpace::constant
      );
    } else {
      IRBuilder<> ir(&*kept->getEntryBlock().getFirstInsertionPt());
      to = ir.CreateAlloca(G.getValueType());
      if (!isa<UndefValue>(G.getInitializer()))
        ir.CreateStore(G.getInitializer(), to);
    }
    if (auto err = retarget(&G, to))
      return err;
    G.eraseFromParent();
  }
  // DXC declares groupshared variables without a definition; each threadgroup's is its own
  for (auto &G : (*module)->globals())
    if (G.getAddressSpace() == (unsigned)air::AddressSpace::threadgroup && G.isDeclaration())
      G.setInitializer(UndefValue::get(G.getValueType()));
  for (auto &G : (*module)->globals())
    if (!G.isDeclaration())
      G.setLinkage(GlobalValue::InternalLinkage);
  for (auto &md : make_early_inc_range((*module)->named_metadata()))
    (*module)->eraseNamedMetadata(&md);
  (*module)->setDataLayout(ctx.module.getDataLayout());
  (*module)->setTargetTriple(ctx.module.getTargetTriple());
  if (Linker::linkModules(ctx.module, std::move(*module)))
    return make_error<UnsupportedFeature>("failed to link the DXIL module");

  // the function's body runs at this point of the stage, then continues at epilogue
  auto F = ctx.module.getFunction(name);
  if (!F)
    return make_error<UnsupportedFeature>(std::format("DXIL function {} not found", function));
  auto body = &F->getEntryBlock();
  auto &entry_bb = ctx.function->getEntryBlock();
  // a ray tracing shader's function takes its payload and attributes, which the visible function has untyped
  for (auto &argument : F->args())
    argument.replaceAllUsesWith(new BitCastInst(
        ctx.resource.ray_arguments[argument.getArgNo()], argument.getType(), "", &*entry_bb.getFirstInsertionPt()
    ));
  for (auto &I : make_early_inc_range(*body))
    if (isa<AllocaInst>(I))
      I.moveBefore(&*entry_bb.getFirstInsertionPt());
  for (auto &BB : make_early_inc_range(*F)) {
    BB.moveBefore(epilogue);
    if (auto ret = dyn_cast<ReturnInst>(BB.getTerminator())) {
      BranchInst::Create(epilogue, ret);
      ret->eraseFromParent();
    }
  }
  F->eraseFromParent();

  auto &ir = ctx.builder;
  auto &air = ctx.air;
  auto &info = linked_info ? *linked_info : shader->shader_info;
  Converter dxbc(air, ctx, ctx.resource);

  struct Handle {
    llvm::Optional<TextureResourceHandle> texture;
    llvm::Optional<BufferResourceHandle> buffer;
    llvm::Optional<ConstantBufferDescriptor> cbuffer;
    llvm::Optional<SamplerDescriptor> sampler;
    llvm::Optional<CounterDescriptor> counter;
  };
  DenseMap<Value *, Handle> handles;

  std::vector<CallInst *> calls;
  for (auto &BB : *ctx.function)
    for (auto &I : BB)
      if (is_dx_op(&I))
        calls.push_back(cast<CallInst>(&I));

  // a ray tracing shader's system values are fields of its context, and its calls of other shaders go through the
  // pipeline's function table: the runtime's slots (dxmt_command.metal) or, from there, the shaders'
  auto ray = ctx.resource.ray_context;
  auto ray_ty = ray ? ray_context_type(ctx.llvm) : nullptr;
  auto ray_field = [&](unsigned field) { return ctx.builder.CreateStructGEP(ray_ty, ray, field); };
  auto ray_value = [&](unsigned field, Value *element = nullptr) -> Value * {
    auto &b = ctx.builder;
    auto ty = ray_ty->getElementType(field);
    if (!element)
      return b.CreateLoad(ty, ray_field(field));
    return b.CreateLoad(
        ty->getArrayElementType(), b.CreateInBoundsGEP(ty, ray_field(field), {b.getInt32(0), element})
    );
  };
  auto ray_call = [&](uint32_t slot, Value *first, Value *second) {
    auto &b = ctx.builder;
    auto i8p = b.getInt8PtrTy();
    auto table = ray_value(RayShaders);
    auto pointer = b.CreateCall(
        ctx.module.getOrInsertFunction(
            "air.get_function_pointer_visible_function_table",
            FunctionType::get(i8p, {table->getType(), b.getInt32Ty()}, false)
        ),
        {table, b.getInt32(slot)}
    );
    auto ty = FunctionType::get(b.getVoidTy(), {ray->getType(), i8p, i8p}, false);
    auto untyped = [&](Value *v) -> Value * { return v ? b.CreateBitCast(v, i8p) : ConstantPointerNull::get(i8p); };
    b.CreateCall(ty, b.CreateBitCast(pointer, ty->getPointerTo()), {ray, untyped(first), untyped(second)});
  };
  // an any hit shader ends with its verdict where it calls IgnoreHit or AcceptHitAndEndSearch
  auto ray_verdict = [&](CallInst *call, SM50_RAY_VERDICT verdict) {
    ctx.builder.CreateStore(ctx.builder.getInt32(verdict), ray_field(RayVerdict));
    if (auto end = dyn_cast<UnreachableInst>(call->getParent()->getTerminator())) {
      BranchInst::Create(epilogue, end);
      end->eraseFromParent();
    }
  };

  // ray queries. DXIL's are handles: here the order they are allocated in, which still selects the query when a
  // handle reaches an operation through a phi. Metal's are objects, allocated at the function's start and
  // deallocated at its end, with what Metal does not keep for a query: its ray flags, and the address of its
  // structure's instances (AccelerationStructureHeader in d3d12_acceleration_structure.hpp)
  struct RayQuery {
    Value *query, *flags, *instances, *constant_flags;
  };
  std::vector<RayQuery> ray_queries;
  auto opaque = [&](const char *name) {
    auto ty = StructType::getTypeByName(ctx.llvm, name);
    return ty ? ty : StructType::create(ctx.llvm, name);
  };
  auto ray_query_ty = opaque("struct._intersection_query_t")->getPointerTo();
  auto ray_query_call = [&](const std::string &name, Type *ret, ArrayRef<Value *> args) {
    std::vector<Type *> tys;
    for (auto v : args)
      tys.push_back(v->getType());
    return ctx.builder.CreateCall(
        ctx.module.getOrInsertFunction(
            "air." + name + "_intersection_query.instancing.triangle_data", FunctionType::get(ret, tys, false)
        ),
        args
    );
  };
  for (auto call : calls) {
    auto op = opcode(call);
    if (op != AllocateRayQuery && op != AllocateRayQuery2)
      continue;
    auto &b = ctx.builder;
    b.SetInsertPoint(&*ctx.function->getEntryBlock().getFirstInsertionPt());
    call->replaceAllUsesWith(b.getInt32(ray_queries.size()));
    ray_queries.push_back({
        ray_query_call("allocate", ray_query_ty, {}),
        b.CreateAlloca(b.getInt32Ty()),
        b.CreateAlloca(b.getInt64Ty()),
        call->getArgOperand(1),
    });
    b.SetInsertPoint(epilogue);
    ray_query_call("deallocate", b.getVoidTy(), {ray_queries.back().query});
  }
  auto ray_query = [&](Value *handle, Value *RayQuery::*member) {
    Value *v = nullptr;
    for (uint32_t i = 0; i < ray_queries.size(); i++)
      v = v ? ctx.builder.CreateSelect(
                  ctx.builder.CreateICmpEQ(handle, ctx.builder.getInt32(i)), ray_queries[i].*member, v
              )
            : ray_queries[i].*member;
    return v;
  };
  // DXIL's operations that are one call of Metal's: scalars, components of a vector of `width`, and elements of a
  // 3x4 matrix, which Metal has as four columns
  struct RayQueryGetter {
    uint32_t op;
    const char *name;
    uint32_t width = 0;
    bool matrix = false;
  };
  static constexpr RayQueryGetter ray_query_getters[] = {
      {184, "get_committed_intersection_type"},
      {186, "get_candidate_object_to_world_transform", 3, true},
      {187, "get_candidate_world_to_object_transform", 3, true},
      {188, "get_committed_object_to_world_transform", 3, true},
      {189, "get_committed_world_to_object_transform", 3, true},
      {190, "is_candidate_non_opaque_bounding_box"},
      {191, "is_candidate_triangle_front_facing"},
      {192, "is_committed_triangle_front_facing"},
      {193, "get_candidate_triangle_barycentric_coord", 2},
      {194, "get_committed_triangle_barycentric_coord", 2},
      {196, "get_world_space_ray_origin", 3},
      {197, "get_world_space_ray_direction", 3},
      {198, "get_ray_min_distance"},
      {199, "get_candidate_triangle_distance"},
      {200, "get_committed_distance"},
      {201, "get_candidate_instance_id"},
      {202, "get_candidate_user_instance_id"},
      {203, "get_candidate_geometry_id"},
      {204, "get_candidate_primitive_id"},
      {205, "get_candidate_ray_origin", 3},
      {206, "get_candidate_ray_direction", 3},
      {207, "get_committed_instance_id"},
      {208, "get_committed_user_instance_id"},
      {209, "get_committed_geometry_id"},
      {210, "get_committed_primitive_id"},
      {211, "get_committed_ray_origin", 3},
      {212, "get_committed_ray_direction", 3},
  };

  // handles first, so every consumer finds its descriptor
  std::stable_partition(calls.begin(), calls.end(), [](auto call) { return opcode(call) == CreateHandle; });

  auto arg = [](CallInst *call, unsigned i) { return call->getArgOperand(i); };
  auto constant = [](Value *v) { return (uint32_t)cast<ConstantInt>(v)->getZExtValue(); };

  // a mesh shader's outputs. a varying, a clip distance or a primitive's system value is one component, set where it is
  // stored. a position is one Metal attribute, set whole: the components a block stores for one vertex are set where
  // the last of them is stored (`positions`). what cannot be set that way is kept in threadgroup memory, and the group
  // sets it once every thread is through (`export_mesh`): positions when some block stores only a part of one, and
  // cull distances with the indices and SV_CullPrimitive, because Metal culls by primitive and Direct3D by the
  // vertices' distances (D3D11.3 15.4.2)
  struct Position {
    std::array<Value *, 4> components{};
    CallInst *last;
  };
  std::vector<Position> positions;
  DenseMap<CallInst *, uint32_t> position_of;
  bool staged_positions = false, culls = false;
  uint32_t cull_distances = 0;
  if (shader->shader_type == D3D12_SB_MESH_SHADER) {
    std::map<std::pair<llvm::BasicBlock *, Value *>, uint32_t> groups;
    std::vector<uint32_t> stored;
    for (auto call : calls) {
      if (opcode(call) != StoreVertexOutput || shader->mesh_outputs[0][constant(arg(call, 1))].semantic != SemanticPosition)
        continue;
      auto [group, added] = groups.try_emplace({call->getParent(), arg(call, 5)}, positions.size());
      if (added) {
        positions.push_back({});
        stored.push_back(0);
      }
      positions[group->second].last = call;
      position_of[call] = group->second;
      stored[group->second] |= 1u << constant(arg(call, 3));
    }
    staged_positions = std::any_of(stored.begin(), stored.end(), [](uint32_t mask) { return mask != 0xf; });
    for (auto &e : shader->mesh_outputs[0])
      if (e.semantic == SemanticCullDistance)
        cull_distances += e.rows * e.cols;
    for (auto &e : shader->mesh_outputs[1])
      culls |= e.semantic == SemanticCullPrimitive;
  }
  auto threadgroup = [&](Type *type) {
    return new GlobalVariable(
        ctx.module, type, false, GlobalValue::InternalLinkage, UndefValue::get(type), "", nullptr,
        GlobalValue::NotThreadLocal, (unsigned)air::AddressSpace::threadgroup
    );
  };
  auto element_of = [&](GlobalVariable *array, std::initializer_list<Value *> at) {
    SmallVector<Value *> indices{ir.getInt32(0)};
    indices.append(at.begin(), at.end());
    return ir.CreateInBoundsGEP(array->getValueType(), array, indices);
  };
  auto i8 = ir.getInt8Ty();
  uint32_t corners = shader->mesh_lines ? 2 : 3;
  GlobalVariable *staged_position = nullptr, *staged_distance = nullptr, *staged_index = nullptr, *staged_culled = nullptr;
  // the counts SetMeshOutputCounts gave this thread, nothing until then
  Value *mesh_counts[2]{};
  if (staged_positions || cull_distances) {
    IRBuilder<>::InsertPointGuard guard(ir);
    ir.SetInsertPoint(&*ctx.function->getEntryBlock().getFirstInsertionPt());
    for (auto &count : mesh_counts)
      ir.CreateStore(ir.getInt32(0), count = ir.CreateAlloca(ir.getInt32Ty()));
    if (staged_positions)
      staged_position = threadgroup(ArrayType::get(ArrayType::get(ir.getFloatTy(), 4), shader->mesh_max_vertices));
    if (cull_distances) {
      // a distance as whether it puts its vertex out: negative or NaN
      staged_distance = threadgroup(ArrayType::get(ArrayType::get(i8, cull_distances), shader->mesh_max_vertices));
      staged_index = threadgroup(ArrayType::get(ArrayType::get(i8, corners), shader->mesh_max_primitives));
      if (culls)
        staged_culled = threadgroup(ArrayType::get(i8, shader->mesh_max_primitives));
    }
  }
  // runs `body` for 0 to count - 1, at least once, before `at`
  auto repeat = [&](llvm::Instruction *at, Value *count, auto &&body) {
    auto head = at->getParent();
    auto rest = SplitBlock(head, at);
    auto loop = llvm::BasicBlock::Create(ctx.llvm, "", ctx.function, rest);
    head->getTerminator()->setSuccessor(0, loop);
    ir.SetInsertPoint(loop);
    auto i = ir.CreatePHI(ir.getInt32Ty(), 2);
    i->addIncoming(ir.getInt32(0), head);
    body(i);
    auto next = ir.CreateAdd(i, ir.getInt32(1));
    i->addIncoming(next, ir.GetInsertBlock());
    ir.CreateCondBr(ir.CreateICmpULT(next, count), loop, rest);
    ir.SetInsertPoint(at);
  };
  // the row a mesh output is stored to: `store` runs at the call, or under a test of each row where DXIL has the row
  // dynamic, because Metal names an attribute by a constant
  auto mesh_row = [&](CallInst *call, uint32_t rows, auto &&store) {
    if (auto row = dyn_cast<ConstantInt>(arg(call, 2)))
      return store((uint32_t)row->getZExtValue());
    for (uint32_t row = 0; row < rows; row++) {
      ir.SetInsertPoint(SplitBlockAndInsertIfThen(ir.CreateICmpEQ(arg(call, 2), ir.getInt32(row)), call, false));
      store(row);
      ir.SetInsertPoint(call);
    }
  };
  auto defined = [](Value *v) { return !isa<UndefValue>(v); };
  auto count = [](Type *ty) {
    auto v = dyn_cast<FixedVectorType>(ty);
    return v ? v->getNumElements() : 1u;
  };
  // `n` scalar operands from `first`, zero padded to `ty`
  auto vec = [&](CallInst *call, unsigned first, unsigned n, Type *ty) {
    Value *v = Constant::getNullValue(ty);
    if (!ty->isVectorTy())
      return n ? arg(call, first) : v;
    for (unsigned i = 0; i < n; i++)
      v = ir.CreateInsertElement(v, arg(call, first + i), i);
    return v;
  };
  // `n` immediate offsets from `first`; missing ones are 0
  auto offsets = [&](CallInst *call, unsigned first, unsigned n, int32_t out[3]) {
    for (unsigned i = 0; i < 3; i++)
      out[i] =
          i < n && defined(arg(call, first + i)) ? (int32_t)cast<ConstantInt>(arg(call, first + i))->getSExtValue() : 0;
  };
  // the texel address operands from `first` (coordinates, then the array slice), with `w` last
  auto texel_coord = [&](CallInst *call, unsigned first, Value *w) {
    Value *v = UndefValue::get(air.getIntTy(4));
    for (unsigned i = 0; i < 3; i++)
      v = ir.CreateInsertElement(v, arg(call, first + i), i);
    return ir.CreateInsertElement(v, w, 3);
  };
  // DXIL returns vectors as {x, y, z, w} or {x, y, z, w, status}
  // 16-bit values live in 32-bit registers, texels and samples: halves as floats, integers extended by their sign
  auto narrow = [&](Value *v, Type *ty) -> Value * {
    if (ty->getPrimitiveSizeInBits() == v->getType()->getPrimitiveSizeInBits())
      return ir.CreateBitCast(v, ty);
    return ty->isHalfTy() ? ir.CreateFPTrunc(ir.CreateBitCast(v, ir.getFloatTy()), ty)
                          : ir.CreateTrunc(ir.CreateBitCast(v, ir.getInt32Ty()), ty);
  };
  auto widen = [&](Value *v, bool is_signed) -> Value * {
    if (v->getType()->getPrimitiveSizeInBits() != 16)
      return v;
    return v->getType()->isHalfTy() ? ir.CreateFPExt(v, ir.getFloatTy())
           : is_signed              ? ir.CreateSExt(v, ir.getInt32Ty())
                                    : ir.CreateZExt(v, ir.getInt32Ty());
  };
  auto to_struct = [&](Type *ty, Value *v, Value *status) {
    auto sty = cast<StructType>(ty);
    // a scalar result (depth textures) gets D3D's defaults for the missing components: 0, 0, 1
    if (!v->getType()->isVectorTy()) {
      auto one =
          v->getType()->isFloatingPointTy() ? ConstantFP::get(v->getType(), 1.0) : ConstantInt::get(v->getType(), 1);
      v = ir.CreateInsertElement(
          ir.CreateInsertElement(Constant::getNullValue(FixedVectorType::get(v->getType(), 4)), v, (uint64_t)0), one, 3
      );
    }
    Value *ret = UndefValue::get(ty);
    for (unsigned i = 0; i < std::min(4u, count(v->getType())); i++) {
      auto x = ir.CreateExtractElement(v, i);
      ret = ir.CreateInsertValue(ret, narrow(x, sty->getElementType(i)), {i});
    }
    if (sty->getNumElements() == 5)
      // Metal's residency byte sets bit 0 where the texels read are not mapped; the status CheckAccessFullyMapped reads
      // is nonzero where they all are, as it is for resources that are not tiled
      ret = ir.CreateInsertValue(
          ret,
          status ? ir.CreateZExt(ir.CreateICmpEQ(ir.CreateAnd(status, 1), ConstantInt::get(status->getType(), 0)), ir.getInt32Ty())
                 : ir.getInt32(~0u),
          {4}
      );
    return ret;
  };
  // signature element storage: the register file, or the special output the DXBC handlers allocated. files of control
  // points (a patch's, in the tessellation stages) index the point first
  auto element_ptr = [&](register_file &file, const Element &e, CallInst *call, Value *point) -> Value * {
    switch (e.semantic) {
    case Depth:
    case DepthLessEqual:
    case DepthGreaterEqual:
      return ctx.resource.depth_output_reg;
    case StencilRef:
      return ctx.resource.stencil_ref_reg;
    case CoverageOutput:
      return ctx.resource.coverage_mask_reg;
    }
    auto ty = file.ptr_int4->getType()->getNonOpaquePointerElementType();
    SmallVector<Value *, 4> index = {ir.getInt32(0)};
    if (ty->getArrayElementType()->isArrayTy())
      index.push_back(point);
    index.append(
        {ir.CreateAdd(arg(call, 2), ir.getInt32(e.row)),
         ir.CreateAdd(ir.CreateZExt(arg(call, 3), ir.getInt32Ty()), ir.getInt32(e.col))}
    );
    return ir.CreateGEP(ty, file.ptr_int4, index);
  };
  auto sample = [&](CallInst *call, Handle &tex, Handle &smp, unsigned coord_first) {
    auto &info = llvm::air::AIRBuilder::getTextureInfo(tex.texture->Logical);
    auto logical = count(air.getTextureSampleCoordType(Texture{.kind = tex.texture->Logical}));
    auto ty = air.getTextureSampleCoordType(tex.texture->Texture);
    auto array = arg(call, coord_first + logical);
    return std::tuple{
        tex.texture->Texture,
        vec(call, coord_first, logical, ty),
        defined(array) ? dxbc.ClampArrayIndex(array, tex.texture->Metadata) : nullptr,
        info.is_cube ? smp.sampler->CubeSamplerHandle : smp.sampler->SamplerHandle,
        dxbc.DecodeSamplerBias(smp.sampler->Metadata),
        logical,
        ty
    };
  };
  auto min_lod = [&](Handle &tex, Value *lod) {
    return dxbc.ClampMinLOD(tex.texture->Metadata, defined(lod) ? lod : nullptr);
  };
  // typed reads and writes of textures and texel buffers; `coord` holds the LOD last
  auto texel_read = [&](Handle &h, Value *coord, const int32_t offset[3], Value *sample) {
    auto &t = *h.texture;
    auto lod = sample ? ir.getInt32(0) : ir.CreateExtractElement(coord, 3);
    auto [address, array] = dxbc.TexelAddress(t, coord, offset, sample ? nullptr : lod);
    if (t.Texture.memory_access == Texture::acesss_readwrite)
      air.CreateTextureFence(t.Texture, t.Handle);
    return air.CreateRead(t.Texture, t.Handle, address, array, sample, lod, t.GlobalCoherent);
  };
  auto texel_write = [&](Handle &h, Value *coord, CallInst *call, unsigned first) {
    auto &t = *h.texture;
    const int32_t no_offset[3] = {};
    auto [address, array] = dxbc.TexelAddress(t, coord, no_offset, nullptr);
    auto texel = air.getTexelType(t.Texture);
    Value *value = UndefValue::get(texel);
    for (unsigned i = 0; i < 4; i++) {
      auto x = widen(arg(call, first + i), t.Texture.sample_type == Texture::sample_int);
      value = ir.CreateInsertElement(value, ir.CreateBitCast(x, texel->getScalarType()), i);
    }
    air.CreateWrite(t.Texture, t.Handle, address, array, nullptr, ir.getInt32(0), value, t.GlobalCoherent);
  };
  auto element_bits = [](Type *ty) {
    return (ty->isStructTy() ? ty->getStructElementType(0) : ty)->getPrimitiveSizeInBits();
  };
  // byte address of a raw (byte offset) or structured (element, byte offset) buffer address, in 64 bits: an element
  // index scaled by the stride can wrap 32 and pass the bounds check
  auto byte_address = [&](Handle &h, Value *index, Value *offset) -> Value * {
    auto wide = [&](Value *v) { return ir.CreateZExt(v, ir.getInt64Ty()); };
    auto stride = h.buffer->StructureStride;
    if (!stride)
      return wide(index);
    auto element = ir.CreateMul(wide(index), ir.getInt64(stride));
    return defined(offset) ? ir.CreateAdd(element, wide(offset)) : element;
  };
  // a buffer's `index`th unit of `unit` type, or null past its end: Metal reads 0 there and drops the write
  auto unit_ptr = [&](BufferResourceHandle &b, Type *unit, Value *index) -> Value * {
    auto space = b.Pointer->getType()->getPointerAddressSpace();
    auto ptr = ir.CreateGEP(unit, ir.CreatePointerCast(b.Pointer, unit->getPointerTo(space)), {index});
    if (!b.Metadata)
      return ptr;
    auto bytes = ir.CreateZExt(dxbc.DecodeRawBufferByteLength(b.Metadata), ir.getInt64Ty());
    auto units = ir.CreateUDiv(bytes, ir.getInt64(unit->getPrimitiveSizeInBits() / 8));
    return ir.CreateSelect(ir.CreateICmpULT(index, units), ptr, Constant::getNullValue(ptr->getType()));
  };
  // system inputs read the way DXBC reads them (the coverage mask honors the PSO sample mask)
  auto attribute = [&](shader::common::InputAttribute input, uint32_t component) {
    return dxbc.LoadOperand(
        SrcOperandAttribute{._ = {swizzle_identity, false, false, OperandDataType::Integer}, .attribute = input},
        1u << component
    );
  };
  // thread IDs in a group of 2x2 quads: the flattened ID's quad, placed in X, Y and Z order, and its lane (x, then y)
  // in the quad. computed once, at entry
  std::array<Value *, 3> quad_thread{};
  auto group_thread = [&](unsigned c) {
    if (!quad_thread[0]) {
      auto &n = shader->threadgroup_size;
      IRBuilder<>::InsertPointGuard guard(ir);
      ir.SetInsertPoint(&*ctx.function->getEntryBlock().getFirstInsertionPt());
      auto flat = ctx.resource.thread_id_in_group_flat_arg;
      auto quad = ir.CreateLShr(flat, 2), lane = ir.CreateAnd(flat, 3);
      auto half_x = ir.getInt32(n[0] / 2), half_y = ir.getInt32(n[1] / 2);
      quad_thread = {
          ir.CreateOr(ir.CreateShl(ir.CreateURem(quad, half_x), 1), ir.CreateAnd(lane, 1)),
          ir.CreateOr(ir.CreateShl(ir.CreateURem(ir.CreateUDiv(quad, half_x), half_y), 1), ir.CreateLShr(lane, 1)),
          ir.CreateUDiv(quad, ir.getInt32(n[0] / 2 * n[1] / 2)),
      };
    }
    return quad_thread[c];
  };
  // compute thread IDs (ThreadId to FlattenedThreadIdInGroup), from the quads when they are 2x2 blocks
  auto thread_input = [&](uint32_t op, uint32_t c) -> Value * {
    using shader::common::InputAttribute;
    auto &n = shader->threadgroup_size;
    if (!info.quads_2d || op == GroupId) {
      const InputAttribute inputs[] = {
          InputAttribute::ThreadId, InputAttribute::ThreadGroupId, InputAttribute::ThreadIdInGroup,
          InputAttribute::ThreadIdInGroupFlatten
      };
      return attribute(inputs[op - ThreadId], c);
    }
    if (op == FlattenedThreadIdInGroup)
      return ir.CreateAdd(
          ir.CreateMul(
              ir.CreateAdd(ir.CreateMul(group_thread(2), ir.getInt32(n[1])), group_thread(1)), ir.getInt32(n[0])
          ),
          group_thread(0)
      );
    auto id = group_thread(c);
    return op == ThreadId
               ? ir.CreateAdd(
                     ir.CreateMul(ir.CreateExtractElement(ctx.resource.thread_group_id_arg, c), ir.getInt32(n[c])), id
                 )
               : id;
  };
  auto interpolant = [&](CallInst *call) -> llvm::Optional<InterpolantHandle> {
    auto &e = entry.input[constant(arg(call, 1))];
    if (!isa<ConstantInt>(arg(call, 2)))
      return {};
    return dxbc.LoadInterpolant(e.row + constant(arg(call, 2)));
  };
  auto interpolated_component = [&](CallInst *call, Value *v) {
    auto &e = entry.input[constant(arg(call, 1))];
    return ir.CreateExtractElement(v, e.col + constant(arg(call, 3)));
  };
  auto fp_unary = [&](llvm::air::AIRBuilder::FPUnOp op, Value *v) { return air.CreateFPUnOp(op, v); };
  // DxilConstants.h AtomicBinOpCode order
  auto atomic_op = [](uint32_t code) {
    constexpr AtomicRMWInst::BinOp ops[] = {
        AtomicRMWInst::Add, AtomicRMWInst::And,  AtomicRMWInst::Or,   AtomicRMWInst::Xor,  AtomicRMWInst::Min,
        AtomicRMWInst::Max, AtomicRMWInst::UMin, AtomicRMWInst::UMax, AtomicRMWInst::Xchg,
    };
    return ops[code];
  };

  // Metal's SIMD-group functions, named as the Metal compiler names them (integers by signedness); every active lane
  // takes part, so they are convergent
  using llvm::air::Signedness;
  auto convergent = AttributeList::get(
      ctx.llvm, {{~0U, Attribute::get(ctx.llvm, Attribute::Convergent)},
                 {~0U, Attribute::get(ctx.llvm, Attribute::NoUnwind)},
                 {~0U, Attribute::get(ctx.llvm, Attribute::WillReturn)}}
  );
  auto simd = [&](const std::string &name, Type *ret, ArrayRef<Value *> args, Type *overload = nullptr,
                  Signedness sign = Signedness::Unsigned) -> Value * {
    auto fn = "air." + name;
    if (overload)
      fn += air.getTypeOverloadSuffix(overload, overload->isIntOrIntVectorTy() ? sign : Signedness::DontCare);
    SmallVector<Type *> types;
    for (auto a : args)
      types.push_back(a->getType());
    return ir.CreateCall(ctx.module.getOrInsertFunction(fn, FunctionType::get(ret, types, false), convergent), args);
  };
  auto i16 = [&](Value *v) { return ir.CreateTrunc(v, ir.getInt16Ty()); };
  // a function that moves `v` between lanes (shuffle, broadcast): bools travel as 32 bits, 64-bit values as halves
  std::function<Value *(const std::string &, Value *, ArrayRef<Value *>)> move = [&](auto &name, Value *v, auto extra) {
    auto ty = v->getType();
    if (ty->isIntegerTy(1))
      return ir.CreateTrunc(move(name, ir.CreateZExt(v, ir.getInt32Ty()), extra), ty);
    if (ty->getPrimitiveSizeInBits() == 64) {
      auto halves = ir.CreateBitCast(v, FixedVectorType::get(ir.getInt32Ty(), 2));
      Value *r = UndefValue::get(halves->getType());
      for (unsigned i = 0; i < 2; i++)
        r = ir.CreateInsertElement(r, move(name, ir.CreateExtractElement(halves, i), extra), i);
      return ir.CreateBitCast(r, ty);
    }
    SmallVector<Value *> args{v};
    args.append(extra.begin(), extra.end());
    return simd(name, ty, args, ty);
  };
  // the SIMD group's width, and this lane's index: vertex functions have no input for them, but their SIMD groups hold
  // consecutive vertices from lane 0, so a lane is the lanes active before it when the function starts
  auto lane_index = [&]() -> Value * {
    if (ctx.shader_type != D3D10_SB_VERTEX_SHADER)
      return ctx.function->getArg(shader->func_signature.DefineInput(air::InputThreadIndexInSIMDGroup{}));
    IRBuilder<>::InsertPointGuard guard(ir);
    ir.SetInsertPoint(&*ctx.function->getEntryBlock().getFirstInsertionPt());
    return simd("simd_prefix_exclusive_sum", ir.getInt32Ty(), {ir.getInt32(1)}, ir.getInt32Ty());
  };
  // compute shaders differentiate across their quads (lanes x, then y): coarsely from the quad's first lane, finely
  // along this lane's row or column
  auto quad_derivative = [&](Value *v, bool y, bool fine) {
    uint16_t axis = y ? 2 : 1;
    Value *from = ir.getInt16(0), *to = ir.getInt16(axis);
    if (fine) {
      auto lane = i16(ir.CreateAnd(lane_index(), 3));
      from = ir.CreateAnd(lane, ~axis & 3);
      to = ir.CreateOr(lane, axis);
    }
    return ir.CreateFSub(move("quad_shuffle", v, {to}), move("quad_shuffle", v, {from}));
  };
  // the coarse derivatives of a sample coordinate, which compute shaders sample with: the quad's lanes 1 and 2 minus 0
  auto quad_gradients = [&](Value *coord) {
    auto lane = [&](uint16_t i) { return move("quad_shuffle", coord, {ir.getInt16(i)}); };
    return std::pair{ir.CreateFSub(lane(1), lane(0)), ir.CreateFSub(lane(2), lane(0))};
  };
  // D3D's isotropic LOD (D3D11.3 7.18.11) of a quad's coordinates in compute shaders: the longer coarse derivative, in
  // texels of the view's first mip. a cube's directions project first onto this lane's face, where s = (sc / |ma| + 1)
  // / 2: the other two components over the major one, each lane's own
  // the LOD of gradients in texels: log2 of the longer one
  auto gradient_lod = [&](Value *dx, Value *dy) {
    using llvm::air::AIRBuilder;
    auto length2 = [&](Value *d) {
      return d->getType()->isVectorTy() ? air.CreateDotProduct(d, d) : ir.CreateFMul(d, d);
    };
    auto longer = air.CreateFPBinOp(AIRBuilder::fmax, length2(dx), length2(dy));
    return ir.CreateFMul(fp_unary(AIRBuilder::log2, longer), ConstantFP::get(ir.getFloatTy(), 0.5));
  };
  auto quad_lod = [&](const TextureResourceHandle &t, Value *coord, bool cube) {
    using llvm::air::AIRBuilder;
    auto size = [&](Texture::Query q) {
      return air.CreateConvertToFloat(
          air.CreateTextureQuery(t.Texture, t.Handle, q, ir.getInt32(0)), Signedness::Unsigned
      );
    };
    auto c = [&](Value *v, unsigned i) { return ir.CreateExtractElement(v, i); };
    auto abs = [&](unsigned i) { return fp_unary(AIRBuilder::fabs, c(coord, i)); };
    Value *is_x = nullptr, *is_y = nullptr, *is_z = nullptr, *scale;
    if (cube) {
      is_x = ir.CreateAnd(ir.CreateFCmpOGE(abs(0), abs(1)), ir.CreateFCmpOGE(abs(0), abs(2)));
      is_y = ir.CreateAnd(ir.CreateNot(is_x), ir.CreateFCmpOGE(abs(1), abs(2)));
      is_z = ir.CreateNot(ir.CreateOr(is_x, is_y));
      scale = ir.CreateFMul(ConstantFP::get(ir.getFloatTy(), 0.5), size(Texture::width));
    } else if (!coord->getType()->isVectorTy()) {
      scale = size(Texture::width);
    } else {
      const Texture::Query queries[] = {Texture::width, Texture::height, Texture::depth};
      scale = UndefValue::get(coord->getType());
      for (unsigned i = 0; i < count(coord->getType()); i++)
        scale = ir.CreateInsertElement(scale, size(queries[i]), i);
    }
    auto texels = [&](Value *v) -> Value * {
      if (!cube)
        return ir.CreateFMul(v, scale);
      // each lane's direction over its own component along this lane's major axis
      auto major = fp_unary(AIRBuilder::fabs, ir.CreateSelect(is_x, c(v, 0), ir.CreateSelect(is_y, c(v, 1), c(v, 2))));
      Value *face = UndefValue::get(FixedVectorType::get(ir.getFloatTy(), 2));
      face = ir.CreateInsertElement(face, ir.CreateSelect(is_x, c(v, 1), c(v, 0)), (uint64_t)0);
      face = ir.CreateInsertElement(face, ir.CreateSelect(is_z, c(v, 1), c(v, 2)), 1);
      return ir.CreateFMul(face, ir.CreateVectorSplat(2, ir.CreateFDiv(scale, major)));
    };
    auto lane = [&](uint16_t i) { return texels(move("quad_shuffle", coord, {ir.getInt16(i)})); };
    return gradient_lod(ir.CreateFSub(lane(1), lane(0)), ir.CreateFSub(lane(2), lane(0)));
  };
  auto lane_count = [&]() -> Value * {
    if (ctx.shader_type != D3D10_SB_VERTEX_SHADER)
      return ctx.function->getArg(shader->func_signature.DefineInput(air::InputThreadsPerSIMDGroup{}));
    return ir.getInt32(ctx.simd_width);
  };
  // `combine` over the active lanes in turn, of what `count` makes of each lane's `v`, broadcast to all
  using Combine = std::function<Value *(Value *, Value *)>;
  auto over_lanes = [&](Value *v, Value *identity, std::function<Value *(Value * lane, Value * value)> count,
                        Combine combine) -> Value * {
    auto mask = simd("simd_ballot", ir.getInt64Ty(), {ir.getTrue()}, ir.getInt64Ty());
    auto head = ir.GetInsertBlock();
    auto exit = SplitBlock(head, &*ir.GetInsertPoint());
    auto loop = llvm::BasicBlock::Create(ctx.llvm, "wave_lanes", ctx.function, exit);
    head->getTerminator()->setSuccessor(0, loop);
    ir.SetInsertPoint(loop);
    auto remaining = ir.CreatePHI(ir.getInt64Ty(), 2), result = ir.CreatePHI(identity->getType(), 2);
    remaining->addIncoming(mask, head);
    result->addIncoming(identity, head);
    auto ctz = ctx.module.getOrInsertFunction(
        "air.ctz.i64", FunctionType::get(ir.getInt64Ty(), {ir.getInt64Ty(), ir.getInt1Ty()}, false)
    );
    auto from = ir.CreateTrunc(ir.CreateCall(ctz, {remaining, ir.getFalse()}), ir.getInt32Ty());
    auto next = combine(result, count(from, move("simd_broadcast", v, {i16(from)})));
    auto rest = ir.CreateAnd(remaining, ir.CreateSub(remaining, ir.getInt64(1)));
    remaining->addIncoming(rest, loop);
    result->addIncoming(next, loop);
    ir.CreateCondBr(ir.CreateICmpNE(rest, ir.getInt64(0)), loop, exit);
    ir.SetInsertPoint(&*exit->getFirstInsertionPt());
    return next;
  };
  // a 64-bit word an atomic works on: what identifies it (its lock's key), the memory it is in, and its read and write
  struct Word64 {
    Value *key;
    llvm::air::MemFlags memory;
    std::function<Value *()> read;
    std::function<void(Value *)> write;
  };
  // the word at a buffer or groupshared pointer. groupshared words key with their threadgroup's position, so
  // threadgroups do not share locks for the same word
  auto word_at = [&](Value *ptr) -> Word64 {
    auto i64 = ir.getInt64Ty();
    Value *key = ir.CreateLShr(ir.CreatePtrToInt(ptr, i64), 3);
    if (ptr->getType()->getPointerAddressSpace() != (unsigned)air::AddressSpace::threadgroup) {
      // by its halves, whose loads and stores are atomic and so never stale (CreateDeviceCoherentLoad): a stale read
      // under the lock would lose another thread's step
      auto i32 = ir.getInt32Ty();
      auto half = [=, &ir](unsigned i) {
        return ir.CreateConstGEP1_32(i32, ir.CreateBitCast(ptr, i32->getPointerTo(ptr->getType()->getPointerAddressSpace())), i);
      };
      return {key, llvm::air::MemFlags::Device,
              [=, &ir, &air] {
                return ir.CreateOr(
                    ir.CreateZExt(air.CreateDeviceCoherentLoad(i32, half(0)), i64),
                    ir.CreateShl(ir.CreateZExt(air.CreateDeviceCoherentLoad(i32, half(1)), i64), 32)
                );
              },
              [=, &ir, &air](Value *v) {
                air.CreateDeviceCoherentStore(ir.CreateTrunc(v, i32), half(0));
                air.CreateDeviceCoherentStore(ir.CreateTrunc(ir.CreateLShr(v, 32), i32), half(1));
              }};
    }
    for (uint32_t c = 0; c < 3; c++)
      key = ir.CreateAdd(ir.CreateMul(key, ir.getInt64(0x9e3779b97f4a7c15)), ir.CreateZExt(thread_input(GroupId, c), i64));
    return {key, llvm::air::MemFlags::Threadgroup, [=, &ir] { return (Value *)ir.CreateLoad(i64, ptr); },
            [=, &ir](Value *v) { ir.CreateStore(v, ptr); }};
  };
  // a 64-bit atomic, which Apple GPUs lack (but min and max without a result): `update` of `word`'s old value under a
  // lock word of `locks` hashed from its key, returning the old value. every 64-bit atomic takes a lock, so none races
  // a lock-free one. one lane of a SIMD group at a time takes one: a lane waiting on its own group's lock holder would
  // wait forever, as the group's lanes do not run while one spins
  auto locked = [&](const Word64 &word, Value *locks, std::function<Value *(Value *)> update) -> Value * {
    if (air.DiscardWrites && word.memory != llvm::air::MemFlags::Threadgroup)
      return word.read();
    auto scope = word.memory == llvm::air::MemFlags::Threadgroup ? llvm::air::ThreadScope::Threadgroup
                                                                 : llvm::air::ThreadScope::Device;
    auto head = ir.GetInsertBlock();
    auto exit = SplitBlock(head, &*ir.GetInsertPoint());
    auto block = [&](const char *name) { return llvm::BasicBlock::Create(ctx.llvm, name, ctx.function, exit); };
    auto loop = block("atomic64"), pending_lane = block("atomic64_pending"), elected = block("atomic64_elected"),
         spin = block("atomic64_spin"), held = block("atomic64_held"), given_up = block("atomic64_given_up"),
         next = block("atomic64_next");
    // a pixel shader's helper lanes take no part: their atomics do nothing, so one would spin on a lock forever
    ir.SetInsertPoint(head->getTerminator());
    Value *lane = ctx.shader_type == D3D10_SB_PIXEL_SHADER
                      ? ir.CreateNot(simd("simd_is_helper_thread", ir.getInt1Ty(), {}))
                      : (Value *)ir.getTrue();
    head->getTerminator()->setSuccessor(0, loop);
    ir.SetInsertPoint(loop);
    auto pending = ir.CreatePHI(ir.getInt1Ty(), 2), old = ir.CreatePHI(ir.getInt64Ty(), 2);
    pending->addIncoming(lane, head);
    old->addIncoming(PoisonValue::get(ir.getInt64Ty()), head);
    ir.CreateCondBr(simd("simd_any", ir.getInt1Ty(), {pending}), pending_lane, exit);
    ir.SetInsertPoint(pending_lane);
    ir.CreateCondBr(pending, elected, next);
    // the first pending lane
    ir.SetInsertPoint(elected);
    auto first = simd("simd_is_first", ir.getInt1Ty(), {});
    // Fibonacci hashing of the key, the table's mask in its word 0
    auto hash =
        ir.CreateTrunc(ir.CreateLShr(ir.CreateMul(word.key, ir.getInt64(0x9e3779b97f4a7c15)), 32), ir.getInt32Ty());
    auto mask = ir.CreateLoad(ir.getInt32Ty(), locks);
    auto lock = ir.CreateGEP(ir.getInt32Ty(), locks, {ir.CreateAdd(ir.CreateAnd(hash, mask), ir.getInt32(1))});
    ir.CreateCondBr(first, spin, next);
    // the wait is bounded, well inside the GPU's watchdog, so a lock whose holder never runs again cannot hang the GPU:
    // the lane then gives up, leaves memory as it is, and raises the table's flag after its locks, which the device
    // reports (MTLD3D12Device::CheckAtomicLocks)
    constexpr uint32_t kSpins = 1u << 24;
    ir.SetInsertPoint(spin);
    auto spins = ir.CreatePHI(ir.getInt32Ty(), 2);
    spins->addIncoming(ir.getInt32(0), elected);
    auto taken = ir.CreateExtractValue(
        ir.CreateAtomicCmpXchg(lock, ir.getInt32(0), ir.getInt32(1), {}, AtomicOrdering::Monotonic, AtomicOrdering::Monotonic), 1
    );
    auto spun = ir.CreateAdd(spins, ir.getInt32(1));
    auto retry = block("atomic64_retry");
    ir.CreateCondBr(taken, held, retry);
    ir.SetInsertPoint(retry);
    ir.CreateCondBr(ir.CreateICmpEQ(spun, ir.getInt32(kSpins)), given_up, spin);
    spins->addIncoming(spun, retry);
    ir.SetInsertPoint(given_up);
    air.CreateAtomicRMW(AtomicRMWInst::Or, ir.CreateGEP(ir.getInt32Ty(), locks, {ir.CreateAdd(mask, ir.getInt32(2))}), ir.getInt32(1));
    ir.CreateBr(next);
    ir.SetInsertPoint(held);
    air.CreateAtomicFence(word.memory, scope);
    auto value = word.read();
    word.write(update(value));
    air.CreateAtomicFence(word.memory, scope);
    air.CreateAtomicRMW(AtomicRMWInst::Xchg, lock, ir.getInt32(0));
    ir.CreateBr(next);
    ir.SetInsertPoint(next);
    auto still = ir.CreatePHI(ir.getInt1Ty(), 4), got = ir.CreatePHI(ir.getInt64Ty(), 4);
    still->addIncoming(ir.getFalse(), held);
    got->addIncoming(value, held);
    still->addIncoming(ir.getFalse(), given_up);
    got->addIncoming(PoisonValue::get(ir.getInt64Ty()), given_up);
    for (auto from : {pending_lane, elected}) {
      still->addIncoming(pending, from);
      got->addIncoming(old, from);
    }
    pending->addIncoming(still, next);
    old->addIncoming(got, next);
    ir.CreateBr(loop);
    ir.SetInsertPoint(&*exit->getFirstInsertionPt());
    return old;
  };
  // a reduction or prefix (exclusive) of `v` over the active lanes, or over those in `group` (a lane mask). Metal has
  // these for 32 bits and less, without groups; the rest walk the lanes
  auto reduce = [&](const std::string &name, Value *v, Signedness sign, bool prefix, Combine combine, Value *identity,
                    Value *group = nullptr) -> Value * {
    if (v->getType()->getPrimitiveSizeInBits() != 64 && !group)
      return simd(prefix ? "simd_prefix_exclusive_" + name : "simd_" + name, v->getType(), {v}, v->getType(), sign);
    auto lane = prefix ? lane_index() : nullptr;
    return over_lanes(
        v, identity,
        [&](Value *from, Value *value) {
          auto counted = prefix ? ir.CreateICmpULT(from, lane) : ir.getTrue();
          if (group)
            counted = ir.CreateAnd(
                counted, ir.CreateTrunc(ir.CreateLShr(group, ir.CreateZExt(from, ir.getInt64Ty())), ir.getInt1Ty())
            );
          return ir.CreateSelect(counted, value, identity);
        },
        combine
    );
  };
  // lane masks as DXIL's four 32-bit words, from and to Metal's, of at most 64 lanes
  auto lane_mask = [&](CallInst *call, unsigned first) {
    auto word = [&](unsigned i) { return ir.CreateZExt(arg(call, first + i), ir.getInt64Ty()); };
    return ir.CreateOr(word(0), ir.CreateShl(word(1), 32));
  };
  auto mask_words = [&](Value *mask, Type *ty) {
    Value *words[] = {
        ir.CreateTrunc(mask, ir.getInt32Ty()), ir.CreateTrunc(ir.CreateLShr(mask, 32), ir.getInt32Ty()), ir.getInt32(0),
        ir.getInt32(0)
    };
    Value *ret = UndefValue::get(ty);
    for (unsigned i = 0; i < 4; i++)
      ret = ir.CreateInsertValue(ret, words[i], {i});
    return ret;
  };

  // Metal has its own 64-bit minimum and maximum, unsigned and without the old value. Unreal's Nanite writes its
  // visibility buffer so from every thread of its rasterizer, where the locks stall for seconds and give up.
  // they do not take the locks of the other 64-bit atomics, so a shader has them only when it has no other
  auto native_extreme = [&](CallInst *call) {
    if (opcode(call) != AtomicBinOp || !call->use_empty())
      return false;
    auto extreme = atomic_op(constant(arg(call, 2)));
    return extreme == AtomicRMWInst::UMax || extreme == AtomicRMWInst::UMin;
  };
  bool native_extremes = std::all_of(calls.begin(), calls.end(), [&](CallInst *call) {
    return !call->getType()->isIntegerTy(64) || (opcode(call) != AtomicBinOp && opcode(call) != AtomicCompareExchange) ||
           native_extreme(call);
  });

  for (auto call : calls) {
    ir.SetInsertPoint(call);
    auto ty = call->getType();
    auto op = opcode(call);
    Value *ret = nullptr;
    auto a = [&](unsigned i) { return arg(call, i); };
    switch (op) {
    case CreateHandle: {
      auto cls = constant(a(1));
      auto id = constant(a(2));
      Handle h;
      auto texture = [&](auto descriptor) {
        if (descriptor)
          h.texture = dxbc.MakeTextureHandle(*descriptor, swizzle_identity);
      };
      auto buffer = [&](auto descriptor) {
        if (descriptor)
          h.buffer = dxbc.MakeBufferHandle(*descriptor, swizzle_identity);
      };
      switch (cls) {
      case SRV:
        if (info.srvMap[id].resource_type == shader::common::ResourceType::NonApplicable)
          buffer(ctx.binding.GetSRVBuffer(air, id, a(3)));
        else
          texture(ctx.binding.GetSRVTexture(air, id, a(3)));
        break;
      case UAV:
        if (info.uavMap[id].resource_type == shader::common::ResourceType::NonApplicable)
          buffer(ctx.binding.GetUAVBuffer(air, id, a(3)));
        else
          texture(ctx.binding.GetUAVTexture(air, id, a(3)));
        if (info.uavMap[id].with_counter)
          h.counter = ctx.binding.GetUAVCounter(air, id, a(3));
        break;
      case CBV:
        h.cbuffer = ctx.binding.GetConstantBuffer(air, id, a(3));
        break;
      default:
        h.sampler = ctx.binding.GetSampler(air, id, a(3));
        break;
      }
      if (!h.texture && !h.buffer && !h.cbuffer && !h.sampler)
        return make_error<UnsupportedFeature>(std::format("DXIL resource class {} range {} has no binding", cls, id));
      handles[call] = h;
      continue;
    }
    case LoadInput:
    case LoadOutputControlPoint:
    case LoadPatchConstant: {
      if (ty->getPrimitiveSizeInBits() > 32)
        return make_error<UnsupportedFeature>("DXIL 64-bit inputs are not lowered yet");
      // a hull shader reads the control points it writes where it writes them
      auto &file = op == LoadInput                ? ctx.resource.input
                   : op == LoadOutputControlPoint ? ctx.resource.output
                                                  : ctx.resource.patch_constant_output;
      auto &elements = op == LoadInput ? entry.input : op == LoadOutputControlPoint ? entry.output : entry.patch;
      auto ptr = element_ptr(file, elements[constant(a(1))], call, op == LoadPatchConstant ? nullptr : a(4));
      ret = narrow(ir.CreateLoad(ir.getInt32Ty(), ptr), ty);
      break;
    }
    case StorePatchConstant: {
      if (a(4)->getType()->getPrimitiveSizeInBits() > 32)
        return make_error<UnsupportedFeature>("DXIL 64-bit outputs are not lowered yet");
      auto &e = entry.patch[constant(a(1))];
      auto ptr = element_ptr(ctx.resource.patch_constant_output, e, call, nullptr);
      ir.CreateStore(ir.CreateBitCast(widen(a(4), e.component == kComponentI16), ir.getInt32Ty()), ptr);
      break;
    }
    case DomainLocation:
      ret = ir.CreateExtractElement(ctx.resource.domain, a(1));
      break;
    case OutputControlPointID:
      ret = ctx.resource.thread_id_in_patch;
      break;
    case EmitStream:
    case CutStream:
    case EmitThenCutStream: {
      if (op != CutStream)
        if (auto err = ctx.resource.call_emit(constant(a(1))).build(ctx).takeError())
          return err;
      if (op != EmitStream)
        if (auto err = ctx.resource.call_cut(constant(a(1))).build(ctx).takeError())
          return err;
      break;
    }
    case GSInstanceID:
      ret = ctx.resource.gs_instance_id;
      break;
    case PrimitiveID:
      ret = ctx.resource.patch_id;
      break;
    case WaveIsFirstLane:
      ret = simd("simd_is_first", ir.getInt1Ty(), {});
      break;
    case WaveGetLaneIndex:
      ret = lane_index();
      break;
    case WaveGetLaneCount:
      ret = lane_count();
      break;
    case WaveAnyTrue:
    case WaveAllTrue:
      ret = simd(op == WaveAnyTrue ? "simd_any" : "simd_all", ir.getInt1Ty(), {a(1)});
      break;
    case WaveActiveAllEqual: {
      auto first = move("simd_broadcast_first", a(1), {});
      auto equal = a(1)->getType()->isFPOrFPVectorTy() ? ir.CreateFCmpOEQ(a(1), first) : ir.CreateICmpEQ(a(1), first);
      ret = simd("simd_all", ir.getInt1Ty(), {equal});
      break;
    }
    case WaveActiveBallot:
      ret = mask_words(simd("simd_ballot", ir.getInt64Ty(), {a(1)}, ir.getInt64Ty()), ty);
      break;
    case WaveMatch: {
      // the lanes whose value has the same bits, so each lane matches itself
      auto bits = ir.CreateBitCast(a(1), ir.getIntNTy(a(1)->getType()->getPrimitiveSizeInBits()));
      auto mask = over_lanes(
          bits, ir.getInt64(0),
          [&](Value *from, Value *value) {
            return ir.CreateSelect(
                ir.CreateICmpEQ(value, bits), ir.CreateShl(ir.getInt64(1), ir.CreateZExt(from, ir.getInt64Ty())),
                ir.getInt64(0)
            );
          },
          [&](Value *x, Value *y) { return ir.CreateOr(x, y); }
      );
      ret = mask_words(mask, ty);
      break;
    }
    case WaveReadLaneAt:
      ret = move("simd_shuffle", a(1), {i16(a(2))});
      break;
    case WaveReadLaneFirst:
      ret = move("simd_broadcast_first", a(1), {});
      break;
    case QuadReadLaneAt:
      ret = move("quad_shuffle", a(1), {i16(a(2))});
      break;
    case QuadOp:
      // across X, Y or the diagonal: the lane whose quad index differs in bit 0, bit 1, or both
      ret = move("quad_shuffle_xor", a(1), {ir.getInt16(constant(a(2)) + 1)});
      break;
    case WaveActiveOp:
    case WavePrefixOp: {
      // DXIL's WaveOpKind: sum, product, min, max; SignedOpKind: signed (and float) 0, unsigned 1
      auto kind = constant(a(2));
      auto sign = constant(a(3)) ? Signedness::Unsigned : Signedness::Signed;
      auto vty = a(1)->getType();
      bool fp = vty->isFloatingPointTy(), is_signed = sign == Signedness::Signed;
      const char *names[] = {"sum", "product", "min", "max"};
      std::function<Value *(Value *, Value *)> combine[] = {
          [&](Value *x, Value *y) { return fp ? ir.CreateFAdd(x, y) : ir.CreateAdd(x, y); },
          [&](Value *x, Value *y) { return fp ? ir.CreateFMul(x, y) : ir.CreateMul(x, y); },
          [&](Value *x, Value *y) {
            return ir.CreateSelect(is_signed ? ir.CreateICmpSLT(x, y) : ir.CreateICmpULT(x, y), x, y);
          },
          [&](Value *x, Value *y) {
            return ir.CreateSelect(is_signed ? ir.CreateICmpSGT(x, y) : ir.CreateICmpUGT(x, y), x, y);
          },
      };
      auto width = vty->getPrimitiveSizeInBits();
      Value *identity[] = {
          Constant::getNullValue(vty),
          fp ? ConstantFP::get(vty, 1.0) : ConstantInt::get(vty, 1),
          is_signed ? ConstantInt::get(vty, APInt::getSignedMaxValue(width))
                    : ConstantInt::get(vty, APInt::getMaxValue(width)),
          is_signed ? ConstantInt::get(vty, APInt::getSignedMinValue(width)) : Constant::getNullValue(vty),
      };
      ret = reduce(names[kind], a(1), sign, op == WavePrefixOp, combine[kind], identity[kind]);
      break;
    }
    case WaveActiveBit: {
      // DXIL's WaveBitOpKind: and, or, xor
      auto kind = constant(a(2));
      auto vty = a(1)->getType();
      const char *names[] = {"and", "or", "xor"};
      std::function<Value *(Value *, Value *)> combine[] = {
          [&](Value *x, Value *y) { return ir.CreateAnd(x, y); },
          [&](Value *x, Value *y) { return ir.CreateOr(x, y); },
          [&](Value *x, Value *y) { return ir.CreateXor(x, y); },
      };
      Value *identity[] = {Constant::getAllOnesValue(vty), Constant::getNullValue(vty), Constant::getNullValue(vty)};
      ret = reduce(names[kind], a(1), Signedness::Unsigned, false, combine[kind], identity[kind]);
      break;
    }
    case WaveMultiPrefixOp: {
      // DXIL's WaveMultiPrefixOpKind: sum, and, or, xor, product, over the lanes before this one in its mask
      auto vty = a(1)->getType();
      bool fp = vty->isFloatingPointTy();
      Combine combine[] = {
          [&](Value *x, Value *y) { return fp ? ir.CreateFAdd(x, y) : ir.CreateAdd(x, y); },
          [&](Value *x, Value *y) { return ir.CreateAnd(x, y); },
          [&](Value *x, Value *y) { return ir.CreateOr(x, y); },
          [&](Value *x, Value *y) { return ir.CreateXor(x, y); },
          [&](Value *x, Value *y) { return fp ? ir.CreateFMul(x, y) : ir.CreateMul(x, y); },
      };
      Value *identity[] = {
          Constant::getNullValue(vty), fp ? nullptr : Constant::getAllOnesValue(vty), Constant::getNullValue(vty),
          Constant::getNullValue(vty), fp ? ConstantFP::get(vty, 1.0) : ConstantInt::get(vty, 1)
      };
      auto kind = constant(a(6));
      ret = reduce("", a(1), Signedness::DontCare, true, combine[kind], identity[kind], lane_mask(call, 2));
      break;
    }
    case WaveMultiPrefixBitCount: {
      // the set bits of the lanes before this one in its mask
      auto before =
          ir.CreateSub(ir.CreateShl(ir.getInt64(1), ir.CreateZExt(lane_index(), ir.getInt64Ty())), ir.getInt64(1));
      auto set = simd("simd_ballot", ir.getInt64Ty(), {a(1)}, ir.getInt64Ty());
      auto counted = ir.CreateAnd(ir.CreateAnd(set, lane_mask(call, 2)), before);
      ret = ir.CreateTrunc(air.CreateIntUnOp(llvm::air::AIRBuilder::popcount, counted), ir.getInt32Ty());
      break;
    }
    case WaveAllBitCount:
    case WavePrefixBitCount: {
      auto one = ir.CreateZExt(a(1), ir.getInt32Ty());
      ret = simd(
          op == WaveAllBitCount ? "simd_sum" : "simd_prefix_exclusive_sum", ir.getInt32Ty(), {one}, ir.getInt32Ty()
      );
      break;
    }
    case StoreOutput: {
      if (a(4)->getType()->getPrimitiveSizeInBits() > 32)
        return make_error<UnsupportedFeature>("DXIL 64-bit outputs are not lowered yet");
      auto &e = entry.output[constant(a(1))];
      // a hull shader's control-point function writes its own point
      auto ptr = element_ptr(ctx.resource.output, e, call, ctx.resource.thread_id_in_patch);
      auto v = widen(a(4), e.component == kComponentI16);
      auto slot = e.semantic >= Depth && e.semantic <= DepthGreaterEqual ? ir.getFloatTy() : ir.getInt32Ty();
      ir.CreateStore(ir.CreateBitCast(v, slot), ptr);
      break;
    }
    case SetMeshOutputCounts:
      // Metal counts the vertices itself
      air.CreateSetMeshPrimitiveCount(a(2));
      if (mesh_counts[0])
        for (unsigned i = 0; i < 2; i++)
          ir.CreateStore(a(1 + i), mesh_counts[i]);
      break;
    case EmitIndices:
      for (unsigned i = 0; i < corners; i++) {
        auto index = ir.CreateTrunc(a(2 + i), i8);
        air.CreateSetMeshIndex(ir.CreateAdd(ir.CreateMul(a(1), ir.getInt32(corners)), ir.getInt32(i)), index);
        if (staged_index)
          ir.CreateStore(index, element_of(staged_index, {a(1), ir.getInt32(i)}));
      }
      break;
    case GetMeshPayload:
    case DispatchMesh: {
      // the payload is the stage's, in its own address space: a mesh shader reads it in place, an amplification
      // shader copies its own there and sets how many mesh threadgroups follow
      auto local = op == DispatchMesh ? a(4) : call;
      auto type = local->getType()->getNonOpaquePointerElementType();
      auto payload = ir.CreateBitCast(
          ctx.resource.payload, type->getPointerTo(ctx.resource.payload->getType()->getPointerAddressSpace())
      );
      if (op == GetMeshPayload) {
        if (auto err = retarget(call, payload))
          return std::move(err);
        break;
      }
      // DispatchMesh is a group barrier, and takes its arguments from the group's first thread (Mesh Shader spec,
      // "DispatchMesh intrinsic"): the threads build the payload together, and one sends it
      air.CreateBarrier(llvm::air::MemFlags::Threadgroup);
      ir.SetInsertPoint(SplitBlockAndInsertIfThen(
          ir.CreateICmpEQ(ctx.resource.thread_id_in_group_flat_arg, ir.getInt32(0)), call, false
      ));
      ir.CreateStore(ir.CreateLoad(type, local), payload);
      Value *grid = UndefValue::get(FixedVectorType::get(ir.getInt32Ty(), 3));
      for (unsigned i = 0; i < 3; i++)
        grid = ir.CreateInsertElement(grid, a(1 + i), i);
      air.CreateSetMeshProperties(grid);
      ir.SetInsertPoint(call);
      break;
    }
    case StoreVertexOutput:
    case StorePrimitiveOutput: {
      bool primitive = op == StorePrimitiveOutput;
      auto element = constant(a(1)), column = constant(a(3));
      auto &e = shader->mesh_outputs[primitive][element];
      auto data = ctx.resource.mesh_data[primitive][element];
      auto &varyings = ctx.resource.mesh_varyings[primitive][element];
      // the component as 32 bits of the element's type
      Value *v = a(4);
      if (v->getType()->isIntegerTy(1))
        v = ir.CreateZExt(v, ir.getInt32Ty());
      if (v->getType()->getPrimitiveSizeInBits() == 16)
        v = v->getType()->isHalfTy()                  ? ir.CreateFPExt(v, ir.getFloatTy())
            : e.type == RegisterComponentType::Int ? ir.CreateSExt(v, ir.getInt32Ty())
                                                   : ir.CreateZExt(v, ir.getInt32Ty());
      v = ir.CreateBitCast(v, e.type == RegisterComponentType::Float ? ir.getFloatTy() : ir.getInt32Ty());
      mesh_row(call, e.rows, [&](uint32_t row) {
        auto component = row * e.cols + column;
        if (auto varying = varyings[component]; ~varying)
          primitive ? air.CreateSetMeshPrimitiveData(a(5), ir.getInt32(varying), v)
                    : air.CreateSetMeshVertexData(a(5), ir.getInt32(varying), v);
        switch (e.semantic) {
        case SemanticPosition: {
          if (staged_position) {
            ir.CreateStore(v, element_of(staged_position, {a(5), ir.getInt32(column)}));
            break;
          }
          auto &position = positions[position_of[call]];
          position.components[column] = v;
          if (position.last != call)
            break;
          Value *value = UndefValue::get(FixedVectorType::get(ir.getFloatTy(), 4));
          for (unsigned c = 0; c < 4; c++)
            value = ir.CreateInsertElement(value, position.components[c], c);
          air.CreateSetMeshPosition(a(5), value);
          break;
        }
        case SemanticClipDistance:
          air.CreateSetMeshClipDistance(a(5), ir.getInt32(data + component), v);
          break;
        case SemanticCullDistance:
          ir.CreateStore(
              ir.CreateZExt(ir.CreateFCmpULT(v, ConstantFP::get(ir.getFloatTy(), 0)), i8),
              element_of(staged_distance, {a(5), ir.getInt32(data + component)})
          );
          break;
        case SemanticPrimitiveID:
          air.CreateSetMeshPrimitiveID(a(5), v);
          break;
        case SemanticRenderTargetArrayIndex:
          air.CreateSetMeshRenderTargetArrayIndex(a(5), v);
          break;
        case SemanticViewportArrayIndex:
          air.CreateSetMeshViewportArrayIndex(a(5), v);
          break;
        case SemanticCullPrimitive:
          if (staged_culled)
            ir.CreateStore(ir.CreateZExt(ir.CreateICmpNE(v, ir.getInt32(0)), i8), element_of(staged_culled, {a(5)}));
          else
            air.CreateSetMeshPrimitiveCulled(a(5), ir.CreateICmpNE(v, ir.getInt32(0)));
          break;
        default:
          break;
        }
      });
      break;
    }
    case CBufferLoadLegacy: {
      // a 16-byte row, as the elements DXIL reads it as: four of 32 bits, eight of 16 or two of 64
      auto sty = cast<StructType>(ty);
      auto bits = sty->getElementType(0)->getPrimitiveSizeInBits();
      auto row = ir.CreateBitCast(
          dxbc.LoadConstantBuffer(*handles[a(1)].cbuffer, a(2)), FixedVectorType::get(ir.getIntNTy(bits), 128 / bits)
      );
      ret = UndefValue::get(ty);
      for (unsigned i = 0; i < sty->getNumElements(); i++)
        ret = ir.CreateInsertValue(ret, ir.CreateBitCast(ir.CreateExtractElement(row, i), sty->getElementType(i)), {i});
      break;
    }
    case Sample:
    case SampleBias:
    case SampleLevel:
    case SampleGrad:
    case SampleCmp:
    case SampleCmpLevelZero: {
      auto &tex = handles[a(1)];
      auto &smp = handles[a(2)];
      auto [texture, coord, array, sampler, bias, logical, coord_ty] = sample(call, tex, smp, 3);
      int32_t offset[3];
      offsets(call, 7, 3, offset);
      auto handle = tex.texture->Handle;
      auto reference = op == SampleCmp || op == SampleCmpLevelZero ? a(10) : nullptr;
      // compute shaders have no derivatives: they sample with their quads' gradients, or compare at their LOD
      bool compute = ctx.shader_type == D3D11_SB_COMPUTE_SHADER;
      std::function<std::pair<Value *, Value *>(Value *, Value *)> read;
      if (compute && (op == Sample || op == SampleBias)) {
        auto [dx, dy] = quad_gradients(coord);
        if (op == SampleBias)
          bias = ir.CreateFAdd(a(10), bias);
        auto gx = dxbc.BiasGradient(dx, bias), gy = dxbc.BiasGradient(dy, bias);
        auto clamp = min_lod(tex, a(op == Sample ? 10 : 11));
        read = [&, gx, gy, clamp](Value *s, Value *) {
          return air.CreateSampleGrad(texture, handle, s, coord, array, gx, gy, clamp, offset);
        };
      } else if (compute && op == SampleCmp) {
        auto is_cube = llvm::air::AIRBuilder::getTextureInfo(tex.texture->Logical).is_cube;
        auto lod = llvm::air::sample_level{air.CreateFPBinOp(
            llvm::air::AIRBuilder::fmax, min_lod(tex, a(11)), ir.CreateFAdd(quad_lod(*tex.texture, coord, is_cube), bias)
        )};
        read = [&, lod](Value *s, Value *ref) {
          return ref ? air.CreateSampleCmp(texture, handle, s, coord, array, ref, offset, lod)
                     : air.CreateSample(texture, handle, s, coord, array, offset, lod);
        };
      } else {
        switch (op) {
        case Sample: {
          auto options = std::pair{llvm::air::sample_bias{bias}, llvm::air::sample_min_lod_clamp{min_lod(tex, a(10))}};
          read = [&, options](Value *s, Value *) {
            return air.CreateSample(texture, handle, s, coord, array, offset, options.first, options.second);
          };
          break;
        }
        case SampleBias: {
          auto options = std::pair{
              llvm::air::sample_bias{ir.CreateFAdd(a(10), bias)}, llvm::air::sample_min_lod_clamp{min_lod(tex, a(11))}
          };
          read = [&, options](Value *s, Value *) {
            return air.CreateSample(texture, handle, s, coord, array, offset, options.first, options.second);
          };
          break;
        }
        case SampleLevel: {
          auto lod = llvm::air::sample_level{min_lod(tex, ir.CreateFAdd(a(10), bias))};
          read = [&, lod](Value *s, Value *) {
            return air.CreateSample(texture, handle, s, coord, array, offset, lod);
          };
          break;
        }
        case SampleGrad: {
          auto gx = dxbc.BiasGradient(vec(call, 10, logical, coord_ty), bias);
          auto gy = dxbc.BiasGradient(vec(call, 13, logical, coord_ty), bias);
          auto clamp = min_lod(tex, a(16));
          read = [&, gx, gy, clamp](Value *s, Value *) {
            return air.CreateSampleGrad(texture, handle, s, coord, array, gx, gy, clamp, offset);
          };
          break;
        }
        case SampleCmp: {
          auto options = std::pair{llvm::air::sample_bias{bias}, llvm::air::sample_min_lod_clamp{min_lod(tex, a(11))}};
          read = [&, options](Value *s, Value *ref) {
            return ref ? air.CreateSampleCmp(texture, handle, s, coord, array, ref, offset, options.first, options.second)
                       : air.CreateSample(texture, handle, s, coord, array, offset, options.first, options.second);
          };
          break;
        }
        default: {
          auto lod = llvm::air::sample_level{min_lod(tex, bias)};
          read = [&, lod](Value *s, Value *ref) {
            return ref ? air.CreateSampleCmp(texture, handle, s, coord, array, ref, offset, lod)
                       : air.CreateSample(texture, handle, s, coord, array, offset, lod);
          };
          break;
        }
        }
      }
      auto [value, residency] = read(sampler, reference);
      value = dxbc.CustomBorder(
          *tex.texture, sampler, smp.sampler->Border, smp.sampler->Metadata, value,
          [&](Value *s) { return read(s, nullptr).first; }, reference
      );
      ret = to_struct(ty, value, residency);
      break;
    }
    case TextureLoad: {
      auto &h = handles[a(1)];
      if (element_bits(ty) > 32)
        return make_error<UnsupportedFeature>("DXIL 64-bit texture loads are not lowered yet");
      int32_t offset[3];
      offsets(call, 6, 3, offset);
      bool ms = llvm::air::AIRBuilder::getTextureInfo(h.texture->Logical).is_ms;
      auto [value, residency] = texel_read(
          h, texel_coord(call, 3, !ms && defined(a(2)) ? a(2) : ir.getInt32(0)), offset, ms ? a(2) : nullptr
      );
      ret = to_struct(ty, value, residency);
      break;
    }
    case TextureStore:
      if (element_bits(a(5)->getType()) > 32)
        return make_error<UnsupportedFeature>("DXIL 64-bit texture stores are not lowered yet");
      texel_write(handles[a(1)], texel_coord(call, 2, ir.getInt32(0)), call, 5);
      break;
    case BufferLoad:
    case RawBufferLoad:
    case BufferStore:
    case RawBufferStore: {
      auto &h = handles[a(1)];
      bool store = op == BufferStore || op == RawBufferStore;
      // typed buffers are texel buffers
      if (h.texture) {
        const int32_t no_offset[3] = {};
        auto coord = ir.CreateInsertElement(UndefValue::get(air.getIntTy(4)), a(2), (uint64_t)0);
        if (store) {
          texel_write(h, coord, call, 4);
        } else {
          auto [value, residency] = texel_read(h, ir.CreateInsertElement(coord, ir.getInt32(0), 3), no_offset, nullptr);
          ret = to_struct(ty, value, residency);
        }
        break;
      }
      auto &b = *h.buffer;
      if (store && air.DiscardWrites)
        break;
      // memory moves in units of the element's size, so a 16-bit store leaves its neighbour alone; 64-bit elements
      // move as dword pairs, raw buffers aligning them to 4 bytes only
      unsigned bits = element_bits(store ? a(4)->getType() : ty);
      auto unit = ir.getIntNTy(std::min(bits, 32u));
      auto parts = bits / unit->getBitWidth();
      auto base = ir.CreateUDiv(byte_address(h, a(2), a(3)), ir.getInt64(unit->getBitWidth() / 8));
      // a load reads only the components its users extract, so nothing is read past what the shader asks for
      uint32_t mask = store ? constant(a(8)) : 0;
      if (!store)
        for (auto user : call->users()) {
          auto extract = dyn_cast<ExtractValueInst>(user);
          mask |= extract ? 1u << extract->getIndices()[0] : 0xfu;
        }
      auto element = ir.getIntNTy(bits);
      Value *v = UndefValue::get(FixedVectorType::get(element, 4));
      for (unsigned i = 0; i < 4; i++) {
        if (!(mask & (1u << i)))
          continue;
        auto x = store ? ir.CreateBitCast(a(4 + i), element) : nullptr;
        Value *loaded = ConstantInt::get(element, 0);
        for (unsigned part = 0; part < parts; part++) {
          auto ptr = unit_ptr(b, unit, ir.CreateAdd(base, ir.getInt64(i * parts + part)));
          auto shift = part * unit->getBitWidth();
          if (store) {
            auto piece = ir.CreateTrunc(ir.CreateLShr(x, shift), unit);
            if (b.GlobalCoherent)
              air.CreateDeviceCoherentStore(piece, ptr);
            else
              ir.CreateStore(piece, ptr);
            continue;
          }
          Value *piece = b.GlobalCoherent ? (Value *)air.CreateDeviceCoherentLoad(unit, ptr) : ir.CreateLoad(unit, ptr);
          loaded = ir.CreateOr(loaded, ir.CreateShl(ir.CreateZExt(piece, element), shift));
        }
        if (!store)
          v = ir.CreateInsertElement(v, loaded, i);
      }
      if (!store)
        ret = to_struct(ty, v, nullptr);
      break;
    }
    case GetDimensions: {
      auto &h = handles[a(1)];
      Value *v;
      if (h.texture && h.texture->Logical != Texture::texture_buffer) {
        v = dxbc.TextureDimensions(*h.texture, defined(a(2)) ? a(2) : ir.getInt32(0));
        // DXIL reports the sample count where resinfo reports one mip
        if (llvm::air::AIRBuilder::getTextureInfo(h.texture->Logical).is_ms)
          v = ir.CreateInsertElement(
              v, air.CreateTextureQuery(h.texture->Texture, h.texture->Handle, Texture::num_samples, ir.getInt32(0)), 3
          );
      } else {
        // raw buffers report bytes, structured buffers elements, typed buffers texels
        auto count = h.texture ? dxbc.DecodeTextureBufferElement(h.texture->Metadata)
                               : dxbc.DecodeRawBufferByteLength(h.buffer->Metadata);
        if (h.buffer && h.buffer->StructureStride)
          count = ir.CreateUDiv(count, ir.getInt32(h.buffer->StructureStride));
        v = ir.CreateInsertElement(Constant::getNullValue(air.getIntTy(4)), count, (uint64_t)0);
      }
      ret = to_struct(ty, v, nullptr);
      break;
    }
    case TextureGather:
    case TextureGatherCmp: {
      auto &tex = handles[a(1)];
      auto [texture, coord, array, sampler, bias, logical, coord_ty] = sample(call, tex, handles[a(2)], 3);
      // programmable offsets, as many as the coordinates, missing ones 0
      Value *offset = Constant::getNullValue(air.getIntTy(logical));
      for (unsigned i = 0; i < std::min(logical, 2u); i++)
        if (defined(a(7 + i)))
          offset = logical > 1 ? ir.CreateInsertElement(offset, a(7 + i), i) : a(7 + i);
      auto [value, residency] = dxbc.Gather(
          *tex.texture, sampler, handles[a(2)].sampler->Metadata, coord, array, offset,
          op == TextureGather ? a(9) : nullptr, op == TextureGatherCmp ? a(10) : nullptr, handles[a(2)].sampler->Border
      );
      ret = to_struct(ty, value, residency);
      break;
    }
    case CalculateLOD: {
      auto &tex = handles[a(1)];
      auto [texture, coord, array, sampler, bias, logical, coord_ty] = sample(call, tex, handles[a(2)], 3);
      auto is_cube = llvm::air::AIRBuilder::getTextureInfo(tex.texture->Logical).is_cube;
      auto [clamped, unclamped] = dxbc.CalculateLOD(
          *tex.texture, sampler, handles[a(2)].sampler->Metadata, coord,
          ctx.shader_type == D3D11_SB_COMPUTE_SHADER ? quad_lod(*tex.texture, coord, is_cube) : nullptr
      );
      ret = constant(a(6)) ? clamped : unclamped;
      break;
    }
    case WriteSamplerFeedback:
    case WriteSamplerFeedbackBias:
    case WriteSamplerFeedbackLevel:
    case WriteSamplerFeedbackGrad: {
      // a feedback map holds, per mip region of the paired texture's first mip, a bit for each mip wanted there:
      // bit n in the first region of each 2^n by 2^n block of regions, which is that mip's region. a sample wants
      // what its sampler would read (Sampler Feedback spec, "General interpretation and usage"; D3D11.3 7.18): the
      // mips its mip filter picks, and in each the texels its minification or magnification filter reads around
      // every tap of the filter. reads of no weight may be recorded ("Zero-weight reads")
      using llvm::air::AIRBuilder;
      auto &map = *handles[a(1)].texture;
      auto &t = *handles[a(2)].texture;
      auto [texture, coord, array, sampler, bias, logical, coord_ty] = sample(call, handles[a(2)], handles[a(3)], 4);
      auto metadata = handles[a(3)].sampler->Metadata;
      auto i32 = ir.getInt32Ty();
      auto f32 = ir.getFloatTy();
      auto real = [&](double v) { return ConstantFP::get(f32, v); };
      auto size = [&](const TextureResourceHandle &of, unsigned axis, Value *mip) {
        return air.CreateTextureQuery(of.Texture, of.Handle, axis ? Texture::height : Texture::width, mip);
      };
      auto to_float = [&](Value *v) { return air.CreateConvertToFloat(v, Signedness::Unsigned); };
      auto max = [&](Value *l, Value *r) { return air.CreateIntBinOp(AIRBuilder::max, l, r, true); };
      auto min = [&](Value *l, Value *r) { return air.CreateIntBinOp(AIRBuilder::min, l, r, true); };
      auto fmax = [&](Value *l, Value *r) { return air.CreateFPBinOp(AIRBuilder::fmax, l, r); };
      auto fmin = [&](Value *l, Value *r) { return air.CreateFPBinOp(AIRBuilder::fmin, l, r); };
      auto field = [&](unsigned shift, unsigned bits) {
        return ir.CreateAnd(ir.CreateTrunc(ir.CreateLShr(metadata, shift), i32), ir.getInt32((1u << bits) - 1));
      };
      auto set = [&](unsigned shift) { return ir.CreateICmpNE(field(shift, 1), ir.getInt32(0)); };
      // a map is of the paired resource, of which the texture's view may be a part: the view's descriptor says,
      // after its format range (d3d12_descriptor_heap.cpp), where the view starts and what the resource has
      Value *first_mip = ir.getInt32(0), *first_slice = ir.getInt32(0);
      Value *resource_mips = air.CreateTextureQuery(t.Texture, t.Handle, Texture::num_mip_levels, ir.getInt32(0));
      Value *resource_size[2] = {size(t, 0, ir.getInt32(0)), size(t, 1, ir.getInt32(0))};
      if (t.Range) {
        auto place = ir.CreateLoad(ir.getInt64Ty(), ir.CreateConstGEP1_32(ir.getInt64Ty(), t.Range, 1));
        auto part = [&](unsigned shift, unsigned bits) {
          return ir.CreateAnd(ir.CreateTrunc(ir.CreateLShr(place, shift), i32), ir.getInt32((1u << bits) - 1));
        };
        first_mip = part(0, 8);
        resource_mips = part(8, 8);
        first_slice = part(16, 16);
        resource_size[0] = part(32, 16);
        resource_size[1] = part(48, 16);
      }
      // derivatives in texels of the first mip: the operation's, or the pixel's. an explicit LOD has none
      auto texels = [&](Value *gradient) {
        for (unsigned i = 0; i < logical; i++)
          gradient = ir.CreateInsertElement(
              gradient, ir.CreateFMul(ir.CreateExtractElement(gradient, i), to_float(size(t, i, ir.getInt32(0)))), i
          );
        return gradient;
      };
      Value *dx = nullptr, *dy = nullptr;
      if (op == WriteSamplerFeedbackGrad) {
        dx = texels(vec(call, 8, logical, coord_ty));
        dy = texels(vec(call, 11, logical, coord_ty));
      } else if (op != WriteSamplerFeedbackLevel) {
        dx = texels(air.CreateDerivative(coord, false));
        dy = texels(air.CreateDerivative(coord, true));
      }
      // the LOD of the derivatives and the filter's line of anisotropy (7.18.11): the longer derivative is the line,
      // and the footprint's width across it, no less than the line over the sampler's largest ratio, gives the LOD.
      // a sampler that is not anisotropic has a ratio of 1, and its LOD is the line's
      Value *ratio = real(1), *line = Constant::getNullValue(coord_ty), *level = nullptr;
      auto anisotropy =
          to_float(max(field(SM50_SAMPLER_METADATA_ANISOTROPY, SM50_SAMPLER_METADATA_ANISOTROPY_BITS), ir.getInt32(1)));
      if (dx) {
        auto c = [&](Value *v, unsigned i) { return ir.CreateExtractElement(v, i); };
        auto x2 = air.CreateDotProduct(dx, dx), y2 = air.CreateDotProduct(dy, dy);
        auto area = fp_unary(
            AIRBuilder::fabs, ir.CreateFSub(ir.CreateFMul(c(dx, 0), c(dy, 1)), ir.CreateFMul(c(dx, 1), c(dy, 0)))
        );
        auto x_longer = ir.CreateFCmpOGT(x2, y2);
        line = ir.CreateSelect(x_longer, dx, dy);
        auto length2 = ir.CreateSelect(x_longer, x2, y2);
        auto length = fp_unary(AIRBuilder::sqrt, length2);
        ratio = ir.CreateFDiv(length2, area);
        // a footprint without area is as thin as the sampler allows
        auto over = ir.CreateFCmpUGT(ratio, anisotropy);
        ratio = ir.CreateSelect(over, anisotropy, ratio);
        auto width = ir.CreateSelect(over, ir.CreateFDiv(length, anisotropy), ir.CreateFDiv(area, length));
        ratio = ir.CreateSelect(ir.CreateFCmpOLT(width, real(1)), fmax(real(1), ir.CreateFMul(ratio, width)), ratio);
        level = fp_unary(AIRBuilder::log2, width);
        // the pixel's own derivatives are the hardware's, and so is their LOD
        if (op != WriteSamplerFeedbackGrad)
          level = air.CreateCalculateLOD(t.Texture, t.Handle, sampler, coord).second;
        if (op == WriteSamplerFeedbackBias)
          level = ir.CreateFAdd(level, a(8));
      } else {
        level = a(8);
      }
      auto [clamped, unclamped] = dxbc.CalculateLOD(t, sampler, metadata, coord, level);
      auto clamp = a(call->arg_size() - 1);
      if (op != WriteSamplerFeedbackLevel && defined(clamp))
        clamped = fmax(clamped, clamp);
      // a MinMip map, which its descriptor marks, records the mip wanted "with no mip level clamping applied", the
      // "ideal" one ("MinMip feedback map", "Interpretation"): of all the resource's mips, before the sampler's, the
      // view's and the operation's clamps. another map records the mip that is read, one of the view's. either way
      // the mip is counted in the resource from here on
      auto ideal = ir.CreateICmpNE(ir.CreateTrunc(map.Metadata, i32), ir.getInt32(0));
      auto wanted = ir.CreateSelect(ideal, unclamped, clamped);
      auto mips = air.CreateTextureQuery(t.Texture, t.Handle, Texture::num_mip_levels, ir.getInt32(0));
      auto last_mip = [&](Value *count) { return to_float(ir.CreateSub(max(count, ir.getInt32(1)), ir.getInt32(1))); };
      auto lod = ir.CreateSelect(
          ideal, fmin(fmax(ir.CreateFAdd(unclamped, to_float(first_mip)), real(0)), last_mip(resource_mips)),
          ir.CreateFAdd(fmin(fmax(clamped, real(0)), last_mip(mips)), to_float(first_mip))
      );
      // the magnification filter reads the texels of a LOD of 0 or less, the minification filter the others
      // (7.18.11); a point filter reads the texel the coordinate is in, a linear one the two around it (7.18.7,
      // 7.18.8)
      auto linear = ir.CreateSelect(
          ir.CreateFCmpOLE(wanted, real(0)), set(SM50_SAMPLER_METADATA_MAG_LINEAR), set(SM50_SAMPLER_METADATA_MIN_LINEAR)
      );
      // a point mip filter takes the nearest mip, a linear one the two around the LOD (7.18.10)
      auto mip_linear = set(SM50_SAMPLER_METADATA_MIP_LINEAR);
      std::pair<Value *, Value *> levels[2] = {
          {air.CreateConvertToUnsigned(fp_unary(AIRBuilder::floor, ir.CreateSelect(mip_linear, lod, ir.CreateFAdd(lod, real(0.5))))),
           ir.getTrue()},
          {air.CreateConvertToUnsigned(fp_unary(AIRBuilder::ceil, lod)), mip_linear},
      };
      auto modes = field(SM50_SAMPLER_METADATA_ADDRESS, 5);
      auto axis_modes = ir.getInt32(SM50_SAMPLER_ADDRESS_COUNT);
      // an anisotropic filter's taps: as many as its ratio, evenly along its line, which the sample is the middle of.
      // Direct3D leaves the kernel to the implementation
      auto taps = min(
          air.CreateConvertToUnsigned(fp_unary(AIRBuilder::ceil, ratio)),
          ir.getInt32((1u << SM50_SAMPLER_METADATA_ANISOTROPY_BITS) - 1)
      );
      repeat(call, taps, [&](Value *tap) {
        auto along = ir.CreateFSub(ir.CreateFDiv(ir.CreateFAdd(to_float(tap), real(0.5)), to_float(taps)), real(0.5));
        for (auto &[mip, mip_kept] : levels) {
          // along each axis, the region of each of the two texels and whether the filter and the address mode keep
          // the texel
          std::pair<Value *, Value *> regions[2][2];
          for (unsigned axis = 0; axis < 2; axis++) {
            // a null texture has no texels and takes no writes
            auto n = max(ir.CreateLShr(resource_size[axis], mip), ir.getInt32(1));
            auto last = ir.CreateSub(n, ir.getInt32(1));
            auto mode = axis ? ir.CreateUDiv(modes, axis_modes) : ir.CreateURem(modes, axis_modes);
            auto is = [&](SM50_SAMPLER_ADDRESS m) { return ir.CreateICmpEQ(mode, ir.getInt32(m)); };
            auto u = ir.CreateFAdd(
                ir.CreateExtractElement(coord, axis),
                ir.CreateFDiv(ir.CreateFMul(along, ir.CreateExtractElement(line, axis)), to_float(size(t, axis, ir.getInt32(0))))
            );
            // the coordinate's range is reduced before it is scaled, so one far out keeps its fraction (7.18.6)
            auto half = ir.CreateFMul(u, real(0.5));
            auto reduced = ir.CreateSelect(
                is(SM50_SAMPLER_ADDRESS_REPEAT), ir.CreateFSub(u, fp_unary(AIRBuilder::floor, u)),
                ir.CreateSelect(
                    is(SM50_SAMPLER_ADDRESS_MIRROR),
                    ir.CreateFMul(ir.CreateFSub(half, fp_unary(AIRBuilder::floor, half)), real(2)),
                    fmin(fmax(u, real(-10)), real(10))
                )
            );
            auto first = air.CreateConvertToSigned(fp_unary(
                AIRBuilder::floor,
                ir.CreateFSub(ir.CreateFMul(reduced, to_float(n)), ir.CreateSelect(linear, real(0.5), real(0)))
            ));
            auto inside = [&](Value *v) { return min(max(v, ir.getInt32(0)), last); };
            auto wrapped = [&](Value *v, Value *period) {
              auto r = ir.CreateSRem(v, period);
              return ir.CreateSelect(ir.CreateICmpSLT(r, ir.getInt32(0)), ir.CreateAdd(r, period), r);
            };
            // the mip's own grid of regions lies over the whole map, whatever the sizes divide to ("Example of
            // non-power-of-two feedback maps behavior")
            auto coarse = max(ir.CreateLShr(size(map, axis, ir.getInt32(0)), mip), ir.getInt32(1));
            for (unsigned k = 0; k < 2; k++) {
              auto i = ir.CreateAdd(first, ir.getInt32(k));
              auto twice = wrapped(i, ir.CreateShl(n, 1));
              // by SM50_SAMPLER_ADDRESS; a border texel is no texel of the texture
              Value *by_mode[SM50_SAMPLER_ADDRESS_COUNT] = {
                  inside(i),
                  inside(ir.CreateSelect(ir.CreateICmpSLT(i, ir.getInt32(0)), ir.CreateNot(i), i)),
                  wrapped(i, n),
                  ir.CreateSelect(ir.CreateICmpSLT(twice, n), twice, ir.CreateSub(ir.CreateAdd(n, last), twice)),
                  inside(i),
              };
              auto texel = by_mode[0];
              for (unsigned m = 1; m < SM50_SAMPLER_ADDRESS_COUNT; m++)
                texel = ir.CreateSelect(ir.CreateICmpEQ(mode, ir.getInt32(m)), by_mode[m], texel);
              auto kept = ir.CreateOr(ir.CreateNot(is(SM50_SAMPLER_ADDRESS_BORDER)), ir.CreateICmpEQ(texel, i));
              if (k)
                kept = ir.CreateAnd(kept, linear);
              auto center = ir.CreateFDiv(ir.CreateFAdd(air.CreateConvertToFloat(texel), real(0.5)), to_float(n));
              auto region = min(
                  air.CreateConvertToSigned(ir.CreateFMul(center, to_float(coarse))), ir.CreateSub(coarse, ir.getInt32(1))
              );
              regions[axis][k] = {ir.CreateShl(region, mip), kept};
            }
          }
          auto bit = ir.CreateShl(ir.getInt32(1), mip);
          for (unsigned corner = 0; corner < 4; corner++) {
            auto &[x, keep_x] = regions[0][corner & 1];
            auto &[y, keep_y] = regions[1][corner >> 1];
            Value *pos = UndefValue::get(air.getIntTy(2));
            pos = ir.CreateInsertElement(ir.CreateInsertElement(pos, x, (uint64_t)0), y, 1);
            auto kept = ir.CreateAnd(mip_kept, ir.CreateAnd(keep_x, keep_y));
            air.CreateAtomicRMW(
                map.Texture, map.Handle, AtomicRMWInst::Or, pos,
                ir.CreateVectorSplat(4, ir.CreateSelect(kept, bit, ir.getInt32(0))),
                array ? ir.CreateAdd(array, first_slice) : first_slice
            );
          }
        }
      });
      break;
    }
    case AtomicBinOp:
    case AtomicCompareExchange: {
      auto &h = handles[a(1)];
      bool exchange = op == AtomicCompareExchange;
      unsigned first = exchange ? 2 : 3;
      const int32_t no_offset[3] = {};
      if (ty->isIntegerTy(64) && native_extremes) {
        bool max = atomic_op(constant(a(2))) == AtomicRMWInst::UMax;
        if (h.buffer) {
          air.CreateAtomicMinMax64(
              max,
              unit_ptr(*h.buffer, ir.getInt64Ty(), ir.CreateLShr(byte_address(h, a(first), a(first + 1)), 3)), a(6)
          );
        } else {
          auto &t = *h.texture;
          auto [address, array] = dxbc.TexelAddress(t, texel_coord(call, first, ir.getInt32(0)), no_offset, nullptr);
          air.CreateAtomicMinMax64(t.Texture, t.Handle, max, address, array, a(6));
        }
        ret = UndefValue::get(ty);
        break;
      }
      auto locks = ty->isIntegerTy(64) ? ctx.binding.GetAtomicLocks(air) : nullptr;
      if (locks) {
        // a typed resource's 64-bit texel is an R32G32_UINT one, low half first, keyed by resource and position
        auto texel = [&]() -> Word64 {
          auto i64 = ir.getInt64Ty();
          auto &t = *h.texture;
          auto [address, array] = dxbc.TexelAddress(t, texel_coord(call, first, ir.getInt32(0)), no_offset, nullptr);
          Value *key = ir.CreatePtrToInt(t.Handle, i64);
          auto n = isa<FixedVectorType>(address->getType()) ? cast<FixedVectorType>(address->getType())->getNumElements() : 1;
          for (unsigned i = 0; i <= n; i++) {
            auto c = i < n ? (n > 1 ? ir.CreateExtractElement(address, i) : address) : array;
            if (c)
              key = ir.CreateAdd(ir.CreateMul(key, ir.getInt64(0x9e3779b97f4a7c15)), ir.CreateZExt(c, i64));
          }
          return {key, llvm::air::MemFlags::Texture,
                  [=, &ir, &air, &t] {
                    auto v = air.CreateRead(t.Texture, t.Handle, address, array, nullptr, ir.getInt32(0), true).first;
                    return ir.CreateOr(
                        ir.CreateZExt(ir.CreateExtractElement(v, (uint64_t)0), i64),
                        ir.CreateShl(ir.CreateZExt(ir.CreateExtractElement(v, 1), i64), 32)
                    );
                  },
                  [=, &ir, &air, &t](Value *v) {
                    Value *halves = Constant::getNullValue(FixedVectorType::get(ir.getInt32Ty(), 4));
                    halves = ir.CreateInsertElement(halves, ir.CreateTrunc(v, ir.getInt32Ty()), (uint64_t)0);
                    halves = ir.CreateInsertElement(halves, ir.CreateTrunc(ir.CreateLShr(v, 32), ir.getInt32Ty()), 1);
                    air.CreateWrite(t.Texture, t.Handle, address, array, nullptr, ir.getInt32(0), halves, true);
                  }};
        };
        auto code = exchange ? 0 : constant(a(2));
        auto word = h.buffer ? word_at(unit_ptr(
                                   *h.buffer, ir.getInt64Ty(), ir.CreateLShr(byte_address(h, a(first), a(first + 1)), 3)
                               ))
                             : texel();
        ret = locked(word, locks, [&](Value *old) {
          return exchange ? ir.CreateSelect(ir.CreateICmpEQ(old, a(5)), a(6), old)
                          : buildAtomicRMWValue(atomic_op(code), ir, old, a(6));
        });
        break;
      }
      if (ty->isIntegerTy(64))
        return make_error<UnsupportedFeature>(std::format(
            "64-bit atomic {} on a {}", exchange ? "compare exchange" : AtomicRMWInst::getOperationName(atomic_op(constant(a(2)))).str(),
            h.buffer ? "buffer" : "texture"
        ));
      // float exchanges are the integer ones on the same bits
      auto bits = [&](unsigned i) { return ir.CreateBitCast(a(i), ir.getInt32Ty()); };
      if (h.buffer) {
        auto ptr = unit_ptr(*h.buffer, ir.getInt32Ty(), ir.CreateLShr(byte_address(h, a(first), a(first + 1)), 2));
        ret = !exchange            ? air.CreateAtomicRMW(atomic_op(constant(a(2))), ptr, bits(6))
              : air.DiscardWrites ? ir.CreateLoad(ir.getInt32Ty(), ptr)
                                  : ir.CreateExtractValue(
                                        ir.CreateAtomicCmpXchg(
                                            ptr, bits(5), bits(6), {}, AtomicOrdering::Monotonic, AtomicOrdering::Monotonic
                                        ),
                                        0
                                    );
        ret = ir.CreateBitCast(ret, ty);
        break;
      }
      auto &t = *h.texture;
      auto [address, array] = dxbc.TexelAddress(t, texel_coord(call, first, ir.getInt32(0)), no_offset, nullptr);
      auto splat = [&](Value *v) { return ir.CreateVectorSplat(4, v); };
      Value *v = exchange
                     ? air.CreateAtomicCmpXchg(t.Texture, t.Handle, address, splat(bits(5)), splat(bits(6)), array).first
                     : air.CreateAtomicRMW(t.Texture, t.Handle, atomic_op(constant(a(2))), address, splat(bits(6)), array);
      ret = ir.CreateBitCast(v->getType()->isVectorTy() ? ir.CreateExtractElement(v, (uint64_t)0) : v, ty);
      break;
    }
    case BufferUpdateCounter: {
      // IncrementCounter returns the value before, DecrementCounter the value after
      auto &h = handles[a(1)];
      bool increment = cast<ConstantInt>(a(2))->getSExtValue() > 0;
      auto old =
          air.CreateAtomicRMW(increment ? AtomicRMWInst::Add : AtomicRMWInst::Sub, h.counter->Pointer, ir.getInt32(1));
      ret = increment ? old : ir.CreateSub(old, ir.getInt32(1));
      break;
    }
    case ThreadId:
    case GroupId:
    case ThreadIdInGroup:
    case FlattenedThreadIdInGroup:
      ret = thread_input(op, op == FlattenedThreadIdInGroup ? 0 : constant(a(1)));
      break;
    case Coverage:
      ret = attribute(shader::common::InputAttribute::CoverageMask, 0);
      break;
    case SampleIndex:
      ret = ctx.function->getArg(shader->func_signature.DefineInput(air::InputSampleIndex{}));
      break;
    case EvalCentroid:
    case EvalSampleIndex:
    case EvalSnapped: {
      auto itp = interpolant(call);
      if (!itp)
        return make_error<UnsupportedFeature>(
            "DXIL attribute evaluation of an input that is not interpolated in the shader, or with a dynamic row, is not lowered yet"
        );
      Value *v = op == EvalSnapped ? dxbc.InterpolateAtOffset(*itp, vec(call, 4, 2, air.getIntTy(2)))
                                   : dxbc.Interpolate(*itp, [&](Value *handle) {
                                       return op == EvalCentroid
                                                  ? air.CreateInterpolateAtCentroid(handle, itp->Perspective)
                                                  : air.CreateInterpolateAtSample(handle, a(4), itp->Perspective);
                                     });
      ret = narrow(interpolated_component(call, v), ty);
      break;
    }
    case FirstbitLo:
    case FirstbitHi:
    case FirstbitSHi: {
      const IntegerUnaryOp ops[] = {
          IntegerUnaryOp::FirstLowBit, IntegerUnaryOp::FirstHiBit, IntegerUnaryOp::FirstHiBitSigned
      };
      // on 32 bits, as DXIL returns them: a narrower value extends by the sign its operation reads it with, and the
      // high bits it gains move a count from the top back by as many, unless no bit was found
      auto width = a(1)->getType()->getPrimitiveSizeInBits();
      auto v = width >= 32         ? a(1)
               : op == FirstbitSHi ? ir.CreateSExt(a(1), ir.getInt32Ty())
                                   : ir.CreateZExt(a(1), ir.getInt32Ty());
      ret = ir.CreateZExtOrTrunc(dxbc.FirstBit(ops[op - FirstbitLo], v), ty);
      if (width < 32 && op != FirstbitLo)
        ret = ir.CreateSelect(ir.CreateICmpEQ(ret, ir.getInt32(~0u)), ret, ir.CreateSub(ret, ir.getInt32(32 - width)));
      break;
    }
    case Ibfe:
    case Ubfe:
      ret = dxbc.ExtractBits(a(1), a(2), a(3), op == Ibfe);
      break;
    case Bfi:
      ret = dxbc.InsertBits(a(1), a(2), a(3), a(4));
      break;
    case Msad:
      ret = dxbc.MaskedSumOfAbsDiff(a(1), a(2), a(3));
      break;
    case LegacyF32ToF16:
      ret = ir.CreateZExt(ir.CreateBitCast(dxbc.ConvertToHalfTowardZero(a(1)), ir.getInt16Ty()), ir.getInt32Ty());
      break;
    case LegacyF16ToF32:
      ret = air.CreateConvertToFloat(ir.CreateBitCast(ir.CreateTrunc(a(1), ir.getInt16Ty()), ir.getHalfTy()));
      break;
    case Barrier: {
      auto mode = constant(a(1));
      dxbc(
          InstSync{
              .uav_boundary = mode & 2   ? InstSync::UAVBoundary::global
                              : mode & 4 ? InstSync::UAVBoundary::group
                                         : InstSync::UAVBoundary::none,
              .tgsm_memory_barrier = (mode & 8) != 0,
              .tgsm_execution_barrier = (mode & 1) != 0,
          }
      );
      break;
    }
    case Discard: {
      ir.SetInsertPoint(SplitBlockAndInsertIfThen(a(1), call, false));
      dxbc(InstPixelDiscard{});
      break;
    }
    case DerivCoarseX:
    case DerivFineX:
    case DerivCoarseY:
    case DerivFineY:
      ret = ctx.shader_type == D3D11_SB_COMPUTE_SHADER
                ? quad_derivative(a(1), op == DerivCoarseY || op == DerivFineY, op == DerivFineX || op == DerivFineY)
                : air.CreateDerivative(a(1), op == DerivCoarseY || op == DerivFineY);
      break;
    case FAbs:
      ret = fp_unary(llvm::air::AIRBuilder::fabs, a(1));
      break;
    // a NaN saturates to 0, and FMax and FMin give the operand that is not one (DXIL.rst), which Metal's fast
    // variants leave undefined
    case Saturate:
      ret = air.CreateFPUnOp(llvm::air::AIRBuilder::saturate, a(1), false);
      break;
    case Cos:
      ret = dxbc.SinCos(llvm::air::AIRBuilder::cos, a(1));
      break;
    case Sin:
      ret = dxbc.SinCos(llvm::air::AIRBuilder::sin, a(1));
      break;
    case Exp:
      ret = fp_unary(llvm::air::AIRBuilder::exp2, a(1));
      break;
    case Log:
      ret = fp_unary(llvm::air::AIRBuilder::log2, a(1));
      break;
    case Frc:
      ret = fp_unary(llvm::air::AIRBuilder::fract, a(1));
      break;
    case Sqrt:
      ret = fp_unary(llvm::air::AIRBuilder::sqrt, a(1));
      break;
    case Rsqrt:
      ret = fp_unary(llvm::air::AIRBuilder::rsqrt, a(1));
      break;
    case Tan:
    case Acos:
    case Asin:
    case Atan:
    case Hcos:
    case Hsin:
    case Htan: {
      using llvm::air::AIRBuilder;
      const AIRBuilder::FPUnOp ops[] = {AIRBuilder::tan,  AIRBuilder::acos, AIRBuilder::asin, AIRBuilder::atan,
                                        AIRBuilder::cosh, AIRBuilder::sinh, AIRBuilder::tanh};
      // Metal's fast tanh overflows to NaN for large inputs, and its fast tan is NaN from about 2^30 radians on; the
      // precise ones saturate and stay finite as they must
      ret = air.CreateFPUnOp(ops[op - Tan], a(1), op != Htan && op != Tan);
      break;
    }
    case Round_ne:
      ret = fp_unary(llvm::air::AIRBuilder::rint, a(1));
      break;
    case Round_ni:
      ret = fp_unary(llvm::air::AIRBuilder::floor, a(1));
      break;
    case Round_pi:
      ret = fp_unary(llvm::air::AIRBuilder::ceil, a(1));
      break;
    case Round_z:
      ret = fp_unary(llvm::air::AIRBuilder::trunc, a(1));
      break;
    case IsNaN:
      ret = air.CreateIsNaN(a(1));
      break;
    case IsInf:
      ret = ir.CreateFCmpOEQ(fp_unary(llvm::air::AIRBuilder::fabs, a(1)), ConstantFP::getInfinity(a(1)->getType()));
      break;
    case IsFinite:
      ret = ir.CreateFCmpOLT(fp_unary(llvm::air::AIRBuilder::fabs, a(1)), ConstantFP::getInfinity(a(1)->getType()));
      break;
    case Bfrev:
      ret = air.CreateIntUnOp(llvm::air::AIRBuilder::reverse_bits, a(1));
      break;
    case Countbits:
      // DXIL counts into 32 bits, whatever the operand's width
      ret = ir.CreateZExtOrTrunc(air.CreateIntUnOp(llvm::air::AIRBuilder::popcount, a(1)), ty);
      break;
    case FMax:
      ret = air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, a(1), a(2), false);
      break;
    case FMin:
      ret = air.CreateFPBinOp(llvm::air::AIRBuilder::fmin, a(1), a(2), false);
      break;
    case IMax:
      ret = air.CreateIntBinOp(llvm::air::AIRBuilder::max, a(1), a(2), true);
      break;
    case IMin:
      ret = air.CreateIntBinOp(llvm::air::AIRBuilder::min, a(1), a(2), true);
      break;
    case UMax:
      ret = air.CreateIntBinOp(llvm::air::AIRBuilder::max, a(1), a(2));
      break;
    case UMin:
      ret = air.CreateIntBinOp(llvm::air::AIRBuilder::min, a(1), a(2));
      break;
    case FMad:
      ret = ir.CreateFAdd(ir.CreateFMul(a(1), a(2)), a(3));
      break;
    case Fma:
      ret = air.CreateFMA(a(1), a(2), a(3));
      break;
    case IMad:
    case UMad:
      ret = ir.CreateAdd(ir.CreateMul(a(1), a(2)), a(3));
      break;
    case Dot2:
    case Dot3:
    case Dot4: {
      auto n = op - Dot2 + 2;
      auto vty = FixedVectorType::get(a(1)->getType(), n);
      ret = air.CreateDotProduct(vec(call, 1, n, vty), vec(call, 1 + n, n, vty));
      break;
    }
    case Dot2AddHalf: {
      auto f = [&](unsigned i) { return ir.CreateFPExt(a(i), ir.getFloatTy()); };
      ret = air.CreateFMA(f(3), f(5), air.CreateFMA(f(2), f(4), a(1)));
      break;
    }
    case Dot4AddI8Packed:
    case Dot4AddU8Packed: {
      auto bytes = [&](Value *v) {
        auto b = ir.CreateBitCast(v, FixedVectorType::get(ir.getInt8Ty(), 4));
        auto wide = FixedVectorType::get(ir.getInt32Ty(), 4);
        return op == Dot4AddI8Packed ? ir.CreateSExt(b, wide) : ir.CreateZExt(b, wide);
      };
      // summed lane by lane: Metal's compiler service crashes on llvm.vector.reduce.add
      auto products = ir.CreateMul(bytes(a(2)), bytes(a(3)));
      ret = a(1);
      for (unsigned i = 0; i < 4; i++)
        ret = ir.CreateAdd(ret, ir.CreateExtractElement(products, i));
      break;
    }
    case Pack4x8: {
      // DXIL's PackMode: truncate, clamp to u8, clamp to s8
      auto mode = constant(a(1));
      auto vty = a(2)->getType();
      auto clamp = [&](Value *v, int64_t low, int64_t high) {
        auto lo = ConstantInt::get(vty, low), hi = ConstantInt::get(vty, high);
        return ir.CreateSelect(ir.CreateICmpSLT(v, lo), lo, ir.CreateSelect(ir.CreateICmpSGT(v, hi), hi, v));
      };
      Value *packed = ir.getInt32(0);
      for (unsigned i = 0; i < 4; i++) {
        auto v = mode == 1 ? clamp(a(2 + i), 0, 255) : mode == 2 ? clamp(a(2 + i), -128, 127) : a(2 + i);
        auto byte = ir.CreateZExt(ir.CreateTrunc(v, ir.getInt8Ty()), ir.getInt32Ty());
        packed = ir.CreateOr(packed, ir.CreateShl(byte, 8 * i));
      }
      ret = packed;
      break;
    }
    case Unpack4x8: {
      // DXIL's UnpackMode: unsigned, signed
      auto elem = cast<StructType>(ty)->getElementType(0);
      ret = UndefValue::get(ty);
      for (unsigned i = 0; i < 4; i++) {
        auto byte = ir.CreateTrunc(ir.CreateLShr(a(2), 8 * i), ir.getInt8Ty());
        ret = ir.CreateInsertValue(ret, constant(a(1)) ? ir.CreateSExt(byte, elem) : ir.CreateZExt(byte, elem), {i});
      }
      break;
    }
    case ViewID:
      // pipelines have one view: view instancing is unsupported
      ret = ir.getInt32(0);
      break;
    case IsHelperLane:
      ret =
          ctx.shader_type == D3D10_SB_PIXEL_SHADER ? simd("simd_is_helper_thread", ir.getInt1Ty(), {}) : ir.getFalse();
      break;
    case CheckAccessFullyMapped:
      ret = ir.CreateICmpNE(a(1), ir.getInt32(0));
      break;
    case Texture2DMSGetSamplePosition:
    case RenderTargetGetSamplePosition:
    case RenderTargetGetSampleCount: {
      // a texture's samples, or the render target's
      bool texture = op == Texture2DMSGetSamplePosition;
      Value *samples;
      if (texture) {
        auto &t = *handles[a(1)].texture;
        samples = air.CreateTextureQuery(t.Texture, t.Handle, Texture::num_samples, ir.getInt32(0));
      } else {
        samples = air.CreateGetNumSamples();
      }
      if (op == RenderTargetGetSampleCount) {
        ret = samples;
        break;
      }
      auto pos = air.CreateSamplePos(samples, a(texture ? 2 : 1));
      ret = UndefValue::get(ty);
      for (unsigned i = 0; i < 2; i++)
        ret = ir.CreateInsertValue(ret, ir.CreateExtractElement(pos, i), {i});
      break;
    }
    case HitInstanceID:
      ret = ray_value(RayInstanceID);
      break;
    case HitInstanceIndex:
      ret = ray_value(RayInstanceIndex);
      break;
    case HitGeometryIndex:
      ret = ray_value(RayGeometryIndex);
      break;
    case HitPrimitiveIndex:
      ret = ray_value(RayPrimitiveIndex);
      break;
    case HitKind:
      ret = ray_value(RayHitKind);
      break;
    case RayFlags:
      ret = ray_value(RayFlagsField);
      break;
    case RayTMin:
      ret = ray_value(RayTMinField);
      break;
    case RayTCurrent:
      ret = ray_value(RayTCurrentField);
      break;
    case DispatchRaysIndex:
      ret = ray_value(RayIndex, a(1));
      break;
    case DispatchRaysDimensions:
      ret = ir.CreateLoad(
          ir.getInt32Ty(),
          ir.CreateGEP(
              ir.getInt32Ty(), ray_value(RayDispatch),
              ir.CreateAdd(
                  ir.CreateZExt(a(1), ir.getInt32Ty()),
                  ir.getInt32(offsetof(SM50_RAY_DISPATCH, dimensions) / sizeof(uint32_t))
              )
          )
      );
      break;
    case WorldRayOrigin:
      ret = ray_value(RayWorldOrigin, a(1));
      break;
    case WorldRayDirection:
      ret = ray_value(RayWorldDirection, a(1));
      break;
    case ObjectRayOrigin:
      ret = ray_value(RayObjectOrigin, a(1));
      break;
    case ObjectRayDirection:
      ret = ray_value(RayObjectDirection, a(1));
      break;
    case ObjectToWorld:
    case WorldToObject:
      // row-major 3x4: a row, then a column
      ret = ray_value(
          op == ObjectToWorld ? RayObjectToWorld : RayWorldToObject,
          ir.CreateAdd(ir.CreateMul(a(1), ir.getInt32(4)), ir.CreateZExt(a(2), ir.getInt32Ty()))
      );
      break;
    case IgnoreHit:
      ray_verdict(call, SM50_RAY_VERDICT_IGNORE);
      break;
    case AcceptHitAndEndSearch:
      ray_verdict(call, SM50_RAY_VERDICT_END_SEARCH);
      break;
    case ReportHit: {
      auto attributes = cast<PointerType>(a(3)->getType())->getNonOpaquePointerElementType();
      ir.CreateStore(a(1), ray_field(RayReportedT));
      ir.CreateStore(a(2), ray_field(RayReportedKind));
      ir.CreateStore(
          ir.getInt32(ctx.module.getDataLayout().getTypeStoreSize(attributes).getFixedSize()),
          ray_field(RayReportedSize)
      );
      ray_call(SM50_RAY_FUNCTION_REPORT_HIT, a(3), nullptr);
      ret = ir.CreateICmpNE(ray_value(RayAccepted), ir.getInt32(0));
      // a hit that ends the search stops the shader that reported it (DXR spec, "Intersection shaders - procedural
      // primitive geometry")
      auto stop = SplitBlockAndInsertIfThen(ir.CreateICmpNE(ray_value(RayEnded), ir.getInt32(0)), call, true);
      BranchInst::Create(epilogue, stop);
      stop->eraseFromParent();
      ir.SetInsertPoint(call);
      break;
    }
    case CallShader:
      ir.CreateStore(a(1), ray_field(RayCallable));
      ray_call(SM50_RAY_FUNCTION_CALL, a(2), nullptr);
      break;
    case TraceRay: {
      // SM50_RAY_TRACE, in the order of the operation's arguments, then the payload
      Type *f32 = ir.getFloatTy(), *i32 = ir.getInt32Ty();
      auto vector = ArrayType::get(f32, 3);
      auto ty = StructType::get(ir.getInt64Ty(), i32, i32, i32, i32, i32, vector, f32, vector, f32);
      auto trace = IRBuilder<>(&*ctx.function->getEntryBlock().getFirstInsertionPt()).CreateAlloca(ty);
      auto store = [&](Value *v, unsigned field, unsigned element = 0) {
        ir.CreateStore(v, ir.CreateInBoundsGEP(
                              ty, trace,
                              field == 6 || field == 8
                                  ? ArrayRef<Value *>{ir.getInt32(0), ir.getInt32(field), ir.getInt32(element)}
                                  : ArrayRef<Value *>{ir.getInt32(0), ir.getInt32(field)}
                          ));
      };
      store(ir.CreatePtrToInt(handles[a(1)].buffer->Pointer, ir.getInt64Ty()), 0);
      for (unsigned i = 0; i < 5; i++)
        store(a(2 + i), 1 + i);
      for (unsigned i = 0; i < 3; i++) {
        store(a(7 + i), 6, i);
        store(a(11 + i), 8, i);
      }
      store(a(10), 7);
      store(a(14), 9);
      ray_call(SM50_RAY_FUNCTION_TRACE, trace, a(15));
      break;
    }
    case AllocateRayQuery:
    case AllocateRayQuery2:
      break;
    case RayQuery_TraceRayInline: {
      // the structure's memory starts with its header: the Metal structure's handle, then its instances' address. a
      // null structure, at address 0, misses every ray (DXR spec, "Additional SRV type"): it is the structure of
      // nothing, whose header is before the lock table
      auto i64 = ir.getInt64Ty();
      auto structure_ty = opaque("struct._instance_acceleration_structure_t")->getPointerTo(1);
      auto address = ir.CreatePtrToInt(handles[a(2)].buffer->Pointer, i64);
      auto nothing = ir.CreateSub(
          ir.CreatePtrToInt(ctx.binding.GetAtomicLocks(air), i64), ir.getInt64(SM50_NULL_ACCELERATION_STRUCTURE_HEADER_SIZE)
      );
      auto header = ir.CreateIntToPtr(
          ir.CreateSelect(ir.CreateICmpEQ(address, ir.getInt64(0)), nothing, address), i64->getPointerTo(1)
      );
      auto structure = ir.CreateLoad(structure_ty, ir.CreateBitCast(header, structure_ty->getPointerTo(1)));
      ir.CreateStore(ir.CreateLoad(i64, ir.CreateConstGEP1_32(i64, header, 1)), ray_query(a(1), &RayQuery::instances));
      auto flags = ir.CreateOr(a(3), ray_query(a(1), &RayQuery::constant_flags));
      ir.CreateStore(flags, ray_query(a(1), &RayQuery::flags));
      // RAY_FLAG's pairs are Metal's modes: 1 for the first flag, 2 for the second
      auto pair = [&](unsigned first) { return ir.CreateAnd(ir.CreateLShr(flags, std::countr_zero(first)), 3); };
      auto vector = [&](unsigned first) {
        Value *v = UndefValue::get(FixedVectorType::get(ir.getFloatTy(), 3));
        for (unsigned i = 0; i < 3; i++)
          v = ir.CreateInsertElement(v, a(first + i), i);
        return v;
      };
      // front faces wind clockwise for both; RAY_FLAG_CULL_BACK_FACING_TRIANGLES comes before the front flag,
      // Metal's front mode before its back mode
      auto cull = pair(0x10);
      ray_query_call(
          "reset", ir.getVoidTy(),
          {ray_query(a(1), &RayQuery::query), vector(5), vector(9), a(8), a(12), structure,
           ir.CreateAnd(a(4), 0xff), ir.getInt32(0), ir.CreateOr(ir.CreateLShr(cull, 1), ir.CreateAnd(ir.CreateShl(cull, 1), 2)),
           pair(0x100), pair(0x40), pair(0x1),
           // both geometry types may be met, in any structure and by any function
           ir.getInt32(3), ir.getInt32(-1), ir.getInt32(-1), ir.getInt32(0), ir.getFalse(),
           ir.CreateICmpNE(ir.CreateAnd(flags, 0x4), ir.getInt32(0))}
      );
      break;
    }
    case RayQuery_Proceed:
      ret = ray_query_call("next", ir.getInt1Ty(), {ray_query(a(1), &RayQuery::query)});
      break;
    case RayQuery_Abort:
      ray_query_call("abort", ir.getVoidTy(), {ray_query(a(1), &RayQuery::query)});
      break;
    case RayQuery_CommitNonOpaqueTriangleHit:
      ray_query_call("commit_triangle_intersection", ir.getVoidTy(), {ray_query(a(1), &RayQuery::query)});
      break;
    case RayQuery_CommitProceduralPrimitiveHit:
      ray_query_call("commit_bounding_box_intersection", ir.getVoidTy(), {ray_query(a(1), &RayQuery::query), a(2)});
      break;
    case RayQuery_CandidateType:
      // CANDIDATE_TYPE numbers a triangle 0 and a procedural primitive 1, Metal 1 and 2
      ret = ir.CreateSub(
          ray_query_call("get_candidate_intersection_type", ir.getInt32Ty(), {ray_query(a(1), &RayQuery::query)}),
          ir.getInt32(1)
      );
      break;
    case RayQuery_RayFlags:
      ret = ir.CreateLoad(ir.getInt32Ty(), ray_query(a(1), &RayQuery::flags));
      break;
    case RayQuery_CandidateInstanceContributionToHitGroupIndex:
    case RayQuery_CommittedInstanceContributionToHitGroupIndex: {
      // the instance's intersection function table offset, in MTLIndirectAccelerationStructureInstanceDescriptor
      constexpr uint64_t instance_size = 72, table_offset = 56;
      auto index = ray_query_call(
          op == RayQuery_CandidateInstanceContributionToHitGroupIndex ? "get_candidate_instance_id"
                                                                      : "get_committed_instance_id",
          ir.getInt32Ty(), {ray_query(a(1), &RayQuery::query)}
      );
      auto address = ir.CreateAdd(
          ir.CreateLoad(ir.getInt64Ty(), ray_query(a(1), &RayQuery::instances)),
          ir.CreateAdd(ir.CreateMul(ir.CreateZExt(index, ir.getInt64Ty()), ir.getInt64(instance_size)), ir.getInt64(table_offset))
      );
      ret = ir.CreateLoad(ir.getInt32Ty(), ir.CreateIntToPtr(address, ir.getInt32Ty()->getPointerTo(1)));
      break;
    }
    default: {
      auto getter = std::find_if(std::begin(ray_query_getters), std::end(ray_query_getters), [&](auto &g) {
        return g.op == op;
      });
      if (getter == std::end(ray_query_getters))
        return make_error<UnsupportedFeature>(
            std::format("DXIL operation {} ({}) is not lowered yet", op, call->getCalledFunction()->getName().str())
        );
      auto query = ray_query(a(1), &RayQuery::query);
      auto vector = getter->width ? FixedVectorType::get(ir.getFloatTy(), getter->width) : nullptr;
      if (getter->matrix) {
        auto columns = ray_query_call(getter->name, StructType::get(vector, vector, vector, vector), {query});
        Value *column = ir.CreateExtractValue(columns, 0);
        for (unsigned i = 1; i < 4; i++)
          column = ir.CreateSelect(
              ir.CreateICmpEQ(a(3), ConstantInt::get(a(3)->getType(), i)), ir.CreateExtractValue(columns, i), column
          );
        ret = ir.CreateExtractElement(column, a(2));
      } else if (vector) {
        ret = ir.CreateExtractElement(ray_query_call(getter->name, vector, {query}), a(2));
      } else {
        ret = ray_query_call(getter->name, ty, {query});
      }
      break;
    }
    }
    if (ret)
      call->replaceAllUsesWith(ret);
    call->eraseFromParent();
  }
  if (staged_position || staged_distance) {
    // every path of the function ends here first
    auto exported = llvm::BasicBlock::Create(ctx.llvm, "export_mesh", ctx.function, epilogue);
    epilogue->replaceAllUsesWith(exported);
    ir.SetInsertPoint(exported);
    air.CreateBarrier(llvm::air::MemFlags::Threadgroup);
    auto &n = shader->threadgroup_size;
    // each thread takes the vertices or primitives a group's size apart from its own index, of the counts the
    // shader set and never more than it declares
    auto own = [&](unsigned counted, uint32_t limit, auto &&each) {
      auto count = air.CreateIntBinOp(
          llvm::air::AIRBuilder::min, ir.CreateLoad(ir.getInt32Ty(), mesh_counts[counted]), ir.getInt32(limit)
      );
      auto from = ir.GetInsertBlock();
      auto head = llvm::BasicBlock::Create(ctx.llvm, "", ctx.function, epilogue);
      auto body = llvm::BasicBlock::Create(ctx.llvm, "", ctx.function, epilogue);
      auto done = llvm::BasicBlock::Create(ctx.llvm, "", ctx.function, epilogue);
      ir.CreateBr(head);
      ir.SetInsertPoint(head);
      auto i = ir.CreatePHI(ir.getInt32Ty(), 2);
      i->addIncoming(ctx.resource.thread_id_in_group_flat_arg, from);
      ir.CreateCondBr(ir.CreateICmpULT(i, count), body, done);
      ir.SetInsertPoint(body);
      each(i);
      i->addIncoming(ir.CreateAdd(i, ir.getInt32(n[0] * n[1] * n[2])), ir.GetInsertBlock());
      ir.CreateBr(head);
      ir.SetInsertPoint(done);
    };
    if (staged_position)
      own(0, shader->mesh_max_vertices, [&](Value *vertex) {
        Value *value = UndefValue::get(FixedVectorType::get(ir.getFloatTy(), 4));
        for (unsigned c = 0; c < 4; c++)
          value = ir.CreateInsertElement(
              value, ir.CreateLoad(ir.getFloatTy(), element_of(staged_position, {vertex, ir.getInt32(c)})), c
          );
        air.CreateSetMeshPosition(vertex, value);
      });
    if (staged_distance)
      own(1, shader->mesh_max_primitives, [&](Value *primitive) {
        // a primitive is culled when one distance puts all its vertices out, or when the shader culls it itself
        Value *culled = culls ? (Value *)ir.CreateLoad(i8, element_of(staged_culled, {primitive})) : ir.getInt8(0);
        for (uint32_t distance = 0; distance < cull_distances; distance++) {
          Value *out = ir.getInt8(1);
          for (uint32_t corner = 0; corner < corners; corner++) {
            auto vertex = ir.CreateZExt(
                ir.CreateLoad(i8, element_of(staged_index, {primitive, ir.getInt32(corner)})), ir.getInt32Ty()
            );
            out = ir.CreateAnd(out, ir.CreateLoad(i8, element_of(staged_distance, {vertex, ir.getInt32(distance)})));
          }
          culled = ir.CreateOr(culled, out);
        }
        air.CreateSetMeshPrimitiveCulled(primitive, ir.CreateICmpNE(culled, ir.getInt8(0)));
      });
    ir.CreateBr(epilogue);
  }
  // groupshared 64-bit atomics, which DXIL has as LLVM's own atomic instructions
  SmallVector<llvm::Instruction *> wide;
  for (auto &BB : *ctx.function)
    for (auto &I : BB)
      if (auto rmw = dyn_cast<AtomicRMWInst>(&I); rmw && rmw->getType()->isIntegerTy(64))
        wide.push_back(rmw);
      else if (auto cx = dyn_cast<AtomicCmpXchgInst>(&I); cx && cx->getCompareOperand()->getType()->isIntegerTy(64))
        wide.push_back(cx);
  for (auto I : wide) {
    ir.SetInsertPoint(I);
    auto locks = ctx.binding.GetAtomicLocks(air);
    if (!locks)
      return make_error<UnsupportedFeature>("64-bit groupshared atomic without a lock table");
    Value *ret;
    if (auto rmw = dyn_cast<AtomicRMWInst>(I)) {
      ret = locked(word_at(rmw->getPointerOperand()), locks, [&](Value *old) {
        return buildAtomicRMWValue(rmw->getOperation(), ir, old, rmw->getValOperand());
      });
    } else {
      auto cx = cast<AtomicCmpXchgInst>(I);
      auto old = locked(word_at(cx->getPointerOperand()), locks, [&](Value *old) {
        return ir.CreateSelect(ir.CreateICmpEQ(old, cx->getCompareOperand()), cx->getNewValOperand(), old);
      });
      ret = ir.CreateInsertValue(
          ir.CreateInsertValue(UndefValue::get(cx->getType()), old, {0}), ir.CreateICmpEQ(old, cx->getCompareOperand()), {1}
      );
    }
    I->replaceAllUsesWith(ret);
    I->eraseFromParent();
  }
  for (auto &[value, _] : handles)
    cast<llvm::Instruction>(value)->eraseFromParent();
  for (auto &F : make_early_inc_range(ctx.module))
    if (F.getName().startswith("dx.op.") && F.use_empty())
      F.eraseFromParent();
  // the caller continues in the entry block
  ir.SetInsertPoint(&entry_bb);
  return body;
}

// a mesh or amplification shader as a Metal mesh or object function. the DXIL function runs as a compute shader's
// does, its thread IDs made from the stage's; a mesh shader's outputs are the Metal mesh's, named as the pixel shader
// names its inputs of the same semantics
llvm::Error
convert_dxil_mesh_stage(
    SM50ShaderInternal *pShaderInternal, const char *name, llvm::LLVMContext &context, llvm::Module &module,
    SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  auto func_signature = pShaderInternal->func_signature; // copy
  auto shader_info = &(pShaderInternal->shader_info);
  bool mesh = pShaderInternal->shader_type == D3D12_SB_MESH_SHADER;

  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_FLAG shader_flags = {};
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common)) {
    metal_version = sm50_common->metal_version;
    shader_flags = sm50_common->flags;
  }
  SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig = nullptr;
  args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &rootsig);
  SM50_SHADER_MESH_SHADER_DATA *link = nullptr;
  args_get_data<SM50_SHADER_MESH_SHADER, SM50_SHADER_MESH_SHADER_DATA>(pArgs, &link);
  CSignatureParser pixel_inputs;
  const D3D11_SIGNATURE_PARAMETER *pixel_params = nullptr;
  uint32_t pixel_count = 0;
  if (link && link->pixel_shader_bytecode && SUCCEEDED(DXBCGetInputSignature(link->pixel_shader_bytecode, &pixel_inputs)))
    pixel_count = pixel_inputs.GetParameters(&pixel_params);

  io_binding_map resource_map;
  air::AirType types(context);

  uint32_t thread_idx = func_signature.DefineInput(air::InputThreadPositionInThreadgroup{});
  uint32_t group_idx = func_signature.DefineInput(air::InputThreadgroupPositionInGrid{});
  // a Metal payload has a size; a mesh shader without one still takes what its object stage, if any, sent
  uint32_t payload_idx = func_signature.DefineInput(air::InputPayload{.size = std::max(pShaderInternal->payload_size, 4u)});
  auto &threads = pShaderInternal->threadgroup_size;
  if (mesh) {
    func_signature.DefineInput(air::InputMesh{
        pShaderInternal->mesh_max_vertices, pShaderInternal->mesh_max_primitives,
        pShaderInternal->mesh_lines ? air::MeshOutputTopology::Line : air::MeshOutputTopology::Triangle
    });
    func_signature.UseMaxMeshWorkgroupSize(threads[0] * threads[1] * threads[2]);
    uint32_t clip_distances = 0, cull_distances = 0;
    for (bool primitive : {false, true}) {
      uint32_t attributes = 0;
      for (auto &e : pShaderInternal->mesh_outputs[primitive]) {
        uint32_t data = 0;
        switch (e.semantic) {
        case SemanticPosition:
          func_signature.DefineMeshVertexOutput(air::OutputPosition{.type = air::msl_float4});
          break;
        case SemanticClipDistance:
          data = clip_distances;
          clip_distances += e.rows * e.cols;
          break;
        case SemanticCullDistance:
          data = cull_distances;
          cull_distances += e.rows * e.cols;
          break;
        case SemanticPrimitiveID:
          func_signature.DefineMeshPrimitiveOutput(air::OutputPrimitiveID{});
          break;
        case SemanticRenderTargetArrayIndex:
          func_signature.DefineMeshPrimitiveOutput(air::OutputRenderTargetArrayIndex{});
          break;
        case SemanticViewportArrayIndex:
          func_signature.DefineMeshPrimitiveOutput(air::OutputViewportArrayIndex{});
          break;
        case SemanticCullPrimitive:
          func_signature.DefineMeshPrimitiveOutput(air::OutputPrimitiveCulled{});
          break;
        default:
          break;
        }
        resource_map.mesh_data[primitive].push_back(data);
        // the pixel shader finds its inputs by semantic: each component it takes of an element's row as a varying is
        // one from here, at the register and component it has it
        auto &varyings = resource_map.mesh_varyings[primitive].emplace_back(e.rows * e.cols, ~0u);
        for (uint32_t row = 0; row < e.rows; row++)
          for (uint32_t i = 0; i < pixel_count; i++) {
            auto &p = pixel_params[i];
            bool varying = p.SystemValue == D3D10_SB_NAME_UNDEFINED || p.SystemValue == D3D10_SB_NAME_CLIP_DISTANCE ||
                           p.SystemValue == D3D10_SB_NAME_CULL_DISTANCE;
            if (!varying || p.SemanticIndex != e.index + row || strcasecmp(p.SemanticName, e.name.c_str()))
              continue;
            uint32_t first = std::countr_zero(uint32_t(p.Mask));
            for (uint32_t c = 0; c < e.cols && (p.Mask >> (first + c) & 1); c++) {
              air::OutputMeshData attribute{varying_name(p.Register, first + c), varying_type(e.type), attributes};
              if (primitive)
                func_signature.DefineMeshPrimitiveOutput(attribute);
              else
                func_signature.DefineMeshVertexOutput(attribute);
              varyings[row * e.cols + c] = attributes++;
            }
          }
      }
    }
    if (clip_distances)
      func_signature.DefineMeshVertexOutput(air::OutputClipDistance{.count = clip_distances});
    // Metal culls by primitive: the group derives it from the vertices' cull distances
    if (cull_distances)
      func_signature.DefineMeshPrimitiveOutput(air::OutputPrimitiveCulled{});
  } else {
    func_signature.DefineInput(air::InputMeshGridProperties{});
    func_signature.UseMaxWorkgroupSize(threads[0] * threads[1] * threads[2]);
  }

  auto binding_map = rootsig ? setup_binding_rootsig(
                                   shader_info, func_signature, module, pShaderInternal->shader_type, rootsig->bytecode,
                                   rootsig->bytecode_length
                               )
                             : setup_binding_table2(shader_info, func_signature, module);
  setup_tgsm(shader_info, resource_map, types, module);

  auto [function, function_metadata] = func_signature.CreateFunction(name, context, module, 0, true);

  auto entry_bb = llvm::BasicBlock::Create(context, "entry", function);
  auto epilogue_bb = llvm::BasicBlock::Create(context, "epilogue", function);
  llvm::IRBuilder<> builder(entry_bb);
  llvm::raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(
      {
          .sampleNaNToZero = bool(shader_flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO),
          .defuseFma = bool(shader_flags & SM50_SHADER_FLAG_DEFUSE_FMA),
      },
      builder, nulldbg
  );

  setup_metal_version(module, metal_version);
  setup_temp_register(shader_info, resource_map, types, module, builder);
  setup_immediate_constant_buffer(shader_info, resource_map, types, module, builder);

  // the thread's IDs: in its group, the group's, in the dispatch (the group's times the group's size, plus its own),
  // and flattened in its group
  auto thread = function->getArg(thread_idx), group = function->getArg(group_idx);
  auto size = llvm::ConstantDataVector::get(context, llvm::ArrayRef<uint32_t>(threads, 3));
  auto at = [&](unsigned i) { return builder.CreateExtractElement(thread, i); };
  resource_map.thread_id_in_group_arg = thread;
  resource_map.thread_group_id_arg = group;
  resource_map.thread_id_arg = builder.CreateAdd(builder.CreateMul(group, size), thread);
  resource_map.thread_id_in_group_flat_arg = builder.CreateAdd(
      builder.CreateMul(builder.CreateAdd(builder.CreateMul(at(2), builder.getInt32(threads[1])), at(1)), builder.getInt32(threads[0])),
      at(0)
  );
  resource_map.payload = function->getArg(payload_idx);

  struct context ctx{
      .builder = builder,
      .air = air,
      .binding = *binding_map,
      .llvm = context,
      .module = module,
      .function = function,
      .resource = resource_map,
      .types = types,
      .pso_sample_mask = 0xffffffff,
      .shader_type = pShaderInternal->shader_type,
      .metal_version = metal_version,
      .simd_width = sm50_common ? sm50_common->simd_width : 0,
  };

  auto real_entry = convert_basicblocks(pShaderInternal->entry(), ctx, epilogue_bb);
  if (auto err = real_entry.takeError())
    return err;
  builder.CreateBr(real_entry.get());
  builder.SetInsertPoint(epilogue_bb);
  builder.CreateRetVoid();

  module.getOrInsertNamedMetadata(mesh ? "air.mesh" : "air.object")->addOperand(function_metadata);
  return llvm::Error::success();
}

// a ray tracing shader of a library, as a visible function of a ray tracing pipeline: it takes its context, then its
// payload (or a callable shader's parameter) and a hit's attributes where its kind has them
llvm::Error
convert_dxil_ray_shader(
    SM50ShaderInternal *pShaderInternal, const char *name, llvm::LLVMContext &context, llvm::Module &module,
    SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs
) {
  ShaderInfo *shader_info = &(pShaderInternal->shader_info);
  SM50_SHADER_METAL_VERSION metal_version = SM50_SHADER_METAL_310;
  SM50_SHADER_FLAG shader_flags = {};
  SM50_SHADER_COMMON_DATA *sm50_common = nullptr;
  if (args_get_data<SM50_SHADER_COMMON, SM50_SHADER_COMMON_DATA>(pArgs, &sm50_common)) {
    metal_version = sm50_common->metal_version;
    shader_flags = sm50_common->flags;
  }
  // the shader's module, with the functions it calls of its state object's other libraries, and then the resources
  // of them all
  auto linked = parse(pShaderInternal->dxil, context);
  if (!linked)
    return linked.takeError();
  ShaderInfo linked_info;
  SM50_SHADER_LIBRARIES_DATA *libraries = nullptr;
  if (args_get_data<SM50_SHADER_LIBRARIES, SM50_SHADER_LIBRARIES_DATA>(pArgs, &libraries)) {
    bool any = false;
    std::vector<SM50ShaderInternal *> others;
    for (uint32_t i = 0; i < libraries->count; i++)
      if (auto other = (SM50ShaderInternal *)libraries->libraries[i]; other != pShaderInternal)
        others.push_back(other);
    if (auto err = link_libraries(**linked, others, any))
      return err;
    if (any) {
      read_resources(**linked, linked_info);
      shader_info = &linked_info;
    }
  }
  SM50_SHADER_ROOT_SIGNATURE_DATA *global_rootsig = nullptr, *local_rootsig = nullptr;
  args_get_data<SM50_SHADER_ROOT_SIGNATURE, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &global_rootsig);
  args_get_data<SM50_SHADER_ROOT_SIGNATURE2, SM50_SHADER_ROOT_SIGNATURE_DATA>(pArgs, &local_rootsig);
  SM50_SHADER_RAY_SHADER_DATA *ray = nullptr;
  args_get_data<SM50_SHADER_RAY_SHADER, SM50_SHADER_RAY_SHADER_DATA>(pArgs, &ray);
  if (!ray || !ray->name)
    return make_error<UnsupportedFeature>("a library compiles one ray tracing shader at a time, and none is named");

  // the root signatures' argument blocks are not arguments of this function: bind_rootsig_arguments says where
  // they are. a root parameter is visible to these shaders only as D3D12_SHADER_VISIBILITY_ALL, which the
  // library's kind makes of it
  air::FunctionSignatureBuilder unused;
  auto binding = [&](SM50_SHADER_ROOT_SIGNATURE_DATA *rootsig, bool local) {
    return rootsig ? setup_binding_rootsig(
                         shader_info, unused, module, pShaderInternal->shader_type, rootsig->bytecode,
                         rootsig->bytecode_length, SM50_BINDING_INDEX_ROOT_ARGUMENTS,
                         SM50_BINDING_INDEX_STATIC_SAMPLERS, local
                     )
                   : nullptr;
  };
  auto global = binding(global_rootsig, false), local = binding(local_rootsig, true);

  io_binding_map resource_map;
  air::AirType types(context);
  auto context_ty = ray_context_type(context);
  auto i8p = llvm::Type::getInt8PtrTy(context);
  auto function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {context_ty->getPointerTo(), i8p, i8p}, false),
      llvm::GlobalValue::ExternalLinkage, name, module
  );
  auto entry_bb = llvm::BasicBlock::Create(context, "entry", function);
  auto epilogue_bb = llvm::BasicBlock::Create(context, "epilogue", function);
  llvm::IRBuilder<> builder(entry_bb);
  llvm::raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(
      {
          .sampleNaNToZero = bool(shader_flags & SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO),
          .defuseFma = bool(shader_flags & SM50_SHADER_FLAG_DEFUSE_FMA),
      },
      builder, nulldbg
  );
  setup_metal_version(module, metal_version);
  setup_temp_register(shader_info, resource_map, types, module, builder);
  setup_immediate_constant_buffer(shader_info, resource_map, types, module, builder);

  auto ray_context = function->getArg(0);
  auto field = [&](unsigned i) {
    return builder.CreateLoad(context_ty->getElementType(i), builder.CreateStructGEP(context_ty, ray_context, i));
  };
  if (global)
    bind_rootsig_arguments(*global, builder, field(RayRootArguments), field(RayStaticSamplers));
  if (local)
    bind_rootsig_arguments(
        *local, builder,
        builder.CreateIntToPtr(field(RayLocalArguments), llvm::Type::getInt64Ty(context)->getPointerTo(1)),
        local_rootsig->static_samplers ? builder.CreateIntToPtr(
                                             builder.getInt64(local_rootsig->static_samplers),
                                             context_ty->getElementType(RayStaticSamplers)
                                         )
                                       : nullptr
    );
  auto binding_map = combine_bindings(std::move(local), std::move(global));
  resource_map.ray_context = ray_context;
  resource_map.ray_arguments[0] = function->getArg(1);
  resource_map.ray_arguments[1] = function->getArg(2);

  struct context ctx{
      .builder = builder,
      .air = air,
      .binding = *binding_map,
      .llvm = context,
      .module = module,
      .function = function,
      .resource = resource_map,
      .types = types,
      .pso_sample_mask = 0xffffffff,
      .shader_type = pShaderInternal->shader_type,
      .metal_version = metal_version,
      .simd_width = sm50_common ? sm50_common->simd_width : 0,
  };
  auto body = convert_dxil(pShaderInternal, ctx, ray->name, epilogue_bb, std::move(*linked), shader_info);
  if (auto err = body.takeError())
    return err;
  builder.SetInsertPoint(entry_bb);
  builder.CreateBr(*body);
  builder.SetInsertPoint(epilogue_bb);
  builder.CreateRetVoid();

  auto md = [&](std::initializer_list<llvm::Metadata *> nodes) { return llvm::MDNode::get(context, nodes); };
  auto argument = [&](unsigned i, const char *type) {
    return md({llvm::ConstantAsMetadata::get(builder.getInt32(i)), llvm::MDString::get(context, "air.visible_input"),
               llvm::MDString::get(context, "air.arg_type_name"), llvm::MDString::get(context, type)});
  };
  module.getOrInsertNamedMetadata("air.visible")
      ->addOperand(md({llvm::ValueAsMetadata::get(function), md({}),
                       md({argument(0, "DXMTRayContext"), argument(1, "void"), argument(2, "void")})}));
  return llvm::Error::success();
}

} // namespace dxmt::dxbc
