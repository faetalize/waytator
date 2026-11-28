//a tablet pad button bound to a shortcut is saved and typed as "tablet-button-N", GDK counts the buttons from 0
#include <glib.h>

#include "waytator-tablet-button.h"

static void
test_round_trip(void)
{
  const guint buttons[] = { 0, 1, 7, 17 };

  for (guint i = 0; i < G_N_ELEMENTS(buttons); i++) {
    g_autofree char *shortcut = waytator_tablet_button_shortcut(buttons[i]);
    guint parsed = G_MAXUINT;

    g_assert_true(waytator_tablet_button_parse(shortcut, &parsed));
    g_assert_cmpuint(parsed, ==, buttons[i]);
  }
}

static void
test_counted_from_one(void)
{
  g_autofree char *first = waytator_tablet_button_shortcut(0);
  guint button = G_MAXUINT;

  g_assert_cmpstr(first, ==, "tablet-button-1");
  g_assert_true(waytator_tablet_button_parse("tablet-button-3", &button));
  g_assert_cmpuint(button, ==, 2);
}

static void
test_any_case(void)
{
  guint button = G_MAXUINT;

  g_assert_true(waytator_tablet_button_parse("Tablet-Button-12", &button));
  g_assert_cmpuint(button, ==, 11);
}

static void
test_nonsense_is_refused(void)
{
  const char *const shortcuts[] = {
    NULL,
    "",
    "tablet-button-",
    "tablet-button-0",
    "tablet-button--1",
    "tablet-button-+1",
    "tablet-button- 1",
    "tablet-button-1x",
    "tablet-button-99999999999",
    "tablet-button1",
    "right-click",
    "ctrl+t",
  };

  for (guint i = 0; i < G_N_ELEMENTS(shortcuts); i++) {
    guint button = 42;

    g_assert_false(waytator_tablet_button_parse(shortcuts[i], &button));
    g_assert_cmpuint(button, ==, 42);
  }
}

int
main(int    argc,
     char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/tablet-button/round-trip", test_round_trip);
  g_test_add_func("/tablet-button/counted-from-one", test_counted_from_one);
  g_test_add_func("/tablet-button/any-case", test_any_case);
  g_test_add_func("/tablet-button/nonsense-is-refused", test_nonsense_is_refused);
  return g_test_run();
}
