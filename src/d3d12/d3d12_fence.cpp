/*
 * Copyright 2026 Feifan He for CodeWeavers
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

#include "d3d12_device.hpp"
#include "d3d12_device_child.hpp"
#include "com/com_pointer.hpp"
#include "log/log.hpp"

namespace mullion {

// a shared fence's value is its event's, which the devices it is shared with signal too. Metal's events only rise, so
// a shared fence is not set back
struct Completion : Reached, std::enable_shared_from_this<Completion> {
  WMT::Reference<WMT::SharedEvent> event;
  EventListener *listener;

  // the devices it is shared with, and a fence object it was opened as, raise the event without this completion: a
  // value the event has is reached at once, as Windows does, not when a notification of it comes back
  bool
  Has(uint64_t expected) override {
    return event && expected <= event.signaledValue();
  }

  // the listener sets `reached` once the event has `expected`, which gives the completion, if it still lives, the
  // event's value. a watch the event never reaches stays, without the completion
  void
  Waiting(uint64_t expected) override {
    if (!event)
      return;
    struct Watch {
      std::weak_ptr<Completion> completion;
      HANDLE reached;
    };
    auto watch = new Watch{weak_from_this(), CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    auto wait = CreateThreadpoolWait(
        [](PTP_CALLBACK_INSTANCE, void *context, PTP_WAIT wait, TP_WAIT_RESULT) {
          auto watch = static_cast<Watch *>(context);
          if (auto completion = watch->completion.lock())
            completion->Reach(completion->event.signaledValue());
          CloseThreadpoolWait(wait);
          CloseHandle(watch->reached);
          delete watch;
        },
        watch, nullptr
    );
    SetThreadpoolWait(wait, watch->reached, nullptr);
    listener->setEventOnValue(event, watch->reached, expected);
  }
};

class MTLD3D12FenceImpl : public MTLD3D12DeviceChild<MTLD3D12Fence> {
  D3D12_FENCE_FLAGS flags_;
  std::shared_ptr<Completion> completion_ = std::make_shared<Completion>();
  D3DKMT_HANDLE allocation_ = 0;

public:
  MTLD3D12FenceImpl(
      MTLD3D12Device *pDevice, D3D12_FENCE_FLAGS flags, WMT::Reference<WMT::SharedEvent> &&event = {},
      D3DKMT_HANDLE allocation = 0
  ) :
      MTLD3D12DeviceChild<MTLD3D12Fence>(pDevice),
      flags_(flags),
      allocation_(allocation) {
    completion = completion_;
    completion_->event = std::move(event);
    completion_->listener = &device_->GetEventListener();
  }

  ~MTLD3D12FenceImpl() {
    {
      std::lock_guard lock(device_->fences.mutex);
      device_->fences.values.erase(completion);
    }
    if (allocation_) {
      D3DKMT_DESTROYALLOCATION destroy = {};
      destroy.hDevice = device_->GetLocalD3DKMT();
      destroy.hResource = allocation_;
      D3DKMTDestroyAllocation(&destroy);
    }
  }

  // a removed device has reached every fence it has (LoseDevice)
  HRESULT
  Initialize(UINT64 InitialValue) {
    auto &event = completion_->event;
    if (!event && (flags_ & (D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER))) {
      if (!(event = device_->GetMTLDevice().newSharedEvent()))
        return E_OUTOFMEMORY;
      event.signalValue(InitialValue);
      if (!(allocation_ = shareEvent(device_->GetLocalD3DKMT(), event)))
        return E_FAIL;
    }
    // an opened fence has the value of the fence it opens, a created one its initial value
    completion_->value = event ? event.signaledValue() : InitialValue;
    std::lock_guard lock(device_->fences.mutex);
    if (FAILED(device_->GetDeviceRemovedReason()))
      return DXGI_ERROR_DEVICE_REMOVED;
    device_->fences.values.insert(completion);
    return S_OK;
  }

  void
  Signaled(uint64_t Value) override {
    completion_->Reach(Value);
    if (completion_->event)
      completion_->event.signalValue(Value);
  }

  D3DKMT_HANDLE
  SharedAllocation() override {
    return allocation_;
  }

  ULONG STDMETHODCALLTYPE
  Release() {
    uint32_t refCount = --m_refCount;
    if (!refCount) {
      // pending signals keep the object alive after the last public release has unblocked its waiters
      completion->Reach(UINT64_MAX);
      ReleasePrivate();
    }
    return refCount;
  }

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return FAIL(E_POINTER);

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12Fence) || riid == __uuidof(ID3D12Fence1)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12Fence1), riid)) {
      WARN("ID3D12Fence: Unknown interface query ", str::format(riid));
    }

    return MTLD3D12DeviceChild::QueryInterface(riid, ppvObject);
  }

  UINT64 STDMETHODCALLTYPE
  GetCompletedValue() {
    // "if the device has been removed, the return value will be UINT64_MAX" (ID3D12Fence::GetCompletedValue)
    auto &event = completion_->event;
    return FAILED(device_->GetDeviceRemovedReason()) ? UINT64_MAX : event ? event.signaledValue() : completion->value.load();
  }

  HRESULT STDMETHODCALLTYPE
  SetEventOnCompletion(UINT64 Value, HANDLE Event) {
    if (Logger::logLevel() == LogLevel::Trace)
      TRACE("fence ", this, " at ", completion->value.load(), ": thread ", GetCurrentThreadId(),
            Event ? " sets an event at " : " waits for ", Value);
    // the caller is taken to wait from here, as it does for a fence it waits on
    auto waited = [began = diag::profile ? diag::Now() : 0, thread = GetCurrentThreadId()] {
      if (began)
        diag::Waited("fence", began, thread);
    };
    if (Event)
      completion->Expect(Value, [Event, waited] {
        waited();
        SetEvent(Event);
      });
    else {
      completion->Expect(Value)->wait(false);
      waited();
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  Signal(UINT64 Value) {
    if (Logger::logLevel() == LogLevel::Trace)
      TRACE("fence ", this, " signaled to ", Value, " by thread ", GetCurrentThreadId());
    Signaled(Value);
    return S_OK;
  }

  D3D12_FENCE_FLAGS STDMETHODCALLTYPE
  GetCreationFlags() {
    return flags_;
  }
};

HRESULT
CreateFence(MTLD3D12Device *pDevice, UINT64 InitialValue, D3D12_FENCE_FLAGS Flags, REFIID riid, void **ppFence) {
  InitReturnPtr(ppFence);
  auto fence = Com(new MTLD3D12FenceImpl(pDevice, Flags));
  HRESULT hr = fence->Initialize(InitialValue);
  if (FAILED(hr))
    return hr;
  if (!ppFence)
    return S_FALSE;
  return fence->QueryInterface(riid, ppFence);
}

HRESULT
OpenFence(
    MTLD3D12Device *pDevice, WMT::Reference<WMT::SharedEvent> &&Event, D3DKMT_HANDLE Allocation, REFIID riid,
    void **ppFence
) {
  if (!Event)
    return E_INVALIDARG;
  auto fence = Com(new MTLD3D12FenceImpl(pDevice, D3D12_FENCE_FLAG_SHARED, std::move(Event), Allocation));
  if (HRESULT hr = fence->Initialize(0); FAILED(hr))
    return hr;
  if (!ppFence)
    return S_FALSE;
  return fence->QueryInterface(riid, ppFence);
}

}; // namespace mullion