util_src = files([
  'util_env.cpp',
  'util_string.cpp',
  # 'util_fps_limiter.cpp',
  # 'util_flush.cpp',
  # 'util_gdi.cpp',
  # 'util_luid.cpp',
  # 'util_matrix.cpp',
  # 'util_shared_res.cpp',
  # 'util_sleep.cpp',
  'util_bloom.cpp',
  'util_md5.cpp',

  'thread.cpp',

  'com/com_guid.cpp',
  'com/com_private_data.cpp',

  'config/config.cpp',

  'log/log.cpp',
  'log/trace.cpp',

  'sha1/sha1.c',
  'sha1/sha1_util.cpp',
])

# SHA-1 on the ARMv8 SHA-1 instructions where the build is for Arm (sha1_neon.c); x86 keeps sha1.c's
if host_machine.cpu_family() == 'aarch64'
  util_src += files('sha1/sha1_neon.c')
endif

if not mullion_crossbuild
  util_src += files([
    'wsi_monitor_headless.cpp',
    'wsi_window_headless.cpp',
    'wsi_platform_darwin.cpp',
  ])
else
  util_src += files([
    'wsi_monitor_win32.cpp',
    'wsi_window_win32.cpp',
    'wsi_platform_win32.cpp',
  ])
endif

util_lib = static_library('util', util_src,
  include_directories : [ mullion_include_path ],
)

util_dep = declare_dependency(
  link_with           : [ util_lib ],
  include_directories : [ include_directories('.') ],
)
