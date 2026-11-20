subdir('util')
subdir('airconv')

if not mullion_crossbuild
subdir('nativemetal')
else
subdir('winemetal')
endif

subdir('mullion')
subdir('dxgi')
subdir('d3d11')
subdir('d3d10')

# Mullion's descriptor handles require the 64-bit layout (EMBEDDED_DESCRIPTOR_HANDLE)
if get_option('enable_d3d12') and cpu_family != 'x86'
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