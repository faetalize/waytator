#include "waytator-test-report.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

struct _WaytatorTestReportLine {
  GString *json;
  char *path;
};

static void
waytator_test_report_append_string(GString    *json,
                                   const char *text)
{
  //a file name can hold any bytes, and a line that isn't UTF-8 couldn't be read back
  g_autofree char *valid = g_utf8_make_valid(text, -1);

  g_string_append_c(json, '"');
  for (const char *c = valid; *c != '\0'; c++) {
    switch (*c) {
    case '"':
      g_string_append(json, "\\\"");
      break;
    case '\\':
      g_string_append(json, "\\\\");
      break;
    case '\n':
      g_string_append(json, "\\n");
      break;
    case '\r':
      g_string_append(json, "\\r");
      break;
    case '\t':
      g_string_append(json, "\\t");
      break;
    default:
      if ((guchar) *c < 0x20)
        g_string_append_printf(json, "\\u%04x", (guchar) *c);
      else
        g_string_append_c(json, *c);
      break;
    }
  }
  g_string_append_c(json, '"');
}

static void
waytator_test_report_append_double(GString *json,
                                   double   value)
{
  char text[G_ASCII_DTOSTR_BUF_SIZE];

  if (!isfinite(value)) {
    g_string_append(json, "null");
    return;
  }

  //printf would write a comma for the decimal point under some locales
  g_string_append(json, g_ascii_formatd(text, sizeof(text), "%.6f", value));
}

//a comma unless this is the first thing in its object or array, then the key when there is one
static void
waytator_test_report_append_key(WaytatorTestReportLine *line,
                                const char             *key)
{
  const char last = line->json->str[line->json->len - 1];

  if (last != '{' && last != '[')
    g_string_append_c(line->json, ',');

  waytator_test_report_append_string(line->json, key);
  g_string_append_c(line->json, ':');
}

WaytatorTestReportLine *
waytator_test_report_line_new(const char *event)
{
  const char *path = g_getenv("WAYTATOR_TEST_REPORT");
  WaytatorTestReportLine *line;

  if (path == NULL || *path == '\0')
    return NULL;

  line = g_new0(WaytatorTestReportLine, 1);
  line->path = g_strdup(path);
  line->json = g_string_new("{");
  waytator_test_report_line_add_string(line, "event", event);
  return line;
}

void
waytator_test_report_line_add_string(WaytatorTestReportLine *line,
                                     const char             *key,
                                     const char             *value)
{
  if (line == NULL)
    return;

  waytator_test_report_append_key(line, key);
  if (value == NULL)
    g_string_append(line->json, "null");
  else
    waytator_test_report_append_string(line->json, value);
}

void
waytator_test_report_line_add_int(WaytatorTestReportLine *line,
                                  const char             *key,
                                  gint64                  value)
{
  if (line == NULL)
    return;

  waytator_test_report_append_key(line, key);
  g_string_append_printf(line->json, "%" G_GINT64_FORMAT, value);
}

void
waytator_test_report_line_add_double(WaytatorTestReportLine *line,
                                     const char             *key,
                                     double                  value)
{
  if (line == NULL)
    return;

  waytator_test_report_append_key(line, key);
  waytator_test_report_append_double(line->json, value);
}

void
waytator_test_report_line_add_bool(WaytatorTestReportLine *line,
                                   const char             *key,
                                   gboolean                value)
{
  if (line == NULL)
    return;

  waytator_test_report_append_key(line, key);
  g_string_append(line->json, value ? "true" : "false");
}

void
waytator_test_report_line_add_doubles(WaytatorTestReportLine *line,
                                      const char             *key,
                                      const double           *values,
                                      guint                   n_values)
{
  if (line == NULL)
    return;

  waytator_test_report_append_key(line, key);
  g_string_append_c(line->json, '[');
  for (guint i = 0; i < n_values; i++) {
    if (i > 0)
      g_string_append_c(line->json, ',');
    waytator_test_report_append_double(line->json, values[i]);
  }
  g_string_append_c(line->json, ']');
}

void
waytator_test_report_line_begin_object(WaytatorTestReportLine *line,
                                       const char             *key)
{
  if (line == NULL)
    return;

  waytator_test_report_append_key(line, key);
  g_string_append_c(line->json, '{');
}

void
waytator_test_report_line_end_object(WaytatorTestReportLine *line)
{
  if (line == NULL)
    return;

  g_string_append_c(line->json, '}');
}

void
waytator_test_report_line_write(WaytatorTestReportLine *line)
{
  FILE *file;

  if (line == NULL)
    return;

  g_string_append(line->json, "}\n");

  //opened for every line, so each one is on its way to whoever reads along as soon as it is written
  file = fopen(line->path, "a");
  if (file == NULL) {
    g_warning("Failed to open the test report %s: %s", line->path, g_strerror(errno));
  } else {
    if (fwrite(line->json->str, 1, line->json->len, file) != line->json->len || fflush(file) != 0)
      g_warning("Failed to write to the test report %s", line->path);
    fclose(file);
  }

  g_string_free(line->json, TRUE);
  g_free(line->path);
  g_free(line);
}
