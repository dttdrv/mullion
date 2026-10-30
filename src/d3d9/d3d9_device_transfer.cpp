// the copies between surfaces a frame is read back through and the surfaces a frame is drawn from: ColorFill, which
// fills a default surface with a colour, StretchRect, which copies a rectangle of one surface to another, and
// UpdateSurface, which copies the CPU's bytes of a system-memory surface into a default one (Microsoft Learn,
// IDirect3DDevice9::ColorFill, ::StretchRect, ::UpdateSurface).
#include "d3d9_device.hpp"
#include <algorithm>
#include <vector>

namespace mullion::d3d9 {

namespace {

float channel(D3DCOLOR colour, unsigned shift) { return (float)((colour >> shift) & 0xFF) / 255.0f; }

// a rectangle of a surface, clamped to it: D3D9's calls take NULL for the whole of it
bool clamp_rect(const RECT *rect, UINT width, UINT height, D3D11_BOX &box) {
  const int64_t left = rect ? std::max<int64_t>(rect->left, 0) : 0;
  const int64_t top = rect ? std::max<int64_t>(rect->top, 0) : 0;
  const int64_t right = rect ? std::min<int64_t>(rect->right, width) : width;
  const int64_t bottom = rect ? std::min<int64_t>(rect->bottom, height) : height;
  if (right <= left || bottom <= top)
    return false;
  box = D3D11_BOX{(UINT)left, (UINT)top, 0, (UINT)right, (UINT)bottom, 1};
  return true;
}

bool overlaps(const D3D11_BOX &a, const D3D11_BOX &b) {
  return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

} // namespace

// A default-pool surface filled with one colour, whole or in the rectangles the call names. The surface takes a render
// target view of its own (ColorFill is not a clear of the frame's target, so it neither reads nor changes what is
// bound); a system-memory surface or a depth-stencil surface is refused, as D3D9 refuses one.
HRESULT
MTLD3D9Device::ColorFill(IDirect3DSurface9 *surface, const RECT *rect, D3DCOLOR colour) {
  if (!surface)
    return D3DERR_INVALIDCALL;
  ComPtr<IMTLD3D9Surface> ours;
  if (FAILED(device_surface(surface, false, &ours)))
    return D3DERR_INVALIDCALL;
  if (ours->lockable()) {
    ERR_ONCE("D3D9: ColorFill fills a default-pool surface; this one's bytes are the CPU's.");
    return D3DERR_INVALIDCALL;
  }

  D3DSURFACE_DESC desc{};
  if (FAILED(surface->GetDesc(&desc)))
    return D3DERR_INVALIDCALL;
  D3D11_BOX box{};
  if (rect && !clamp_rect(rect, desc.Width, desc.Height, box))
    return S_OK;

  D3D11_TEXTURE2D_DESC texture_desc{};
  ours->texture()->GetDesc(&texture_desc);
  D3D11_RENDER_TARGET_VIEW_DESC view_desc{};
  view_desc.Format = texture_desc.Format;
  if (texture_desc.SampleDesc.Count > 1) {
    if (texture_desc.ArraySize == 1) {
      view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
    } else {
      view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
      view_desc.Texture2DMSArray.FirstArraySlice = ours->array_slice();
      view_desc.Texture2DMSArray.ArraySize = 1;
    }
  } else if (texture_desc.ArraySize == 1) {
    view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    view_desc.Texture2D.MipSlice = ours->level();
  } else {
    view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    view_desc.Texture2DArray.MipSlice = ours->level();
    view_desc.Texture2DArray.FirstArraySlice = ours->array_slice();
    view_desc.Texture2DArray.ArraySize = 1;
  }
  ComPtr<ID3D11RenderTargetView> view;
  if (FAILED(d3d11_device_->CreateRenderTargetView(ours->texture(), &view_desc, &view))) {
    ERR_ONCE("D3D9: this surface's format takes no render target view, so ColorFill cannot fill it.");
    return D3DERR_INVALIDCALL;
  }
  const float rgba[4] = {channel(colour, 16), channel(colour, 8), channel(colour, 0), channel(colour, 24)};
  if (!rect) {
    context_->ClearRenderTargetView(view.Get(), rgba);
    return S_OK;
  }
  // a part of the surface is a clear of part of the view, which the 11.1 context has
  if (!context1_) {
    ERR_ONCE("D3D9: ColorFill of part of a surface needs the context's ClearView, which this device's Direct3D 11 "
             "context has not.");
    return D3DERR_INVALIDCALL;
  }
  const D3D11_RECT region{(LONG)box.left, (LONG)box.top, (LONG)box.right, (LONG)box.bottom};
  context1_->ClearView(view.Get(), rgba, &region, 1);
  return S_OK;
}

// A rectangle of one surface copied to another, without stretching: D3D9's StretchRect with the rectangles the same
// size. A scaled copy and a multi-sampled resolve are what this build has not (src/d3d9/README), and are refused rather
// than copied as an unscaled one or as a single sample.
HRESULT
MTLD3D9Device::StretchRect(IDirect3DSurface9 *src_surface, const RECT *src_rect, IDirect3DSurface9 *dst_surface,
                           const RECT *dst_rect, D3DTEXTUREFILTERTYPE filter) {
  if (!src_surface || !dst_surface)
    return D3DERR_INVALIDCALL;
  if (filter != D3DTEXF_NONE && filter != D3DTEXF_POINT && filter != D3DTEXF_LINEAR)
    return D3DERR_INVALIDCALL;

  ComPtr<IMTLD3D9Surface> source, target;
  if (FAILED(device_surface(src_surface, false, &source)) || FAILED(device_surface(dst_surface, false, &target)))
    return D3DERR_INVALIDCALL;
  if (target->lockable()) {
    ERR_ONCE("D3D9: StretchRect's destination is a default-pool surface; this one's bytes are the CPU's.");
    return D3DERR_INVALIDCALL;
  }

  D3DSURFACE_DESC src_desc{}, dst_desc{};
  if (FAILED(src_surface->GetDesc(&src_desc)) || FAILED(dst_surface->GetDesc(&dst_desc)))
    return D3DERR_INVALIDCALL;
  if (src_desc.Format != dst_desc.Format) {
    ERR_ONCE("D3D9: StretchRect copies between surfaces of the same format, not from ", (unsigned)src_desc.Format,
             " to ", (unsigned)dst_desc.Format, ".");
    return D3DERR_INVALIDCALL;
  }

  D3D11_BOX source_box{};
  if (!clamp_rect(src_rect, src_desc.Width, src_desc.Height, source_box))
    return S_OK;
  const UINT width = source_box.right - source_box.left;
  const UINT height = source_box.bottom - source_box.top;

  const int64_t dst_left = dst_rect ? std::max<int64_t>(dst_rect->left, 0) : 0;
  const int64_t dst_top = dst_rect ? std::max<int64_t>(dst_rect->top, 0) : 0;
  if (dst_left + width > dst_desc.Width || dst_top + height > dst_desc.Height) {
    ERR_ONCE("D3D9: StretchRect's destination rectangle is not a part of its surface.");
    return D3DERR_INVALIDCALL;
  }
  if (dst_rect && ((UINT)(dst_rect->right - dst_rect->left) != width || (UINT)(dst_rect->bottom - dst_rect->top) != height)) {
    ERR_ONCE("D3D9: StretchRect that stretches is not implemented yet: the source and destination rectangles are the "
             "same size.");
    return D3DERR_INVALIDCALL;
  }

  if (source->texture() == target->texture()) {
    const D3D11_BOX destination_box{(UINT)dst_left, (UINT)dst_top, 0, (UINT)(dst_left + width),
                                    (UINT)(dst_top + height), 1};
    if (overlaps(source_box, destination_box)) {
      ERR_ONCE("D3D9: StretchRect's source and destination overlap on one surface.");
      return D3DERR_INVALIDCALL;
    }
  }

  D3D11_TEXTURE2D_DESC source_texture{}, target_texture{};
  source->texture()->GetDesc(&source_texture);
  target->texture()->GetDesc(&target_texture);
  const UINT source_sub = D3D11CalcSubresource(source->level(), source->array_slice(), source_texture.MipLevels);
  const UINT target_sub = D3D11CalcSubresource(target->level(), target->array_slice(), target_texture.MipLevels);
  // a multisampled source is resolved into a single-sample destination of the same format (Microsoft Learn,
  // IDirect3DDevice9::StretchRect: "StretchRect can be used to perform a stretch blit from a multisampled surface to
  // a non-multisampled surface")
  if (source_texture.SampleDesc.Count > 1 && target_texture.SampleDesc.Count == 1) {
    if (src_rect || dst_rect) {
      ERR_ONCE("D3D9: StretchRect of a multisampled surface resolves the whole surface; a rectangle is refused.");
      return D3DERR_INVALIDCALL;
    }
    context_->ResolveSubresource(target->texture(), target_sub, source->texture(), source_sub, source_texture.Format);
    return S_OK;
  }
  if (source_texture.SampleDesc.Count != target_texture.SampleDesc.Count) {
    ERR_ONCE("D3D9: StretchRect copies between surfaces of the same sample count, or resolves a multisampled source.");
    return D3DERR_INVALIDCALL;
  }
  context_->CopySubresourceRegion(
      target->texture(), target_sub, (UINT)dst_left, (UINT)dst_top, 0, source->texture(), source_sub, &source_box
  );
  return S_OK;
}

// A managed texture's levels copied into a default-pool texture's, which is D3D9's UpdateTexture: the two are the same
// format and the same size, with the same level count, and each level's bytes are the ones the application authored into
// the source. The source is a managed (or system-memory) texture and the destination a default-pool one. Volume textures
// copy GPU-side after the source's last unlock; cubes stay refused until they share this path.
HRESULT
MTLD3D9Device::UpdateTexture(IDirect3DBaseTexture9 *source, IDirect3DBaseTexture9 *destination) {
  if (!source || !destination || source == destination)
    return D3DERR_INVALIDCALL;

  ComPtr<IDirect3DVolumeTexture9> source_volume, destination_volume;
  if (SUCCEEDED(source->QueryInterface(
          __uuidof(IDirect3DVolumeTexture9), reinterpret_cast<void **>(source_volume.GetAddressOf())
      )) &&
      SUCCEEDED(destination->QueryInterface(
          __uuidof(IDirect3DVolumeTexture9), reinterpret_cast<void **>(destination_volume.GetAddressOf())
      ))) {
    ComPtr<IMTLD3D9Texture> source_ours, destination_ours;
    if (FAILED(source_volume->QueryInterface(IID_IMTLD3D9Texture, reinterpret_cast<void **>(source_ours.GetAddressOf()))) ||
        FAILED(destination_volume->QueryInterface(
            IID_IMTLD3D9Texture, reinterpret_cast<void **>(destination_ours.GetAddressOf())
        )))
      return D3DERR_INVALIDCALL;
    if (!source_ours->managed() || destination_ours->pool() != D3DPOOL_DEFAULT || !source_ours->volume() ||
        !destination_ours->volume())
      return D3DERR_INVALIDCALL;
    if (source_volume->GetLevelCount() != destination_volume->GetLevelCount())
      return D3DERR_INVALIDCALL;
    context_->CopyResource(destination_ours->volume(), source_ours->volume());
    return S_OK;
  }

  ComPtr<IDirect3DTexture9> source_texture, destination_texture;
  if (FAILED(source->QueryInterface(
          __uuidof(IDirect3DTexture9), reinterpret_cast<void **>(source_texture.GetAddressOf())
      )) ||
      FAILED(destination->QueryInterface(
          __uuidof(IDirect3DTexture9), reinterpret_cast<void **>(destination_texture.GetAddressOf())
      ))) {
    ERR_ONCE("D3D9: UpdateTexture copies between 2D or volume textures; a cube texture is not implemented yet.");
    return D3DERR_INVALIDCALL;
  }

  ComPtr<IMTLD3D9Texture> source_ours, destination_ours;
  if (FAILED(source_texture->QueryInterface(IID_IMTLD3D9Texture, reinterpret_cast<void **>(source_ours.GetAddressOf()))) ||
      FAILED(destination_texture->QueryInterface(
          IID_IMTLD3D9Texture, reinterpret_cast<void **>(destination_ours.GetAddressOf())
      )))
    return D3DERR_INVALIDCALL;
  if (!source_ours->managed() || destination_ours->pool() != D3DPOOL_DEFAULT) {
    ERR_ONCE("D3D9: UpdateTexture's source is a managed texture and its destination a default-pool one.");
    return D3DERR_INVALIDCALL;
  }

  const UINT levels = source_texture->GetLevelCount();
  if (levels != destination_texture->GetLevelCount()) {
    ERR_ONCE("D3D9: UpdateTexture's textures have the same level count; these have ", levels, " and ",
             destination_texture->GetLevelCount(), ".");
    return D3DERR_INVALIDCALL;
  }

  D3DSURFACE_DESC source_desc{}, destination_desc{};
  if (FAILED(source_texture->GetLevelDesc(0, &source_desc)) ||
      FAILED(destination_texture->GetLevelDesc(0, &destination_desc)))
    return D3DERR_INVALIDCALL;
  if (source_desc.Format != destination_desc.Format || source_desc.Width != destination_desc.Width ||
      source_desc.Height != destination_desc.Height) {
    ERR_ONCE("D3D9: UpdateTexture's textures are the same format and size, and these are not.");
    return D3DERR_INVALIDCALL;
  }

  // each level as the application authored it: the bytes a lock of the source level gives, uploaded into the
  // destination's level at the pitch D3D9's own lock reports for it (D3D11's UpdateSubresource takes the same rows)
  ID3D11Texture2D *target = destination_ours->texture();
  for (UINT level = 0; level < levels; level++) {
    D3DLOCKED_RECT locked{};
    if (FAILED(source_texture->LockRect(level, &locked, nullptr, D3DLOCK_READONLY))) {
      ERR_ONCE("D3D9: UpdateTexture's source level ", level, " could not be locked.");
      return D3DERR_INVALIDCALL;
    }
    context_->UpdateSubresource(target, level, nullptr, locked.pBits, (UINT)locked.Pitch, 0);
    source_texture->UnlockRect(level);
  }
  return S_OK;
}

// A system-memory surface's bytes copied into a default-pool one at a point, which is D3D9's UpdateSurface: no stretch,
// no format conversion, and the destination's bytes stay the device's. The source is mapped and its rectangle written
// into the destination with the mapped row pitch, so the copy is the pixels the application wrote and no others.
HRESULT
MTLD3D9Device::UpdateSurface(IDirect3DSurface9 *src_surface, const RECT *src_rect, IDirect3DSurface9 *dst_surface,
                             const POINT *dst_point) {
  if (!src_surface || !dst_surface)
    return D3DERR_INVALIDCALL;
  const POINT origin{0, 0};
  if (!dst_point)
    dst_point = &origin;
  ComPtr<IMTLD3D9Surface> source, target;
  if (FAILED(device_surface(src_surface, false, &source)) || FAILED(device_surface(dst_surface, false, &target)))
    return D3DERR_INVALIDCALL;
  if (!source->lockable() || target->lockable()) {
    ERR_ONCE("D3D9: UpdateSurface copies a system-memory surface into a default-pool one.");
    return D3DERR_INVALIDCALL;
  }

  D3DSURFACE_DESC src_desc{}, dst_desc{};
  if (FAILED(src_surface->GetDesc(&src_desc)) || FAILED(dst_surface->GetDesc(&dst_desc)))
    return D3DERR_INVALIDCALL;
  if (src_desc.Format != dst_desc.Format) {
    ERR_ONCE("D3D9: UpdateSurface does not convert formats: this copy is from ", (unsigned)src_desc.Format, " to ",
             (unsigned)dst_desc.Format, ".");
    return D3DERR_INVALIDCALL;
  }

  D3D11_BOX source_box{};
  if (!clamp_rect(src_rect, src_desc.Width, src_desc.Height, source_box))
    return S_OK;
  const UINT width = source_box.right - source_box.left;
  const UINT height = source_box.bottom - source_box.top;
  if (dst_point->x < 0 || dst_point->y < 0 || (UINT)dst_point->x + width > dst_desc.Width ||
      (UINT)dst_point->y + height > dst_desc.Height) {
    ERR_ONCE("D3D9: UpdateSurface's rectangle does not fit the destination at the point it names.");
    return D3DERR_INVALIDCALL;
  }
  const UINT pixel_bytes = pixel_size(src_desc.Format);
  if (!pixel_bytes)
    return D3DERR_INVALIDCALL;

  D3D11_TEXTURE2D_DESC source_texture{}, target_texture{};
  source->texture()->GetDesc(&source_texture);
  target->texture()->GetDesc(&target_texture);
  const UINT source_sub = D3D11CalcSubresource(source->level(), source->array_slice(), source_texture.MipLevels);
  const UINT target_sub = D3D11CalcSubresource(target->level(), target->array_slice(), target_texture.MipLevels);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context_->Map(source->texture(), source_sub, D3D11_MAP_READ, 0, &mapped))) {
    ERR_ONCE("D3D9: UpdateSurface's source could not be mapped.");
    return D3DERR_INVALIDCALL;
  }
  const char *bytes = static_cast<const char *>(mapped.pData) + (size_t)source_box.top * mapped.RowPitch +
                      (size_t)source_box.left * pixel_bytes;
  const D3D11_BOX destination_box{(UINT)dst_point->x, (UINT)dst_point->y, 0, (UINT)dst_point->x + width,
                                  (UINT)dst_point->y + height, 1};
  context_->UpdateSubresource(target->texture(), target_sub, &destination_box, bytes, mapped.RowPitch, 0);
  context_->Unmap(source->texture(), source_sub);
  return S_OK;
}

} // namespace mullion::d3d9
