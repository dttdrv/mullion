d3d9_res = wrc_generator.process('version.rc')

d3d9_src = [
  'd3d9.cpp',
  'd3d9_buffer.cpp',
  'd3d9_direct3d.cpp',
  'd3d9_device.cpp',
  'd3d9_fixed_function.cpp',
  'd3d9_state.cpp',
  'd3d9_swapchain.cpp',
  'd3d9_surface.cpp',
]

d3d9_ld_args      = ['-L' + windows_native_install_dir] # a nice trick to force relink

# a device of Direct3D 9's is a device of this build's Direct3D 11 over the same Metal device, and its chain is DXGI's
d3d9_dll = shared_library('d3d9', d3d9_src, d3d9_res,
  name_prefix         : '',
  dependencies        : [
    d3d11_dep,
    dxgi_dep,
    util_dep ],
  include_directories : [ mullion_include_path ],
  install             : true,
  install_dir         : windows_native_install_dir,
  vs_module_defs      : 'd3d9.def',
  link_args           : d3d9_ld_args,
)

d3d9_dep = declare_dependency(
  link_with           : [ d3d9_dll ],
  include_directories : [ mullion_include_path ],
)

if wine_builtin_dll
custom_target('postprocess_libd3d9',
  input : d3d9_dll,
  output: 'd3d9.dll.postproc',
  command : [ winebuild, '--builtin', '@INPUT@' ],
  depends : d3d9_dll,
  build_by_default : true
)
endif
