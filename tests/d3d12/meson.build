d3d12_test_deps = [ cpp.find_library('d3d12'), cpp.find_library('d3dcompiler_47'), cpp.find_library('dxgi'), cpp.find_library('psapi') ]
# 'window' tests present to a real window: through a Metal view where winemac.drv exports it, through GDI elsewhere
# the sources a test has beside its own
d3d12_test_sources = { 'd3d12_tessellator': files('../reference/reference.cpp') }
foreach t : [ ['d3d12_sampling', []], ['d3d12_compute', []], ['d3d12_resources', []], ['d3d12_math', []], ['d3d12_msaa', []], ['d3d12_features', []], ['d3d12_indirect', []], ['d3d12_indirect_draw', []], ['d3d12_indirect_vertices', []], ['d3d12_barrier', []], ['d3d12_fences', []], ['d3d12_wave', []], ['d3d12_heap', []], ['d3d12_sm66', []], ['d3d12_quads', []], ['d3d12_half', []], ['d3d12_levels', []], ['d3d12_viewports', []], ['d3d12_queries', []], ['d3d12_predication', []], ['d3d12_cached', []], ['d3d12_resolve', []], ['d3d12_tiled', []], ['d3d12_bundle', []], ['d3d12_recording', []], ['d3d12_loader', []], ['d3d12_copytiles', []], ['d3d12_depthbounds', []], ['d3d12_tilemap', []], ['d3d12_border', []], ['d3d12_tessellation', []], ['d3d12_patches', []], ['d3d12_tessellator', []], ['d3d12_atomic64', []], ['d3d12_geometry', []], ['d3d12_streamout', []], ['d3d12_streamout_writes', []], ['d3d12_assembly', []], ['d3d12_draws', []], ['d3d12_copies', []], ['d3d12_wide', []], ['d3d12_mesh', []], ['d3d12_meshout', []], ['d3d12_varyings', []], ['d3d12_feedback', []], ['d3d12_rayquery', []], ['d3d12_raytracing', []], ['d3d12_present', ['window']], ['d3d12_presented', ['window']], ['d3d12_swapchain', ['window']], ['d3d12_child', ['window']] ]
  exe = executable(t[0], [t[0] + '.cpp'] + d3d12_test_sources.get(t[0], []), dependencies: d3d12_test_deps)
  foreach front_end : ['dxil', 'dxbc']
    test(t[0] + '_' + front_end, exe, args: [ front_end ], suite: t[1], timeout: 600)
  endforeach
endforeach
