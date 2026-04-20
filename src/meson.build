waytator_conf = configuration_data()
waytator_conf.set_quoted('WAYTATOR_VERSION', meson.project_version())

waytator_sources = files(
  'main.c',
  'waytator-canvas-input.c',
  'waytator-canvas-layout.c',
  'waytator-canvas-render.c',
  'waytator-canvas-state.c',
  'waytator-document.c',
  'waytator-document.h',
  'waytator-export.c',
  'waytator-export.h',
  'waytator-ocr.c',
  'waytator-ocr.h',
  'waytator-render.c',
  'waytator-render.h',
  'waytator-stroke.c',
  'waytator-stroke.h',
  'waytator-types.h',
  'waytator-window.c',
  'waytator-window.h',
  'waytator-window-private.h',
)

configure_file(
  input: 'waytator-config.h.in',
  output: 'waytator-config.h',
  configuration: waytator_conf,
)

waytator_resources = gnome.compile_resources(
  'waytator-resources',
  'waytator.gresource.xml',
  source_dir: 'ui',
  c_name: 'waytator',
)

executable(
  'waytator',
  waytator_sources,
  waytator_resources,
  dependencies: [gtk_dep, adw_dep, m_dep],
  install: true,
)

install_data(
  'dev.faetalize.waytator.desktop',
  install_dir: join_paths(get_option('datadir'), 'applications'),
)

install_data(
  'ui/icons/icon.png',
  install_dir: join_paths(get_option('datadir'), 'icons/hicolor/128x128/apps'),
  rename: 'dev.faetalize.waytator.png',
)

install_data(
  'ui/icons/icon-256.png',
  install_dir: join_paths(get_option('datadir'), 'icons/hicolor/256x256/apps'),
  rename: 'dev.faetalize.waytator.png',
)

install_data(
  'ui/icons/icon-512.png',
  install_dir: join_paths(get_option('datadir'), 'icons/hicolor/512x512/apps'),
  rename: 'dev.faetalize.waytator.png',
)
