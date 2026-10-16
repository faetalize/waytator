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
    '../src/waytator-video-frames.c',
    '../src/waytator-video-source.c',
    '../src/waytator-video-thumbnails.c',
  ),
  include_directories: waytator_include,
  dependencies: [gtk_dep, m_dep, video_deps],
)
test('video-source', test_video_source, timeout: 120)
