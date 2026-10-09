project(
  'crepido',
  'c',
  version: '0.1.0',
  meson_version: '>= 1.0.0',
  default_options: ['c_std=c11', 'warning_level=3']
)

gtk = dependency('gtk+-3.0', version: '>= 3.24')
x11 = dependency('x11')
gio = dependency('gio-2.0')
gio_unix = dependency('gio-unix-2.0')
m = meson.get_compiler('c').find_library('m', required: false)

executable(
  'crepido',
  'src/main.c',
  'src/dock-clip.c',
  'src/dock-monitor.c',
  'src/dock.c',
  'src/dock-dnd.c',
  'src/dock-icon.c',
  'src/dock-launcher.c',
  'src/dock-config.c',
  'src/dock-x11.c',
  'src/dock-x11-match.c',
  'src/dock-x11-property32.c',
  dependencies: [gtk, x11, gio, gio_unix, m],
  install: true
)

if get_option('install_mate_autostart')
  install_data(
    'data/crepido.desktop',
    install_dir: join_paths(
      get_option('sysconfdir'),
      'xdg',
      'autostart'))
endif

# The configuration module is independent of GTK, so its tests can run
# without a display server or a running desktop session.
glib = dependency('glib-2.0')

config_test = executable(
  'test-dock-config',
  'tests/test-dock-config.c',
  'src/dock-config.c',
  include_directories: include_directories('src'),
  dependencies: [glib]
)

test(
  'dock-config',
  config_test,
  suite: 'unit'
)

monitor_test = executable(
  'test-dock-monitor',
  'tests/test-dock-monitor.c',
  'src/dock-monitor.c',
  include_directories: include_directories('src'),
  dependencies: [glib]
)

test(
  'dock-monitor',
  monitor_test,
  suite: 'unit'
)

dnd_test = executable(
  'test-dock-dnd',
  'tests/test-dock-dnd.c',
  'src/dock-dnd.c',
  include_directories: include_directories('src'),
  dependencies: [glib]
)

test(
  'dock-dnd',
  dnd_test,
  suite: 'unit'
)

x11_match_test = executable(
  'test-dock-x11-match',
  'tests/test-dock-x11-match.c',
  'src/dock-x11-match.c',
  include_directories: include_directories('src'),
  dependencies: [glib]
)

test(
  'dock-x11-match',
  x11_match_test,
  suite: 'unit'
)

x11_property32_test = executable(
  'test-dock-x11-property32',
  'tests/test-dock-x11-property32.c',
  'src/dock-x11-property32.c',
  include_directories: include_directories('src'),
  dependencies: [glib]
)

test(
  'dock-x11-property32',
  x11_property32_test,
  suite: 'unit'
)

# The integration test uses Xvfb so it never edits the developer's real
# desktop EWMH properties. It is registered only where xvfb-run is installed.
xvfb_run = find_program('xvfb-run', required: false)

x11_integration_test = executable(
  'test-dock-x11-integration',
  'tests/test-dock-x11-integration.c',
  'src/dock-x11.c',
  'src/dock-icon.c',
  'src/dock-launcher.c',
  'src/dock-x11-match.c',
  'src/dock-x11-property32.c',
  include_directories: include_directories('src'),
  dependencies: [gtk, x11, gio, gio_unix, m]
)

if xvfb_run.found()
  test(
    'dock-x11-integration',
    xvfb_run,
    args: ['-a', x11_integration_test],
    suite: 'integration',
    timeout: 30
  )
else
  warning('Skipping dock-x11-integration because xvfb-run is not installed')
endif
