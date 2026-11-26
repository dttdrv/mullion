#include "d3d11_input_layout.hpp"
#include "d3d11_device_child.hpp"
#include "DXBCParser/DXBCUtils.h"
#include "d3d11_shader.hpp"
#include "d3d11_state_object.hpp"
#include "../d3d10/d3d10_shader.hpp"
#include "mullion_format.hpp"
#include "sha1/sha1_util.hpp"
#include "util_math.hpp"
#include "log/log.hpp"

namespace mullion {

HRESULT ExtractMTLInputLayoutElements(
    MTLD3D11Device *device, const void *pShaderBytecodeWithInputSignature,
    const D3D11_INPUT_ELEMENT_DESC *pInputElementDescs, uint32_t NumElements,
    MTL_SHADER_INPUT_LAYOUT_ELEMENT_DESC *pInputLayout,
    uint32_t *pNumElementsOut) {

  using namespace microsoft;
  uint16_t append_offset[32] = {0};
  uint32_t register_mask = 0;

  CSignatureParser parser;
  HRESULT hr =
      DXBCGetInputSignature(pShaderBytecodeWithInputSignature, &parser);
  if (FAILED(hr)) {
    return hr;
  }
  const D3D11_SIGNATURE_PARAMETER *pParameters;
  auto num_parameters = parser.GetParameters(&pParameters);

  UINT attribute_count = 0;
  for (UINT j = 0; j < NumElements; j++) {
    auto &desc = pInputElementDescs[j];

    MTL_DXGI_FORMAT_DESC metal_format;
    if (FAILED(MTLQueryDXGIFormat(device->GetMTLDevice(), desc.Format, metal_format))) {
      ERR("CreateInputLayout: Unsupported vertex format: ", desc.Format);
      return E_FAIL;
    }

    if (!metal_format.AttributeFormat) {
      ERR("CreateInputLayout: Unsupported vertex format: ", desc.Format);
      return E_INVALIDARG;
    }
    if (!metal_format.BytesPerTexel) {
      ERR("CreateInputLayout: not an ordinary or packed format: ", desc.Format);
      return E_INVALIDARG;
    }
    // a slot's elements step alike, in class and in rate (D3D11.3 8.1.3, D3D11_INPUT_ELEMENT_DESC), the shader's or not
    if (std::any_of(pInputElementDescs, &desc, [&](auto &other) {
          return other.InputSlot == desc.InputSlot && (other.InputSlotClass != desc.InputSlotClass ||
                                                       other.InstanceDataStepRate != desc.InstanceDataStepRate);
        }))
      return E_INVALIDARG;
    // an element is aligned to its format's size or to 4 bytes, whichever is less (D3D11.3 4.4.6), and Windows refuses
    // one that is not, whether the shader reads it or not (Wine's test_input_layout_alignment)
    auto alignment = std::min(4u, metal_format.BytesPerTexel);
    auto aligned_byte_offset = desc.AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT
                                   ? align(append_offset[desc.InputSlot], alignment)
                                   : desc.AlignedByteOffset;
    if (aligned_byte_offset % alignment)
      return E_INVALIDARG;
    append_offset[desc.InputSlot] = aligned_byte_offset + metal_format.BytesPerTexel;

    auto pSig = std::find_if(
        pParameters, pParameters + num_parameters,
        [&](const D3D11_SIGNATURE_PARAMETER &inputSig) {
          return desc.SemanticIndex == inputSig.SemanticIndex &&
                 strcasecmp(desc.SemanticName, inputSig.SemanticName) == 0;
        });
    if (pSig == pParameters + num_parameters)
      continue; // shader has no such input register, so skip it
    auto &inputSig = *pSig;
    auto &attribute = pInputLayout[attribute_count++];

    attribute.Format = metal_format.AttributeFormat;

    attribute.Slot = desc.InputSlot;
    attribute.Index = inputSig.Register;
    attribute.Offset = aligned_byte_offset;
    // the layout stride is provided in IASetVertexBuffer
    attribute.StepFunction = desc.InputSlotClass;
    attribute.InstanceStepRate =
        desc.InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA
            ? desc.InstanceDataStepRate
            : 1;
    register_mask |= (1 << inputSig.Register);
  }
  for (UINT i = 0; i < num_parameters; i++) {
    auto &inputSig = pParameters[i];
    // the input assembler makes these two itself. every other input needs an element, whatever stage the signature
    // is of (Wine's test_create_input_layout has it from Windows)
    if (!strcasecmp(inputSig.SemanticName, "SV_VertexID") || !strcasecmp(inputSig.SemanticName, "SV_InstanceID"))
      continue;
    if (!(register_mask & (1 << inputSig.Register))) {
      WARN("CreateInputLayout: Vertex shader expects ", inputSig.SemanticName,
           "_", inputSig.SemanticIndex, " but it's not in input layout element descriptors");
      return E_INVALIDARG;
    }
  }
  *pNumElementsOut = attribute_count;

  return S_OK;
}

class MTLD3D11StreamOutputLayout final
    : public ManagedDeviceChild<IMTLD3D11StreamOutputLayout> {
public:
  MTLD3D11StreamOutputLayout(MTLD3D11Device *device,
                             const MTL_STREAM_OUTPUT_DESC &desc)
      : ManagedDeviceChild<IMTLD3D11StreamOutputLayout>(device), desc_(desc),
        null_gs(this), d3d10(this) {
    Sha1HashState h;
    h.update(desc.Strides);
    h.update(desc.RasterizedStream);
    h.update(desc.Elements.size());
    for (auto &el : desc.Elements) {
        h.update(el);
    }
    sha1_ = h.final();
    null_gs.shader = desc.GeometryShader;
  }

  ~MTLD3D11StreamOutputLayout() {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
        riid == __uuidof(ID3D11GeometryShader) ||
        riid == __uuidof(IMTLD3D11StreamOutputLayout)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(IMTLD3D11Shader)) {
      *ppvObject = ref(&null_gs);
      return S_OK;
    }

    // what ID3D10Device::CreateGeometryShaderWithStreamOutput returns
    if (riid == __uuidof(ID3D10DeviceChild) || riid == __uuidof(ID3D10GeometryShader)) {
      *ppvObject = ref(&d3d10);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IMTLD3D11StreamOutputLayout), riid)) {
      WARN("IMTLD3D11StreamOutputLayout: Unknown interface query ",
           str::format(riid));
    }

    return E_NOINTERFACE;
  };

  uint32_t STDMETHODCALLTYPE
  GetStreamOutputElements(const SM50_STREAM_OUTPUT_ELEMENT2 **ppElements,
                          uint32_t Strides[4]) override {
    *ppElements = desc_.Elements.data();
    memcpy(Strides, desc_.Strides, sizeof(desc_.Strides));
    return desc_.Elements.size();
  }

  Sha1Digest &Digest() override {
    return sha1_;
  };

  virtual UINT RasterizedStream() override {
    return desc_.RasterizedStream;
  }

private:
  MTL_STREAM_OUTPUT_DESC desc_;
  Sha1Digest sha1_;

  class NullGeometryShader : public IMTLD3D11Shader {
    MTLD3D11StreamOutputLayout *container;

  public:
    NullGeometryShader(MTLD3D11StreamOutputLayout *container) : container(container) {};

    ULONG STDMETHODCALLTYPE AddRef() override { return container->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return container->Release(); }
    void AddRefPrivate() override { container->AddRefPrivate(); }
    void ReleasePrivate() override { container->ReleasePrivate(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
      return container->QueryInterface(riid, ppvObject);
    }

    virtual ManagedShader GetManagedShader() override { return shader; };
    ManagedShader shader = nullptr;
  };

  // the layout is the context's geometry shader: the real one it was created with, or none
  NullGeometryShader null_gs;
  MTLD3D10GeometryShader d3d10;
};

template <>
HRESULT StateObjectCache<MTL_STREAM_OUTPUT_DESC, IMTLD3D11StreamOutputLayout>::
    CreateStateObject(const MTL_STREAM_OUTPUT_DESC *pSOLayoutDesc,
                      IMTLD3D11StreamOutputLayout **ppSOLayout) {
  std::lock_guard<mullion::mutex> lock(mutex_cache);
  InitReturnPtr(ppSOLayout);

  if (!pSOLayoutDesc)
    return E_INVALIDARG;

  if (!ppSOLayout)
    return S_FALSE;

  if (cache.contains(*pSOLayoutDesc)) {
    cache.at(*pSOLayoutDesc)->QueryInterface(IID_PPV_ARGS(ppSOLayout));
    return S_OK;
  }

  cache.emplace(*pSOLayoutDesc, std::make_unique<MTLD3D11StreamOutputLayout>(
                                    device, *pSOLayoutDesc));
  cache.at(*pSOLayoutDesc)->QueryInterface(IID_PPV_ARGS(ppSOLayout));

  return S_OK;
}

} // namespace mullion