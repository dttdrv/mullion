# each test checks one contract of Direct3D 9 as applications reach it; run.sh also runs them outside tests/run.py
lib_d3d9 = cpp.find_library('d3d9')
lib_d3dcompiler_d3d9 = cpp.find_library('d3dcompiler_47')

foreach t : [['readback', []], ['resources', []], ['draw', [lib_d3dcompiler_d3d9]], ['states', [lib_d3dcompiler_d3d9]],
             ['fixed_function', [lib_d3dcompiler_d3d9]], ['present', []], ['queries', []], ['state_blocks', []],
             ['ps1', []], ['stretch', []], ['misc', []]]
  test('d3d9_' + t[0], executable('d3d9_' + t[0], ['d3d9_' + t[0] + '.cpp'], dependencies : [lib_d3d9] + t[1], link_args : ['-mwindows']),
       suite : 'window', timeout : 300)
endforeach

# a window to look at, and a test that takes over the display: run by hand
executable('d3d9_clear', ['d3d9_clear.cpp'], dependencies : [lib_d3d9], link_args : ['-mwindows'])
executable('d3d9_fullscreen', ['d3d9_fullscreen.cpp'], dependencies : [lib_d3d9], link_args : ['-mwindows'])

# what a program from before DEP, which only exists as 32-bit, needs of Wine: it loads none of Mullion, so run.sh runs it
if host_machine.cpu_family() == 'x86'
  executable('d3d9_rosetta_memory', ['d3d9_rosetta_memory.c'], link_args : ['-Wl,--disable-nxcompat'])
endif
