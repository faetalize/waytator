test_video_edit = executable(
  'test-video-edit',
  'test-video-edit.c',
  waytator_video_edit_sources,
  include_directories: waytator_include,
  dependencies: [gobject_dep, m_dep],
)
test('video-edit', test_video_edit)

#makes its own videos to read, and draws frames offscreen when there is a display to draw on
test_video_source = executable(
  'test-video-source',
  'test-video-source.c',
  files(
    '../src/waytator-video-encoder.c',
    '../src/waytator-video-frames.c',
    '../src/waytator-video-source.c',
    '../src/waytator-video-thumbnails.c',
  ),
  include_directories: waytator_include,
  dependencies: [gtk_dep, m_dep, video_deps],
)
test('video-source', test_video_source, timeout: 120)

#the timeline on its own, for looking at it by hand. built with the rest but not a test, it needs a display
executable(
  'demo-video-timeline',
  'demo-video-timeline.c',
  waytator_resources,
  waytator_video_edit_sources,
  include_directories: waytator_include,
  dependencies: [gtk_dep, adw_dep, m_dep],
)

#the sound of the editor against a stand-in for the source, so it builds without the decoder
test_video_audio = executable(
  'test-video-audio',
  'test-video-audio.c',
  waytator_video_edit_sources,
  files('../src/waytator-video-audio.c', '../src/waytator-video-audio-output.c', '../src/waytator-video-waveform.c'),
  include_directories: waytator_include,
  dependencies: [gtk_dep, m_dep, video_deps],
)
test('video-audio', test_video_audio, timeout: 120)

#makes its own videos and saves them through every turn and flip, checking the corners land where the editor shows them
test_video_export = executable(
  'test-video-export',
  'test-video-export.c',
  files(
    '../src/waytator-video-audio.c',
    '../src/waytator-video-encoder.c',
    '../src/waytator-video-export.c',
    '../src/waytator-video-cache.c',
    '../src/waytator-video-profile.c',
    '../src/waytator-video-source.c',
  ),
  waytator_video_edit_sources,
  include_directories: waytator_include,
  dependencies: [gtk_dep, adw_dep, m_dep, video_deps],
)
test('video-export', test_video_export, timeout: 300)

test_video_cache = executable(
  'test-video-cache',
  'test-video-cache.c',
  files('../src/waytator-video-cache.c'),
  include_directories: waytator_include,
  dependencies: [gobject_dep],
)
test('video-cache', test_video_cache)

#talks to a stand-in for the screen cast portal on a bus of its own, so it needs neither a display nor the real portal
test_record_portal = executable(
  'test-record-portal',
  'test-record-portal.c',
  files('../src/waytator-record-portal.c'),
  include_directories: waytator_include,
  dependencies: [gio_unix_dep],
)
test('record-portal', test_record_portal)

#the mix of a recording's sound with made up sources and clocks, so it needs no pipewire
test_record_mixer = executable(
  'test-record-mixer',
  'test-record-mixer.c',
  files('../src/waytator-record-mixer.c'),
  include_directories: waytator_include,
  dependencies: [gobject_dep, m_dep],
)
test('record-mixer', test_record_mixer, timeout: 120)

#the capture of a recording's sound against a pipewire and wireplumber of the test's own, skipping when they
#aren't installed
test_record_audio = executable(
  'test-record-audio',
  'test-record-audio.c',
  files(
    '../src/waytator-record-audio.c',
    '../src/waytator-record-mixer.c',
  ),
  include_directories: waytator_include,
  dependencies: [gio_unix_dep, pipewire_dep, m_dep],
)
test('record-audio', test_record_audio, timeout: 180)

#only the placing of the bar is run, which is plain arithmetic, so it needs no display. the rest of the bar is
#stood in for where it would reach into the windows
test_recording_bar_place = executable(
  'test-recording-bar-place',
  'test-recording-bar-place.c',
  files('../src/waytator-recording-bar.c', '../src/waytator-settings.c'),
  include_directories: waytator_include,
  dependencies: [layer_shell_dep, gtk_dep, adw_dep, m_dep],
)
test('recording-bar-place', test_recording_bar_place)

#makes pictures and sound of its own, records them through the writer and decodes the files, with a gpu off screen when there is one
test_record_writer = executable(
  'test-record-writer',
  'test-record-writer.c',
  files(
    '../src/waytator-record-writer.c',
    '../src/waytator-video-cache.c',
    '../src/waytator-video-encoder.c',
  ),
  include_directories: waytator_include,
  dependencies: [gio_unix_dep, m_dep, video_deps],
)
test('record-writer', test_record_writer, timeout: 300)

#a PipeWire daemon of its own stands in for the compositor, sending pictures in memory and on the gpu
test_record_stream = executable(
  'test-record-stream',
  'test-record-stream.c',
  files('../src/waytator-record-stream.c'),
  include_directories: waytator_include,
  dependencies: [gobject_dep, m_dep, video_deps],
)
test('record-stream', test_record_stream, timeout: 120)
