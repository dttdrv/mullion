mullion_src = [
  'mullion_format.cpp',
  'mullion_names.cpp',
  'mullion_command_queue.cpp',
  'mullion_command.cpp',
  'mullion_capture.cpp',
  'mullion_info.cpp',
  'mullion_device.cpp',
  'mullion_buffer.cpp',
  'mullion_texture.cpp',
  'mullion_context.cpp',
  'mullion_dynamic.cpp',
  'mullion_staging.cpp',
  'mullion_hud_state.cpp',
  'mullion_allocation.cpp',
  'mullion_presenter.cpp',
  'mullion_sampler.cpp',
  'mullion_resource_initializer.cpp',
  'mullion_shader_cache.cpp',
  'mullion_scaler.cpp',
  'mullion_subresource.cpp',
  'mullion_deptrack.cpp',
  'mullion_counter.cpp',
  'mullion_fence.cpp',
  'mullion_keyed_mutex.cpp',
]

mullion_shaders = [
  'mullion_command.metal'
]

mullion_shaders_ir = metalir_generator.process(mullion_shaders, extra_args: macos_min)

mullion_shaders_lib = metallib_generator.process(mullion_shaders_ir)

mullion_shaders_header = hexdump_generator.process(mullion_shaders_lib)

mullion_lib = static_library('mullion', mullion_src, mullion_shaders_header, mullion_version,
  dependencies        : [ util_dep, airconv_forward_dep, winemetal_dep ],
  include_directories : [ mullion_include_path ],
)

mullion_dep = declare_dependency(
  link_with           : [ mullion_lib ],
  include_directories : [ mullion_include_path, include_directories('.') ],
)
