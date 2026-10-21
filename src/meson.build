subdir('util')
subdir('airconv')

if not mullion_crossbuild
subdir('nativemetal')
else
subdir('winemetal')
endif

subdir('mullion')
subdir('dxgi')
subdir('dxcore')
subdir('d3d11')
subdir('d3d10')

if get_option('enable_d3d12')
subdir('d3d12')
endif

if get_option('enable_d3d9')
subdir('d3d9')
endif

if get_option('enable_nvapi')
subdir('nvapi')
endif

if get_option('enable_nvngx')
subdir('nvngx')
endif