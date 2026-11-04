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

#pragma once

#include "d3d9_device_child.hpp"
#include "airconv_public.h"
#include "d3d9_bytecode.hpp"
#include "Metal.hpp"
#include "sha1/sha1_util.hpp"
#include "thread.hpp"
#include <array>
#include <unordered_map>
#include <vector>

namespace mullion {

class MTLD3D9Device;

struct D3D9ShaderInput {
  d3d9bc::Usage usage;
  uint32_t usage_index;
  uint32_t reg;
};

class MTLD3D9ShaderCode {
public:
  ~MTLD3D9ShaderCode();

  static HRESULT Create(const DWORD *pFunction, bool Pixel, std::unique_ptr<MTLD3D9ShaderCode> &Out);

  // compiles (or fetches) the Metal function for one variant. Args is the variant-specific argument chain.
  WMT::Function GetFunction(
      WMT::Device Device, const Sha1Digest &VariantKey, SM50_SHADER_COMPILATION_ARGUMENT_DATA *Args,
      SM50_SHADER_METAL_VERSION MetalVersion
  );

  const MTL_SHADER_REFLECTION &
  reflection() const {
    return reflection_;
  }

  const MTL_SM50_SHADER_ARGUMENT *
  constantBuffers() const {
    return cbuffers_.data();
  }

  const MTL_SM50_SHADER_ARGUMENT *
  arguments() const {
    return arguments_.data();
  }

  const std::vector<D3D9ShaderInput> &
  inputs() const {
    return inputs_;
  }

  // def constants a relatively addressed constant read may reach; the device writes them over the app's
  struct FloatDef {
    uint32_t reg;
    float value[4];
  };
  const std::vector<FloatDef> &
  relativeDefs() const {
    return relative_defs_;
  }

  // vertex shaders: the semantics written, as SM30_SHADER_D3D9_DATA::vs_outputs bits
  const std::array<uint32_t, 7> &
  outputs() const {
    return outputs_;
  }

  // vertex shaders: the output register each semantic is written to, for ProcessVertices
  const std::vector<D3D9ShaderInput> &
  outputRegisters() const {
    return output_registers_;
  }

  const std::vector<DWORD> &
  bytecode() const {
    return bytecode_;
  }

  const Sha1Digest &
  sha1() const {
    return sha1_;
  }

  bool
  pixel() const {
    return pixel_;
  }

private:
  std::vector<DWORD> bytecode_;
  bool pixel_ = false;
  sm50_shader_t handle_ = nullptr;
  MTL_SHADER_REFLECTION reflection_ = {};
  std::vector<MTL_SM50_SHADER_ARGUMENT> cbuffers_;
  std::vector<MTL_SM50_SHADER_ARGUMENT> arguments_;
  std::vector<D3D9ShaderInput> inputs_;
  std::array<uint32_t, 7> outputs_ = {};
  std::vector<D3D9ShaderInput> output_registers_;
  std::vector<FloatDef> relative_defs_;
  Sha1Digest sha1_;
  mullion::mutex mutex_;
  std::unordered_map<Sha1Digest, WMT::Reference<WMT::Function>> variants_;
};

template <typename Base> class MTLD3D9ShaderObject final : public MTLD3D9DeviceChild<Base> {
public:
  MTLD3D9ShaderObject(MTLD3D9Device *pDevice, MTLD3D9ShaderCode *Code);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final;
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) final;
  HRESULT STDMETHODCALLTYPE GetFunction(void *pData, UINT *pSizeOfData) final;

  MTLD3D9ShaderCode *
  code() {
    return code_;
  }

private:
  // owned by the device so that recorded commands can keep pointing at its reflection.
  MTLD3D9ShaderCode *code_;
};

using MTLD3D9VertexShader = MTLD3D9ShaderObject<IDirect3DVertexShader9>;
using MTLD3D9PixelShader = MTLD3D9ShaderObject<IDirect3DPixelShader9>;

} // namespace mullion
