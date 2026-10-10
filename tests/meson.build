depth_formats = run_command(find_program('python3', native : true), '-c', '''
import re, subprocess, sys
text = subprocess.check_output(sys.argv[1:] + ['-E', '-x', 'c++', '-'],
                               input=b'#include <dxgiformat.h>').decode()
names = list(dict.fromkeys(re.findall('DXGI_FORMAT_D[0-9]+[A-Z0-9_]*', text)))
if not names:
    raise SystemExit('no depth formats in dxgiformat.h')
print(', '.join('{' + name + ', "' + name + '"}' for name in names))
''', cpp.cmd_array(), check : true).stdout().strip()
depth_config = configuration_data()
depth_config.set('DXGI_DEPTH_FORMATS', depth_formats)
depth_formats_header = configure_file(output : 'depth_formats.hpp', configuration : depth_config)

subdir('dx11')
if get_option('enable_d3d12')
  subdir('d3d12')
  subdir('dxgi')
endif
subdir('d3d11')
subdir('d3d10')
subdir('host')
