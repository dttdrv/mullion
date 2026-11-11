/*
 * Copyright 2026 David Skoland
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "d3d9_query.hpp"
#include "d3d9_device.hpp"
#include "log/log.hpp"
#include <cstring>

namespace mullion {

// polls without D3DGETDATA_FLUSH before the pending commands are flushed anyway, so a spinning app can't deadlock.
constexpr uint32_t kPollsBeforeFlush = 64;

MTLD3D9Query::MTLD3D9Query(MTLD3D9Device *pDevice, D3DQUERYTYPE Type) :
    MTLD3D9DeviceChild(pDevice),
    parent_(pDevice),
    type_(Type) {}

bool
MTLD3D9Query::Supported(D3DQUERYTYPE Type) {
  switch (Type) {
  case D3DQUERYTYPE_VCACHE:
  case D3DQUERYTYPE_EVENT:
  case D3DQUERYTYPE_OCCLUSION:
  case D3DQUERYTYPE_TIMESTAMP:
  case D3DQUERYTYPE_TIMESTAMPDISJOINT:
  case D3DQUERYTYPE_TIMESTAMPFREQ:
    return true;
  default:
    return false;
  }
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Query::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;
  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DQuery9)) {
    *ppvObject = ref(this);
    return S_OK;
  }
  WARN("IDirect3DQuery9::QueryInterface: Unknown interface query ", str::format(riid));
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Query::GetDevice(IDirect3DDevice9 **ppDevice) {
  return GetDeviceInternal(ppDevice);
}

D3DQUERYTYPE STDMETHODCALLTYPE
MTLD3D9Query::GetType() {
  return type_;
}

DWORD STDMETHODCALLTYPE
MTLD3D9Query::GetDataSize() {
  switch (type_) {
  case D3DQUERYTYPE_VCACHE:
    return sizeof(D3DDEVINFO_VCACHE);
  case D3DQUERYTYPE_EVENT:
  case D3DQUERYTYPE_TIMESTAMPDISJOINT:
    return sizeof(BOOL);
  case D3DQUERYTYPE_OCCLUSION:
    return sizeof(DWORD);
  default:
    return sizeof(UINT64);
  }
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Query::Issue(DWORD dwIssueFlags) {
  auto lock = parent_->Lock();
  bool begin = dwIssueFlags & D3DISSUE_BEGIN, end = dwIssueFlags & D3DISSUE_END;
  if (type_ == D3DQUERYTYPE_OCCLUSION) {
    if (begin) {
      if (state_ == State::Building)
        parent_->Emit([query = occlusion_](ArgumentEncodingContext &enc) mutable {
          enc.endVisibilityResultQuery(std::move(query));
        });
      occlusion_ = new VisibilityResultQuery();
      parent_->Emit([query = occlusion_](ArgumentEncodingContext &enc) mutable {
        enc.beginVisibilityResultQuery(std::move(query));
      });
      state_ = State::Building;
    }
    if (end) {
      if (state_ != State::Building) {
        occlusion_ = new VisibilityResultQuery();
        parent_->Emit([query = occlusion_](ArgumentEncodingContext &enc) mutable {
          enc.beginVisibilityResultQuery(std::move(query));
        });
      }
      parent_->Emit([query = occlusion_](ArgumentEncodingContext &enc) mutable {
        enc.endVisibilityResultQuery(std::move(query));
      });
      state_ = State::Issued;
    }
    polls_ = 0;
    return D3D_OK;
  }

  if (begin && type_ == D3DQUERYTYPE_TIMESTAMPDISJOINT)
    state_ = State::Building;
  if (end) {
    if (type_ == D3DQUERYTYPE_TIMESTAMP) {
      timestamp_ = new TimestampQuery();
      parent_->EndRenderPass();
      parent_->Emit([query = timestamp_](ArgumentEncodingContext &enc) mutable {
        enc.sampleTimestamp(std::move(query));
      });
    }
    seq_ = parent_->queue().CurrentSeqId();
    state_ = State::Issued;
    polls_ = 0;
  }
  return D3D_OK;
}

bool
MTLD3D9Query::Ready() {
  if (type_ == D3DQUERYTYPE_VCACHE)
    return true;
  uint64_t value;
  if (type_ == D3DQUERYTYPE_OCCLUSION)
    return occlusion_->getValue(&value);
  if (type_ == D3DQUERYTYPE_TIMESTAMP)
    return timestamp_->getValue(&value) != S_FALSE;
  return parent_->queue().CoherentSeqId() >= seq_;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Query::GetData(void *pData, DWORD dwSize, DWORD dwGetDataFlags) {
  auto lock = parent_->Lock();
  if (dwSize && !pData)
    return D3DERR_INVALIDCALL;
  if (state_ == State::Building)
    return S_FALSE;
  // a smaller buffer gets the first dwSize bytes; a query never issued fills it with 0xdd
  if (state_ == State::Created && type_ != D3DQUERYTYPE_VCACHE) {
    if (dwSize)
      std::memset(pData, 0xdd, dwSize);
    return S_OK;
  }

  if (!Ready()) {
    if ((dwGetDataFlags & D3DGETDATA_FLUSH) || ++polls_ >= kPollsBeforeFlush) {
      parent_->FlushCommands();
      polls_ = 0;
    }
    if (!Ready())
      return S_FALSE;
  }
  if (!dwSize)
    return S_OK;

  union {
    D3DDEVINFO_VCACHE vcache;
    BOOL boolean;
    DWORD dword;
    UINT64 u64;
  } result = {};
  uint64_t value = 0;
  switch (type_) {
  case D3DQUERYTYPE_VCACHE:
    result.vcache = {MAKEFOURCC('C', 'A', 'C', 'H'), 0, 0, 0};
    break;
  case D3DQUERYTYPE_EVENT:
    result.boolean = TRUE;
    break;
  case D3DQUERYTYPE_TIMESTAMPDISJOINT:
    result.boolean = FALSE;
    break;
  case D3DQUERYTYPE_OCCLUSION:
    occlusion_->getValue(&value);
    result.u64 = value;
    break;
  case D3DQUERYTYPE_TIMESTAMP:
    timestamp_->getValue(&value);
    result.u64 = value;
    break;
  case D3DQUERYTYPE_TIMESTAMPFREQ:
    result.u64 = 1'000'000'000;
    break;
  default:
    break;
  }
  // an occlusion query reports a DWORD but fills a larger buffer with the full 64-bit count (Wine's test_occlusion_query)
  DWORD size = type_ == D3DQUERYTYPE_OCCLUSION ? sizeof(UINT64) : GetDataSize();
  std::memcpy(pData, &result, std::min<DWORD>(dwSize, size));
  return S_OK;
}

} // namespace mullion
