// contract: FinishCommandList's ppCommandList is "[out, optional]" (Microsoft Learn,
// ID3D11DeviceContext::FinishCommandList). without it the call returns E_UNEXPECTED and the commands recorded so far
// are dropped, as Windows does (Wine's test_deferred_context_rendering: "A NULL command list returns E_UNEXPECTED, but
// still drops the recorded commands"). a copy recorded before such a call does not run from the next list; one recorded
// after it does.
#include "d3d11_test.hpp"
#include <cstdlib>
#include <random>

int
main(int argc, char **argv) {
  const UINT seed = argc > 1 ? strtoul(argv[1], nullptr, 0) : 21;
  std::minstd_rand next(seed);
  step("seed %u: create device and buffers", seed);
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &immediate
  ));
  CHECK(device->CreateDeferredContext(0, &deferred));
  const UINT copied = next(), kept = next() ^ 1;
  auto source = buffer(device.Get(), sizeof(copied), 0, copied);
  auto destination = buffer(device.Get(), sizeof(kept), 0, kept);
  if (!expect(source && destination, "copy buffers were not created"))
    return verdict();

  step("seed %u: a copy, then FinishCommandList without a list", seed);
  deferred->CopyResource(destination.Get(), source.Get());
  HRESULT hr = deferred->FinishCommandList(FALSE, nullptr);
  expect(hr == E_UNEXPECTED, "FinishCommandList without a list returned %#lx, not E_UNEXPECTED", hr);
  ComPtr<ID3D11CommandList> list;
  CHECK(deferred->FinishCommandList(FALSE, &list));
  immediate->ExecuteCommandList(list.Get(), FALSE);
  auto words = read(device.Get(), immediate.Get(), destination.Get());
  expect(words.size() == 1 && words[0] == kept, "the dropped copy ran: %#x, not the kept %#x",
         words.empty() ? 0u : words[0], kept);

  step("seed %u: a copy recorded after it runs", seed);
  deferred->CopyResource(destination.Get(), source.Get());
  list.Reset();
  CHECK(deferred->FinishCommandList(FALSE, &list));
  immediate->ExecuteCommandList(list.Get(), FALSE);
  words = read(device.Get(), immediate.Get(), destination.Get());
  expect(words.size() == 1 && words[0] == copied, "the recorded copy gave %#x, not %#x", words.empty() ? 0u : words[0],
         copied);
  return verdict();
}
