/*
 * Copyright 2026 David Skoland
 *
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

#include "d3d9_shader_object.hpp"
#include "d3d9_device.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <cstring>

namespace mullion {

static size_t
ShaderTokenCount(const DWORD *pFunction) {
  d3d9bc::Version version;
  if (!d3d9bc::read_version(pFunction[0], version))
    return 0;
  constexpr size_t kMaxTokens = 1 << 22;
  for (size_t i = 1; i < kMaxTokens;) {
    DWORD token = pFunction[i];
    if (token == 0x0000FFFF)
      return i + 1;
    if ((token & 0xFFFF) == 0xFFFE) {
      i += 1 + ((token >> 16) & 0x7FFF);
      continue;
    }
    i += version.major >= 2 ? 1 + ((token >> 24) & 0xF) : 1;
  }
  return 0;
}

MTLD3D9ShaderCode::~MTLD3D9ShaderCode() {
  if (handle_)
    SM50Destroy(handle_);
}

HRESULT
MTLD3D9ShaderCode::Create(const DWORD *pFunction, bool Pixel, std::unique_ptr<MTLD3D9ShaderCode> &Out) {
  if (!pFunction)
    return D3DERR_INVALIDCALL;
  size_t count = ShaderTokenCount(pFunction);
  d3d9bc::Version version;
  if (!count || !d3d9bc::read_version(pFunction[0], version) || version.pixel != Pixel)
    return D3DERR_INVALIDCALL;

  auto code = std::make_unique<MTLD3D9ShaderCode>();
  code->bytecode_.assign(pFunction, pFunction + count);
  code->pixel_ = Pixel;
  code->sha1_ = Sha1HashState::compute(pFunction, count * sizeof(DWORD));

  sm50_error_t err = nullptr;
  if (SM30Initialize(pFunction, count * sizeof(DWORD), &code->handle_, &code->reflection_, &err)) {
    ERR("D3D9: failed to translate ", Pixel ? "pixel" : "vertex", " shader ", code->sha1_.string(), ": ",
        SM50GetErrorMessageString(err));
    SM50FreeError(err);
    return D3DERR_INVALIDCALL;
  }
  code->cbuffers_.resize(code->reflection_.NumConstantBuffers);
  code->arguments_.resize(code->reflection_.NumArguments);
  SM50GetArgumentsInfo(code->handle_, code->cbuffers_.data(), code->arguments_.data());

  {
    std::vector<FloatDef> defs;
    bool relative = false;
    d3d9bc::Reader reader((const uint32_t *)pFunction, count);
    d3d9bc::Instruction inst;
    while (reader.next(inst)) {
      if (inst.opcode == d3d9bc::Opcode::Def) {
        FloatDef def = {d3d9bc::register_number(inst.operands[0])};
        std::memcpy(def.value, inst.operands + 1, sizeof(def.value));
        defs.push_back(def);
        continue;
      }
      for (uint32_t i = 0; i < inst.length; i++) {
        uint32_t token = inst.operands[i];
        if ((token & 0x80000000u) && d3d9bc::register_type(token) == d3d9bc::RegisterType::Const &&
            d3d9bc::relative_addressing(token))
          relative = true;
      }
    }
    if (relative)
      code->relative_defs_ = std::move(defs);
  }

  if (!Pixel) {
    auto set_output = [&](d3d9bc::Usage usage, uint32_t index) {
      uint32_t bit = (uint32_t)usage * 16 + index;
      if (bit < 224)
        code->outputs_[bit / 32] |= 1u << (bit % 32);
    };
    // airconv fills colors 0-1, texture coordinates 0-7 and fog for every vertex shader
    for (uint32_t i = 0; i < 2; i++)
      set_output(d3d9bc::Usage::Color, i);
    for (uint32_t i = 0; i < 8; i++)
      set_output(d3d9bc::Usage::TexCoord, i);
    set_output(d3d9bc::Usage::Fog, 0);
    set_output(d3d9bc::Usage::Position, 0);
    // before vs_3_0 the output registers are fixed: oPos, oFog, oPts, oD0-1, oT0-7
    if (pFunction[0] < 0xFFFE0300) {
      code->output_registers_ = {
          {d3d9bc::Usage::Position, 0, 0}, {d3d9bc::Usage::Fog, 0, 1}, {d3d9bc::Usage::PSize, 0, 2},
          {d3d9bc::Usage::Color, 0, 3},    {d3d9bc::Usage::Color, 1, 4},
      };
      for (uint32_t i = 0; i < 8; i++)
        code->output_registers_.push_back({d3d9bc::Usage::TexCoord, i, 5 + i});
    }
    d3d9bc::Reader reader((const uint32_t *)pFunction, count);
    d3d9bc::Instruction inst;
    while (reader.next(inst)) {
      if (inst.opcode != d3d9bc::Opcode::Dcl)
        continue;
      uint32_t reg_token = inst.operands[1];
      if (d3d9bc::register_type(reg_token) == d3d9bc::RegisterType::Output) {
        set_output(d3d9bc::Usage(inst.operands[0] & 0x1F), (inst.operands[0] >> 16) & 0xF);
        code->output_registers_.push_back(
            {d3d9bc::Usage(inst.operands[0] & 0x1F), (inst.operands[0] >> 16) & 0xF, d3d9bc::register_number(reg_token)}
        );
        continue;
      }
      if (d3d9bc::register_type(reg_token) != d3d9bc::RegisterType::Input)
        continue;
      code->inputs_.push_back(
          {d3d9bc::Usage(inst.operands[0] & 0x1F), (inst.operands[0] >> 16) & 0xF, d3d9bc::register_number(reg_token)}
      );
    }
  }

  Out = std::move(code);
  return D3D_OK;
}

WMT::Function
MTLD3D9ShaderCode::GetFunction(
    WMT::Device Device, const Sha1Digest &VariantKey, SM50_SHADER_COMPILATION_ARGUMENT_DATA *Args,
    SM50_SHADER_METAL_VERSION MetalVersion
) {
  std::lock_guard<mullion::mutex> lock(mutex_);
  if (auto it = variants_.find(VariantKey); it != variants_.end())
    return it->second;

  SM50_SHADER_COMMON_DATA common = {};
  common.type = SM50_SHADER_COMMON;
  common.metal_version = MetalVersion;
  common.next = Args;

  std::string name = (pixel_ ? "ps9_" : "vs9_") + sha1_.string().substr(0, 8) + "_" + VariantKey.string().substr(0, 8);
  sm50_bitcode_t bitcode = nullptr;
  sm50_error_t err = nullptr;
  WMT::Reference<WMT::Function> function;
  if (SM50Compile(handle_, (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&common, name.c_str(), &bitcode, &err)) {
    ERR("D3D9: failed to compile shader ", sha1_.string(), ": ", SM50GetErrorMessageString(err));
    SM50FreeError(err);
  } else {
    SM50_COMPILED_BITCODE compiled;
    SM50GetCompiledBitcode(bitcode, &compiled);
    WMT::Reference<WMT::Error> error;
    auto library = Device.newLibrary(WMT::MakeDispatchData(compiled.Data, compiled.Size), error);
    if (error || !library)
      ERR("D3D9: failed to create Metal library: ", error.description().getUTF8String());
    else
      function = library.newFunction(name.c_str());
    SM50DestroyBitcode(bitcode);
  }
  variants_.emplace(VariantKey, function);
  return function;
}

template <typename Base>
MTLD3D9ShaderObject<Base>::MTLD3D9ShaderObject(MTLD3D9Device *pDevice, MTLD3D9ShaderCode *Code) :
    MTLD3D9DeviceChild<Base>(pDevice),
    code_(Code) {}

template <typename Base>
HRESULT STDMETHODCALLTYPE
MTLD3D9ShaderObject<Base>::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;
  if (riid == __uuidof(IUnknown) || riid == __uuidof(Base)) {
    *ppvObject = ref(this);
    return S_OK;
  }
  WARN("D3D9 shader: Unknown interface query ", str::format(riid));
  return E_NOINTERFACE;
}

template <typename Base>
HRESULT STDMETHODCALLTYPE
MTLD3D9ShaderObject<Base>::GetDevice(IDirect3DDevice9 **ppDevice) {
  return this->GetDeviceInternal(ppDevice);
}

template <typename Base>
HRESULT STDMETHODCALLTYPE
MTLD3D9ShaderObject<Base>::GetFunction(void *pData, UINT *pSizeOfData) {
  if (!pSizeOfData)
    return D3DERR_INVALIDCALL;
  UINT size = code_->bytecode().size() * sizeof(DWORD);
  if (!pData) {
    *pSizeOfData = size;
    return D3D_OK;
  }
  if (*pSizeOfData < size)
    return D3DERR_MOREDATA;
  std::memcpy(pData, code_->bytecode().data(), size);
  *pSizeOfData = size;
  return D3D_OK;
}

template class MTLD3D9ShaderObject<IDirect3DVertexShader9>;
template class MTLD3D9ShaderObject<IDirect3DPixelShader9>;

} // namespace mullion
