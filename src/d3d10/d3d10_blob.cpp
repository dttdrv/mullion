#include "d3d10_blob.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"

namespace dxmt {

class MallocBlob : public ComObject<ID3D10Blob> {
public:
  MallocBlob(SIZE_T Size) : size_(Size) {
    data_ = malloc(Size);
  }
  ~MallocBlob() {
    if (data_)
      free(data_);
    data_ = nullptr;
    size_ = 0;
  }

  HRESULT STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    InitReturnPtr(ppvObject);

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D10Blob)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    return E_NOINTERFACE;
  };

  void *STDMETHODCALLTYPE
  GetBufferPointer() final {
    return data_;
  }

  SIZE_T STDMETHODCALLTYPE
  GetBufferSize() final {
    return size_;
  };

private:
  void *data_;
  SIZE_T size_;
};

HRESULT
CreateBlobFromMalloc(SIZE_T Size, ID3D10Blob **ppBlob) {
  auto blob = Com(new MallocBlob(Size));
  if (!blob->GetBufferPointer())
    return E_OUTOFMEMORY;
  return blob->QueryInterface(IID_PPV_ARGS(ppBlob));
}

} // namespace dxmt