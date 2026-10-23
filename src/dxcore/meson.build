dxcore_ld_args      = ['-L' + windows_native_install_dir] # a nice trick to force relink
dxcore_link_depends = []

dxcore_dll = shared_library('dxcore', 'dxcore.cpp',
  name_prefix         : '',
  dependencies        : [ dxgi_dep, mullion_dep, util_dep ],
  include_directories : [ mullion_include_path ],
  install             : true,
  install_dir         : windows_native_install_dir,
  vs_module_defs      : 'dxcore.def',
  link_args           : dxcore_ld_args,
  link_depends        : [ dxcore_link_depends ],
)

dxcore_dep = declare_dependency(
  link_with           : [ dxcore_dll ],
  include_directories : [ mullion_include_path ],
)

if wine_builtin_dll
custom_target('postprocess_libdxcore',
  input : dxcore_dll,
  output: 'dxcore.dll.postproc',
  command : [ winebuild, '--builtin', '@INPUT@' ],
  depends : dxcore_dll,
  build_by_default : true
)
endif
