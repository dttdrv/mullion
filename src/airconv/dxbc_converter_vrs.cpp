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

// variable rate shading, tier 2 (D3D12 Variable Rate Shading): Metal shades every pixel, so the pixels of a coarse
// pixel shade together. a coarse pixel two wide or high is half or all of a quad (its lanes pixels, (y & 1) * 2 +
// (x & 1)), and a quad's pixels are one primitive's and in one tile of the shading rate image: they share a rate

#include "dxbc_converter.hpp"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

namespace mullion::dxbc {

CoarseShading::CoarseShading(air::FunctionSignatureBuilder &signature, uint32_t varying) {
  // the inputs are where the coarse pixel's center is, whatever the shader's interpolation: a centroid is a center
  signature.CenterCentroids();
  auto &all = signature.Inputs();
  for (uint32_t index = 0; index < all.size(); index++) {
    auto &input = all[index];
    if (auto stage_in = std::get_if<air::InputFragmentStageIn>(&input);
        stage_in && !stage_in->pull_mode && stage_in->interpolation != air::Interpolation::flat)
      inputs_.push_back({index, stage_in->interpolation == air::Interpolation::center_perspective});
    if (std::holds_alternative<air::InputInputCoverage>(input))
      coverage_ = index;
  }
  position_ = signature.DefineInput(air::InputPosition{.interpolation = air::Interpolation::center_no_perspective});
  if (varying)
    varying_ = signature.DefineInput(
        air::InputFragmentStageIn{
            .user = varying_name((varying - 1) / 4, (varying - 1) % 4),
            .type = air::msl_uint,
            .interpolation = air::Interpolation::flat,
            .pull_mode = false
        }
    );
  rates_ = signature.DefineInput(
      air::ArgumentBindingBuffer{
          .buffer_size = sizeof(SM50_SHADING_RATE),
          .location_index = SM50_BINDING_INDEX_SHADING_RATE,
          .array_size = 0,
          .memory_access = air::MemoryAccess::read,
          .address_space = air::AddressSpace::constant,
          .type = air::msl_uint,
          .arg_name = "shading_rate",
          .raster_order_group = {}
      }
  );
  image_ = signature.DefineInput(
      air::ArgumentBindingBuffer{
          .buffer_size = {},
          .location_index = SM50_BINDING_INDEX_SHADING_RATE_IMAGE,
          .array_size = 0,
          .memory_access = air::MemoryAccess::read,
          .address_space = air::AddressSpace::device,
          .type = air::msl_uint,
          .arg_name = "shading_rate_image",
          .raster_order_group = {}
      }
  );
}

// a quad lane's value of a 32-bit or boolean scalar: `lane`, or this lane's xor `lane`
static Value *
quad_shuffle(IRBuilder<> &ir, llvm::air::AIRBuilder &air, Value *v, Value *lane, bool xor_lane = false) {
  if (v->getType()->isIntegerTy(1))
    return ir.CreateTrunc(quad_shuffle(ir, air, ir.CreateZExt(v, ir.getInt32Ty()), lane, xor_lane), ir.getInt1Ty());
  auto &context = ir.getContext();
  auto convergent = AttributeList::get(
      context, {{~0U, Attribute::get(context, Attribute::Convergent)},
                {~0U, Attribute::get(context, Attribute::NoUnwind)},
                {~0U, Attribute::get(context, Attribute::WillReturn)}}
  );
  auto ty = v->getType();
  auto name = std::string(xor_lane ? "air.quad_shuffle_xor" : "air.quad_shuffle") +
              air.getTypeOverloadSuffix(
                  ty, ty->isIntegerTy() ? llvm::air::Signedness::Unsigned : llvm::air::Signedness::DontCare
              );
  auto module = ir.GetInsertBlock()->getModule();
  return ir.CreateCall(
      module->getOrInsertFunction(name, FunctionType::get(ty, {ty, ir.getInt16Ty()}, false), convergent),
      {v, ir.CreateZExtOrTrunc(lane, ir.getInt16Ty())}
  );
}

// `f` of each component of a scalar or vector
static Value *
each(IRBuilder<> &ir, Value *v, const std::function<Value *(Value *)> &f) {
  auto vector = dyn_cast<FixedVectorType>(v->getType());
  if (!vector)
    return f(v);
  Value *r = UndefValue::get(vector);
  for (unsigned i = 0; i < vector->getNumElements(); i++)
    r = ir.CreateInsertElement(r, f(ir.CreateExtractElement(v, i)), i);
  return r;
}

// a pixel's place in its coarse pixel, left to right, then top to bottom, from its place in the quad
Value *
CoarseShading::fine_index(Value *x, Value *y) {
  auto &ir = *builder_;
  auto row = ir.CreateSelect(wide_, ir.CreateShl(y, 1), y);
  return ir.CreateAdd(ir.CreateSelect(high_, row, ir.getInt32(0)), ir.CreateSelect(wide_, x, ir.getInt32(0)));
}

void
CoarseShading::Enter(Function *function, IRBuilder<> &ir, llvm::air::AIRBuilder &air, io_binding_map &resource) {
  function_ = function;
  builder_ = &ir;
  Type *i32 = ir.getInt32Ty(), *f32 = ir.getFloatTy();
  {
    IRBuilder<> entry(&function->getEntryBlock(), function->getEntryBlock().begin());
    discarded_ = entry.CreateAlloca(ir.getInt1Ty(), nullptr, "coarse_discarded");
    pixel_discarded_ = entry.CreateAlloca(ir.getInt1Ty(), nullptr, "pixel_discarded");
  }
  ir.CreateStore(ir.getFalse(), discarded_);
  ir.CreateStore(ir.getFalse(), pixel_discarded_);

  auto position = function->getArg(position_);
  auto px = ir.CreateExtractElement(position, uint64_t(0)), py = ir.CreateExtractElement(position, uint64_t(1));
  auto x = ir.CreateFPToUI(px, i32), y = ir.CreateFPToUI(py, i32);
  auto lx = ir.CreateAnd(x, 1), ly = ir.CreateAnd(y, 1);
  lane_ = ir.CreateOr(ir.CreateShl(ly, 1), lx);

  // the rate: RSSetShadingRate's, combined with the primitive's and then the image's tile's, a rate outside the image
  // 1x1 (D3D12_SHADING_RATE: log2 of the width, then of the height, two bits each)
  auto rates = function->getArg(rates_);
  auto word = [&](uint32_t i) { return ir.CreateLoad(i32, ir.CreateConstGEP1_32(i32, rates, i)); };
  auto base = word(0), first = word(1), second = word(2), image_width = word(3), image_height = word(4);
  Value *primitive = ~varying_ ? (Value *)function->getArg(varying_) : ir.getInt32(0);
  auto tx = ir.CreateLShr(x, 4), ty = ir.CreateLShr(y, 4);
  auto inside = ir.CreateAnd(ir.CreateICmpULT(tx, image_width), ir.CreateICmpULT(ty, image_height));
  auto image = function->getArg(image_);
  auto bytes = ir.CreateBitCast(image, ir.getInt8Ty()->getPointerTo(image->getType()->getPointerAddressSpace()));
  auto at = ir.CreateSelect(inside, ir.CreateAdd(ir.CreateMul(ty, image_width), tx), ir.getInt32(0));
  auto tile = ir.CreateSelect(
      inside, ir.CreateZExt(ir.CreateLoad(ir.getInt8Ty(), ir.CreateGEP(ir.getInt8Ty(), bytes, {at})), i32),
      ir.getInt32(0)
  );
  // D3D12_SHADING_RATE_COMBINER: passthrough, override, min, max, sum; on each axis. a sum's maximum, and the rate's,
  // is 2x without the additional rates
  auto combine = [&](Value *a, Value *b, Value *combiner) {
    auto axis = [&](Value *a, Value *b) {
      Value *c = a;
      c = ir.CreateSelect(ir.CreateICmpEQ(combiner, ir.getInt32(1)), b, c);
      c = ir.CreateSelect(ir.CreateICmpEQ(combiner, ir.getInt32(2)), ir.CreateSelect(ir.CreateICmpULT(a, b), a, b), c);
      c = ir.CreateSelect(ir.CreateICmpEQ(combiner, ir.getInt32(3)), ir.CreateSelect(ir.CreateICmpUGT(a, b), a, b), c);
      c = ir.CreateSelect(ir.CreateICmpEQ(combiner, ir.getInt32(4)), ir.CreateAdd(a, b), c);
      return c;
    };
    auto x = axis(ir.CreateAnd(ir.CreateLShr(a, 2), 3), ir.CreateAnd(ir.CreateLShr(b, 2), 3));
    auto y = axis(ir.CreateAnd(a, 3), ir.CreateAnd(b, 3));
    return ir.CreateOr(ir.CreateShl(x, 2), y);
  };
  auto rate = combine(combine(base, primitive, first), tile, second);
  wide_ = ir.CreateICmpNE(ir.CreateAnd(rate, 0b1100), ir.getInt32(0));
  high_ = ir.CreateICmpNE(ir.CreateAnd(rate, 0b0011), ir.getInt32(0));
  auto coarse = ir.CreateOr(wide_, high_);
  resource.shading_rate = ir.CreateOr(ir.CreateShl(ir.CreateZExt(wide_, i32), 2), ir.CreateZExt(high_, i32));

  // the lanes of the coarse pixel, and its leader: the first that is not a helper, whose writes Metal keeps
  auto &context = ir.getContext();
  auto helper = ir.CreateCall(function->getParent()->getOrInsertFunction(
      "air.simd_is_helper_thread", FunctionType::get(ir.getInt1Ty(), false),
      AttributeList::get(
          context, {{~0U, Attribute::get(context, Attribute::Convergent)},
                    {~0U, Attribute::get(context, Attribute::NoUnwind)},
                    {~0U, Attribute::get(context, Attribute::WillReturn)}}
      )
  ));
  auto live = ir.CreateNot(helper);
  Value *members[4];
  Value *leader = lane_;
  for (int j = 3; j >= 0; j--) {
    members[j] = ir.CreateAnd(
        ir.CreateOr(wide_, ir.CreateICmpEQ(lx, ir.getInt32(j & 1))),
        ir.CreateOr(high_, ir.CreateICmpEQ(ly, ir.getInt32(j >> 1)))
    );
    auto lane_live = quad_shuffle(ir, air, live, ir.getInt32(j));
    leader = ir.CreateSelect(ir.CreateAnd(members[j], lane_live), ir.getInt32(j), leader);
  }
  leader_ = leader;
  leads_ = ir.CreateICmpEQ(leader, lane_);

  // where each lane's inputs are: the leader's at the coarse pixel's center, the others a coarse pixel away from it on
  // each axis the coarse pixel spans, so that a derivative (across the quad) is the coarse pixels'
  auto offset = [&](Value *spans, Value *own, Value *at, Value *leader_at, Value *p) {
    auto center = ir.CreateFAdd(ir.CreateUIToFP(ir.CreateAnd(at, ~1u), f32), ConstantFP::get(f32, 1.0));
    auto away = ir.CreateFMul(ir.CreateSIToFP(ir.CreateSub(own, leader_at), f32), ConstantFP::get(f32, 2.0));
    return ir.CreateSelect(spans, ir.CreateFSub(ir.CreateFAdd(center, away), p), ConstantFP::get(f32, 0.0));
  };
  auto dx = offset(wide_, lx, x, ir.CreateAnd(leader, 1), px);
  auto dy = offset(high_, ly, y, ir.CreateLShr(leader, 1), py);
  // every input is affine in the window over the primitive (perspective ones over w, as 1/w is), so its derivatives
  // across the quad (the lane beside on x, then y) move it exactly
  auto sx = ir.CreateSelect(ir.CreateICmpEQ(lx, ir.getInt32(1)), ConstantFP::get(f32, -1.0), ConstantFP::get(f32, 1.0));
  auto sy = ir.CreateSelect(ir.CreateICmpEQ(ly, ir.getInt32(1)), ConstantFP::get(f32, -1.0), ConstantFP::get(f32, 1.0));
  auto moved = [&](Value *v) {
    return each(ir, v, [&](Value *c) {
      auto ddx = ir.CreateFMul(ir.CreateFSub(quad_shuffle(ir, air, c, ir.getInt32(1), true), c), sx);
      auto ddy = ir.CreateFMul(ir.CreateFSub(quad_shuffle(ir, air, c, ir.getInt32(2), true), c), sy);
      return ir.CreateFAdd(c, ir.CreateFAdd(ir.CreateFMul(ddx, dx), ir.CreateFMul(ddy, dy)));
    });
  };
  // Metal's position.w is 1/w
  auto rcp_w = ir.CreateExtractElement(position, uint64_t(3));
  auto moved_rcp_w = moved(rcp_w);
  Value *centered = position;
  centered = ir.CreateInsertElement(centered, ir.CreateFAdd(px, dx), uint64_t(0));
  centered = ir.CreateInsertElement(centered, ir.CreateFAdd(py, dy), uint64_t(1));
  centered = ir.CreateInsertElement(centered, moved(ir.CreateExtractElement(position, uint64_t(2))), uint64_t(2));
  centered = ir.CreateInsertElement(centered, moved_rcp_w, uint64_t(3));
  centered_.push_back({function->getArg(position_), ir.CreateSelect(coarse, centered, position)});
  for (auto input : inputs_) {
    auto value = function->getArg(input.index);
    if (!value->getType()->getScalarType()->isFloatingPointTy())
      continue;
    Value *at_center;
    if (input.perspective) {
      auto over_w = each(ir, value, [&](Value *c) { return ir.CreateFMul(c, rcp_w); });
      at_center = each(ir, moved(over_w), [&](Value *c) { return ir.CreateFDiv(c, moved_rcp_w); });
    } else {
      at_center = moved(value);
    }
    centered_.push_back({value, ir.CreateSelect(coarse, at_center, value)});
  }

  // the coarse pixel's coverage: its pixels' samples, a pixel after another left to right, then top to bottom
  if (~coverage_) {
    auto own = function->getArg(coverage_);
    auto samples = air.CreateGetNumSamples();
    Value *all = ir.getInt32(0);
    for (unsigned j = 0; j < 4; j++) {
      auto index = fine_index(ir.getInt32(j & 1), ir.getInt32(j >> 1));
      auto bits = ir.CreateShl(quad_shuffle(ir, air, own, ir.getInt32(j)), ir.CreateMul(index, samples));
      all = ir.CreateOr(all, ir.CreateSelect(members[j], bits, ir.getInt32(0)));
    }
    centered_.push_back({function->getArg(coverage_), ir.CreateSelect(coarse, all, own)});
  }

  for (auto &block : *function)
    for (auto &instruction : block)
      before_.insert(&instruction);
}

void
CoarseShading::Leave(IRBuilder<> &ir, llvm::air::AIRBuilder &air, io_binding_map &resource) {
  builder_ = &ir;
  auto i32 = ir.getInt32Ty();
  auto int4 = FixedVectorType::get(i32, 4);
  for (uint32_t reg = 0; reg < resource.output_element_count; reg++) {
    auto at = ir.CreateGEP(
        resource.output.ptr_int4->getType()->getNonOpaquePointerElementType(), resource.output.ptr_int4,
        {ir.getInt32(0), ir.getInt32(reg)}
    );
    auto value = ir.CreateLoad(int4, at);
    ir.CreateStore(each(ir, value, [&](Value *c) { return quad_shuffle(ir, air, c, leader_); }), at);
  }
  // each pixel's samples of the coarse pixel's output coverage
  if (resource.coverage_mask_reg) {
    auto mask = quad_shuffle(ir, air, ir.CreateLoad(i32, resource.coverage_mask_reg), leader_);
    auto index = fine_index(ir.CreateAnd(lane_, 1), ir.CreateLShr(lane_, 1));
    auto samples = air.CreateGetNumSamples();
    auto own = ir.CreateAnd(
        ir.CreateLShr(mask, ir.CreateMul(index, samples)),
        ir.CreateSub(ir.CreateShl(ir.getInt32(1), samples), ir.getInt32(1))
    );
    ir.CreateStore(own, resource.coverage_mask_reg);
  }
  // the leader's discard is the coarse pixel's; a pixel's own (the depth bounds test's) is its alone
  auto discarded = ir.CreateOr(
      quad_shuffle(ir, air, ir.CreateLoad(ir.getInt1Ty(), discarded_), leader_),
      ir.CreateLoad(ir.getInt1Ty(), pixel_discarded_)
  );
  auto discard = llvm::BasicBlock::Create(ir.getContext(), "coarse_discard", function_);
  auto kept = llvm::BasicBlock::Create(ir.getContext(), "coarse_kept", function_);
  ir.CreateCondBr(discarded, discard, kept);
  ir.SetInsertPoint(discard);
  discard_ = air.CreateDiscard();
  ir.CreateBr(kept);
  ir.SetInsertPoint(kept);
}

// what a lane that is not the leader, or after the leader's discard, makes of a write it does not do: an exchange
// succeeds (so a lock's loop ends), the others read 0
static Value *
skipped(IRBuilder<> &ir, llvm::Instruction *write) {
  if (auto exchange = dyn_cast<AtomicCmpXchgInst>(write))
    return ir.CreateInsertValue(
        ir.CreateInsertValue(UndefValue::get(write->getType()), exchange->getCompareOperand(), {0}), ir.getTrue(), {1}
    );
  if (auto call = dyn_cast<CallInst>(write);
      call && call->getType()->isIntegerTy(1) && call->getCalledFunction()->getName().contains("compare_exchange"))
    return ir.getTrue();
  return Constant::getNullValue(write->getType());
}

void
CoarseShading::Finish() {
  for (auto [argument, value] : centered_)
    argument->replaceUsesWithIf(value, [&](Use &use) {
      auto user = dyn_cast<llvm::Instruction>(use.getUser());
      return user && !before_.contains(user);
    });

  std::vector<llvm::Instruction *> discards, writes;
  for (auto &block : *function_)
    for (auto &instruction : block) {
      if (&instruction == discard_)
        continue;
      auto call = dyn_cast<CallInst>(&instruction);
      auto callee = call && call->getCalledFunction() ? call->getCalledFunction()->getName() : StringRef();
      if (callee == "air.discard_fragment") {
        discards.push_back(&instruction);
        continue;
      }
      if (before_.contains(&instruction))
        continue;
      bool device = false;
      if (auto store = dyn_cast<StoreInst>(&instruction))
        device = store->getPointerAddressSpace() == 1;
      else if (auto rmw = dyn_cast<AtomicRMWInst>(&instruction))
        device = rmw->getPointerAddressSpace() == 1;
      else if (auto exchange = dyn_cast<AtomicCmpXchgInst>(&instruction))
        device = exchange->getPointerAddressSpace() == 1;
      else if (auto memory = dyn_cast<AnyMemIntrinsic>(&instruction))
        device = memory->getDestAddressSpace() == 1;
      else if (call)
        device = callee.startswith("air.write_") ||
                 (callee.contains("atomic") && !callee.contains("fence") && !callee.contains("load"));
      if (device)
        writes.push_back(&instruction);
    }

  auto i1 = Type::getInt1Ty(function_->getContext());
  for (auto discard : discards) {
    IRBuilder<> ir(discard);
    ir.CreateStore(ir.getTrue(), before_.contains(discard) ? pixel_discarded_ : discarded_);
    discard->eraseFromParent();
  }
  for (auto write : writes) {
    IRBuilder<> ir(write);
    auto head = write->getParent();
    auto writes = ir.CreateAnd(leads_, ir.CreateNot(ir.CreateLoad(i1, discarded_)));
    auto then = SplitBlockAndInsertIfThen(writes, write, false);
    auto tail = write->getParent();
    write->moveBefore(then);
    if (write->getType()->isVoidTy())
      continue;
    IRBuilder<> before_branch(head->getTerminator());
    auto otherwise = skipped(before_branch, write);
    auto phi = PHINode::Create(write->getType(), 2, "", &tail->front());
    write->replaceAllUsesWith(phi);
    phi->addIncoming(write, then->getParent());
    phi->addIncoming(otherwise, head);
  }
}

} // namespace mullion::dxbc
