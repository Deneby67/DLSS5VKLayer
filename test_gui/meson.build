qt6 = import('qt6')
qt6_dep = dependency('qt6', modules: ['Widgets'])
test_inc = include_directories('../common', '../gui')

test_qt_sources = qt6.preprocess(
  moc_headers: ['../gui/shm_binder.h'],
  include_directories: test_inc,
  dependencies: qt6_dep,
)

binder_test = executable('binder_test',
  'binder_test.cpp',
  '../gui/shm_binder.cpp',
  test_qt_sources,
  include_directories: test_inc,
  dependencies: qt6_dep,
  link_args: [
    '-static-libstdc++', '-static-libgcc',
    '-pie', '-Wl,-z,nocopyreloc', '-Wl,-z,indirect-extern-access',
  ],
  install: false,
)

test('binder_test', binder_test,
  env: ['QT_QPA_PLATFORM=offscreen'],
)

inline_panel_test = executable('inline_panel_test',
  'inline_panel_test.cpp', '../gui/inline_nr_panel.cpp',
  include_directories: test_inc, dependencies: qt6_dep, install: false,
)
test('inline_panel_test', inline_panel_test, env: ['QT_QPA_PLATFORM=offscreen'])
python3 = find_program('python3')
test('inline_control_test', python3, args: files('inline_control_test.py'))

sr_panel_test = executable('dlss_sr_panel_test',
  'dlss_sr_panel_test.cpp', '../gui/dlss_sr_panel.cpp',
  include_directories: test_inc, dependencies: qt6_dep, install: false,
)
test('dlss_sr_panel_test', sr_panel_test, env: ['QT_QPA_PLATFORM=offscreen'])
test('dlss_sr_control_test', python3, args: files('dlss_sr_control_test.py'))
