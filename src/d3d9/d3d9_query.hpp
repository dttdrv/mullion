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

#pragma once

#include "d3d9_device_child.hpp"
#include "mullion_query.hpp"

namespace mullion {

class MTLD3D9Device;

class MTLD3D9Query final : public MTLD3D9DeviceChild<IDirect3DQuery9> {
public:
  MTLD3D9Query(MTLD3D9Device *pDevice, D3DQUERYTYPE Type);

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final;
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) final;
  D3DQUERYTYPE STDMETHODCALLTYPE GetType() final;
  DWORD STDMETHODCALLTYPE GetDataSize() final;
  HRESULT STDMETHODCALLTYPE Issue(DWORD dwIssueFlags) final;
  HRESULT STDMETHODCALLTYPE GetData(void *pData, DWORD dwSize, DWORD dwGetDataFlags) final;

  static bool Supported(D3DQUERYTYPE Type);

private:
  enum class State { Created, Building, Issued };

  bool Ready();

  MTLD3D9Device *parent_;
  D3DQUERYTYPE type_;
  State state_ = State::Created;
  uint64_t seq_ = 0;
  uint32_t polls_ = 0;
  Rc<VisibilityResultQuery> occlusion_;
  Rc<TimestampQuery> timestamp_;
};

} // namespace mullion
