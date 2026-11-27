#include "waytator-file-name.h"

#include <string.h>

//leaves room under the usual 255 byte limit for a counter, " (unfinished)" and the extension
#define WAYTATOR_FILE_NAME_MAX_STEM 200

//the name a template gives at a time, without its extension, or NULL when the template has a code strftime doesn't
//know or comes out empty. a slash would make folders and a colon is refused by FAT and exFAT drives and Windows
//shares, and both turn up in locale codes like %x and %X, so they become dashes like control characters do
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
    if (*c == '/' || *c == ':' || (guchar) *c < 0x20 || *c == 0x7f)
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
