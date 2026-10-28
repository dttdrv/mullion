// a game's vertex or pixel shader: the d3dbc program it was made from and the version its first token names. the
// D3D11 shader it draws through is made at the draw, not here, because its input signature is the vertex format the
// draw reads (D3D9 gives a shader its inputs by register; which semantics those registers carry is the FVF's or the
// declaration's, which the device does not know when the shader is made).
#pragma once
#include "d3d9_common.hpp"
#include <vector>

namespace mullion::d3d9 {

// a d3dbc program of D3D9's: its tokens, whether it is a pixel shader's, and the version that names it
struct ShaderCode {
  std::vector<DWORD> tokens;
  bool pixel = false;
  UINT major = 0;
  UINT minor = 0;

  bool operator==(const ShaderCode &other) const {
    return pixel == other.pixel && major == other.major && minor == other.minor && tokens == other.tokens;
  }
};

// reads the version a d3dbc program starts with and checks that the program is one this build reads: a version token
// of a vertex or pixel shader, and a stream whose instructions end at D3DSIO_END. false for a program that is not,
// which CreateVertexShader and CreatePixelShader refuse as D3D9 does
bool read_shader_code(const DWORD *code, ShaderCode &out);

class MTLD3D9VertexShader final : private Object9, public IDirect3DVertexShader9 {
public:
  MTLD3D9VertexShader(IDirect3DDevice9 *device, ShaderCode code) : device_(device), code_(std::move(code)) {}

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override;
  ULONG STDMETHODCALLTYPE AddRef() override { return Object9::AddRef(); }
  ULONG STDMETHODCALLTYPE Release() override { return Object9::Release(); }

  // IDirect3DVertexShader9
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override;
  HRESULT STDMETHODCALLTYPE GetFunction(void *data, UINT *size) override;

  const ShaderCode &code() const { return code_; }

private:
  // the device that made the shader owns it, so this is not a reference (a cycle); GetDevice hands one out as D3D9's
  // contract has it
  IDirect3DDevice9 *device_ = nullptr;
  ShaderCode code_;
};

class MTLD3D9PixelShader final : private Object9, public IDirect3DPixelShader9 {
public:
  MTLD3D9PixelShader(IDirect3DDevice9 *device, ShaderCode code) : device_(device), code_(std::move(code)) {}

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override;
  ULONG STDMETHODCALLTYPE AddRef() override { return Object9::AddRef(); }
  ULONG STDMETHODCALLTYPE Release() override { return Object9::Release(); }

  // IDirect3DPixelShader9
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override;
  HRESULT STDMETHODCALLTYPE GetFunction(void *data, UINT *size) override;

  const ShaderCode &code() const { return code_; }

private:
  IDirect3DDevice9 *device_ = nullptr;
  ShaderCode code_;
};

} // namespace mullion::d3d9
