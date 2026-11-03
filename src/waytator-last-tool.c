#include "waytator-last-tool.h"

#include <math.h>

#include "waytator-settings.h"

//names rather than numbers, so a tool added in the middle doesn't hand its neighbour's size to the wrong one
static const char *const waytator_last_tool_names[WAYTATOR_N_TOOLS] = {
  [WAYTATOR_TOOL_PAN] = "pan",
  [WAYTATOR_TOOL_CROP] = "crop",
  [WAYTATOR_TOOL_BRUSH] = "brush",
  [WAYTATOR_TOOL_MARKER] = "highlighter",
  [WAYTATOR_TOOL_ERASER] = "eraser",
  [WAYTATOR_TOOL_RECTANGLE] = "rectangle",
  [WAYTATOR_TOOL_CIRCLE] = "circle",
  [WAYTATOR_TOOL_LINE] = "line",
  [WAYTATOR_TOOL_ARROW] = "arrow",
  [WAYTATOR_TOOL_OCR] = "ocr",
  [WAYTATOR_TOOL_TEXT] = "text",
  [WAYTATOR_TOOL_BLUR] = "blur",
};

static void
waytator_last_tool_read_rgba(GKeyFile   *key_file,
                             const char *key,
                             GdkRGBA    *rgba)
{
  g_autofree char *value = g_key_file_get_string(key_file, WAYTATOR_LAST_TOOL_GROUP, key, NULL);
  GdkRGBA parsed;

  if (value != NULL && gdk_rgba_parse(&parsed, value))
    *rgba = parsed;
}

static void
waytator_last_tool_write_rgba(GKeyFile      *key_file,
                              const char    *key,
                              const GdkRGBA *rgba)
{
  g_autofree char *value = gdk_rgba_to_string(rgba);

  g_key_file_set_string(key_file, WAYTATOR_LAST_TOOL_GROUP, key, value);
}

//a key that is missing or out of range leaves the value it replaces alone
void
waytator_last_tool_read(WaytatorLastTool *last_tool,
                        GKeyFile         *key_file)
{
  g_autofree char *tool = g_key_file_get_string(key_file, WAYTATOR_LAST_TOOL_GROUP, "tool", NULL);

  for (int i = 0; i < WAYTATOR_N_TOOLS; i++) {
    g_autofree char *width_key = g_strdup_printf("%s_width", waytator_last_tool_names[i]);
    g_autofree char *color_key = g_strdup_printf("%s_color", waytator_last_tool_names[i]);
    g_autofree char *fill_color_key = g_strdup_printf("%s_fill_color", waytator_last_tool_names[i]);

    if (g_strcmp0(tool, waytator_last_tool_names[i]) == 0)
      last_tool->tool = i;

    if (g_key_file_has_key(key_file, WAYTATOR_LAST_TOOL_GROUP, width_key, NULL)) {
      const double width = g_key_file_get_double(key_file, WAYTATOR_LAST_TOOL_GROUP, width_key, NULL);

      //the text tool goes up to 200, the rest stop at 100
      if (isfinite(width) && width >= 1.0 && width <= 200.0)
        last_tool->widths[i] = width;
    }

    waytator_last_tool_read_rgba(key_file, color_key, &last_tool->colors[i]);
    waytator_last_tool_read_rgba(key_file, fill_color_key, &last_tool->fill_colors[i]);
  }

  if (g_key_file_has_key(key_file, WAYTATOR_LAST_TOOL_GROUP, "blur_type", NULL)) {
    const int blur_type = g_key_file_get_integer(key_file, WAYTATOR_LAST_TOOL_GROUP, "blur_type", NULL);

    //blur or pixelate
    if (blur_type == 0 || blur_type == 1)
      last_tool->blur_type = blur_type;
  }
}

void
waytator_last_tool_write(const WaytatorLastTool *last_tool,
                         GKeyFile               *key_file)
{
  g_key_file_set_string(key_file, WAYTATOR_LAST_TOOL_GROUP, "tool", waytator_last_tool_names[last_tool->tool]);

  for (int i = 0; i < WAYTATOR_N_TOOLS; i++) {
    g_autofree char *width_key = g_strdup_printf("%s_width", waytator_last_tool_names[i]);
    g_autofree char *color_key = g_strdup_printf("%s_color", waytator_last_tool_names[i]);
    g_autofree char *fill_color_key = g_strdup_printf("%s_fill_color", waytator_last_tool_names[i]);

    g_key_file_set_double(key_file, WAYTATOR_LAST_TOOL_GROUP, width_key, last_tool->widths[i]);
    waytator_last_tool_write_rgba(key_file, color_key, &last_tool->colors[i]);
    waytator_last_tool_write_rgba(key_file, fill_color_key, &last_tool->fill_colors[i]);
  }

  g_key_file_set_integer(key_file, WAYTATOR_LAST_TOOL_GROUP, "blur_type", last_tool->blur_type);
}

//saving the preferences writes the file afresh, this keeps the remembered tool in it
void
waytator_last_tool_carry_over(GKeyFile *from,
                              GKeyFile *to)
{
  g_auto(GStrv) keys = g_key_file_get_keys(from, WAYTATOR_LAST_TOOL_GROUP, NULL, NULL);

  for (guint i = 0; keys != NULL && keys[i] != NULL; i++) {
    g_autofree char *value = g_key_file_get_value(from, WAYTATOR_LAST_TOOL_GROUP, keys[i], NULL);

    g_key_file_set_value(to, WAYTATOR_LAST_TOOL_GROUP, keys[i], value);
  }
}

//fills in what the file remembers over what last_tool already holds, FALSE when there is no file to read
gboolean
waytator_last_tool_load(WaytatorLastTool *last_tool)
{
  g_autofree char *path = waytator_settings_path();
  g_autoptr(GKeyFile) key_file = g_key_file_new();

  if (!g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, NULL))
    return FALSE;

  waytator_last_tool_read(last_tool, key_file);
  return TRUE;
}

//changes only the remembered tool in the file and leaves the preferences as they are
gboolean
waytator_last_tool_save(const WaytatorLastTool *last_tool)
{
  g_autofree char *path = waytator_settings_path();
  g_autofree char *directory = g_path_get_dirname(path);
  g_autoptr(GKeyFile) key_file = g_key_file_new();
  g_autoptr(GError) error = NULL;

  g_key_file_load_from_file(key_file, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
  waytator_last_tool_write(last_tool, key_file);

  if (g_mkdir_with_parents(directory, 0700) != 0) {
    g_warning("Failed to create preferences directory %s", directory);
    return FALSE;
  }

  if (!g_key_file_save_to_file(key_file, path, &error)) {
    g_warning("Failed to save preferences to %s: %s", path, error->message);
    return FALSE;
  }

  return TRUE;
}
