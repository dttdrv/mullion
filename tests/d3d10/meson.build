# each test checks one contract of Direct3D 10 as applications reach it; see the comment at the top of each file
d3d10_test_deps = [cpp.find_library('dxgi'), cpp.find_library('d3dcompiler_47')]
foreach name : ['device']
  test('d3d10_' + name, executable('d3d10_' + name, ['d3d10_' + name + '.cpp'], dependencies : d3d10_test_deps),
       timeout : 300)
endforeach
