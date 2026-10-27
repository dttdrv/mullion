#include "d3d9_buffer.hpp"
#include <cstring>

namespace mullion::d3d9 {

const IID IID_IMTLD3D9Buffer = {0x2f6a5d4c, 0x8b1e, 0x4c07, {0x9a, 0x63, 0x1d, 0x7e, 0x52, 0xb0, 0x8c, 0x45}};

Buffer9Storage::Buffer9Storage(
    ComPtr<ID3D11Buffer> buffer, const ComPtr<ID3D11DeviceContext> &context, UINT size, DWORD usage, D3DFORMAT format
)
    : buffer_(std::move(buffer)), context_(context), size_(size), usage_(usage), format_(format) {
  // the bytes the D3D11 buffer has, which are whole units of 16 where the D3D9 buffer's length need not be
  D3D11_BUFFER_DESC desc{};
  buffer_->GetDesc(&desc);
  bytes_.resize(desc.ByteWidth);
}

// a lock's flags and range, as D3D9's contract has them: DISCARD and NOOVERWRITE are a dynamic buffer's alone and not
// both at once, a buffer locks once until it is unlocked, and a lock names the bytes it maps within the buffer
HRESULT
Buffer9Storage::lock(UINT offset, UINT bytes, void **data, DWORD flags) {
  if (!data)
    return D3DERR_INVALIDCALL;
  *data = nullptr;
  if (locked_)
    return D3DERR_INVALIDCALL;
  if ((flags & D3DLOCK_DISCARD) && (flags & D3DLOCK_NOOVERWRITE))
    return D3DERR_INVALIDCALL;
  if ((flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) && !(usage_ & D3DUSAGE_DYNAMIC))
    return D3DERR_INVALIDCALL;
  // these buffers are written, not read: their D3D11 buffers are made for writing alone
  if (flags & D3DLOCK_READONLY)
    return D3DERR_INVALIDCALL;
  if (offset > size_)
    return D3DERR_INVALIDCALL;
  if (!bytes)
    bytes = size_ - offset;
  if (bytes > size_ - offset)
    return D3DERR_INVALIDCALL;

  // DISCARD says the old contents need not be there and NOOVERWRITE says the GPU will not read them: either way the
  // CPU writes these bytes, and Unlock is what makes them the buffer's
  *data = bytes_.data() + offset;
  locked_ = true;
  return S_OK;
}

HRESULT
Buffer9Storage::unlock() {
  if (!locked_)
    return D3DERR_INVALIDCALL;

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context_->Map(buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    return D3DERR_INVALIDCALL;
  memcpy(mapped.pData, bytes_.data(), bytes_.size());
  context_->Unmap(buffer_.Get(), 0);
  locked_ = false;
  return S_OK;
}

MTLD3D9VertexBuffer::MTLD3D9VertexBuffer(
    IDirect3DDevice9 *device, ComPtr<ID3D11Buffer> buffer, const ComPtr<ID3D11DeviceContext> &context, UINT size,
    DWORD usage, D3DPOOL pool, DWORD fvf
)
    : device_(device), fvf_(fvf), pool_(pool),
      storage_(std::move(buffer), context, size, usage, D3DFMT_VERTEXDATA) {}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::QueryInterface(REFIID riid, void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (riid == IID_IUnknown || riid == __uuidof(IDirect3DResource9) || riid == __uuidof(IDirect3DVertexBuffer9)) {
    *object = static_cast<IDirect3DVertexBuffer9 *>(this);
  } else if (riid == IID_IMTLD3D9Buffer) {
    *object = static_cast<IMTLD3D9Buffer *>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::GetDevice(IDirect3DDevice9 **device) {
  if (!device)
    return D3DERR_INVALIDCALL;
  *device = device_;
  device_->AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::SetPrivateData(REFGUID, const void *, DWORD, DWORD) {
  return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::GetPrivateData(REFGUID, void *, DWORD *) {
  return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::FreePrivateData(REFGUID) {
  return D3DERR_INVALIDCALL;
}

DWORD STDMETHODCALLTYPE
MTLD3D9VertexBuffer::SetPriority(DWORD) {
  return 0;
}

DWORD STDMETHODCALLTYPE
MTLD3D9VertexBuffer::GetPriority() {
  return 0;
}

void STDMETHODCALLTYPE
MTLD3D9VertexBuffer::PreLoad() {}

D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9VertexBuffer::GetType() {
  return D3DRTYPE_VERTEXBUFFER;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC *desc) {
  if (!desc)
    return D3DERR_INVALIDCALL;
  desc->Format = D3DFMT_VERTEXDATA;
  desc->Type = D3DRTYPE_VERTEXBUFFER;
  desc->Usage = storage_.usage();
  desc->Pool = pool_;
  desc->Size = storage_.size();
  desc->FVF = fvf_;
  return S_OK;
}

MTLD3D9IndexBuffer::MTLD3D9IndexBuffer(
    IDirect3DDevice9 *device, ComPtr<ID3D11Buffer> buffer, const ComPtr<ID3D11DeviceContext> &context, UINT size,
    DWORD usage, D3DPOOL pool, D3DFORMAT format
)
    : device_(device), pool_(pool), storage_(std::move(buffer), context, size, usage, format) {}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::QueryInterface(REFIID riid, void **object) {
  if (!object)
    return E_POINTER;
  *object = nullptr;
  if (riid == IID_IUnknown || riid == __uuidof(IDirect3DResource9) || riid == __uuidof(IDirect3DIndexBuffer9)) {
    *object = static_cast<IDirect3DIndexBuffer9 *>(this);
  } else if (riid == IID_IMTLD3D9Buffer) {
    *object = static_cast<IMTLD3D9Buffer *>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::GetDevice(IDirect3DDevice9 **device) {
  if (!device)
    return D3DERR_INVALIDCALL;
  *device = device_;
  device_->AddRef();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::SetPrivateData(REFGUID, const void *, DWORD, DWORD) {
  return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::GetPrivateData(REFGUID, void *, DWORD *) {
  return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::FreePrivateData(REFGUID) {
  return D3DERR_INVALIDCALL;
}

DWORD STDMETHODCALLTYPE
MTLD3D9IndexBuffer::SetPriority(DWORD) {
  return 0;
}

DWORD STDMETHODCALLTYPE
MTLD3D9IndexBuffer::GetPriority() {
  return 0;
}

void STDMETHODCALLTYPE
MTLD3D9IndexBuffer::PreLoad() {}

D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9IndexBuffer::GetType() {
  return D3DRTYPE_INDEXBUFFER;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::GetDesc(D3DINDEXBUFFER_DESC *desc) {
  if (!desc)
    return D3DERR_INVALIDCALL;
  desc->Format = storage_.format();
  desc->Type = D3DRTYPE_INDEXBUFFER;
  desc->Usage = storage_.usage();
  desc->Pool = pool_;
  desc->Size = storage_.size();
  return S_OK;
}

} // namespace mullion::d3d9
