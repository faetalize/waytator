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
