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
#include <set>

namespace mullion {

struct Completion;

// the shared fences something has waited on. a notification of any of their events raises one event, which makes
// each fence that is still there take its event's value. the event and its wait last as long as the process: a
// notification Metal keeps for a value a fence never gets, or for a fence that is gone, costs no handle or wait, and
// sets nothing else
class Watcher {
public:
  HANDLE reached = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  static Watcher &
  Get() {
    static Watcher *watcher = new Watcher;
    return *watcher;
  }

  void
  Watch(std::weak_ptr<Completion> completion) {
    std::lock_guard<mullion::mutex> lock(mutex_);
    // a fence that is gone leaves the list here too, as no notification may ever come for it, and its entry keeps the
    // storage of its completion. a sweep each time the list has doubled since the last one costs O(1) a fence
    if (watched_.size() >= 2 * swept_) {
      std::erase_if(watched_, [](auto &watched) { return watched.expired(); });
      swept_ = std::max<size_t>(watched_.size(), 16);
    }
    watched_.push_back(std::move(completion));
  }

private:
  mullion::mutex mutex_;
  std::vector<std::weak_ptr<Completion>> watched_;
  size_t swept_ = 16;

  Watcher() {
    auto wait = CreateThreadpoolWait(
        [](PTP_CALLBACK_INSTANCE, void *context, PTP_WAIT wait, TP_WAIT_RESULT) {
          static_cast<Watcher *>(context)->Look(wait);
        },
        this, nullptr
    );
    SetThreadpoolWait(wait, reached, nullptr);
  }

  void Look(PTP_WAIT wait);
};

// a shared fence's value is its event's, which the devices it is shared with signal too. Metal's events only rise, so
// a shared fence is not set back
struct Completion : Reached, std::enable_shared_from_this<Completion> {
  WMT::Reference<WMT::SharedEvent> event;
  EventListener *listener;
  // the values Metal notifies the watcher of, of those the event did not have when last asked; under Reached's lock,
  // which Waiting runs under
  std::set<uint64_t> notified;
  bool watched = false;

  // the event's value, once it is past this object's
  void
  Look() {
    uint64_t signaled = event.signaledValue();
    if (signaled > value)
      Reach(signaled);
  }

  // one notification for each value something waits for, which raises the watcher's event once the event has it
  void
  Waiting(uint64_t expected) override {
    if (!event)
      return;
    uint64_t signaled = event.signaledValue();
    std::erase_if(notified, [&](uint64_t notifying) { return notifying <= signaled; });
    if (!notified.insert(expected).second)
      return;
    auto &watcher = Watcher::Get();
    if (!std::exchange(watched, true))
      watcher.Watch(weak_from_this());
    listener->setEventOnValue(event, watcher.reached, expected);
  }
};

void
Watcher::Look(PTP_WAIT wait) {
  // set again first: a notification that comes while the fences are looked at has them looked at again
  SetThreadpoolWait(wait, reached, nullptr);
  std::vector<std::shared_ptr<Completion>> live;
  {
    std::lock_guard<mullion::mutex> lock(mutex_);
    // a fence that is gone leaves the list
    std::erase_if(watched_, [&](auto &watched) {
      if (auto completion = watched.lock()) {
        live.push_back(std::move(completion));
        return false;
      }
      return true;
    });
  }
  for (auto &completion : live)
    completion->Look();
}

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
    if (event)
      completion_->value = event.signaledValue();
    else if (flags_ & (D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER)) {
      if (!(event = device_->GetMTLDevice().newSharedEvent()))
        return E_OUTOFMEMORY;
      event.signalValue(InitialValue);
      if (!(allocation_ = shareEvent(device_->GetLocalD3DKMT(), event)))
        return E_FAIL;
    }
    if (!event)
      completion_->value = InitialValue;
    std::lock_guard lock(device_->fences.mutex);
    if (FAILED(device_->RemovedReason()))
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
    return FAILED(device_->RemovedReason()) ? UINT64_MAX : event ? event.signaledValue() : completion->value.load();
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
    if (Event) {
      auto object = EventObject(Event);
      if (!object)
        return FAIL(E_INVALIDARG);
      completion->Expect(Value, [object, waited] {
        waited();
        SetEvent(object.get());
      });
    } else {
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