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

gobject_dep = dependency('gobject-2.0')
gtk_dep = dependency('gtk4', version: '>=4.14')
adw_dep = dependency('libadwaita-1', version: '>=1.6')
layer_shell_dep = dependency('gtk4-layer-shell-0')
wayland_client_dep = dependency('wayland-client', version: '>=1.20')
wayland_protocols_dep = dependency('wayland-protocols', version: '>=1.14')
wayland_scanner_dep = dependency('wayland-scanner', version: '>=1.15', native: true)
#the video editor: ffmpeg decodes, filters and encodes, rubber band changes speed and pitch and pipewire
#plays the sound
avformat_dep = dependency('libavformat', version: '>=60')
avcodec_dep = dependency('libavcodec', version: '>=60')
avfilter_dep = dependency('libavfilter', version: '>=9')
avutil_dep = dependency('libavutil', version: '>=58')
swscale_dep = dependency('libswscale', version: '>=7')
swresample_dep = dependency('libswresample', version: '>=4.10')
libdrm_dep = dependency('libdrm')
pipewire_dep = dependency('libpipewire-0.3', version: '>=0.3.50')
rubberband_dep = dependency('rubberband', version: '>=3.0')
gio_unix_dep = dependency('gio-unix-2.0')
#the recorder asks the gpu which of its buffers it can take, to tell the compositor which to send
gbm_dep = dependency('gbm')
egl_dep = dependency('egl')
video_deps = [avformat_dep, avcodec_dep, avfilter_dep, avutil_dep, swscale_dep, swresample_dep, libdrm_dep, pipewire_dep, rubberband_dep, gio_unix_dep, gbm_dep, egl_dep]
cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: false)

subdir('protocols')
subdir('src')
subdir('tests')
