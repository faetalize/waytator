#include "waytator-tablet-button.h"

#include <string.h>

char *
waytator_tablet_button_shortcut(guint button)
{
  return g_strdup_printf(WAYTATOR_TABLET_BUTTON_PREFIX "%u", button + 1);
}

//takes any case, since it may come from a command line
gboolean
waytator_tablet_button_parse(const char *shortcut,
                             guint      *button)
{
  const gsize prefix_length = strlen(WAYTATOR_TABLET_BUTTON_PREFIX);
  guint64 number;

  if (shortcut == NULL || g_ascii_strncasecmp(shortcut, WAYTATOR_TABLET_BUTTON_PREFIX, prefix_length) != 0)
    return FALSE;

  //g_ascii_string_to_unsigned lets a sign through, a button number is only digits
  for (const char *c = shortcut + prefix_length; *c != '\0'; c++) {
    if (!g_ascii_isdigit(*c))
      return FALSE;
  }

  if (!g_ascii_string_to_unsigned(shortcut + prefix_length, 10, 1, G_MAXUINT, &number, NULL))
    return FALSE;

  if (button != NULL)
    *button = (guint) number - 1;
  return TRUE;
}
