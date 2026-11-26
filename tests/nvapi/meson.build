# the driver NVAPI reports; it writes the device list, which it puts back, so it runs alone
test('nvapi_driver', executable('nvapi_driver', ['nvapi_driver.cpp'],
                                include_directories : include_directories('../../external/nvapi')),
     is_parallel : false, timeout : 300)
# NVAPI's ray tracing functions, on a D3D12 device's structures, through DXIL's ray queries
if test_d3d12
  test('nvapi_raytracing_dxil', executable('nvapi_raytracing', ['nvapi_raytracing.cpp'], dependencies : d3d12_test_deps,
                                           include_directories : include_directories('../../external/nvapi')),
       args : ['dxil'], env : ['MULLION_ENABLE_NVEXT=1'], timeout : 600)
endif
