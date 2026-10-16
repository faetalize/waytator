test_video_edit = executable(
  'test-video-edit',
  'test-video-edit.c',
  waytator_video_edit_sources,
  include_directories: waytator_include,
  dependencies: [gobject_dep, m_dep],
)
test('video-edit', test_video_edit)
