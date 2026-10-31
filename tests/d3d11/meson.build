# each test checks one contract of the D3D11 API; see the comment at the top of each file
d3d11_test_deps = [cpp.find_library('d3d11'), cpp.find_library('dxgi'), cpp.find_library('d3dcompiler_47')]

# the sources a test has beside its own
d3d11_test_sources = { 'tessellator': files('../reference/reference.cpp') }
# the libraries a test links beside Direct3D's: the Metal bridge, for a test of the bridge itself
d3d11_test_links = { 'bridge_errors': [winemetal_dep] }
foreach name : ['blending', 'block_values', 'clear_values', 'constant_updates', 'depth_stencil', 'depth_values', 'discard', 'dxbc_arithmetic', 'dxbc_calls', 'dynamic_initial', 'format_blend', 'format_logic', 'format_msaa', 'format_values', 'generate_mips', 'ia_draws', 'ia_formats', 'ia_patches', 'logic', 'om_order', 'om_uav', 'raster', 'raster_bias', 'raster_samples', 'raster_state', 'resinfo', 'resolve', 'resource_dimensions', 'resource_usage', 'sample_mask', 'coverage_blend', 'sample_positions', 'shader_groups', 'structured_copies', 'texture_updates', 'uav_counters', 'anisotropy_bias', 'geometry_indirect_parts', 'geometry_parts', 'geometry_oversize', 'trace_encoders', 'sample_compare', 'integer_rare', 'gs_restart', 'atomics_rare', 'rgb32', 'sincos', 'clear_rectangles', 'copy_formats', 'resource_creation', 'view_creation', 'device_arguments', 'psout', 'device_teardown', 'default_updates', 'default_allocations', 'default_growth', 'bridge_errors', 'planar', 'feature_queries', 'shared_resource', 'subresource_surface', 'interface_eligibility', 'device_interfaces', 'shader_levels', 'swapchain_buffers', 'initial_data', 'input_layout', 'streamout_declarations', 'copy_region', 'staging_depth', 'timestamp', 'fullscreen', 'profile', 'queries', 'deferred_release', 'state_objects', 'buffer_loads', 'atomic_bounds', 'shared_fence', 'staging_order', 'commandlist_state', 'map_contract', 'state_transitions', 'occlusion_many', 'streamout', 'streamout_scratch', 'drawauto', 'assembly', 'primitiveid', 'tessellator', 'predication', 'staging', 'state', 'viewports', 'rectangles_many', 'surface', 'indices', 'device', 'constants', 'minlod', 'formats', 'packed', 'garbled', 'null_indices', 'null_stage_indices', 'dynamic_generations', 'ps_writes']
  # without the shader cache, so a test meets the shaders this build makes
  test('d3d11_' + name,
       executable('d3d11_' + name, ['d3d11_' + name + '.cpp'] + d3d11_test_sources.get(name, []),
                  dependencies : d3d11_test_deps + d3d11_test_links.get(name, [])),
       env : ['MULLION_SHADER_CACHE=0'], timeout : 600)
endforeach
if get_option('enable_d3d12')
  on12 = executable('d3d11_on12', 'd3d11_on12.cpp',
                    dependencies : d3d11_test_deps + [cpp.find_library('d3d12')])
foreach part : ['creation', 'wrapped', 'wait', 'unwrap', 'swapchain', 'lifetime', 'ordering', 'teardown', 'longwait', 'dimensions', 'cpu', 'flags', 'aliases', 'predication', 'independent']
    test(part == 'creation' ? 'd3d11_on12' : 'd3d11_on12_' + part, on12, args : [part],
         env : ['MULLION_SHADER_CACHE=0'], timeout : 600)
  endforeach
endif
# no test: the runner's way to see where the threads of a test that outlived its time are
# workloads tests/held.py profiles
executable('d3d11_held', 'd3d11_held.cpp', dependencies : d3d11_test_deps)
executable('where', ['where.cpp'], dependencies : [cpp.find_library('dbghelp')])
# 'window' tests present to a real window
test('d3d11_xrbias', executable('d3d11_xrbias', ['d3d11_xrbias.cpp'],
     dependencies : d3d11_test_deps + [cpp.find_library('gdi32')]),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 600)
test('d3d11_swapchain_formats', executable('d3d11_swapchain_formats', ['d3d11_swapchain_formats.cpp'], dependencies : d3d11_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)
test('d3d11_swapchain', executable('d3d11_swapchain', ['d3d11_swapchain.cpp'], dependencies : d3d11_test_deps),
     env : ['MULLION_SHADER_CACHE=0'], suite : 'window', timeout : 300)
