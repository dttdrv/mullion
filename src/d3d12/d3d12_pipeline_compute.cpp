/*
 * Copyright 2026 Feifan He for CodeWeavers
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

#include "Metal.hpp"
#include "com/com_pointer.hpp"
#include "d3d12_device.hpp"
#include "d3d12_pageable.hpp"
#include "d3d12_pipeline.hpp"
#include "log/log.hpp"
#include <array>

namespace dxmt {

class MTLD3D12ComputePipelineStateImpl : public MTLD3D12Pageable<MTLD3D12ComputePipelineState> {

  MTL_SHADER_REFLECTION ref_cs;

public:
  MTLD3D12ComputePipelineStateImpl(MTLD3D12Device *pDevice) : MTLD3D12Pageable<MTLD3D12ComputePipelineState>(pDevice) {
    IsComputePipelineState = 1;
  }

  HRESULT
  Initialize(const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc) {
    auto start = device_->NamesPasses() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
    SM50Shader shader_cs;

    SM50_SHADER_ROOT_SIGNATURE_DATA rootsig{};
    rootsig.type = SM50_SHADER_ROOT_SIGNATURE;
    rootsig.bytecode_length = static_cast<MTLD3D12RootSignature *>(pDesc->pRootSignature)->GetBlob(&rootsig.bytecode);
    rootsig.next = nullptr;

    SM50_SHADER_COMMON_DATA common;
    common.flags = {};
    common.type = SM50_SHADER_COMMON;
    common.metal_version = device_->GetMetalVersion();
    common.simd_width = device_->GetSIMDWidth();
    common.next = &rootsig;

    Name({pDesc->CS});
    if (HRESULT hr = InitializeShader(pDesc->CS, &shader_cs, &ref_cs); FAILED(hr))
      return hr;

    threadgroup_size = {ref_cs.ThreadgroupSize[0], ref_cs.ThreadgroupSize[1], ref_cs.ThreadgroupSize[2]};
    groups_work_together = ref_cs.GroupsWorkTogether;
    if (groups_work_together)
      Logger::info(str::format("compute pipeline ", name, ": its threadgroups work together, it is dispatched in parts"));

    auto metal = device_->GetMTLDevice();
    auto args = (SM50_SHADER_COMPILATION_ARGUMENT_DATA *)&common;
    HRESULT status = E_FAIL;
    auto cs_func = CompileFunction(
        device_, std::array{pDesc->CS}, args, "cs_main", [&](auto args, auto name, auto bitcode, auto error) {
          auto result = SM50Compile(shader_cs, args, name, bitcode, error);
          if (result)
            status = E_INVALIDARG;
          return result;
        }
    );
    if (!cs_func)
      return status;
    WMT::Reference<WMT::Error> err;

    // PSO
    {
      WMTComputePipelineInfo info;
      WMT::InitializeComputePipelineInfo(info);
      info.compute_function = cs_func;
      info.support_indirect_command_buffers = true;

      pso = metal.newComputePipelineState(info, err);
      if (!pso) {
        ERR("Failed to create compute PSO of shader ", name, ": ", err.description().getUTF8String());
        DumpShaders({pDesc->CS});
        return E_FAIL;
      }
    }

    if (device_->NamesPasses())
      device_->PipelineMade(name, "compute", start);
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12PipelineState)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12PipelineState), riid)) {
      WARN("D3D12ComputePipelineState: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  virtual HRESULT STDMETHODCALLTYPE
  GetCachedBlob(ID3DBlob **blob) {
    return GetCachedPipelineBlob(blob);
  }
};

HRESULT
CreateComputePipelineState(
    MTLD3D12Device *pDevice, const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState
) {
  if (HRESULT hr = CheckCachedPipeline(pDesc->CachedPSO); FAILED(hr))
    return hr;
  auto desc = *pDesc;
  Com<ID3D12RootSignature> embedded;
  if (!desc.pRootSignature) {
    if (!ShaderContainerHolds(desc.CS.pShaderBytecode, desc.CS.BytecodeLength))
      return E_INVALIDARG;
    HRESULT hr = CreateRootSignature(
        pDevice, 0, desc.CS.pShaderBytecode, desc.CS.BytecodeLength, __uuidof(ID3D12RootSignature),
        reinterpret_cast<void **>(&embedded)
    );
    if (FAILED(hr))
      return hr == E_FAIL ? E_INVALIDARG : hr;
    desc.pRootSignature = embedded.ptr();
  }
  auto pso = Com(new MTLD3D12ComputePipelineStateImpl(pDevice));
  HRESULT hr = pso->Initialize(&desc);
  if (FAILED(hr))
    return hr;
  return pso->QueryInterface(riid, ppPipelineState);
};

} // namespace dxmt