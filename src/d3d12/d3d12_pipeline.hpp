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

#pragma once
#include "airconv_public.h"
#include "d3d12.h"
#include "../d3d10/d3d10_blob.hpp"
#include <cstring>

namespace dxmt {

class SM50Shader {
  sm50_shader_t sm50_shader_{};

public:
  sm50_shader_t *
  operator&() {
    return &sm50_shader_;
  }

  operator sm50_shader_t() {
    return sm50_shader_;
  }

  ~SM50Shader() {
    if (sm50_shader_)
      SM50Destroy(sm50_shader_);
  }
};

class SM50ShaderBitcode {
  sm50_bitcode_t sm50_bitcode_{};

public:
  sm50_bitcode_t *
  operator&() {
    return &sm50_bitcode_;
  }

  operator sm50_bitcode_t() {
    return sm50_bitcode_;
  }

  ~SM50ShaderBitcode() {
    if (sm50_bitcode_)
      SM50DestroyBitcode(sm50_bitcode_);
  }
};

class SM50Error {
  sm50_error_t sm50_error_{};

public:
  sm50_error_t *
  operator&() {
    return &sm50_error_;
  }

  operator sm50_error_t() {
    return sm50_error_;
  }

  ~SM50Error() {
    if (sm50_error_)
      SM50FreeError(sm50_error_);
  }
};

// a pipeline's cached blob (ID3D12PipelineState::GetCachedBlob, D3D12_CACHED_PIPELINE_STATE). what makes a pipeline
// quick to create again is kept by the shader cache and by Metal's, by the shaders themselves, so a blob says only
// that it is one of these: a cache another driver gave is told apart, and the application drops it
inline constexpr char kCachedPipeline[] = "Mullion pipeline 1";

inline HRESULT
GetCachedPipelineBlob(ID3DBlob **ppBlob) {
  HRESULT hr = CreateBlobFromMalloc(sizeof(kCachedPipeline), ppBlob);
  if (SUCCEEDED(hr))
    memcpy((*ppBlob)->GetBufferPointer(), kCachedPipeline, sizeof(kCachedPipeline));
  return hr;
}

inline HRESULT
CheckCachedPipeline(const D3D12_CACHED_PIPELINE_STATE &Cached) {
  bool ours = Cached.CachedBlobSizeInBytes == sizeof(kCachedPipeline) &&
              !memcmp(Cached.pCachedBlob, kCachedPipeline, sizeof(kCachedPipeline));
  return ours || !Cached.CachedBlobSizeInBytes ? S_OK : D3D12_ERROR_DRIVER_VERSION_MISMATCH;
}

} // namespace dxmt