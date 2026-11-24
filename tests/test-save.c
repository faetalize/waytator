//which saves put the edits into the file being edited
#include <gio/gio.h>

#include "waytator-save.h"

//a copy leaves the open file without the edits, so the image is still unsaved
static void
test_copy_is_not_document(void)
{
  g_autoptr(GFile) original = g_file_new_for_path("/tmp/waytator-test/shot.png");
  g_autoptr(GFile) copy = g_file_new_for_path("/tmp/waytator-test/shot (copy).png");

  g_assert_false(waytator_save_target_is_document(original, copy));
}

//picking the open file itself in the dialog overwrites it
static void
test_open_file_is_document(void)
{
  g_autoptr(GFile) original = g_file_new_for_path("/tmp/waytator-test/shot.png");
  g_autoptr(GFile) same = g_file_new_for_path("/tmp/waytator-test/shot.png");

  g_assert_true(waytator_save_target_is_document(original, same));
}

//an image with no file yet takes on the one it is saved to
static void
test_first_save_is_document(void)
{
  g_autoptr(GFile) file = g_file_new_for_path("/tmp/waytator-test/shot.png");

  g_assert_true(waytator_save_target_is_document(NULL, file));
}

int
main(int   argc,
     char *argv[])
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/save/copy-is-not-document", test_copy_is_not_document);
  g_test_add_func("/save/open-file-is-document", test_open_file_is_document);
  g_test_add_func("/save/first-save-is-document", test_first_save_is_document);
  return g_test_run();
}
