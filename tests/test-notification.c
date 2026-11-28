//what a notification's body looks like on the bus
#include <gio/gio.h>

#include "waytator-notification.h"

//a server that reads markup would take a file name's & or < for markup and garble or drop the text
static void
test_body_escaped_for_markup(void)
{
  g_autofree char *body = waytator_notification_body_text("Error loading image 'R&D <draft>.png'", TRUE);

  g_assert_cmpstr(body, ==, "Error loading image 'R&amp;D &lt;draft&gt;.png'");
}

//a server that shows the text as it is gets it as it is
static void
test_body_plain_without_markup(void)
{
  g_autofree char *body = waytator_notification_body_text("R&D <draft>.png", FALSE);

  g_assert_cmpstr(body, ==, "R&D <draft>.png");
}

int
main(int   argc,
     char *argv[])
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/notification/body-escaped-for-markup", test_body_escaped_for_markup);
  g_test_add_func("/notification/body-plain-without-markup", test_body_plain_without_markup);
  return g_test_run();
}
