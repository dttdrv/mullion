# each test checks one contract of DXGI; see the comment at the top of each file
dxgi_test_deps = [cpp.find_library('dxgi'), cpp.find_library('d3d12'), cpp.find_library('d3dcompiler_47')]
dxgi_adapter = executable('dxgi_adapter', ['dxgi_adapter.cpp'], dependencies : dxgi_test_deps)
test('dxgi_adapter', dxgi_adapter, timeout : 300)
# with a vendor from the configuration: Apple's own
test('dxgi_adapter_vendor', dxgi_adapter, args : ['-', '106b'], timeout : 300)
