depth_formats = run_command(find_program('python3', native : true), '-c', '''
import re, subprocess, sys
text = subprocess.check_output(sys.argv[1:] + ['-E', '-x', 'c++', '-'],
                               input=b'#include <dxgiformat.h>\n#include <d3d12.h>').decode()
names = list(dict.fromkeys(re.findall('DXGI_FORMAT_[A-Z0-9_]+', text)))
names.remove('DXGI_FORMAT_FORCE_UINT')
depth = [name for name in names if re.match('DXGI_FORMAT_D[0-9]', name)]
if not depth:
    raise SystemExit('no depth formats in dxgiformat.h')
for group in depth, names:
    print(', '.join('{' + name + ', "' + name + '"}' for name in group))
for enum in ('D3D12_FORMAT_SUPPORT1', 'D3D12_FORMAT_SUPPORT2'):
    body = re.search(r'enum ' + enum + r'\s*\{(.*?)\}', text, re.S)
    if not body:
        raise SystemExit('no ' + enum + ' in d3d12.h')
    print(' | '.join(re.findall(r'\b(' + enum + r'_\w+)\s*=', body[1])))
''', cpp.cmd_array(), check : true).stdout().strip().split('\n')
depth_config = configuration_data()
depth_config.set('DXGI_DEPTH_FORMATS', depth_formats[0])
depth_config.set('DXGI_FORMATS', depth_formats[1])
depth_config.set('D3D12_FORMAT_SUPPORT1_ALLOWED', depth_formats[2])
depth_config.set('D3D12_FORMAT_SUPPORT2_ALLOWED', depth_formats[3])
depth_formats_header = configure_file(output : 'depth_formats.hpp', configuration : depth_config)

copy_formats_header = configure_file(output : 'copy_formats.hpp',
                                     command : [find_program('python3', native : true), files('copy_formats.py')] + cpp.cmd_array(),
                                     capture : true)
configure_file(input : 'host/resource_cases.py', output : 'resource_cases.hpp',
  command : [find_program('python3', native : true), '@INPUT@', '@OUTPUT@'] + cpp.cmd_array())

subdir('dx11')
# as the library builds D3D12 (src/meson.build)
test_d3d12 = get_option('enable_d3d12') and cpu_family != 'x86'
if test_d3d12
  levels = run_command(cpp.cmd_array() + ['-E', '-P', '-x', 'c++', '-include', 'd3dcommon.h', '/dev/null'],
                       check: true).stdout().split('enum D3D_FEATURE_LEVEL')[1].split('{')[1].split('}')[0]
  names = []
  foreach level : levels.split(',')
    name = level.strip().split('=')[0].strip()
    if name != ''
      names += name
    endif
  endforeach
  data = configuration_data()
  data.set('D3D12_TEST_FEATURE_LEVELS', ', '.join(names))
  configure_file(output: 'feature_levels.hpp', configuration: data)
  subdir('d3d12')
  subdir('dxgi')
  subdir('perf')
endif
subdir('d3d11')
subdir('d3d10')
# the host's own tests build against the macOS LLVM, which a 32-bit build does not make
if is_variable('llvm_include_path_darwin')
  subdir('host')
endif
