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
#include "d3d12_pipeline.hpp"
#include "log/log.hpp"
#include <array>
#include <limits>
#include <map>

namespace dxmt {

class MTLD3D12ComputePipelineStateImpl : public MTLD3D12PipelineObject<MTLD3D12ComputePipelineState> {

  MTL_SHADER_REFLECTION ref_cs;

public:
  MTLD3D12ComputePipelineStateImpl(MTLD3D12Device *pDevice) :
      MTLD3D12PipelineObject<MTLD3D12ComputePipelineState>(pDevice) {
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
    common.flags = device_->GetShaderFlags();
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
        device_, this, std::array{pDesc->CS}, args, "cs_main", [&](auto args, auto name, auto bitcode, auto error) {
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

    return MTLD3D12PipelineObject::QueryInterface(riid, ppvObject);
  }

  virtual HRESULT STDMETHODCALLTYPE
  GetCachedBlob(ID3DBlob **blob) {
    return GetCachedPipelineBlob(blob);
  }
};

HRESULT
CreateComputePipelineState(
    MTLD3D12Device *pDevice, const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState,
    const PipelineShaderCache *cached
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
  if (HRESULT hr = PipelineKey(*pDesc, pso->library_key); FAILED(hr))
    return hr;
  if (cached)
    pso->cached_shaders = *cached;
  HRESULT hr = pso->Initialize(&desc);
  if (FAILED(hr))
    return hr;
  return pso->QueryInterface(riid, ppPipelineState);
};

class MTLD3D12PipelineLibrary : public MTLD3D12DeviceChild<ID3D12PipelineLibrary1> {
  static constexpr char kMagic[] = "Mullion library 2";
  static constexpr SIZE_T kHeaderSize = sizeof(kMagic) + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(Sha1Digest);
  struct Entry {
    Sha1Digest key;
    Com<MTLD3D12PipelineState, false> pipeline;
    std::vector<Sha1Digest> shaders;
  };
  dxmt::mutex mutex_;
  std::map<std::wstring, Entry, std::less<>> entries_;
  PipelineShaderCache shaders_;

  SIZE_T
  SerializedSize() const {
    SIZE_T size = kHeaderSize + sizeof(uint64_t);
    for (auto &[key, data] : shaders_)
      size += sizeof(key) + sizeof(uint64_t) + data.getBytes(nullptr, 0);
    for (auto &[name, entry] : entries_)
      size += 2 * sizeof(uint64_t) + sizeof(entry.key) + entry.shaders.size() * sizeof(Sha1Digest) +
              name.size() * sizeof(WCHAR);
    return size;
  }

  HRESULT
  Load(LPCWSTR name, const PipelineStateStream &desc, REFIID iid, void **pipeline) try {
    InitReturnPtr(pipeline);
    if (!name)
      return E_INVALIDARG;
    Sha1Digest key;
    if (HRESULT hr = desc.Key(key); FAILED(hr))
      return hr;
    std::unique_lock lock(mutex_);
    auto found = entries_.find(name);
    if (found == entries_.end() || found->second.key != key)
      return E_INVALIDARG;
    if (!found->second.pipeline) {
      PipelineShaderCache cached;
      for (auto shader : found->second.shaders)
        cached.emplace(shader, shaders_.at(shader));
      lock.unlock();
      Com<ID3D12PipelineState> made;
      if (HRESULT hr = desc.Create(device_, __uuidof(ID3D12PipelineState), (void **)&made, &cached); FAILED(hr))
        return hr;
      lock.lock();
      found = entries_.find(name);
      if (!found->second.pipeline)
        found->second.pipeline = static_cast<MTLD3D12PipelineState *>(made.ptr());
    }
    return found->second.pipeline->QueryInterface(iid, pipeline);
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }

public:
  MTLD3D12PipelineLibrary(MTLD3D12Device *device) :
      MTLD3D12DeviceChild<ID3D12PipelineLibrary1>(device) {}

  HRESULT
  Initialize(const void *blob, SIZE_T size) {
    if (!size)
      return S_OK;
    if (!blob || size < kHeaderSize || memcmp(blob, kMagic, sizeof(kMagic)))
      return E_INVALIDARG;
    auto data = static_cast<const char *>(blob);
    SIZE_T offset = sizeof(kMagic);
    auto read = [&](auto &value) {
      if (sizeof(value) > size - offset)
        return false;
      memcpy(&value, data + offset, sizeof(value));
      offset += sizeof(value);
      return true;
    };
    uint32_t version;
    uint64_t adapter;
    Sha1Digest checksum;
    read(version), read(adapter), read(checksum);
    Sha1HashState hash;
    hash.update(data, kHeaderSize - sizeof(checksum));
    hash.update(data + kHeaderSize, size - kHeaderSize);
    if (hash.final() != checksum)
      return E_INVALIDARG;
    if (version != AIRCONV_VERSION)
      return D3D12_ERROR_DRIVER_VERSION_MISMATCH;
    if (adapter != device_->GetMTLDevice().registryID())
      return D3D12_ERROR_ADAPTER_NOT_FOUND;
    uint64_t shaders;
    if (!read(shaders) || shaders > (size - offset) / (sizeof(Sha1Digest) + sizeof(uint64_t)))
      return E_INVALIDARG;
    for (uint64_t i = 0; i < shaders; i++) {
      Sha1Digest key;
      uint64_t length;
      if (!read(key) || !read(length) || !length || length > size - offset || shaders_.contains(key))
        return E_INVALIDARG;
      auto compiled = WMT::MakeDispatchData(uint64_t(data + offset), length);
      if (!compiled)
        return E_OUTOFMEMORY;
      shaders_.emplace(key, std::move(compiled));
      offset += length;
    }
    while (offset < size) {
      uint64_t length, count;
      Entry entry{};
      if (!read(length) || !read(entry.key) || !read(count) || !count || count > (size - offset) / sizeof(Sha1Digest))
        return E_INVALIDARG;
      for (uint64_t i = 0; i < count; i++) {
        Sha1Digest key;
        read(key);
        if (!shaders_.contains(key))
          return E_INVALIDARG;
        entry.shaders.push_back(key);
      }
      if (length > (size - offset) / sizeof(WCHAR))
        return E_INVALIDARG;
      std::wstring name(length, L'\0');
      memcpy(name.data(), data + offset, length * sizeof(WCHAR));
      offset += length * sizeof(WCHAR);
      if (name.find(L'\0') != std::wstring::npos || !entries_.try_emplace(std::move(name), std::move(entry)).second)
        return E_INVALIDARG;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  QueryInterface(REFIID iid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12Object) || iid == __uuidof(ID3D12DeviceChild) ||
        iid == __uuidof(ID3D12PipelineLibrary) || iid == __uuidof(ID3D12PipelineLibrary1))
      *object = static_cast<ID3D12PipelineLibrary1 *>(ref(this));
    return *object ? S_OK : MTLD3D12DeviceChild::QueryInterface(iid, object);
  }

  HRESULT STDMETHODCALLTYPE
  StorePipeline(LPCWSTR name, ID3D12PipelineState *pipeline) override {
    if (!pipeline)
      return E_INVALIDARG;
    Com<ID3D12Device> parent;
    if (FAILED(pipeline->GetDevice(__uuidof(ID3D12Device), (void **)&parent)) || parent.ptr() != device_)
      return E_INVALIDARG;
    std::lock_guard lock(mutex_);
    try {
      auto state = static_cast<MTLD3D12PipelineState *>(pipeline);
      if (entries_.contains(name ? name : L""))
        return E_INVALIDARG;
      Entry entry{state->library_key, state, {}};
      std::lock_guard shader_lock(state->shader_mutex);
      for (auto &[key, data] : state->cached_shaders) {
        data = shaders_.try_emplace(key, data).first->second;
        entry.shaders.push_back(key);
      }
      entries_.emplace(name ? name : L"", std::move(entry));
      return S_OK;
    } catch (const std::bad_alloc &) {
      return E_OUTOFMEMORY;
    }
  }

  HRESULT STDMETHODCALLTYPE
  LoadGraphicsPipeline(
      LPCWSTR name, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, REFIID iid, void **pipeline
  ) override {
    InitReturnPtr(pipeline);
    if (!desc)
      return E_INVALIDARG;
    PipelineStateStream description;
    description.desc_graphics = *desc;
    return Load(name, description, iid, pipeline);
  }

  HRESULT STDMETHODCALLTYPE
  LoadComputePipeline(
      LPCWSTR name, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc, REFIID iid, void **pipeline
  ) override {
    InitReturnPtr(pipeline);
    if (!desc)
      return E_INVALIDARG;
    PipelineStateStream description;
    description.desc_cs = *desc;
    return Load(name, description, iid, pipeline);
  }

  HRESULT STDMETHODCALLTYPE
  LoadPipeline(LPCWSTR name, const D3D12_PIPELINE_STATE_STREAM_DESC *desc, REFIID iid, void **pipeline) override {
    InitReturnPtr(pipeline);
    PipelineStateStream description;
    if (HRESULT hr = description.Parse(desc); FAILED(hr))
      return hr;
    return Load(name, description, iid, pipeline);
  }

  SIZE_T STDMETHODCALLTYPE
  GetSerializedSize() override {
    std::lock_guard lock(mutex_);
    return SerializedSize();
  }

  HRESULT STDMETHODCALLTYPE
  Serialize(void *blob, SIZE_T size) override {
    std::lock_guard lock(mutex_);
    auto required = SerializedSize();
    if (!blob || size < required)
      return E_INVALIDARG;
    auto data_ptr = static_cast<char *>(blob);
    SIZE_T offset = 0;
    auto write = [&](const void *value, SIZE_T bytes) {
      memcpy(data_ptr + offset, value, bytes);
      offset += bytes;
    };
    uint32_t version = AIRCONV_VERSION;
    uint64_t adapter = device_->GetMTLDevice().registryID();
    write(kMagic, sizeof(kMagic)), write(&version, sizeof(version)), write(&adapter, sizeof(adapter));
    offset = kHeaderSize;
    uint64_t count = shaders_.size();
    write(&count, sizeof(count));
    for (auto &[key, data] : shaders_) {
      uint64_t length = data.getBytes(nullptr, 0);
      write(&key, sizeof(key)), write(&length, sizeof(length));
      data.getBytes(data_ptr + offset, length);
      offset += length;
    }
    for (auto &[name, entry] : entries_) {
      uint64_t length = name.size();
      uint64_t shaders = entry.shaders.size();
      write(&length, sizeof(length)), write(&entry.key, sizeof(entry.key)), write(&shaders, sizeof(shaders));
      write(entry.shaders.data(), shaders * sizeof(Sha1Digest));
      write(name.data(), length * sizeof(WCHAR));
    }
    Sha1HashState hash;
    hash.update(data_ptr, kHeaderSize - sizeof(Sha1Digest));
    hash.update(data_ptr + kHeaderSize, required - kHeaderSize);
    auto checksum = hash.final();
    memcpy(data_ptr + kHeaderSize - sizeof(checksum), &checksum, sizeof(checksum));
    return S_OK;
  }
};

HRESULT
CreatePipelineLibrary(MTLD3D12Device *Device, const void *Blob, SIZE_T Size, REFIID Iid, void **Library) {
  InitReturnPtr(Library);
  try {
    auto library = Com(new MTLD3D12PipelineLibrary(Device));
    if (HRESULT hr = library->Initialize(Blob, Size); FAILED(hr))
      return hr;
    return Library ? library->QueryInterface(Iid, Library) : S_FALSE;
  } catch (const std::bad_alloc &) {
    return E_OUTOFMEMORY;
  }
}

} // namespace dxmt
