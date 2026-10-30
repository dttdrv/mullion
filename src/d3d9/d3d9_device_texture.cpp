// the texture stage a Direct3D 9 draw samples through: the textures the device makes, the one stage 0 has set, the
// sampler that stage samples with and the operation it performs (the texture objects are d3d9_texture.cpp's, and the
// draw that binds them d3d9_device_draw.cpp's).
#include "d3d9_device.hpp"
#include <algorithm>
#include <vector>

namespace mullion::d3d9 {

// a texture a draw samples: a D3D11 texture over the same device, whose bytes are the CPU's while its pool lets the
// CPU write them (a managed texture's are), with the levels asked for and the view a draw samples it through. a level
// of it is a surface of this device's, and locking one through that surface is the same lock as through the texture.
HRESULT
MTLD3D9Device::CreateTexture(
    UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9 **texture,
    HANDLE *shared_handle
) {
  if (!texture || shared_handle)
    return D3DERR_INVALIDCALL;
  *texture = nullptr;
  const DXGI_FORMAT dxgi_format = to_dxgi_format(format);
  if (dxgi_format == DXGI_FORMAT_UNKNOWN || !width || !height || width > caps_.MaxTextureWidth ||
      height > caps_.MaxTextureHeight) {
    ERR_ONCE(
        "D3D9: no texture of this format (", format, ") or this size (", width, "x", height, "): the device makes up to ",
        caps_.MaxTextureWidth, "x", caps_.MaxTextureHeight, "."
    );
    return D3DERR_INVALIDCALL;
  }
  // a compressed texture is stored as four-by-four blocks, so D3D9 makes only ones whose size is whole blocks
  if (block_size(format) && ((width & 3) || (height & 3))) {
    ERR_ONCE(
        "D3D9: a compressed texture is made of whole four-by-four blocks, so its size is a multiple of four; ", width,
        "x", height, " is not."
    );
    return D3DERR_INVALIDCALL;
  }
  if (pool != D3DPOOL_DEFAULT && pool != D3DPOOL_MANAGED) {
    ERR_ONCE("D3D9: only a default-pool and a managed texture are made yet: a system-memory texture is a copy this "
             "build has none of.");
    return D3DERR_INVALIDCALL;
  }
  // a default-pool resource belongs to the device the frames failed on, which a Reset replaces: one made while the
  // device is lost would be a resource of the device that is gone (IDirect3DDevice9::Reset). A managed texture is the
  // CPU's and is made as it always is
  if (pool == D3DPOOL_DEFAULT) {
    if (HRESULT lost = lost_reason(); FAILED(lost))
      return lost;
  }
  // A render-target texture is one a frame is drawn into and later samples: it is a default-pool resource with a render
  // target view of each level. A dynamic texture is one the CPU streams into, locked with DISCARD or NOOVERWRITE: it is
  // a default-pool resource whose levels are mapped for writing. The two cannot be combined, and the other usage flags
  // are refused rather than kept by nobody (Microsoft Learn, D3DUSAGE_RENDERTARGET, D3DUSAGE_DYNAMIC).
  if (usage & ~(DWORD)(D3DUSAGE_RENDERTARGET | D3DUSAGE_DYNAMIC)) {
    ERR_ONCE(
        "D3D9: texture usage 0x", std::hex, usage, " is not honoured yet (render target and dynamic are)."
    );
    return D3DERR_INVALIDCALL;
  }
  if (usage && pool != D3DPOOL_DEFAULT) {
    ERR_ONCE("D3D9: a used texture (render target or dynamic) is a default-pool texture; pool ", (unsigned)pool,
             " is refused.");
    return D3DERR_INVALIDCALL;
  }
  if ((usage & D3DUSAGE_RENDERTARGET) && (usage & D3DUSAGE_DYNAMIC)) {
    ERR_ONCE("D3D9: a texture is a render target or dynamic, not both.");
    return D3DERR_INVALIDCALL;
  }

  // the levels a D3D9 texture is asked for: the ones it names, or (none named) the whole chain down to one texel
  UINT chain = 1;
  for (UINT size = std::max(width, height); size > 1; size >>= 1)
    chain++;
  if (levels > chain) {
    ERR_ONCE("D3D9: a texture asked for ", levels, " levels; this size has ", chain, " (the whole chain down to one "
                                                                                        "texel).");
    return D3DERR_INVALIDCALL;
  }
  UINT level_count = levels ? levels : chain;

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = level_count;
  desc.ArraySize = 1;
  desc.Format = dxgi_format;
  desc.SampleDesc.Count = 1;
  // A dynamic texture's levels are the CPU's and are uploaded on unlock: D3D11 maps a texture with WRITE_DISCARD only,
  // which would throw away the pixels a NOOVERWRITE lock leaves alone, so the texture is the device's and the bytes
  // the application writes reach it through the upload (the same rows a map would hand back)
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (usage & D3DUSAGE_RENDERTARGET)
    desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
  // A managed texture's levels below the top one are made out of it only when the game asks (GenerateMipSubLevels),
  // which D3D11 does through a render target view of the whole chain; the chain itself is made whether or not this
  // format can be generated, so the levels the game authored survive and GetLevelCount is the count it asked for. A
  // compressed format's levels are always the game's own blocks (D3D11 makes no level of a compressed texture out of
  // another). Nothing reduces the chain to one level behind the game's back.
  bool can_generate_mips = false;
  if (level_count > 1 && !(usage & D3DUSAGE_DYNAMIC)) {
    UINT support = 0;
    if (SUCCEEDED(d3d11_device_->CheckFormatSupport(dxgi_format, &support)) &&
        (support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN)) {
      desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
      desc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
      can_generate_mips = true;
    }
  }

  ComPtr<ID3D11Texture2D> d3d11_texture;
  HRESULT create_hr = d3d11_device_->CreateTexture2D(&desc, nullptr, &d3d11_texture);
  if (FAILED(create_hr) && can_generate_mips) {
    // the format said it can generate mips but the device refuses the render-target binding that needs: the chain is
    // still the game's, it simply cannot be generated here. The requested level count is kept.
    desc.BindFlags &= ~D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags &= ~D3D11_RESOURCE_MISC_GENERATE_MIPS;
    can_generate_mips = false;
    create_hr = d3d11_device_->CreateTexture2D(&desc, nullptr, &d3d11_texture);
  }
  if (FAILED(create_hr)) {
    ERR("D3D9: no texture of ", width, "x", height, " with ", level_count, " levels: 0x", std::hex, create_hr);
    return D3DERR_INVALIDCALL;
  }

  ComPtr<ID3D11ShaderResourceView> view;
  if (FAILED(d3d11_device_->CreateShaderResourceView(d3d11_texture.Get(), nullptr, &view))) {
    ERR_ONCE("D3D9: the texture's view is not there, so the texture is not either.");
    return D3DERR_INVALIDCALL;
  }

  ComPtr<MTLD3D9Texture> result;
  result.Attach(new MTLD3D9Texture(
      static_cast<IDirect3DDevice9 *>(this), d3d11_texture, view, context_, format, pool, level_count, can_generate_mips,
      usage
  ));

  *texture = result.Detach();
  return S_OK;
}

// a cube texture: six square faces as a D3D11 TEXTURECUBE (Microsoft Learn, IDirect3DDevice9::CreateCubeTexture).
// each face is an array slice, so a lock of GetCubeMapSurface(face, level) is the same lock as LockRect(face, level).
HRESULT
MTLD3D9Device::CreateCubeTexture(
    UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture9 **texture,
    HANDLE *shared_handle
) {
  if (!texture || shared_handle)
    return D3DERR_INVALIDCALL;
  *texture = nullptr;
  const DXGI_FORMAT dxgi_format = to_dxgi_format(format);
  if (dxgi_format == DXGI_FORMAT_UNKNOWN || !edge || edge > caps_.MaxTextureWidth) {
    ERR_ONCE(
        "D3D9: no cube texture of this format (", format, ") or this edge (", edge, "): the device makes up to ",
        caps_.MaxTextureWidth, "."
    );
    return D3DERR_INVALIDCALL;
  }
  if (block_size(format) && (edge & 3)) {
    ERR_ONCE("D3D9: a compressed cube texture's edge is a multiple of four; ", edge, " is not.");
    return D3DERR_INVALIDCALL;
  }
  if (pool != D3DPOOL_DEFAULT && pool != D3DPOOL_MANAGED) {
    ERR_ONCE("D3D9: only a default-pool and a managed cube texture are made yet.");
    return D3DERR_INVALIDCALL;
  }
  if (pool == D3DPOOL_DEFAULT) {
    if (HRESULT lost = lost_reason(); FAILED(lost))
      return lost;
  }
  if (usage & ~(DWORD)(D3DUSAGE_RENDERTARGET | D3DUSAGE_DYNAMIC)) {
    ERR_ONCE("D3D9: cube texture usage 0x", std::hex, usage, " is not honoured yet (render target and dynamic are).");
    return D3DERR_INVALIDCALL;
  }
  if (usage && pool != D3DPOOL_DEFAULT) {
    ERR_ONCE("D3D9: a used cube texture is a default-pool texture; pool ", (unsigned)pool, " is refused.");
    return D3DERR_INVALIDCALL;
  }
  if ((usage & D3DUSAGE_RENDERTARGET) && (usage & D3DUSAGE_DYNAMIC)) {
    ERR_ONCE("D3D9: a cube texture is a render target or dynamic, not both.");
    return D3DERR_INVALIDCALL;
  }

  UINT chain = 1;
  for (UINT size = edge; size > 1; size >>= 1)
    chain++;
  if (levels > chain) {
    ERR_ONCE("D3D9: a cube texture asked for ", levels, " levels; this edge has ", chain, ".");
    return D3DERR_INVALIDCALL;
  }
  UINT level_count = levels ? levels : chain;

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = edge;
  desc.Height = edge;
  desc.MipLevels = level_count;
  desc.ArraySize = 6;
  desc.Format = dxgi_format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
  if (usage & D3DUSAGE_RENDERTARGET)
    desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
  bool can_generate_mips = false;
  if (level_count > 1 && !(usage & D3DUSAGE_DYNAMIC)) {
    UINT support = 0;
    if (SUCCEEDED(d3d11_device_->CheckFormatSupport(dxgi_format, &support)) &&
        (support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN)) {
      desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
      desc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
      can_generate_mips = true;
    }
  }

  ComPtr<ID3D11Texture2D> d3d11_texture;
  HRESULT create_hr = d3d11_device_->CreateTexture2D(&desc, nullptr, &d3d11_texture);
  if (FAILED(create_hr) && can_generate_mips) {
    desc.BindFlags &= ~D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags &= ~D3D11_RESOURCE_MISC_GENERATE_MIPS;
    can_generate_mips = false;
    create_hr = d3d11_device_->CreateTexture2D(&desc, nullptr, &d3d11_texture);
  }
  if (FAILED(create_hr)) {
    ERR("D3D9: no cube texture of edge ", edge, " with ", level_count, " levels: 0x", std::hex, create_hr);
    return D3DERR_INVALIDCALL;
  }

  D3D11_SHADER_RESOURCE_VIEW_DESC view_desc{};
  view_desc.Format = dxgi_format;
  view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
  view_desc.TextureCube.MostDetailedMip = 0;
  view_desc.TextureCube.MipLevels = level_count;
  ComPtr<ID3D11ShaderResourceView> view;
  if (FAILED(d3d11_device_->CreateShaderResourceView(d3d11_texture.Get(), &view_desc, &view))) {
    ERR_ONCE("D3D9: the cube texture's view is not there, so the texture is not either.");
    return D3DERR_INVALIDCALL;
  }

  ComPtr<MTLD3D9CubeTexture> result;
  result.Attach(new MTLD3D9CubeTexture(
      static_cast<IDirect3DDevice9 *>(this), d3d11_texture, view, context_, format, pool, level_count,
      can_generate_mips, usage
  ));
  *texture = result.Detach();
  return S_OK;
}

// a volume texture: a D3D11 TEXTURE3D the CPU authors with LockBox and a draw samples through dcl_volume / tex3D
// (Microsoft Learn, IDirect3DDevice9::CreateVolumeTexture)
HRESULT
MTLD3D9Device::CreateVolumeTexture(
    UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
    IDirect3DVolumeTexture9 **texture, HANDLE *shared_handle
) {
  if (!texture || shared_handle)
    return D3DERR_INVALIDCALL;
  *texture = nullptr;
  const DXGI_FORMAT dxgi_format = to_dxgi_format(format);
  if (dxgi_format == DXGI_FORMAT_UNKNOWN || !width || !height || !depth || width > caps_.MaxVolumeExtent ||
      height > caps_.MaxVolumeExtent || depth > caps_.MaxVolumeExtent) {
    ERR_ONCE(
        "D3D9: no volume texture of this format (", format, ") or this size (", width, "x", height, "x", depth,
        "): the device makes up to ", caps_.MaxVolumeExtent, "."
    );
    return D3DERR_INVALIDCALL;
  }
  if (pool != D3DPOOL_DEFAULT && pool != D3DPOOL_MANAGED) {
    ERR_ONCE("D3D9: only a default-pool and a managed volume texture are made yet.");
    return D3DERR_INVALIDCALL;
  }
  if (pool == D3DPOOL_DEFAULT) {
    if (HRESULT lost = lost_reason(); FAILED(lost))
      return lost;
  }
  if (usage & ~(DWORD)D3DUSAGE_DYNAMIC) {
    ERR_ONCE("D3D9: volume texture usage 0x", std::hex, usage, " is not honoured yet (dynamic is).");
    return D3DERR_INVALIDCALL;
  }
  if (usage && pool != D3DPOOL_DEFAULT) {
    ERR_ONCE("D3D9: a used volume texture is a default-pool texture; pool ", (unsigned)pool, " is refused.");
    return D3DERR_INVALIDCALL;
  }

  UINT longest = std::max(width, std::max(height, depth));
  UINT chain = 1;
  for (UINT size = longest; size > 1; size >>= 1)
    chain++;
  if (levels > chain) {
    ERR_ONCE("D3D9: a volume texture asked for ", levels, " levels; this size has ", chain, ".");
    return D3DERR_INVALIDCALL;
  }
  UINT level_count = levels ? levels : chain;

  D3D11_TEXTURE3D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.Depth = depth;
  desc.MipLevels = level_count;
  desc.Format = dxgi_format;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  ComPtr<ID3D11Texture3D> d3d11_texture;
  HRESULT create_hr = d3d11_device_->CreateTexture3D(&desc, nullptr, &d3d11_texture);
  if (FAILED(create_hr)) {
    ERR("D3D9: no volume texture of ", width, "x", height, "x", depth, ": 0x", std::hex, create_hr);
    return D3DERR_INVALIDCALL;
  }

  D3D11_SHADER_RESOURCE_VIEW_DESC view_desc{};
  view_desc.Format = dxgi_format;
  view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
  view_desc.Texture3D.MostDetailedMip = 0;
  view_desc.Texture3D.MipLevels = level_count;
  ComPtr<ID3D11ShaderResourceView> view;
  if (FAILED(d3d11_device_->CreateShaderResourceView(d3d11_texture.Get(), &view_desc, &view))) {
    ERR_ONCE("D3D9: the volume texture's view is not there, so the texture is not either.");
    return D3DERR_INVALIDCALL;
  }

  ComPtr<MTLD3D9VolumeTexture> result;
  result.Attach(new MTLD3D9VolumeTexture(
      static_cast<IDirect3DDevice9 *>(this), d3d11_texture, view, context_, format, pool, level_count, usage
  ));
  *texture = result.Detach();
  return S_OK;
}

// the textures a draw samples: D3D9's eight stages (the caps' MaxSimultaneousTextures), each keeping the texture
// SetTexture set for it, and a stage past them refused as D3D9 refuses one a device does not have
HRESULT
MTLD3D9Device::SetTexture(DWORD stage, IDirect3DBaseTexture9 *texture) {
  if (stage >= kTextureStages) {
    ERR_ONCE("D3D9: this device has ", kTextureStages, " texture stages (MaxSimultaneousTextures); stage ", stage,
             " is refused.");
    return D3DERR_INVALIDCALL;
  }
  if (!texture) {
    textures_[stage].Reset();
    texture_sources_[stage].Reset();
    if (recording_)
      record(StateKey{StateKey::Texture, stage, 0});
    return S_OK;
  }

  ComPtr<IMTLD3D9Texture> ours;
  if (FAILED(texture->QueryInterface(IID_IMTLD3D9Texture, reinterpret_cast<void **>(ours.GetAddressOf()))))
    return D3DERR_INVALIDCALL;
  textures_[stage] = texture;
  texture_sources_[stage] = ours;
  if (recording_)
    record(StateKey{StateKey::Texture, stage, 0});
  return S_OK;
}

HRESULT
MTLD3D9Device::GetTexture(DWORD stage, IDirect3DBaseTexture9 **texture) {
  if (stage >= kTextureStages || !texture)
    return D3DERR_INVALIDCALL;
  *texture = textures_[stage].Get();
  if (*texture)
    (*texture)->AddRef();
  return S_OK;
}

// the texture a stage with none samples: D3D9's default texture is one opaque white texel, which a program that samples
// an empty stage reads the way it reads any other texture. it is made once and bound as a draw's input when the pixel
// shader samples and SetTexture has set nothing
HRESULT
MTLD3D9Device::ensure_default_texture() {
  if (default_texture_view_)
    return S_OK;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = 1;
  desc.Height = 1;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  const DWORD white = 0xFFFFFFFF;
  D3D11_SUBRESOURCE_DATA data{};
  data.pSysMem = &white;
  data.SysMemPitch = sizeof(white);
  if (FAILED(d3d11_device_->CreateTexture2D(&desc, &data, &default_texture_)) ||
      FAILED(d3d11_device_->CreateShaderResourceView(default_texture_.Get(), nullptr, &default_texture_view_))) {
    default_texture_.Reset();
    default_texture_view_.Reset();
    ERR_ONCE("D3D9: the default texture (the white texel a stage with no texture samples) is not made.");
    return D3DERR_INVALIDCALL;
  }
  return S_OK;
}

// a sampler is a stage's: D3D9's stage # samples through sampler #, and the device has one sampler for each of its
// eight stages
HRESULT
MTLD3D9Device::SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
  if (sampler >= kTextureStages) {
    ERR_ONCE("D3D9: this device has ", kTextureStages, " samplers (MaxSimultaneousTextures); sampler ", sampler,
             " is refused.");
    return D3DERR_INVALIDCALL;
  }
  const HRESULT hr = sampler_states_[sampler].set(type, value);
  if (SUCCEEDED(hr) && recording_)
    record(StateKey{StateKey::Sampler, sampler, (UINT)type});
  return hr;
}

HRESULT
MTLD3D9Device::GetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD *value) {
  if (sampler >= kTextureStages || !value)
    return D3DERR_INVALIDCALL;
  return sampler_states_[sampler].get(type, value);
}

// the fixed-function blender has two stages (the caps' MaxTextureBlendStages): stage 0 and the extra the extended
// pixel shader blends after it
HRESULT
MTLD3D9Device::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
  if (stage >= kMaxBlendStages) {
    ERR_ONCE("D3D9: this device's fixed-function blender has ", kMaxBlendStages,
             " stages (MaxTextureBlendStages); stage ", stage, "'s operations are refused.");
    return D3DERR_INVALIDCALL;
  }
  const HRESULT hr = texture_stages_[stage].set(type, value);
  if (SUCCEEDED(hr) && recording_)
    record(StateKey{StateKey::TextureStage, stage, (UINT)type});
  return hr;
}

HRESULT
MTLD3D9Device::GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD *value) {
  if (stage >= kMaxBlendStages || !value)
    return D3DERR_INVALIDCALL;
  return texture_stages_[stage].get(type, value);
}

} // namespace mullion::d3d9
