// contract: a shared NT handle opens the same fence on another device or process, even after duplication and
// release of its creator. Microsoft Learn, ID3D11Fence::CreateSharedHandle: "Creates a shared handle to a fence
// object." ID3D11Device5::OpenSharedFence: "only operates on fences".
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11fence-createsharedhandle
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_4/nf-d3d11_4-id3d11device5-opensharedfence
// ID3D11DeviceContext4::Signal: "Updates a fence to a specified value after all previous work has completed."
// ID3D11DeviceContext4::Wait: "Waits until the specified fence reaches or exceeds the specified value before
// future work can begin." ID3D11Fence::SetEventOnCompletion: "Specifies an event that should be fired when the
// fence reaches a certain value." the initial and signaled values cross UINT's width; event expiry is failure,
// never an expected result. there are no shaders.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11devicecontext4-signal
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11devicecontext4-wait
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11fence-seteventoncompletion
// CreateSharedHandle requires SHARED and GENERIC_ALL; invalid parameters return "DXGI_ERROR_INVALID_CALL".
// "The name is limited to MAX_PATH characters." names below that limit succeed; a longer one is invalid.
// DuplicateHandle: "The duplicate handle refers to the same object as the original handle."
// https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-duplicatehandle
#include "d3d11_test.hpp"
#include <d3d11_4.h>
#include <cstdlib>
#include <limits>

int
main(int argc, char **argv) {
  const UINT64 initial = UINT64(std::numeric_limits<UINT>::max()) + 1;
  const DWORD wait_ms = 1000;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Device5> device5;
  ComPtr<ID3D11DeviceContext4> context4;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  CHECK(device.As(&device5));
  CHECK(context.As(&context4));

  if (argc == 4 && !strcmp(argv[1], "--open")) {
    step("another process opens the duplicated handle after the creator was released");
    HANDLE shared = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[2], nullptr, 10)));
    HANDLE ready = reinterpret_cast<HANDLE>(uintptr_t(strtoull(argv[3], nullptr, 10)));
    ComPtr<ID3D11Fence> fence;
    CHECK(device5->OpenSharedFence(shared, IID_PPV_ARGS(&fence)));
    CloseHandle(shared);
    expect(fence->GetCompletedValue() == initial, "imported initial value is %llu", fence->GetCompletedValue());
    HANDLE completed = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!expect(completed != nullptr, "CreateEvent: %lu", GetLastError()))
      return verdict();
    CHECK(fence->SetEventOnCompletion(initial + 2, completed));
    CHECK(context4->Wait(fence.Get(), initial + 1));
    CHECK(context4->Signal(fence.Get(), initial + 2));
    if (!expect(!!SetEvent(ready), "SetEvent: %lu", GetLastError()))
      ExitProcess(1);
    CloseHandle(ready);
    if (!expect(WaitForSingleObject(completed, wait_ms) == WAIT_OBJECT_0, "child fence did not complete"))
      ExitProcess(1);
    expect(fence->GetCompletedValue() == initial + 2, "child completed value is %llu", fence->GetCompletedValue());
    CloseHandle(completed);
    return verdict();
  }

  step("unshared fences and non-fence handles cannot be shared or opened");
  ComPtr<ID3D11Fence> unshared, invalid;
  CHECK(device5->CreateFence(initial, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&unshared)));
  HANDLE shared = nullptr;
  expect(FAILED(unshared->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared)),
         "unshared fence accepted CreateSharedHandle");
  if (shared)
    CloseHandle(shared);
  HANDLE completed = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (!expect(completed != nullptr, "CreateEvent: %lu", GetLastError()))
    return verdict();
  expect(FAILED(device5->OpenSharedFence(completed, IID_PPV_ARGS(&invalid))), "event handle opened as a fence");
  expect(FAILED(device5->OpenSharedFence(nullptr, IID_PPV_ARGS(&invalid))), "null handle opened as a fence");

  ComPtr<ID3D11Device> other;
  ComPtr<ID3D11DeviceContext> other_context;
  ComPtr<ID3D11Device5> other5;
  ComPtr<ID3D11DeviceContext4> other4;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &other, nullptr,
                          &other_context));
  CHECK(other.As(&other5));
  CHECK(other_context.As(&other4));
  step("a shared texture still opens as a texture, and cannot open as a fence");
  D3D11_TEXTURE2D_DESC texture_desc{1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
  ComPtr<ID3D11Texture2D> texture, imported;
  ComPtr<IDXGIResource1> resource;
  CHECK(device->CreateTexture2D(&texture_desc, nullptr, &texture));
  CHECK(texture.As(&resource));
  CHECK(resource->CreateSharedHandle(
      nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &shared));
  CHECK(other5->OpenSharedResource1(shared, IID_PPV_ARGS(&imported)));
  expect(FAILED(other5->OpenSharedFence(shared, IID_PPV_ARGS(&invalid))), "texture handle opened as a fence");
  CloseHandle(shared);
  for (bool release_creator : {false, true}) {
    step("second device, creator %s, duplicated handle and two-way signaling", release_creator ? "released" : "live");
    ComPtr<ID3D11Fence> creator, sender, receiver;
    CHECK(device5->CreateFence(initial, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&creator)));
    CHECK(creator->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared));
    HANDLE duplicate = nullptr;
    if (!expect(!!DuplicateHandle(GetCurrentProcess(), shared, GetCurrentProcess(), &duplicate, 0, TRUE,
                                 DUPLICATE_SAME_ACCESS), "DuplicateHandle: %lu", GetLastError()))
      return verdict();
    CloseHandle(shared);
    if (release_creator)
      creator.Reset();
    CHECK(other5->OpenSharedFence(duplicate, IID_PPV_ARGS(&receiver)));
    CHECK(device5->OpenSharedFence(duplicate, IID_PPV_ARGS(&sender)));
    expect(receiver->GetCompletedValue() == initial, "receiver initial value is %llu", receiver->GetCompletedValue());
    CHECK(receiver->SetEventOnCompletion(initial + 1, completed));
    expect(WaitForSingleObject(completed, 0) == WAIT_TIMEOUT, "completion event was set before Signal");
    CHECK(context4->Signal(sender.Get(), initial + 1));
    if (!expect(WaitForSingleObject(completed, wait_ms) == WAIT_OBJECT_0, "receiver did not see Signal"))
      ExitProcess(1);
    expect(receiver->GetCompletedValue() == initial + 1, "receiver value is %llu", receiver->GetCompletedValue());
    CHECK(sender->SetEventOnCompletion(initial + 2, completed));
    CHECK(other4->Wait(receiver.Get(), initial + 1));
    CHECK(other4->Signal(receiver.Get(), initial + 2));
    if (!expect(WaitForSingleObject(completed, wait_ms) == WAIT_OBJECT_0, "sender did not see the return Signal"))
      ExitProcess(1);
    expect(sender->GetCompletedValue() == initial + 2, "sender value is %llu", sender->GetCompletedValue());
    CHECK(receiver->SetEventOnCompletion(initial + 1, completed));
    if (!expect(WaitForSingleObject(completed, wait_ms) == WAIT_OBJECT_0, "already completed value did not set event"))
      ExitProcess(1);
    CloseHandle(duplicate);
    expect(FAILED(device5->OpenSharedFence(duplicate, IID_PPV_ARGS(&invalid))), "closed handle opened as a fence");
    expect(sender->CreateSharedHandle(nullptr, 0, nullptr, &shared) == DXGI_ERROR_INVALID_CALL,
           "invalid sharing access was accepted");
    expect(sender->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, nullptr) == DXGI_ERROR_INVALID_CALL,
           "null shared-handle output was accepted");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, FALSE};
    CHECK(sender->CreateSharedHandle(&attributes, GENERIC_ALL, nullptr, &shared));
    CloseHandle(shared);
    std::wstring name(MAX_PATH - 1, L'f');
    CHECK(sender->CreateSharedHandle(nullptr, GENERIC_ALL, name.c_str(), &shared));
    CloseHandle(shared);
    name += L"ff";
    expect(sender->CreateSharedHandle(nullptr, GENERIC_ALL, name.c_str(), &shared) == DXGI_ERROR_INVALID_CALL,
           "sharing accepted a name longer than MAX_PATH");
  }

  step("cross-process Signal and Wait after creator release");
  ComPtr<ID3D11Fence> creator, sender;
  CHECK(device5->CreateFence(initial, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&creator)));
  CHECK(creator->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &shared));
  HANDLE duplicate = nullptr;
  if (!expect(!!DuplicateHandle(GetCurrentProcess(), shared, GetCurrentProcess(), &duplicate, 0, TRUE,
                               DUPLICATE_SAME_ACCESS), "DuplicateHandle: %lu", GetLastError()))
    return verdict();
  CloseHandle(shared);
  creator.Reset();
  CHECK(device5->OpenSharedFence(duplicate, IID_PPV_ARGS(&sender)));
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
  HANDLE ready = CreateEventA(&attributes, FALSE, FALSE, nullptr);
  if (!expect(ready != nullptr, "CreateEvent: %lu", GetLastError()))
    return verdict();
  char executable[MAX_PATH];
  if (!expect(!!GetModuleFileNameA(nullptr, executable, sizeof(executable)), "GetModuleFileName: %lu", GetLastError()))
    return verdict();
  std::string command = "\"" + std::string(executable) + "\" --open " +
                        std::to_string(reinterpret_cast<uintptr_t>(duplicate)) + " " +
                        std::to_string(reinterpret_cast<uintptr_t>(ready));
  STARTUPINFOA startup{sizeof(startup)};
  PROCESS_INFORMATION child{};
  if (!expect(!!CreateProcessA(
                  executable, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &child),
              "CreateProcess: %lu", GetLastError()))
    return verdict();
  CloseHandle(child.hThread);
  CloseHandle(duplicate);
  CHECK(sender->SetEventOnCompletion(initial + 2, completed));
  bool opened = expect(WaitForSingleObject(ready, wait_ms) == WAIT_OBJECT_0, "child did not open the shared fence");
  if (opened) {
    CHECK(context4->Signal(sender.Get(), initial + 1));
    expect(WaitForSingleObject(completed, wait_ms) == WAIT_OBJECT_0, "parent did not see the child's Signal");
  }
  if (!expect(WaitForSingleObject(child.hProcess, wait_ms) == WAIT_OBJECT_0, "child did not exit")) {
    expect(!!TerminateProcess(child.hProcess, 1), "TerminateProcess: %lu", GetLastError());
    expect(WaitForSingleObject(child.hProcess, wait_ms) == WAIT_OBJECT_0, "child did not terminate");
  }
  DWORD code = 1;
  GetExitCodeProcess(child.hProcess, &code);
  expect(code == 0, "child exited %lu", code);
  CloseHandle(child.hProcess);
  CloseHandle(ready);
  CloseHandle(completed);
  return verdict();
}
