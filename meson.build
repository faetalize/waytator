project(
  'waytator',
  'c',
  version: '1.4.0',
  license: 'GPL-3.0-or-later',
  license_files: 'LICENSE',
  default_options: [
    'c_std=c11',
    'warning_level=2',
  ],
)

gnome = import('gnome')

gtk_dep = dependency('gtk4', version: '>=4.14')
adw_dep = dependency('libadwaita-1', version: '>=1.6')
layer_shell_dep = dependency('gtk4-layer-shell-0')
wayland_client_dep = dependency('wayland-client', version: '>=1.20')
wayland_protocols_dep = dependency('wayland-protocols', version: '>=1.14')
wayland_scanner_dep = dependency('wayland-scanner', version: '>=1.15', native: true)
cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: false)

subdir('protocols')
subdir('src')
