# the driver NVAPI reports; it writes the device list, which it puts back, so it runs alone
test('nvapi_driver', executable('nvapi_driver', ['nvapi_driver.cpp'],
                                include_directories : include_directories('../../external/nvapi')),
     is_parallel : false, timeout : 300)
