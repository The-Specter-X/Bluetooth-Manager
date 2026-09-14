project('mytooth', 'c', version: '0.1.0', meson_version: '>=0.61',
  default_options: ['c_std=c17', 'warning_level=3'])

cc = meson.get_compiler('c')
add_project_arguments('-D_GNU_SOURCE', '-DG_LOG_USE_STRUCTURED',
  '-DG_LOG_DOMAIN="mytooth"', language: 'c')
add_project_arguments(cc.get_supported_arguments([
  '-Wformat=2', '-Wshadow', '-Wstrict-prototypes', '-Wmissing-prototypes',
  '-Werror=implicit-function-declaration', '-Wno-unused-parameter']), language: 'c')
gio = dependency('gio-unix-2.0', version: '>=2.66')
gtk = dependency('gtk+-3.0', version: '>=3.24')
xapp = dependency('xapp', version: '>=2.0')
inc = include_directories('src')
core = static_library('mytooth-core', 'src/bluetooth.c', 'src/agent.c',
  'src/util.c', dependencies: gio, include_directories: inc)
executable('mytooth', 'src/main.c', 'src/window.c', 'src/tray.c',
  'src/settings.c', 'src/rfkill.c', 'src/session.c',
  link_with: core, dependencies: [gio, gtk, xapp], install: true)

install_data('data/io.github.the_specter_x.Mytooth.desktop',
  install_dir: get_option('datadir') / 'applications')
install_data('data/io.github.the_specter_x.Mytooth.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')
install_man('data/mytooth.1')
subdir('tests')
