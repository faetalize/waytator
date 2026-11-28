//which saves put the edits into the file being edited, and where a first save goes
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

//a piped image given --save-path saves there, without asking
static void
test_first_save_target(void)
{
  g_assert_cmpint(waytator_save_first_destination(TRUE, FALSE, TRUE), ==, WAYTATOR_FIRST_SAVE_TARGET);
  g_assert_cmpint(waytator_save_first_destination(TRUE, FALSE, FALSE), ==, WAYTATOR_FIRST_SAVE_TARGET);
}

//a piped image without one is asked about, the screenshot folder is only for captures
static void
test_first_save_piped_asks(void)
{
  g_assert_cmpint(waytator_save_first_destination(FALSE, FALSE, TRUE), ==, WAYTATOR_FIRST_SAVE_DIALOG);
}

//a capture goes to the screenshot folder when one is set, and is asked about when not
static void
test_first_save_capture(void)
{
  g_assert_cmpint(waytator_save_first_destination(FALSE, TRUE, TRUE), ==, WAYTATOR_FIRST_SAVE_FOLDER);
  g_assert_cmpint(waytator_save_first_destination(FALSE, TRUE, FALSE), ==, WAYTATOR_FIRST_SAVE_DIALOG);
}

//a menu takes _ for the key that picks an entry, so a path's own underscores are doubled to show as they are
static void
test_menu_label_keeps_underscores(void)
{
  g_autofree char *label = waytator_save_menu_label("~/bug_report_1.png");

  g_assert_cmpstr(label, ==, "Save to ~/bug__report__1.png");
}

int
main(int   argc,
     char *argv[])
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/save/copy-is-not-document", test_copy_is_not_document);
  g_test_add_func("/save/open-file-is-document", test_open_file_is_document);
  g_test_add_func("/save/first-save-is-document", test_first_save_is_document);
  g_test_add_func("/save/first-save-target", test_first_save_target);
  g_test_add_func("/save/first-save-piped-asks", test_first_save_piped_asks);
  g_test_add_func("/save/first-save-capture", test_first_save_capture);
  g_test_add_func("/save/menu-label-keeps-underscores", test_menu_label_keeps_underscores);
  return g_test_run();
}
