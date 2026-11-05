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

#include "d3d9_buffer.hpp"
#include "d3d9_device.hpp"
#include "log/log.hpp"
#include "util_string.hpp"

namespace mullion {

MTLD3D9BufferStorage::MTLD3D9BufferStorage(
    MTLD3D9Device *pDevice, UINT Length, DWORD Usage, std::optional<D3DPOOL> Pool
) :
    device_(pDevice),
    length_(Length),
    discardable_(
        !Pool || ((Usage & D3DUSAGE_DYNAMIC) && *Pool == D3DPOOL_DEFAULT &&
                  !(pDevice->BehaviorFlags() & D3DCREATE_SOFTWARE_VERTEXPROCESSING))
    ) {
  buffer_ = new Buffer(Length, pDevice->GetMTLDevice());

  Flags<BufferAllocationFlag> flags;
  if (Usage & D3DUSAGE_WRITEONLY)
    flags.set(BufferAllocationFlag::CpuWriteCombined);
  if (Usage & D3DUSAGE_DYNAMIC)
    flags.set(BufferAllocationFlag::SuballocateFromOnePage);
#ifdef __i386__
  flags.set(BufferAllocationFlag::CpuPlaced);
#endif

  auto allocation = buffer_->allocate(flags);
  std::memset(allocation->mappedMemory(0), 0, Length);
  auto _ = buffer_->rename(std::move(allocation));
  dynamic_ = new DynamicBuffer(buffer_.ptr(), flags);
}

HRESULT
MTLD3D9BufferStorage::Lock(UINT Offset, UINT Size, void **ppData, DWORD Flags) {
  if (!ppData)
    return D3DERR_INVALIDCALL;
  *ppData = nullptr;

  if (Offset > length_ || (Size && Offset + Size > length_))
    return D3DERR_INVALIDCALL;

  auto &queue = device_->queue();
  if (!discardable_ || (Flags & D3DLOCK_NOOVERWRITE))
    Flags &= ~D3DLOCK_DISCARD;
  if ((Flags & D3DLOCK_DISCARD) && !lock_count_) {
    if (auto next = dynamic_->nextSuballocation()) {
      device_->Emit([allocation = dynamic_->immediateName(), next](ArgumentEncodingContext &enc) mutable {
        allocation->useSuballocation(next);
      });
    } else {
      dynamic_->updateImmediateName(queue.CurrentSeqId(), dynamic_->allocate(queue.CoherentSeqId()), 0, false);
      device_->Emit([allocation = dynamic_->immediateName(), buffer = buffer_](ArgumentEncodingContext &enc) mutable {
        allocation->useSuballocation(0);
        auto _ = buffer->rename(std::move(allocation));
      });
    }
  } else if (!(Flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY)) && last_used_seq_ > queue.CoherentSeqId()) {
    if (Flags & D3DLOCK_DONOTWAIT)
      return D3DERR_WASSTILLDRAWING;
    device_->FlushCommands();
    queue.WaitCPUFence(last_used_seq_);
  }

  *ppData = (char *)dynamic_->immediateMappedMemory() + Offset;
  lock_count_++;
  return D3D_OK;
}

HRESULT
MTLD3D9BufferStorage::Unlock() {
  if (!lock_count_)
    return D3DERR_INVALIDCALL;
  lock_count_--;
  return D3D_OK;
}

MTLD3D9VertexBuffer::MTLD3D9VertexBuffer(MTLD3D9Device *pDevice, const D3DVERTEXBUFFER_DESC &Desc) :
    MTLD3D9Resource(pDevice, Desc.Pool),
    desc_(Desc),
    storage_(pDevice, Desc.Size, Desc.Usage, Desc.Pool) {}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;
  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) ||
      riid == __uuidof(IDirect3DVertexBuffer9)) {
    *ppvObject = ref(this);
    return S_OK;
  }
  WARN("IDirect3DVertexBuffer9::QueryInterface: Unknown interface query ", str::format(riid));
  return E_NOINTERFACE;
}

D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9VertexBuffer::GetType() {
  return D3DRTYPE_VERTEXBUFFER;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) {
  return storage_.Lock(OffsetToLock, SizeToLock, ppbData, Flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::Unlock() {
  return storage_.Unlock();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC *pDesc) {
  if (!pDesc)
    return D3DERR_INVALIDCALL;
  *pDesc = desc_;
  return D3D_OK;
}

MTLD3D9IndexBuffer::MTLD3D9IndexBuffer(MTLD3D9Device *pDevice, const D3DINDEXBUFFER_DESC &Desc) :
    MTLD3D9Resource(pDevice, Desc.Pool),
    desc_(Desc),
    storage_(pDevice, Desc.Size, Desc.Usage, Desc.Pool) {}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;
  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) ||
      riid == __uuidof(IDirect3DIndexBuffer9)) {
    *ppvObject = ref(this);
    return S_OK;
  }
  WARN("IDirect3DIndexBuffer9::QueryInterface: Unknown interface query ", str::format(riid));
  return E_NOINTERFACE;
}

D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9IndexBuffer::GetType() {
  return D3DRTYPE_INDEXBUFFER;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) {
  return storage_.Lock(OffsetToLock, SizeToLock, ppbData, Flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::Unlock() {
  return storage_.Unlock();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::GetDesc(D3DINDEXBUFFER_DESC *pDesc) {
  if (!pDesc)
    return D3DERR_INVALIDCALL;
  *pDesc = desc_;
  return D3D_OK;
}

} // namespace mullion
