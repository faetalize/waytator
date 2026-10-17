waytator_conf = configuration_data()
waytator_conf.set_quoted('WAYTATOR_VERSION', meson.project_version())

#the edit model has no gtk in it, so the tests build it on its own
waytator_video_edit_sources = files(
  'waytator-video-edit.c',
  'waytator-video-frames.c',
)
waytator_include = include_directories('.')

waytator_sources = files(
  'main.c',
  'waytator-canvas-input.c',
  'waytator-canvas-layout.c',
  'waytator-canvas-render.c',
  'waytator-canvas-state.c',
  'waytator-canvas-view.c',
  'waytator-canvas-view.h',
  'waytator-chrome-layout.c',
  'waytator-capture.c',
  'waytator-capture.h',
  'waytator-capture-overlay.c',
  'waytator-capture-overlay.h',
  'waytator-document.c',
  'waytator-document.h',
  'waytator-export.c',
  'waytator-export.h',
  'waytator-ocr.c',
  'waytator-ocr.h',
  'waytator-passthrough.c',
  'waytator-passthrough.h',
  'waytator-pointer-brake.c',
  'waytator-pointer-brake.h',
  'waytator-render.c',
  'waytator-render.h',
  'waytator-screenshot.c',
  'waytator-screenshot.h',
  'waytator-settings.c',
  'waytator-settings.h',
  'waytator-stroke.c',
  'waytator-stroke.h',
  'waytator-types.h',
  'waytator-video-audio.c',
  'waytator-video-audio-output.c',
  'waytator-video-audio.h',
  'waytator-video-cache.c',
  'waytator-video-cache.h',
  'waytator-video-edit.h',
  'waytator-video-editor.c',
  'waytator-video-editor.h',
  'waytator-video-export.c',
  'waytator-video-export.h',
  'waytator-video-frames.h',
  'waytator-video-player.c',
  'waytator-video-player.h',
  'waytator-video-profile.c',
  'waytator-video-profile.h',
  'waytator-video-proxy.c',
  'waytator-video-proxy.h',
  'waytator-video-source.c',
  'waytator-video-source.h',
  'waytator-video-thumbnails.c',
  'waytator-video-thumbnails.h',
  'waytator-video-timeline.c',
  'waytator-video-timeline.h',
  'waytator-video-view.c',
  'waytator-video-view.h',
  'waytator-video-waveform.c',
  'waytator-window.c',
  'waytator-window.h',
  'waytator-window-private.h',
  'waytator-window-video.c',
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
  waytator_video_edit_sources,
  wayland_protocol_sources,
  #the layer shell has to be linked ahead of libwayland-client to hook into it
  dependencies: [layer_shell_dep, gtk_dep, adw_dep, wayland_client_dep, m_dep, video_deps],
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
