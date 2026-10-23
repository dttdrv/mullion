#pragma once
#include "com/com_pointer.hpp"
#include "d3d11_4.h"

namespace mullion {

/**
A context's ID3D11VideoContext. Every immediate context must expose it (Wine's dlls/d3d11/tests/d3d11.c,
test_immediate_context, queries it on all of them), and because ID3D11VideoContext inherits ID3D11DeviceChild its
device-child methods are the context's own: COM identity, private data and device are the container's, not a copy's.
https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nn-d3d11-id3d11videocontext

The rest of the interface operates on ID3D11VideoDevice's decoder, processor and authenticated-channel objects. This
device does not offer that interface, so those operations refuse with E_NOTIMPL instead of pretending success: an
application that can query the video device discovers the gap before it calls one.
*/
class D3D11VideoContext : public ID3D11VideoContext {
public:
  D3D11VideoContext(ID3D11DeviceChild *container, ID3D11Device *device) : container_(container), device_(device) {}

  HRESULT STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) final {
    if (ppvObject == nullptr)
      return E_POINTER;

    if (riid == __uuidof(ID3D11VideoContext)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    return container_->QueryInterface(riid, ppvObject);
  }

  ULONG STDMETHODCALLTYPE
  AddRef() final {
    return container_->AddRef();
  }

  ULONG STDMETHODCALLTYPE
  Release() final {
    return container_->Release();
  }

  void STDMETHODCALLTYPE
  GetDevice(ID3D11Device **ppDevice) final {
    *ppDevice = ref(device_);
  }

  HRESULT STDMETHODCALLTYPE
  GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData) final {
    return container_->GetPrivateData(guid, pDataSize, pData);
  }

  HRESULT STDMETHODCALLTYPE
  SetPrivateData(REFGUID guid, UINT DataSize, const void *pData) final {
    return container_->SetPrivateData(guid, DataSize, pData);
  }

  HRESULT STDMETHODCALLTYPE
  SetPrivateDataInterface(REFGUID guid, const IUnknown *pUnknown) final {
    return container_->SetPrivateDataInterface(guid, pUnknown);
  }

  HRESULT STDMETHODCALLTYPE
  GetDecoderBuffer(
      ID3D11VideoDecoder *decoder, D3D11_VIDEO_DECODER_BUFFER_TYPE type, UINT *buffer_size, void **buffer
  ) final {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE
  ReleaseDecoderBuffer(ID3D11VideoDecoder *decoder, D3D11_VIDEO_DECODER_BUFFER_TYPE type) final {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE
  DecoderBeginFrame(
      ID3D11VideoDecoder *decoder, ID3D11VideoDecoderOutputView *view, UINT key_size, const void *key
  ) final {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE
  DecoderEndFrame(ID3D11VideoDecoder *decoder) final {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE
  SubmitDecoderBuffers(
      ID3D11VideoDecoder *decoder, UINT buffers_count, const D3D11_VIDEO_DECODER_BUFFER_DESC *buffer_desc
  ) final {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE
  DecoderExtension(ID3D11VideoDecoder *decoder, const D3D11_VIDEO_DECODER_EXTENSION *extension) final {
    return E_NOTIMPL;
  }
  void STDMETHODCALLTYPE
  VideoProcessorSetOutputTargetRect(ID3D11VideoProcessor *processor, WINBOOL enable, const RECT *rect) final {}
  void STDMETHODCALLTYPE
  VideoProcessorSetOutputBackgroundColor(
      ID3D11VideoProcessor *processor, WINBOOL y_cb_cr, const D3D11_VIDEO_COLOR *color
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorSetOutputColorSpace(
      ID3D11VideoProcessor *processor, const D3D11_VIDEO_PROCESSOR_COLOR_SPACE *color_space
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorSetOutputAlphaFillMode(
      ID3D11VideoProcessor *processor, D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE alpha_fill_mode, UINT stream_idx
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorSetOutputConstriction(ID3D11VideoProcessor *processor, WINBOOL enable, SIZE size) final {}
  void STDMETHODCALLTYPE
  VideoProcessorSetOutputStereoMode(ID3D11VideoProcessor *processor, WINBOOL enable) final {}
  HRESULT STDMETHODCALLTYPE
  VideoProcessorSetOutputExtension(
      ID3D11VideoProcessor *processor, const GUID *guid, UINT data_size, void *data
  ) final {
    return E_NOTIMPL;
  }
  void STDMETHODCALLTYPE
  VideoProcessorGetOutputTargetRect(ID3D11VideoProcessor *processor, WINBOOL *enabled, RECT *rect) final {}
  void STDMETHODCALLTYPE
  VideoProcessorGetOutputBackgroundColor(
      ID3D11VideoProcessor *processor, WINBOOL *y_cb_cr, D3D11_VIDEO_COLOR *color
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorGetOutputColorSpace(
      ID3D11VideoProcessor *processor, D3D11_VIDEO_PROCESSOR_COLOR_SPACE *color_space
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorGetOutputAlphaFillMode(
      ID3D11VideoProcessor *processor, D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE *alpha_fill_mode, UINT *stream_idx
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorGetOutputConstriction(ID3D11VideoProcessor *processor, WINBOOL *enabled, SIZE *size) final {}
  void STDMETHODCALLTYPE
  VideoProcessorGetOutputStereoMode(ID3D11VideoProcessor *processor, WINBOOL *enabled) final {}
  HRESULT STDMETHODCALLTYPE
  VideoProcessorGetOutputExtension(
      ID3D11VideoProcessor *processor, const GUID *guid, UINT data_size, void *data
  ) final {
    return E_NOTIMPL;
  }
  void STDMETHODCALLTYPE
  VideoProcessorSetStreamFrameFormat(
      ID3D11VideoProcessor *processor, UINT stream_idx, D3D11_VIDEO_FRAME_FORMAT format
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorSetStreamColorSpace(
      ID3D11VideoProcessor *processor, UINT stream_idx, const D3D11_VIDEO_PROCESSOR_COLOR_SPACE *color_space
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorSetStreamOutputRate(
      ID3D11VideoProcessor *processor, UINT stream_idx, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE rate, WINBOOL repeat,
      const DXGI_RATIONAL *custom_rate
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorSetStreamSourceRect(
      ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL enable, const RECT *rect
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorSetStreamDestRect(
      ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL enable, const RECT *rect
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorSetStreamAlpha(ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL enable, float alpha) final {}
  void STDMETHODCALLTYPE
  VideoProcessorSetStreamPalette(
      ID3D11VideoProcessor *processor, UINT stream_idx, UINT entry_count, const UINT *entries
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorSetStreamPixelAspectRatio(
      ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL enable, const DXGI_RATIONAL *src_aspect_ratio,
      const DXGI_RATIONAL *dst_aspect_ratio
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorSetStreamLumaKey(
      ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL enable, float lower, float upper
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorSetStreamStereoFormat(
      ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL enable, D3D11_VIDEO_PROCESSOR_STEREO_FORMAT format,
      WINBOOL left_view_frame0, WINBOOL base_view_frame0, D3D11_VIDEO_PROCESSOR_STEREO_FLIP_MODE flip_mode,
      int mono_offset
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorSetStreamAutoProcessingMode(ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL enable) final {}
  void STDMETHODCALLTYPE
  VideoProcessorSetStreamFilter(
      ID3D11VideoProcessor *processor, UINT stream_idx, D3D11_VIDEO_PROCESSOR_FILTER filter, WINBOOL enable, int level
  ) final {}
  HRESULT STDMETHODCALLTYPE
  VideoProcessorSetStreamExtension(
      ID3D11VideoProcessor *processor, UINT stream_idx, const GUID *guid, UINT data_size, void *data
  ) final {
    return E_NOTIMPL;
  }
  void STDMETHODCALLTYPE
  VideoProcessorGetStreamFrameFormat(
      ID3D11VideoProcessor *processor, UINT stream_idx, D3D11_VIDEO_FRAME_FORMAT *format
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorGetStreamColorSpace(
      ID3D11VideoProcessor *processor, UINT stream_idx, D3D11_VIDEO_PROCESSOR_COLOR_SPACE *color_space
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorGetStreamOutputRate(
      ID3D11VideoProcessor *processor, UINT stream_idx, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE *rate, WINBOOL *repeat,
      DXGI_RATIONAL *custom_rate
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorGetStreamSourceRect(
      ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL *enabled, RECT *rect
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorGetStreamDestRect(
      ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL *enabled, RECT *rect
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorGetStreamAlpha(ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL *enabled, float *alpha) final {
  }
  void STDMETHODCALLTYPE
  VideoProcessorGetStreamPalette(
      ID3D11VideoProcessor *processor, UINT stream_idx, UINT entry_count, UINT *entries
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorGetStreamPixelAspectRatio(
      ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL *enabled, DXGI_RATIONAL *src_aspect_ratio,
      DXGI_RATIONAL *dst_aspect_ratio
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorGetStreamLumaKey(
      ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL *enabled, float *lower, float *upper
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorGetStreamStereoFormat(
      ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL *enabled, D3D11_VIDEO_PROCESSOR_STEREO_FORMAT *format,
      WINBOOL *left_view_frame0, WINBOOL *base_view_frame0, D3D11_VIDEO_PROCESSOR_STEREO_FLIP_MODE *flip_mode,
      int *mono_offset
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorGetStreamAutoProcessingMode(ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL *enabled) final {}
  void STDMETHODCALLTYPE
  VideoProcessorGetStreamFilter(
      ID3D11VideoProcessor *processor, UINT stream_idx, D3D11_VIDEO_PROCESSOR_FILTER filter, WINBOOL *enabled,
      int *level
  ) final {}
  HRESULT STDMETHODCALLTYPE
  VideoProcessorGetStreamExtension(
      ID3D11VideoProcessor *processor, UINT stream_idx, const GUID *guid, UINT data_size, void *data
  ) final {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE
  VideoProcessorBlt(
      ID3D11VideoProcessor *processor, ID3D11VideoProcessorOutputView *view, UINT frame_idx, UINT stream_count,
      const D3D11_VIDEO_PROCESSOR_STREAM *streams
  ) final {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE
  NegotiateCryptoSessionKeyExchange(ID3D11CryptoSession *session, UINT data_size, void *data) final {
    return E_NOTIMPL;
  }
  void STDMETHODCALLTYPE
  EncryptionBlt(
      ID3D11CryptoSession *session, ID3D11Texture2D *src_surface, ID3D11Texture2D *dst_surface, UINT iv_size, void *iv
  ) final {}
  void STDMETHODCALLTYPE
  DecryptionBlt(
      ID3D11CryptoSession *session, ID3D11Texture2D *src_surface, ID3D11Texture2D *dst_surface,
      D3D11_ENCRYPTED_BLOCK_INFO *block_info, UINT key_size, const void *key, UINT iv_size, void *iv
  ) final {}
  void STDMETHODCALLTYPE
  StartSessionKeyRefresh(ID3D11CryptoSession *session, UINT random_number_size, void *random_number) final {}
  void STDMETHODCALLTYPE
  FinishSessionKeyRefresh(ID3D11CryptoSession *session) final {}
  HRESULT STDMETHODCALLTYPE
  GetEncryptionBltKey(ID3D11CryptoSession *session, UINT key_size, void *key) final {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE
  NegotiateAuthenticatedChannelKeyExchange(ID3D11AuthenticatedChannel *channel, UINT data_size, void *data) final {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE
  QueryAuthenticatedChannel(
      ID3D11AuthenticatedChannel *channel, UINT input_size, const void *input, UINT output_size, void *output
  ) final {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE
  ConfigureAuthenticatedChannel(
      ID3D11AuthenticatedChannel *channel, UINT input_size, const void *input,
      D3D11_AUTHENTICATED_CONFIGURE_OUTPUT *output
  ) final {
    return E_NOTIMPL;
  }
  void STDMETHODCALLTYPE
  VideoProcessorSetStreamRotation(
      ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL enable, D3D11_VIDEO_PROCESSOR_ROTATION rotation
  ) final {}
  void STDMETHODCALLTYPE
  VideoProcessorGetStreamRotation(
      ID3D11VideoProcessor *processor, UINT stream_idx, WINBOOL *enable, D3D11_VIDEO_PROCESSOR_ROTATION *rotation
  ) final {}

private:
  ID3D11DeviceChild *const container_;
  ID3D11Device *const device_;
};

} // namespace mullion
