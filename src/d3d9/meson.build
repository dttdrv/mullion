d3d9_src = [
  'd3d9.cpp',
  'd3d9_adapter.cpp',
  'd3d9_buffer.cpp',
  'd3d9_caps.cpp',
  'd3d9_device.cpp',
  'd3d9_device_draw.cpp',
  'd3d9_device_ff.cpp',
  'd3d9_device_misc.cpp',
  'd3d9_device_process.cpp',
  'd3d9_device_unimpl.cpp',
  'd3d9_fixed_function.cpp',
  'd3d9_format.cpp',
  'd3d9_interface.cpp',
  'd3d9_query.cpp',
  'd3d9_state.cpp',
  'd3d9_state_block.cpp',
  'd3d9_surface.cpp',
  'd3d9_swapchain.cpp',
  'd3d9_window.cpp',
  'd3d9_shader_object.cpp',
  'd3d9_texture.cpp',
  'd3d9_vertex_declaration.cpp',
]

d3d9_ld_args      = ['-L' + windows_native_install_dir] # a nice trick to force relink
d3d9_link_depends = []

d3d9_dll = shared_library('d3d9', d3d9_src,
  name_prefix         : '',
  dependencies        : [ dxgi_dep, mullion_dep, airconv_forward_dep, util_dep, winemetal_dep ],
  include_directories : [ mullion_include_path ],
  install             : true,
  install_dir         : windows_native_install_dir,
  vs_module_defs      : 'd3d9.def',
  link_args           : d3d9_ld_args,
  link_depends        : [ d3d9_link_depends ],
)

d3d9_dep = declare_dependency(
  link_with           : [ d3d9_dll ],
  include_directories : [ mullion_include_path, include_directories('.') ],
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
