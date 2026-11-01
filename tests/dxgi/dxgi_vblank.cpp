// contract: IDXGIOutput::WaitForVBlank "Halt[s] a thread until the next vertical blank occurs" (Microsoft Learn,
// IDXGIOutput::WaitForVBlank), so calls in a row return once per refresh of the output's display: a caller that paces
// itself with them runs at the refresh rate, not as fast as it can call.
// https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgioutput-waitforvblank
// the refresh rate is the display's current mode as user32 reports it (EnumDisplaySettings, ENUM_CURRENT_SETTINGS,
// dmDisplayFrequency); where it reports none (0 or 1, "the hardware's default"), the bound is that of a 240 Hz display.
// one call lines the thread up with a vertical blank, then the time of `calls` more is measured. those take `calls`
// refresh periods; the test asks for half of that less one period, so a loaded machine whose sleeps overrun cannot
// make it fail, and for no more than three periods a call and a second besides, so that a wait that never ends or
// waits a whole second does.
#include "dxgi_test.hpp"

int
main() {
  ComPtr<IDXGIFactory1> factory;
  CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
  ComPtr<IDXGIAdapter1> adapter;
  CHECK(factory->EnumAdapters1(0, &adapter));
  ComPtr<IDXGIOutput> output;
  CHECK(adapter->EnumOutputs(0, &output));
  DXGI_OUTPUT_DESC desc{};
  CHECK(output->GetDesc(&desc));
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  DWORD hz = EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode) ? mode.dmDisplayFrequency : 0;
  printf("%ls: %lu Hz\n", desc.DeviceName, hz);
  if (hz <= 1)
    hz = 240;
  LARGE_INTEGER frequency, before, after;
  QueryPerformanceFrequency(&frequency);
  const double period = 1.0 / hz;

  step("line up with a vertical blank");
  CHECK(output->WaitForVBlank());
  step("wait for the next vertical blanks in a row");
  constexpr UINT calls = 10;
  QueryPerformanceCounter(&before);
  for (UINT i = 0; i < calls; i++)
    CHECK(output->WaitForVBlank());
  QueryPerformanceCounter(&after);
  double seconds = double(after.QuadPart - before.QuadPart) / frequency.QuadPart;
  printf("%u calls took %.2f ms, %.2f refresh periods\n", calls, seconds * 1e3, seconds / period);
  expect(seconds >= (calls - 1) * period / 2, "%u calls returned in %.2f ms; %u refreshes at %lu Hz take %.2f ms",
         calls, seconds * 1e3, calls, hz, calls * period * 1e3);
  expect(seconds <= 3 * calls * period + 1.0, "%u calls took %.2f ms; %u refreshes at %lu Hz take %.2f ms", calls,
         seconds * 1e3, calls, hz, calls * period * 1e3);
  return verdict();
}
