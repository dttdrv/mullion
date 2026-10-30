# each test checks one contract of DXGI; see the comment at the top of each file
dxgi_test_deps = [cpp.find_library('dxgi'), cpp.find_library('d3d12'), cpp.find_library('d3d11'), cpp.find_library('d3d10'), cpp.find_library('d3dcompiler_47')]
dxgi_adapter = executable('dxgi_adapter', ['dxgi_adapter.cpp'], dependencies : dxgi_test_deps)
test('dxgi_adapter', dxgi_adapter, timeout : 300)
# with a vendor from the configuration: Apple's own
test('dxgi_adapter_vendor', dxgi_adapter, args : ['-', '106b'], timeout : 300)
# and over a configuration file whose last section is another executable's
test('dxgi_adapter_vendor_file', dxgi_adapter, args : ['-', '106b', '-'], timeout : 300)
test('dxgi_adapter_memory', dxgi_adapter, args : ['-', '1002', 'no-file', '256'], timeout : 300)
# these present to a real window
foreach name : ['buffers', 'color_space', 'composition', 'factory', 'formats', 'gamma', 'latency', 'object', 'output', 'present', 'present_regions', 'resize_queues', 'swapchain_flags']
  test('dxgi_' + name, executable('dxgi_' + name, ['dxgi_' + name + '.cpp'], dependencies : dxgi_test_deps),
       suite : 'window', timeout : 300)
endforeach
