// a Direct3D 9 state block: the device state a game saves and restores around its own drawing, which every title's UI
// and its effects use. a block carries the states of the kind it was made for -- all device state, the pixel pipeline's,
// or the vertex pipeline's (Microsoft Learn, "Saving All Device States with a StateBlock", "Saving Pixel State ...",
// "Saving Vertex State ...") -- with the values they had when the block was made, when Capture re-read them, or when
// BeginStateBlock/EndStateBlock recorded them. what this build's device holds is what a block carries: the render,
// texture stage and sampler states it honours, the eight stages' textures, the shaders and their constant tables, the
// FVF or declaration, the stream source, the indices and the viewport. the state it has no use for yet (lights,
// transforms, materials, clip planes, a scissor rectangle) is neither saved nor restored, and Apply leaves it alone.
#pragma once
#include "d3d9_common.hpp"
#include <vector>

namespace mullion::d3d9 {

class MTLD3D9Device;

// which state a block carries: the kind, the state of that kind (a D3DRENDERSTATETYPE, a stage, a sampler, a register),
// and what the kind needs besides it (a texture stage state's type, a sampler state's, a constant range's length)
struct StateKey {
  enum Kind {
    Render,
    TextureStage,
    Sampler,
    Texture,
    VertexShader,
    PixelShader,
    VertexConstantF,
    VertexConstantI,
    VertexConstantB,
    PixelConstantF,
    PixelConstantI,
    PixelConstantB,
    FVF,
    Declaration,
    Stream,
    Indices,
    Viewport,
  };
  Kind kind = Render;
  UINT index = 0;
  UINT index2 = 0;

  bool operator==(const StateKey &other) const {
    return kind == other.kind && index == other.index && index2 == other.index2;
  }
};

// the value a state had: a word for a state, a register's four components for a constant, the object a texture, shader,
// declaration, stream or index buffer is, and the viewport's own structure
struct StateValue {
  DWORD value = 0;
  float floats[4] = {};
  int ints[4] = {};
  WINBOOL flags[4] = {};
  ComPtr<IUnknown> object;
  D3DVIEWPORT9 viewport{};
  UINT offset = 0;
  UINT stride = 0;
  // a constant range's data, which is longer than four components when several registers were set at once
  std::vector<float> float_range;
  std::vector<int> int_range;
  std::vector<WINBOOL> flag_range;
};

class MTLD3D9StateBlock final : private Object9, public IDirect3DStateBlock9 {
public:
  MTLD3D9StateBlock(MTLD3D9Device *device, D3DSTATEBLOCKTYPE type) : device_(device), type_(type) {}

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override;
  ULONG STDMETHODCALLTYPE AddRef() override { return Object9::AddRef(); }
  ULONG STDMETHODCALLTYPE Release() override { return Object9::Release(); }

  // IDirect3DStateBlock9
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override;
  HRESULT STDMETHODCALLTYPE Capture() override;
  HRESULT STDMETHODCALLTYPE Apply() override;

  // the states `type` names, with the values the device holds now: what CreateStateBlock makes a block of
  HRESULT capture_all();
  // the states a recording touched, with the values they have now: what EndStateBlock makes a block of
  HRESULT capture_recorded(const std::vector<StateKey> &keys);

private:
  // the value of one state, as the device holds it now
  HRESULT read(const StateKey &key, StateValue &value);
  // the device set back to one state's value
  HRESULT write(const StateKey &key, const StateValue &value);

  // the device that made the block owns it, so this is not a reference (a cycle); GetDevice hands one out as D3D9's
  // contract has it
  MTLD3D9Device *device_ = nullptr;
  D3DSTATEBLOCKTYPE type_ = D3DSBT_ALL;
  std::vector<StateKey> keys_;
  std::vector<StateValue> values_;
};

extern const IID IID_IMTLD3D9StateBlock;

} // namespace mullion::d3d9
