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

// D3D9's alpha test as a pixel shader carries it: whether D3DRS_ALPHATESTENABLE is on, and the comparison
// D3DRS_ALPHAFUNC names. Direct3D 11 has no alpha test of its own, so a program translated with one enabled ends every
// pixel with the comparison and a discard; the reference (D3DRS_ALPHAREF) is a per-draw value, so the program reads it
// from a constant buffer of its own (register b3) rather than carrying it
struct AlphaTest {
  bool enabled = false;
  DWORD func = D3DCMP_ALWAYS;

  bool operator==(const AlphaTest &other) const {
    return enabled == other.enabled && (!enabled || func == other.func);
  }
};

// the register a pixel shader reads one of its inputs at: the compiled programs link the two stages by register (a
// varying is named after its register and component), so a vertex shader has to write each output where the pixel
// shader it draws with reads it
struct PixelInputSlot {
  std::string semantic;
  UINT index = 0;
  UINT reg = 0;
};

// one vertex declaration element, as the vertex shader sees it: the input register it feeds and what it means
struct VertexInput {
  UINT reg = 0;
  UINT usage = 0; // D3DDECLUSAGE_*
  UINT usage_index = 0;
  UINT mask = 0xF; // the components the element carries
};

// what the device asks a vertex shader of this build for: the D3D11 pipeline it draws with through a declaration
struct IMTLD3D9VertexShader {
  virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
  virtual ULONG STDMETHODCALLTYPE Release() = 0;
  virtual HRESULT compiled(
      ID3D11Device *device, const std::vector<D3DVERTEXELEMENT9> &declaration, const std::vector<VertexInput> &inputs,
      const std::vector<PixelInputSlot> &pixel_inputs, const std::vector<D3D11_INPUT_ELEMENT_DESC> &layout,
      ID3D11VertexShader **shader, ID3D11InputLayout **input_layout
  ) = 0;
};

// and what it asks a pixel shader for: the D3D11 pixel shader its program is translated to, for the alpha test the draw
// is made under (a program with the test is a different program from the same one without it, and both are kept)
struct IMTLD3D9PixelShader {
  virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
  virtual ULONG STDMETHODCALLTYPE Release() = 0;
  virtual HRESULT compiled(ID3D11Device *device, const AlphaTest &alpha, ID3D11PixelShader **shader) = 0;
  // the registers this shader reads its inputs at, which the vertex shader it draws with writes to
  virtual const std::vector<PixelInputSlot> &pixel_inputs() = 0;
  // whether the program samples a texture: the draw binds the texture stage 0 holds (and its sampler) only when it
  // does, so a program that never samples never makes a render target it draws into a shader input too
  virtual bool samples() = 0;
};

extern const IID IID_IMTLD3D9VertexShader;
extern const IID IID_IMTLD3D9PixelShader;

class MTLD3D9VertexShader final : private Object9, public IDirect3DVertexShader9, public IMTLD3D9VertexShader {
public:
  MTLD3D9VertexShader(IDirect3DDevice9 *device, ShaderCode code)
      : device_(device), device_ref_(device), code_(std::move(code)) {}

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override;
  ULONG STDMETHODCALLTYPE AddRef() override { return Object9::AddRef(); }
  ULONG STDMETHODCALLTYPE Release() override { return Object9::Release(); }

  // IDirect3DVertexShader9
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override;
  HRESULT STDMETHODCALLTYPE GetFunction(void *data, UINT *size) override;

  // IMTLD3D9VertexShader: the D3D11 vertex shader and input layout this program draws with through `declaration`,
  // translated the first time a draw uses the pair and kept for every draw after it
  HRESULT compiled(
      ID3D11Device *device, const std::vector<D3DVERTEXELEMENT9> &declaration, const std::vector<VertexInput> &inputs,
      const std::vector<PixelInputSlot> &pixel_inputs, const std::vector<D3D11_INPUT_ELEMENT_DESC> &layout,
      ID3D11VertexShader **shader, ID3D11InputLayout **input_layout
  ) override;

private:
  // the device that made the shader is held through a private reference (DeviceChildRef), so a shader kept after the
  // application released its device still has one to answer GetDevice with
  IDirect3DDevice9 *device_ = nullptr;
  DeviceChildRef device_ref_;
  ShaderCode code_;

  // one D3D11 pipeline per vertex declaration a draw has used the shader with
  struct Variant {
    std::vector<D3DVERTEXELEMENT9> declaration;
    std::vector<PixelInputSlot> pixel_inputs;
    ComPtr<ID3D11VertexShader> shader;
    ComPtr<ID3D11InputLayout> layout;
  };
  std::vector<Variant> variants_;
};

class MTLD3D9PixelShader final : private Object9, public IDirect3DPixelShader9, public IMTLD3D9PixelShader {
public:
  MTLD3D9PixelShader(IDirect3DDevice9 *device, ShaderCode code)
      : device_(device), device_ref_(device), code_(std::move(code)) {}

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override;
  ULONG STDMETHODCALLTYPE AddRef() override { return Object9::AddRef(); }
  ULONG STDMETHODCALLTYPE Release() override { return Object9::Release(); }

  // IDirect3DPixelShader9
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override;
  HRESULT STDMETHODCALLTYPE GetFunction(void *data, UINT *size) override;

  // IMTLD3D9PixelShader: the D3D11 pixel shader this program draws through, translated the first time a draw uses it
  // with that alpha test, and kept for every draw after it
  HRESULT compiled(ID3D11Device *device, const AlphaTest &alpha, ID3D11PixelShader **shader) override;
  const std::vector<PixelInputSlot> &pixel_inputs() override;
  bool samples() override {
    return samples_;
  }
  // whether a translation of this program carries the alpha test, which is what a draw binds the reference for
  bool alpha_tested() const {
    return alpha_tested_;
  }

private:
  IDirect3DDevice9 *device_ = nullptr;
  DeviceChildRef device_ref_;
  ShaderCode code_;
  // one translated program per alpha test a draw has used it with: the program itself is the same bytes, and the test is
  // the program's end
  struct Variant {
    AlphaTest alpha;
    ComPtr<ID3D11PixelShader> shader;
  };
  std::vector<Variant> variants_;
  std::vector<PixelInputSlot> pixel_inputs_;
  bool samples_ = false;
  bool alpha_tested_ = false;
};

} // namespace mullion::d3d9
