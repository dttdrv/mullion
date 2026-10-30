// a Direct3D 9 vertex or index buffer over a D3D11 buffer. every buffer this milestone makes is a dynamic D3D11 one,
// since a D3D9 buffer locks its bytes and a D3D11 buffer that is not dynamic cannot be mapped; the D3D9 contract's
// rules for DISCARD and NOOVERWRITE are applied from the D3D9 usage the buffer was made with.
#pragma once
#include "d3d9_common.hpp"
#include <vector>

namespace mullion::d3d9 {

extern const IID IID_IMTLD3D9Buffer;

// what the device asks a buffer for: the D3D11 buffer under it, how many bytes it has, and what it was made as
struct IMTLD3D9Buffer {
  virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
  virtual ULONG STDMETHODCALLTYPE Release() = 0;
  virtual ID3D11Buffer *buffer() = 0;
  virtual UINT size() = 0;
  virtual DWORD usage() = 0;
  virtual D3DFORMAT format() = 0;
};

// the bytes a D3D9 buffer locks: the buffer's whole contents, the CPU's while a lock is held, uploaded into the D3D11
// buffer when the lock is released. a D3D9 lock that does not discard the buffer keeps its contents, while a dynamic
// D3D11 buffer maps only for a whole-buffer discard or a write past what the GPU reads, so the wrapper keeps the bytes
// and Unlock writes all of them.
class Buffer9Storage {
public:
  Buffer9Storage(
      ComPtr<ID3D11Buffer> buffer, const ComPtr<ID3D11DeviceContext> &context, UINT size, DWORD usage, D3DFORMAT format
  );

  HRESULT lock(UINT offset, UINT bytes, void **data, DWORD flags);
  HRESULT unlock();

  ID3D11Buffer *buffer() const {
    return buffer_.Get();
  }
  UINT size() const {
    return size_;
  }
  DWORD usage() const {
    return usage_;
  }
  D3DFORMAT format() const {
    return format_;
  }

private:
  ComPtr<ID3D11Buffer> buffer_;
  ComPtr<ID3D11DeviceContext> context_;
  UINT size_ = 0;
  DWORD usage_ = 0;
  D3DFORMAT format_ = D3DFMT_UNKNOWN;
  std::vector<char> bytes_;
  bool locked_ = false;
};

class MTLD3D9VertexBuffer final : private Object9, public IDirect3DVertexBuffer9, public IMTLD3D9Buffer {
public:
  MTLD3D9VertexBuffer(
      IDirect3DDevice9 *device, ComPtr<ID3D11Buffer> buffer, const ComPtr<ID3D11DeviceContext> &context, UINT size,
      DWORD usage, D3DPOOL pool, DWORD fvf
  );

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override;
  ULONG STDMETHODCALLTYPE AddRef() override {
    return Object9::AddRef();
  }
  ULONG STDMETHODCALLTYPE Release() override {
    return Object9::Release();
  }

  ID3D11Buffer *buffer() override {
    return storage_.buffer();
  }
  UINT size() override {
    return storage_.size();
  }
  DWORD usage() override {
    return storage_.usage();
  }
  D3DFORMAT format() override {
    return storage_.format();
  }

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void *data, DWORD size, DWORD flags) override;
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void *data, DWORD *size) override;
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override;
  DWORD STDMETHODCALLTYPE SetPriority(DWORD priority) override;
  DWORD STDMETHODCALLTYPE GetPriority() override;
  void STDMETHODCALLTYPE PreLoad() override;
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override;

  HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT bytes, void **data, DWORD flags) override {
    return storage_.lock(offset, bytes, data, flags);
  }
  HRESULT STDMETHODCALLTYPE Unlock() override {
    return storage_.unlock();
  }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC *desc) override;

private:
  IDirect3DDevice9 *device_ = nullptr;
  DeviceChildRef device_ref_;
  PrivateData9 private_data_;
  DWORD fvf_ = 0;
  D3DPOOL pool_ = D3DPOOL_DEFAULT;
  Buffer9Storage storage_;
};

class MTLD3D9IndexBuffer final : private Object9, public IDirect3DIndexBuffer9, public IMTLD3D9Buffer {
public:
  MTLD3D9IndexBuffer(
      IDirect3DDevice9 *device, ComPtr<ID3D11Buffer> buffer, const ComPtr<ID3D11DeviceContext> &context, UINT size,
      DWORD usage, D3DPOOL pool, D3DFORMAT format
  );

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override;
  ULONG STDMETHODCALLTYPE AddRef() override {
    return Object9::AddRef();
  }
  ULONG STDMETHODCALLTYPE Release() override {
    return Object9::Release();
  }

  ID3D11Buffer *buffer() override {
    return storage_.buffer();
  }
  UINT size() override {
    return storage_.size();
  }
  DWORD usage() override {
    return storage_.usage();
  }
  D3DFORMAT format() override {
    return storage_.format();
  }

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void *data, DWORD size, DWORD flags) override;
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void *data, DWORD *size) override;
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override;
  DWORD STDMETHODCALLTYPE SetPriority(DWORD priority) override;
  DWORD STDMETHODCALLTYPE GetPriority() override;
  void STDMETHODCALLTYPE PreLoad() override;
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override;

  HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT bytes, void **data, DWORD flags) override {
    return storage_.lock(offset, bytes, data, flags);
  }
  HRESULT STDMETHODCALLTYPE Unlock() override {
    return storage_.unlock();
  }
  HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC *desc) override;

private:
  IDirect3DDevice9 *device_ = nullptr;
  DeviceChildRef device_ref_;
  PrivateData9 private_data_;
  D3DPOOL pool_ = D3DPOOL_DEFAULT;
  Buffer9Storage storage_;
};

} // namespace mullion::d3d9
