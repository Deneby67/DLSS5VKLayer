windows_inc = include_directories('../standalone_runner/third_party', '../core')
windows_cpp_link_args = [
  '-static',
  '-nostdlib++',
  '-Wl,--start-group',
  '-lstdc++',
  '-lwinpthread',
  '-Wl,--end-group',
  '-pthread',
  '-lshell32',
]

executable('dlssnr_helper',
  '../helper/main.cpp',
  '../core/ngx_snippet.cpp',
  '../core/guard.cpp',
  '../core/ngx_fg.cpp',
  '../common/fg_profile.cpp',
  '../test_layer/fg_probe.cpp',
  cpp_args: ['-DDLSSFG_EMBED_PROBE'],
  include_directories: windows_inc,
  link_args: windows_cpp_link_args,
  install: false,
  gui_app: true
)

executable('fg_probe', '../test_layer/fg_probe.cpp', '../core/ngx_fg.cpp', '../core/guard.cpp',
  include_directories: windows_inc,
  link_args: windows_cpp_link_args,
  install: false,
)

executable('smoke', '../test_layer/smoke.cpp',
  include_directories: windows_inc,
  link_args: windows_cpp_link_args,
  install: false,
)

# Keep a descriptive alias for the Windows cross build's analyzer target.
if scan_build.found()
  run_target('analyze-windows',
    command: [scan_build, '--status-bugs', 'ninja', '-C', meson.project_build_root()],
  )
endif
