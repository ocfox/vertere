project(
  'vertere',
  'c',
  version: '0.1.0',
  license: 'MIT',
  default_options: [
    'c_std=gnu2y',
    'warning_level=2',
    'werror=true',
  ],
)

cc = meson.get_compiler('c')

extra_c_args = cc.get_supported_arguments([
  '-Wshadow',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
  '-ftrivial-auto-var-init=zero',
])

add_project_arguments(extra_c_args, language: 'c')

glib_dep = dependency('glib-2.0')
gio_dep = dependency('gio-2.0')
gdk_pixbuf_dep = dependency('gdk-pixbuf-2.0')
gtk4_dep = dependency('gtk4')
layer_shell_dep = dependency('gtk4-layer-shell-0')
soup_dep = dependency('libsoup-3.0')
json_dep = dependency('json-glib-1.0')
sqlite_dep = dependency('sqlite3')
m_dep = cc.find_library('m', required: false)

core_deps = [
  glib_dep,
  gio_dep,
  gdk_pixbuf_dep,
  soup_dep,
  json_dep,
  sqlite_dep,
  m_dep,
]

vertere_core = static_library(
  'vertere_core',
  files(
    'src/capture.c',
    'src/provider.c',
    'src/store.c',
    'src/translate.c',
    'src/tray.c',
    'src/window_state.c',
  ),
  include_directories: include_directories('src'),
  dependencies: core_deps,
)

executable(
  'vertere',
  files(
    'src/app.c',
    'src/main.c',
    'src/ui.c',
  ),
  include_directories: include_directories('src'),
  link_with: vertere_core,
  dependencies: core_deps + [gtk4_dep, layer_shell_dep],
  install: true,
)

install_data(
  'data/applications/me.ocfox.Vertere.desktop',
  install_dir: get_option('datadir') / 'applications',
)
install_subdir(
  'data/icons',
  install_dir: get_option('datadir'),
)

test_inc = include_directories('src')
foreach t : ['translate', 'store', 'window_state', 'capture', 'provider']
  exe = executable(
    'test_' + t,
    files('tests/test_' + t + '.c'),
    include_directories: test_inc,
    link_with: vertere_core,
    dependencies: core_deps,
  )
  test(t, exe)
endforeach
