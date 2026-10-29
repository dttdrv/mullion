// a Direct3D 9 texture and the stage that samples it: the texture over a D3D11 texture, whose bytes are the CPU's
// while its pool lets the CPU write them (as a D3D9 buffer's are), the sampler its stage samples through, and the
// operation that stage performs on the texture. a level of a texture is a surface of its own, and a lock of one
// through the texture and through that surface is the same lock (IMTLD3D9LockTarget, d3d9_surface.hpp).
#pragma once
#include "d3d9_common.hpp"
#include "d3d9_surface.hpp"
#include <vector>

namespace mullion::d3d9 {

extern const IID IID_IMTLD3D9Texture;

// what the device asks a texture for: the view a draw samples it through, and whether its bytes are the CPU's (a
// managed texture's are, and a lock of one marks it for the upload its unlock does)
struct IMTLD3D9Texture {
  virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
  virtual ULONG STDMETHODCALLTYPE Release() = 0;
  virtual ID3D11ShaderResourceView *view() = 0;
  virtual bool managed() = 0;
};

// the bytes a D3D9 texture locks: a level's contents, the CPU's while a lock is held and uploaded into the texture when
// the lock is released. one level locks at a time, and unlocking the top level of a texture that has mip levels makes
// the levels below it out of it, which is what a managed texture of D3D9's does by itself.
class Texture9Storage {
public:
  Texture9Storage(
      ComPtr<ID3D11Texture2D> texture, const ComPtr<ID3D11ShaderResourceView> &view,
      const ComPtr<ID3D11DeviceContext> &context, D3DFORMAT format, bool lockable, bool generate_mips
  );

  HRESULT lock(UINT level, const RECT *rect, D3DLOCKED_RECT *locked, DWORD flags);
  HRESULT unlock();
  // the levels below the top one, made out of it: what a managed texture's own mipmapping is
  HRESULT generate_mips();
  bool locked() const {
    return locked_level_ != kNoLevel;
  }
  UINT locked_level() const {
    return locked_level_;
  }
  UINT level_count() const {
    return (UINT)levels_.size();
  }

private:
  static constexpr UINT kNoLevel = UINT_MAX;

  struct Level {
    std::vector<char> bytes;
    UINT pitch = 0;
    UINT width = 0, height = 0;
  };

  ComPtr<ID3D11Texture2D> texture_;
  ComPtr<ID3D11ShaderResourceView> view_;
  ComPtr<ID3D11DeviceContext> context_;
  std::vector<Level> levels_;
  UINT locked_level_ = kNoLevel;
  bool read_only_ = false;
  UINT pixel_bytes_ = 0;
  // how many bytes a four-by-four block of a compressed format takes, 0 for a format stored as pixels
  UINT block_bytes_ = 0;
  // whether the levels below the top one are made out of it (a texture this build could give the mip-generating flag)
  bool generate_mips_ = false;
};

class MTLD3D9Texture final : private Object9,
                             public IDirect3DTexture9,
                             public IMTLD3D9Texture,
                             public IMTLD3D9LockTarget {
public:
  MTLD3D9Texture(
      IDirect3DDevice9 *device, ComPtr<ID3D11Texture2D> texture, ComPtr<ID3D11ShaderResourceView> view,
      const ComPtr<ID3D11DeviceContext> &context, D3DFORMAT format, D3DPOOL pool, UINT levels,
      bool generate_mips
  );
  // the surfaces of its levels, once the texture is complete (they hold this object as their lock's target)
  void set_levels(std::vector<ComPtr<MTLD3D9Surface>> levels) {
    levels_ = std::move(levels);
  }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override;
  ULONG STDMETHODCALLTYPE AddRef() override {
    return Object9::AddRef();
  }
  ULONG STDMETHODCALLTYPE Release() override {
    return Object9::Release();
  }

  // IMTLD3D9Texture
  ID3D11ShaderResourceView *view() override {
    return view_.Get();
  }
  bool managed() override {
    return pool_ == D3DPOOL_MANAGED;
  }

  // IMTLD3D9LockTarget: a level of this texture locked through its surface
  HRESULT lock_level(UINT level, const RECT *rect, D3DLOCKED_RECT *locked, DWORD flags) override;
  HRESULT unlock_level(UINT level) override;

  // IDirect3DResource9
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, const void *data, DWORD size, DWORD flags) override;
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, void *data, DWORD *size) override;
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID guid) override;
  DWORD STDMETHODCALLTYPE SetPriority(DWORD priority) override;
  DWORD STDMETHODCALLTYPE GetPriority() override;
  void STDMETHODCALLTYPE PreLoad() override;
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override;

  // IDirect3DBaseTexture9
  DWORD STDMETHODCALLTYPE SetLOD(DWORD lod) override;
  DWORD STDMETHODCALLTYPE GetLOD() override;
  DWORD STDMETHODCALLTYPE GetLevelCount() override;
  HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE filter_type) override;
  D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override;
  void STDMETHODCALLTYPE GenerateMipSubLevels() override;

  // IDirect3DTexture9
  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC *desc) override;
  HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT level, IDirect3DSurface9 **surface_level) override;
  HRESULT STDMETHODCALLTYPE LockRect(UINT level, D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags) override;
  HRESULT STDMETHODCALLTYPE UnlockRect(UINT level) override;
  HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT *dirty_rect) override;

  D3DFORMAT format() const {
    return format_;
  }
  D3DPOOL pool() const {
    return pool_;
  }

private:
  IDirect3DDevice9 *device_ = nullptr;
  ComPtr<ID3D11Texture2D> texture_;
  ComPtr<ID3D11ShaderResourceView> view_;
  std::vector<ComPtr<MTLD3D9Surface>> levels_;
  Texture9Storage storage_;
  D3DFORMAT format_ = D3DFMT_UNKNOWN;
  D3DPOOL pool_ = D3DPOOL_DEFAULT;
  DWORD levels_count_ = 1;
};

// the sampler a texture stage samples through: D3D9's sampler states as the D3D11 sampler they make, made on demand and
// remade when a value changes. a value D3D11 has no sampler for is refused rather than sampled as something else.
class SamplerState9 {
public:
  static bool supported(D3DSAMPLERSTATETYPE type);
  static bool usable_value(D3DSAMPLERSTATETYPE type, DWORD value);
  // every type the device takes, which is what a state block enumerates
  static const D3DSAMPLERSTATETYPE *all_types(UINT &count);

  HRESULT set(D3DSAMPLERSTATETYPE type, DWORD value);
  HRESULT get(D3DSAMPLERSTATETYPE type, DWORD *value) const;
  // the sampler the values make, or nullptr when the device would not make one; the three filter picks become the
  // closest D3D11 filter to them rather than a refusal (d3d9_texture.cpp)
  ID3D11SamplerState *state(ID3D11Device *device);

private:
  // what D3D9's device starts with (its documented defaults for these states)
  D3DTEXTUREFILTERTYPE min_filter_ = D3DTEXF_POINT;
  D3DTEXTUREFILTERTYPE mag_filter_ = D3DTEXF_POINT;
  D3DTEXTUREFILTERTYPE mip_filter_ = D3DTEXF_NONE;
  D3DTEXTUREADDRESS address_u_ = D3DTADDRESS_WRAP;
  D3DTEXTUREADDRESS address_v_ = D3DTADDRESS_WRAP;
  D3DTEXTUREADDRESS address_w_ = D3DTADDRESS_WRAP;
  DWORD max_anisotropy_ = 1;

  ComPtr<ID3D11SamplerState> sampler_;
  bool current_ = false;
};

// the operation texture stage 0 performs: D3D9's colour and alpha operations and the arguments they take, as the
// constants the textured pixel shader reads (d3d9_fixed_function_shaders.hpp). the operations this build honours are
// the ones a fixed-function frame needs; every other operation, argument and modifier is refused rather than drawn as
// a different one.
class TextureStageState9 {
public:
  static bool supported(D3DTEXTURESTAGESTATETYPE type);
  static bool usable_value(D3DTEXTURESTAGESTATETYPE type, DWORD value);
  // every type the device takes, which is what a state block enumerates
  static const D3DTEXTURESTAGESTATETYPE *all_types(UINT &count);

  HRESULT set(D3DTEXTURESTAGESTATETYPE type, DWORD value);
  HRESULT get(D3DTEXTURESTAGESTATETYPE type, DWORD *value) const;

  // the shader constants the operations and arguments make: ops is colour op, colour arg1, colour arg2 and alpha op,
  // args is alpha arg1 and alpha arg2
  void constants(UINT32 ops[4], UINT32 args[4]) const;
  // whether this stage is skipped while no texture is bound: "when a texture stage has D3DTSS_COLORARG1 equal to
  // D3DTA_TEXTURE and the texture pointer for the stage is NULL, this stage and all stages after it are not processed"
  // (Microsoft Learn, Texture Blending), which for stage 0 is the vertex colour the frame already carries
  bool skipped_without_texture() const {
    return colour_arg1_ == D3DTA_TEXTURE;
  }

private:
  // what D3D9's device starts with for stage 0 (its documented defaults: MODULATE of the texture and the current
  // colour, and the texture's own alpha)
  DWORD colour_op_ = D3DTOP_MODULATE;
  DWORD colour_arg1_ = D3DTA_TEXTURE;
  DWORD colour_arg2_ = D3DTA_CURRENT;
  DWORD alpha_op_ = D3DTOP_SELECTARG1;
  DWORD alpha_arg1_ = D3DTA_TEXTURE;
  DWORD alpha_arg2_ = D3DTA_CURRENT;
};

} // namespace mullion::d3d9
