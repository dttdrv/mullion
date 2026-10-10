depth_formats = run_command(find_program('python3', native : true), '-c', '''
import re, subprocess, sys
text = subprocess.check_output(sys.argv[1:] + ['-E', '-x', 'c++', '-'],
                               input=b'#include <dxgiformat.h>').decode()
names = list(dict.fromkeys(re.findall('DXGI_FORMAT_[A-Z0-9_]+', text)))
names.remove('DXGI_FORMAT_FORCE_UINT')
depth = [name for name in names if re.match('DXGI_FORMAT_D[0-9]', name)]
if not depth:
    raise SystemExit('no depth formats in dxgiformat.h')
for group in depth, names:
    print(', '.join('{' + name + ', "' + name + '"}' for name in group))
''', cpp.cmd_array(), check : true).stdout().strip().split('\n')
depth_config = configuration_data()
depth_config.set('DXGI_DEPTH_FORMATS', depth_formats[0])
depth_config.set('DXGI_FORMATS', depth_formats[1])
depth_formats_header = configure_file(output : 'depth_formats.hpp', configuration : depth_config)

subdir('dx11')
if get_option('enable_d3d12')
  subdir('d3d12')
  subdir('dxgi')
endif
subdir('d3d11')
subdir('d3d10')
subdir('host')
