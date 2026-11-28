//the lines a test build tells the end to end tests what happened with, see tests/e2e
#include <glib.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <math.h>

#include "waytator-test-report.h"

static char *
waytator_test_new_report_path(char **directory)
{
  g_autoptr(GError) error = NULL;

  *directory = g_dir_make_tmp("waytator-test-report-XXXXXX", &error);
  g_assert_no_error(error);
  return g_build_filename(*directory, "report.jsonl", NULL);
}

static void
waytator_test_remove_report(const char *path,
                            const char *directory)
{
  g_remove(path);
  g_assert_cmpint(g_rmdir(directory), ==, 0);
}

static char *
waytator_test_read(const char *path)
{
  g_autoptr(GError) error = NULL;
  char *contents = NULL;

  g_file_get_contents(path, &contents, NULL, &error);
  g_assert_no_error(error);
  return contents;
}

static void
waytator_test_write_everything(WaytatorTestReportLine *line)
{
  const double numbers[3] = { 1.0, -2.5, 0.125 };

  waytator_test_report_line_add_string(line, "name", "x");
  waytator_test_report_line_add_int(line, "count", 1);
  waytator_test_report_line_add_double(line, "zoom", 1.0);
  waytator_test_report_line_add_bool(line, "ok", TRUE);
  waytator_test_report_line_add_doubles(line, "rect", numbers, 3);
  waytator_test_report_line_begin_object(line, "first");
  waytator_test_report_line_end_object(line);
  waytator_test_report_line_write(line);
}

static void
test_disabled_without_the_variable(void)
{
  g_unsetenv("WAYTATOR_TEST_REPORT");
  g_assert_null(waytator_test_report_line_new("ready"));

  //every call site hands over what it got, NULL included
  waytator_test_write_everything(NULL);

  g_setenv("WAYTATOR_TEST_REPORT", "", TRUE);
  g_assert_null(waytator_test_report_line_new("ready"));
}

static void
test_values(void)
{
  g_autofree char *directory = NULL;
  g_autofree char *path = waytator_test_new_report_path(&directory);
  g_autofree char *contents = NULL;
  const double rect[4] = { 0.0, -1.5, 1280.0, 800.25 };
  WaytatorTestReportLine *line;

  g_setenv("WAYTATOR_TEST_REPORT", path, TRUE);
  line = waytator_test_report_line_new("stroke");
  waytator_test_report_line_add_string(line, "tool", "brush");
  waytator_test_report_line_add_string(line, "missing", NULL);
  waytator_test_report_line_add_int(line, "points", 12);
  waytator_test_report_line_add_int(line, "big", G_GINT64_CONSTANT(-9000000000));
  waytator_test_report_line_add_double(line, "zoom", 0.8);
  waytator_test_report_line_add_double(line, "not_a_number", NAN);
  waytator_test_report_line_add_double(line, "endless", INFINITY);
  waytator_test_report_line_add_bool(line, "pressure", TRUE);
  waytator_test_report_line_add_bool(line, "rotated", FALSE);
  waytator_test_report_line_add_doubles(line, "rect", rect, 4);
  waytator_test_report_line_add_doubles(line, "empty", rect, 0);
  waytator_test_report_line_begin_object(line, "first");
  waytator_test_report_line_add_int(line, "x", 1);
  waytator_test_report_line_begin_object(line, "inner");
  waytator_test_report_line_end_object(line);
  waytator_test_report_line_end_object(line);
  waytator_test_report_line_add_int(line, "after", 2);
  waytator_test_report_line_write(line);

  contents = waytator_test_read(path);
  g_assert_cmpstr(contents, ==,
                  "{\"event\":\"stroke\",\"tool\":\"brush\",\"missing\":null,\"points\":12,\"big\":-9000000000,"
                  "\"zoom\":0.800000,\"not_a_number\":null,\"endless\":null,\"pressure\":true,\"rotated\":false,"
                  "\"rect\":[0.000000,-1.500000,1280.000000,800.250000],\"empty\":[],"
                  "\"first\":{\"x\":1,\"inner\":{}},\"after\":2}\n");
  waytator_test_remove_report(path, directory);
}

static void
test_escaping(void)
{
  g_autofree char *directory = NULL;
  g_autofree char *path = waytator_test_new_report_path(&directory);
  g_autofree char *contents = NULL;
  WaytatorTestReportLine *line;

  g_setenv("WAYTATOR_TEST_REPORT", path, TRUE);
  line = waytator_test_report_line_new("a \"quoted\" event");
  waytator_test_report_line_add_string(line, "text", "back\\slash\nnew\rline\ttab\x01\x1f é ✓");
  //a lone byte that is not UTF-8 becomes the replacement character
  waytator_test_report_line_add_string(line, "bytes", "a\xff" "b");
  waytator_test_report_line_add_string(line, "key \"too\"", "");
  waytator_test_report_line_write(line);

  contents = waytator_test_read(path);
  g_assert_cmpstr(contents, ==,
                  "{\"event\":\"a \\\"quoted\\\" event\","
                  "\"text\":\"back\\\\slash\\nnew\\rline\\ttab\\u0001\\u001f é ✓\","
                  "\"bytes\":\"a\xef\xbf\xbd" "b\",\"key \\\"too\\\"\":\"\"}\n");
  waytator_test_remove_report(path, directory);
}

static void
test_appends_a_line_at_a_time(void)
{
  g_autofree char *directory = NULL;
  g_autofree char *path = waytator_test_new_report_path(&directory);
  g_autofree char *first = NULL;
  g_autofree char *both = NULL;
  WaytatorTestReportLine *line;

  g_assert_true(g_file_set_contents(path, "already there\n", -1, NULL));
  g_setenv("WAYTATOR_TEST_REPORT", path, TRUE);

  line = waytator_test_report_line_new("undo");
  waytator_test_report_line_write(line);
  //there to read as soon as write returns
  first = waytator_test_read(path);
  g_assert_cmpstr(first, ==, "already there\n{\"event\":\"undo\"}\n");

  line = waytator_test_report_line_new("redo");
  waytator_test_report_line_write(line);
  both = waytator_test_read(path);
  g_assert_cmpstr(both, ==, "already there\n{\"event\":\"undo\"}\n{\"event\":\"redo\"}\n");
  waytator_test_remove_report(path, directory);
}

//the app runs under the user's locale, and some write a comma for the decimal point
static void
test_decimal_point_in_any_locale(void)
{
  g_autofree char *directory = NULL;
  g_autofree char *path = NULL;
  g_autofree char *contents = NULL;
  g_autofree char *previous = g_strdup(setlocale(LC_NUMERIC, NULL));
  WaytatorTestReportLine *line;

  if (setlocale(LC_NUMERIC, "de_DE.UTF-8") == NULL) {
    g_test_skip("no locale with a decimal comma is installed");
    return;
  }

  path = waytator_test_new_report_path(&directory);
  g_setenv("WAYTATOR_TEST_REPORT", path, TRUE);
  line = waytator_test_report_line_new("view");
  waytator_test_report_line_add_double(line, "zoom", 0.5);
  waytator_test_report_line_write(line);
  setlocale(LC_NUMERIC, previous);

  contents = waytator_test_read(path);
  g_assert_cmpstr(contents, ==, "{\"event\":\"view\",\"zoom\":0.500000}\n");
  waytator_test_remove_report(path, directory);
}

int
main(int   argc,
     char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/test-report/disabled-without-the-variable", test_disabled_without_the_variable);
  g_test_add_func("/test-report/values", test_values);
  g_test_add_func("/test-report/escaping", test_escaping);
  g_test_add_func("/test-report/appends-a-line-at-a-time", test_appends_a_line_at_a_time);
  g_test_add_func("/test-report/decimal-point-in-any-locale", test_decimal_point_in_any_locale);

  return g_test_run();
}
