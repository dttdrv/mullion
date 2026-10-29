# Each test checks one contract of DXGI; see the comment at the top of each file.
# The common tests reach the same contract through D3D10/10.1/11, and build on x86 and on a
# build without D3D12; the others need a D3D12 device, direct queue or D3D12 format limits.
dxgi_common_deps = [cpp.find_library('dxgi'), cpp.find_library('d3d11'), cpp.find_library('d3d10')]
dxgi_d3d12_deps = dxgi_common_deps + [cpp.find_library('d3d12'), cpp.find_library('d3dcompiler_47')]
# as the library builds D3D12 (src/meson.build)
dxgi_has_d3d12 = get_option('enable_d3d12') and cpu_family != 'x86'

if dxgi_has_d3d12
  dxgi_common_args = []
  dxgi_common_deps = dxgi_d3d12_deps
else
  # the DxgiDevice's D3D12 half and the 12 in dxgi_apis() are compiled out, so no d3d12 import library is needed
  dxgi_common_args = ['-DMULLION_TEST_NO_D3D12']
endif

foreach name : ['buffers', 'composition', 'gamma', 'object', 'output']
  test('dxgi_' + name,
       executable('dxgi_' + name, ['dxgi_' + name + '.cpp'], dependencies : dxgi_common_deps, cpp_args : dxgi_common_args),
       suite : 'window', timeout : 300)
endforeach

if dxgi_has_d3d12
  dxgi_adapter = executable('dxgi_adapter', ['dxgi_adapter.cpp'], dependencies : dxgi_d3d12_deps)
  test('dxgi_adapter', dxgi_adapter, timeout : 300)
  # with a vendor from the configuration: Apple's own
  test('dxgi_adapter_vendor', dxgi_adapter, args : ['-', '106b'], timeout : 300)
  # and over a configuration file whose last section is another executable's
  test('dxgi_adapter_vendor_file', dxgi_adapter, args : ['-', '106b', '-'], timeout : 300)
  foreach name : ['color_space', 'factory', 'formats', 'latency', 'present', 'present_regions', 'resize_queues', \
                  'swapchain_flags']
    test('dxgi_' + name,
         executable('dxgi_' + name, ['dxgi_' + name + '.cpp'], dependencies : dxgi_d3d12_deps),
         suite : 'window', timeout : 300)
  endforeach
endif
