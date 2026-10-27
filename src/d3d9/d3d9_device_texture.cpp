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
  if (pool != D3DPOOL_DEFAULT && pool != D3DPOOL_MANAGED) {
    ERR_ONCE("D3D9: only a default-pool and a managed texture are made yet: a system-memory texture is a copy this "
             "build has none of.");
    return D3DERR_INVALIDCALL;
  }
  if (usage) {
    ERR_ONCE("D3D9: no texture usage flag (dynamic, render target, automatic mipmaps) is honoured yet.");
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
  // a default-pool texture's bytes reach it by rendering into it or by a copy from a system-memory texture, and this
  // build has neither: the level it is made with is the only one that could hold anything
  if (pool == D3DPOOL_DEFAULT && level_count != 1) {
    ERR_ONCE("D3D9: a default-pool texture cannot be filled here yet, so only one level is made; a managed texture "
             "makes its own levels.");
    return D3DERR_INVALIDCALL;
  }

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = level_count;
  desc.ArraySize = 1;
  desc.Format = dxgi_format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  // a managed texture's levels below the top one are made out of it, which D3D11 generates them through a render
  // target view of the whole chain
  if (level_count > 1) {
    UINT support = 0;
    if (SUCCEEDED(d3d11_device_->CheckFormatSupport(dxgi_format, &support)) &&
        (support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN)) {
      desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
      desc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
    } else {
      ERR_ONCE("D3D9: this device cannot make this format's mip levels, so a texture of it is made with one level.");
      level_count = 1;
      desc.MipLevels = 1;
    }
  }

  ComPtr<ID3D11Texture2D> d3d11_texture;
  HRESULT create_hr = d3d11_device_->CreateTexture2D(&desc, nullptr, &d3d11_texture);
  if (FAILED(create_hr) && desc.MipLevels > 1) {
    // the levels are the only thing that can be refused for a format this device otherwise makes: a texture of it is
    // still made, with the one level that can hold anything
    ERR_ONCE("D3D9: this device makes no mipmapped texture of this format (0x", std::hex, create_hr,
             "), so a texture of it is made with one level.");
    desc.MipLevels = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = 0;
    create_hr = d3d11_device_->CreateTexture2D(&desc, nullptr, &d3d11_texture);
    level_count = 1;
  }
  if (FAILED(create_hr)) {
    ERR("D3D9: no texture of ", width, "x", height, ": 0x", std::hex, create_hr);
    return D3DERR_INVALIDCALL;
  }

  ComPtr<ID3D11ShaderResourceView> view;
  if (FAILED(d3d11_device_->CreateShaderResourceView(d3d11_texture.Get(), nullptr, &view))) {
    ERR_ONCE("D3D9: the texture's view is not there, so the texture is not either.");
    return D3DERR_INVALIDCALL;
  }

  ComPtr<MTLD3D9Texture> result;
  result.Attach(new MTLD3D9Texture(
      static_cast<IDirect3DDevice9 *>(this), d3d11_texture, view, context_, format, pool, level_count
  ));

  std::vector<ComPtr<MTLD3D9Surface>> level_surfaces;
  for (UINT level = 0; level < level_count; level++) {
    ComPtr<MTLD3D9Surface> surface;
    surface.Attach(new MTLD3D9Surface(
        static_cast<IDirect3DDevice9 *>(this), d3d11_texture, format, pool, false, context_, false,
        static_cast<IMTLD3D9LockTarget *>(result.Get()), level
    ));
    level_surfaces.push_back(surface);
  }
  result->set_levels(std::move(level_surfaces));

  *texture = result.Detach();
  return S_OK;
}

// the texture stage 0 samples. this build has one stage (its caps say MaxTextureBlendStages and
// MaxSimultaneousTextures are 1), and D3D9 refuses a stage a device does not have, so every other stage is refused
// rather than accepted and kept by nobody.
HRESULT
MTLD3D9Device::SetTexture(DWORD stage, IDirect3DBaseTexture9 *texture) {
  if (stage != 0) {
    ERR_ONCE("D3D9: this device has one texture stage (MaxTextureBlendStages is 1); every other one is refused.");
    return D3DERR_INVALIDCALL;
  }
  if (!texture) {
    texture_.Reset();
    texture_source_.Reset();
    return S_OK;
  }

  ComPtr<IMTLD3D9Texture> ours;
  if (FAILED(texture->QueryInterface(IID_IMTLD3D9Texture, reinterpret_cast<void **>(ours.GetAddressOf()))))
    return D3DERR_INVALIDCALL;
  texture_ = texture;
  texture_source_ = ours;
  return S_OK;
}

HRESULT
MTLD3D9Device::GetTexture(DWORD stage, IDirect3DBaseTexture9 **texture) {
  if (stage != 0 || !texture)
    return D3DERR_INVALIDCALL;
  *texture = texture_.Get();
  if (*texture)
    (*texture)->AddRef();
  return S_OK;
}

HRESULT
MTLD3D9Device::SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
  if (sampler != 0) {
    ERR_ONCE("D3D9: this device has one sampler (MaxSimultaneousTextures is 1); every other one is refused.");
    return D3DERR_INVALIDCALL;
  }
  return sampler_state_.set(type, value);
}

HRESULT
MTLD3D9Device::GetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD *value) {
  if (sampler != 0 || !value)
    return D3DERR_INVALIDCALL;
  return sampler_state_.get(type, value);
}

HRESULT
MTLD3D9Device::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
  if (stage != 0) {
    ERR_ONCE("D3D9: this device has one texture stage (MaxTextureBlendStages is 1); every other one is refused.");
    return D3DERR_INVALIDCALL;
  }
  return texture_stage_.set(type, value);
}

HRESULT
MTLD3D9Device::GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD *value) {
  if (stage != 0 || !value)
    return D3DERR_INVALIDCALL;
  return texture_stage_.get(type, value);
}

} // namespace mullion::d3d9
