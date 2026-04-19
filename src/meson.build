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
