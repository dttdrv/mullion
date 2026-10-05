# tests of source that also builds for the host, run without a GPU or Wine
test('host_tessellator',
     executable('host_tessellator', ['tessellator.cpp', '../reference/reference.cpp'],
                include_directories : include_directories('metal', '../../src/airconv', '../../src/airconv/shaders', '../../libs'),
                # the reference's and the Metal source's, neither written for this compiler's warnings
                cpp_args : ['-w'], override_options : ['cpp_std=c++20'], native : true),
     timeout : 300)
