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

#include <optional>

#include "d3d9_device_child.hpp"
#include "mullion_buffer.hpp"
#include "mullion_dynamic.hpp"

namespace mullion {

class MTLD3D9Device;

class MTLD3D9BufferStorage {
public:
  // pool is the app buffer's; the device's own rings pass none
  MTLD3D9BufferStorage(MTLD3D9Device *pDevice, UINT Length, DWORD Usage, std::optional<D3DPOOL> Pool = std::nullopt);

  HRESULT Lock(UINT Offset, UINT Size, void **ppData, DWORD Flags);
  HRESULT Unlock();

  Rc<Buffer>
  buffer() const {
    return buffer_;
  }

  UINT
  length() const {
    return length_;
  }

  void
  MarkUsed(uint64_t SeqId) {
    last_used_seq_ = SeqId;
  }

private:
  MTLD3D9Device *device_;
  UINT length_;
  Rc<Buffer> buffer_;
  Rc<DynamicBuffer> dynamic_;
  uint32_t lock_count_ = 0;
  // only dynamic DEFAULT pool buffers of hardware vertex processing devices get fresh memory on DISCARD; the rest
  // are pinned and keep their pointer and contents (Wine's test_pinned_buffers)
  bool discardable_;
  uint64_t last_used_seq_ = 0;
};

class MTLD3D9VertexBuffer final : public MTLD3D9Resource<IDirect3DVertexBuffer9> {
public:
  MTLD3D9VertexBuffer(MTLD3D9Device *pDevice, const D3DVERTEXBUFFER_DESC &Desc);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final;
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final;
  HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) final;
  HRESULT STDMETHODCALLTYPE Unlock() final;
  HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC *pDesc) final;

  MTLD3D9BufferStorage &
  storage() {
    return storage_;
  }

private:
  D3DVERTEXBUFFER_DESC desc_;
  MTLD3D9BufferStorage storage_;
};

class MTLD3D9IndexBuffer final : public MTLD3D9Resource<IDirect3DIndexBuffer9> {
public:
  MTLD3D9IndexBuffer(MTLD3D9Device *pDevice, const D3DINDEXBUFFER_DESC &Desc);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final;
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() final;
  HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) final;
  HRESULT STDMETHODCALLTYPE Unlock() final;
  HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC *pDesc) final;

  MTLD3D9BufferStorage &
  storage() {
    return storage_;
  }

  const D3DINDEXBUFFER_DESC &
  Desc() const {
    return desc_;
  }

private:
  D3DINDEXBUFFER_DESC desc_;
  MTLD3D9BufferStorage storage_;
};

} // namespace mullion
