#include "waytator-file-name.h"

#include <string.h>

//leaves room under the usual 255 byte limit for a counter, " (unfinished)" and the extension
#define WAYTATOR_FILE_NAME_MAX_STEM 200

//the name a template gives at a time, without its extension, or NULL when the template has a code strftime doesn't
//know or comes out empty. a slash would make folders, and FAT and exFAT drives and Windows shares refuse \ : * ? " < >
//and |, with the slash and colon turning up in locale codes like %x and %X. they all become dashes, like control
//characters do
char *
waytator_file_name_format(const char *template,
                          GDateTime  *time)
{
  g_autofree char *formatted = NULL;
  char *stem;
  gsize length;

  if (template == NULL)
    return NULL;

  formatted = g_date_time_format(time, template);
  if (formatted == NULL)
    return NULL;

  for (char *c = formatted; *c != '\0'; c++) {
    if (strchr("/\\:*?\"<>|", *c) != NULL || (guchar) *c < 0x20 || *c == 0x7f)
      *c = '-';
  }

  stem = g_strstrip(formatted);
  length = strlen(stem);
  if (length > WAYTATOR_FILE_NAME_MAX_STEM) {
    const char *end = NULL;

    //g_date_time_format gives UTF-8, so the only thing that fails here is the character the limit falls in, and
    //the cut goes before it rather than through it
    g_utf8_validate(stem, WAYTATOR_FILE_NAME_MAX_STEM, &end);
    stem[end - stem] = '\0';
    g_strchomp(stem);
    length = strlen(stem);
  }

  if (length == 0)
    return NULL;

  return g_strdup(stem);
}

//the template's name, or the fallback's when the template gives none
char *
waytator_file_name_make(const char *template,
                        const char *fallback,
                        GDateTime  *time)
{
  char *name = waytator_file_name_format(template, time);

  return name != NULL ? name : waytator_file_name_format(fallback, time);
}

//what the preferences show under the name being typed: the file it gives, the default an emptied field goes back to,
//or when it gives none, the name files keep, since a name that gives none is never saved. usable is whether the
//field can be applied
char *
waytator_file_name_preview(const char *text,
                           const char *saved,
                           const char *fallback,
                           const char *extension,
                           GDateTime  *time,
                           gboolean   *usable)
{
  g_autofree char *stem = waytator_file_name_format(text, time);
  g_autofree char *stripped = g_strstrip(g_strdup(text != NULL ? text : ""));
  g_autofree char *instead = NULL;

  *usable = stem != NULL || *stripped == '\0';
  if (stem != NULL)
    return g_strdup_printf("%s.%s", stem, extension);

  if (*stripped == '\0') {
    instead = waytator_file_name_format(fallback, time);
    return g_strdup_printf("Left empty, files get the default name %s.%s", instead, extension);
  }

  instead = waytator_file_name_make(saved, fallback, time);
  return g_strdup_printf("No name comes out of this, files keep the name %s.%s", instead, extension);
}
