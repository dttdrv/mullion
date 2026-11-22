# tests of source that also builds for the host, run without a GPU or Wine: the suite 'host', which CI runs whole
test('host_colorspace', executable('host_colorspace', 'colorspace.cpp',
                                 override_options : ['cpp_std=c++20'], native : true), suite : 'host')
test('host_geometry_budget', executable('host_geometry_budget', 'geometry_budget.cpp',
                                      include_directories : mullion_native_include_path,
                                      override_options : ['cpp_std=c++20'], native : true), suite : 'host')
test('host_format', executable('host_format', 'format.cpp', include_directories : mullion_native_include_path,
                             override_options : ['cpp_std=c++20'], native : true), suite : 'host')
test('host_rc_self_move', executable('host_rc_self_move', 'rc.cpp',
                                    override_options : ['cpp_std=c++20'], native : true), suite : 'host')
test('host_private_data', executable('host_private_data', ['private_data.cpp', '../../src/util/com/com_private_data.cpp'],
     include_directories : [mullion_native_include_path, include_directories('../../src/util')],
     override_options : ['cpp_std=c++20'], native : true), suite : 'host')
test('host_denorm', executable('host_denorm', 'denorm.cpp', include_directories : include_directories('metal'),
                             override_options : ['cpp_std=c++20'], native : true), suite : 'host')
test('host_tessellator',
     executable('host_tessellator', ['tessellator.cpp', '../reference/reference.cpp'],
                include_directories : include_directories('metal', '../../src/airconv', '../../src/airconv/shaders', '../../libs'),
                # the reference's and the Metal source's, neither written for this compiler's warnings
                cpp_args : ['-w'], override_options : ['cpp_std=c++20'], native : true),
     # every factor and partitioning on one core: about 270 s on CI's runner, past 300 s when the runner is loaded
     suite : 'host', timeout : 900)
# the converter's own functions, built for the host as the bridge links them
test('host_readerio_frame', executable('host_readerio_frame', 'readerio_frame.cpp',
     include_directories : llvm_include_path_darwin, dependencies : airconv_dep_darwin,
     native : true), suite : 'host')
test('host_ray_dispatchers',
     executable('host_ray_dispatchers', 'ray_dispatchers.cpp', dependencies : airconv_dep_darwin,
                cpp_args : ['-w'], override_options : ['cpp_std=c++20'], native : true),
     suite : 'host', timeout : 120)
test('host_parse_shared',
     executable('host_parse_shared', 'parse_shared.cpp', dependencies : airconv_dep_darwin,
                cpp_args : ['-w'], override_options : ['cpp_std=c++20'], native : true),
     suite : 'host', timeout : 120)
test('host_coherent_version',
     executable('host_coherent_version', 'coherent_version.cpp', dependencies : [airconv_dep_darwin, dxbc_parser_native_dep],
                cpp_args : ['-w'], override_options : ['cpp_std=c++20'], native : true),
     suite : 'host', timeout : 120)
test('host_pixel_input_declarations',
     executable('host_pixel_input_declarations', 'pixel_input_declarations.cpp',
                include_directories : llvm_include_path_darwin,
                dependencies : [airconv_dep_darwin, dxbc_parser_native_dep],
                cpp_args : ['-w'], override_options : ['cpp_std=c++20'], native : true),
     suite : 'host', timeout : 120)
test('host_geometry_payload',
     executable('host_geometry_payload', 'geometry_payload.cpp',
                include_directories : llvm_include_path_darwin,
                dependencies : [airconv_dep_darwin, dxbc_parser_native_dep],
                cpp_args : ['-w'], override_options : ['cpp_std=c++20'], native : true),
     suite : 'host', timeout : 120)
# which DXIL operations shaders use, for tests/coverage.py; it reads DXIL with the LLVM the converter is built with
executable('dxilops', ['dxilops.cpp'], include_directories : llvm_include_path_darwin,
           link_args : [ llvm_ld_flags_darwin, llvm_deps ], cpp_args : ['-w'], override_options : ['cpp_std=c++20'], native : true)
# the tests' own container hash against the hashes real shaders carry; it wants directories of them (MULLION_HASHED)
test('host_dxbc_hash', executable('host_dxbc_hash', ['dxbchash.cpp'], override_options : ['cpp_std=c++20'], native : true),
     suite : 'host')
# the bridge's shader store, which is Objective-C built as the bridge builds it
test('host_shader_store',
     executable('host_shader_store', ['shader_store.c', '../../src/winemetal/unix/cache.c'],
                include_directories : [mullion_include_path, include_directories('../../src/winemetal', '../../src/airconv')],
                c_args : ['-ObjC', '-w'],
                # the native architecture reaches only C++ links (the project's link arguments)
                link_args : ['-arch', native_compiler_arch, '-framework', 'Foundation', '-framework', 'Metal',
                             '-lsqlite3'],
                native : true),
     suite : 'host', timeout : 120)
# converted texture reads and atomics through Apple's verifier, which xcrun finds beside Metal's compiler
test('host_texel_forms',
     executable('host_texel_forms', 'texel_forms.cpp', dependencies : [airconv_dep_darwin, dxbc_parser_native_dep],
                cpp_args : ['-w'], override_options : ['cpp_std=c++20'], native : true),
     suite : 'host', timeout : 300)
# SHA-1's digests; where the native compiler targets Arm, its blocks go through the ARMv8 SHA-1 instructions as an Arm build's do
test('host_sha1', executable('host_sha1', 'sha1.cpp', '../../src/util/sha1/sha1.c',
                           include_directories : include_directories('../../src/util'), override_options : ['cpp_std=c++20'],
                           native : true), suite : 'host')
test('host_diag_profile', find_program('python3', native : true), args : files('diag.py'), suite : 'host')
test('host_mutate', find_program('python3', native : true), args : files('mutate.py'), suite : 'host')
test('host_install', find_program('python3', native : true),
     args : files('install.py') + [meson.project_source_root() / 'install.sh'], suite : 'host')
test('host_lock', find_program('python3', native : true),
     args : files('lock.py') + [meson.project_source_root() / 'tests/run.py'], suite : 'host')
test('host_admission', find_program('python3', native : true),
     args : files('admission.py') + [meson.project_source_root() / 'tests/run.py'], suite : 'host')
test('host_frames', find_program('python3', native : true),
     args : files('frames.py') + [meson.project_source_root() / 'tools/frames.py'], suite : 'host')
test('host_metal_target', find_program('python3', native : true), args : [files('metal_target.py'), d3d11_dll, macos_min],
     suite : 'host')
