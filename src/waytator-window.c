#include "waytator-window.h"
#include "waytator-window-private.h"
#include "waytator-config.h"

#include "waytator-capture-overlay.h"
#include "waytator-export.h"
#include "waytator-instance.h"
#include "waytator-ocr.h"
#include "waytator-passthrough.h"
#include "waytator-record-audio.h"
#include "waytator-recording.h"
#include "waytator-recording-bar.h"
#include "waytator-render.h"
#include "waytator-screenshot.h"
#include "waytator-stroke.h"
#include "waytator-video-editor.h"
#include "waytator-video-profile.h"

#include <cairo.h>
#include <string.h>

G_DEFINE_FINAL_TYPE(WaytatorWindow, waytator_window, ADW_TYPE_APPLICATION_WINDOW)

#define WAYTATOR_WINDOW_STYLE_PROVIDER_PRIORITY (GTK_STYLE_PROVIDER_PRIORITY_USER + 1)
#define WAYTATOR_RESOURCE_PREFIX "/dev/faetalize/waytator"

static void waytator_window_clear_ocr_results(WaytatorWindow *self);
static void waytator_window_set_ocr_panel_visible(WaytatorWindow *self,
                                                  gboolean        visible);
static void waytator_window_ocr_panel_open_changed(GObject    *object,
                                                   GParamSpec *pspec,
                                                   gpointer    user_data);
static gboolean waytator_window_has_unsaved_changes(WaytatorWindow *self);
static void waytator_window_copy_export_ready(GObject      *source_object,
                                              GAsyncResult *result,
                                              gpointer      user_data);
static gboolean waytator_window_parse_accelerator(const char       *accelerator,
                                                  guint            *keyval,
                                                  GdkModifierType  *modifiers);
static void waytator_window_apply_shortcut(WaytatorWindow  *self,
                                           WaytatorShortcut shortcut,
                                           const char      *accelerator);
static void waytator_window_update_capture_controls(WaytatorWindow *self);
static void waytator_window_update_shortcut_label(GtkShortcutLabel *label,
                                                  const char       *accelerator);
static const char *waytator_window_angle_snap_modifier_label(GdkModifierType modifiers);
static void waytator_window_highlighter_overlap_changed(AdwSwitchRow   *row,
                                                        GParamSpec     *pspec,
                                                        WaytatorWindow *self);

static void waytator_window_update_window_controls(WaytatorWindow *self);
static void waytator_window_update_window_background(WaytatorWindow *self);
static void waytator_window_update_widget_appearance(WaytatorWindow *self);

static void
waytator_window_apply_default_tool_colors(WaytatorWindow *self)
{
  int i;

  for (i = 0; i <= WAYTATOR_TOOL_BLUR; i++) {
    self->tool_colors[i] = self->default_primary_color;
    self->tool_fill_colors[i] = self->default_fill_color;
  }

  self->tool_colors[WAYTATOR_TOOL_MARKER] = self->default_highlighter_color;
  self->tool_colors[WAYTATOR_TOOL_BLUR] = (GdkRGBA){0.0, 0.0, 0.0, 1.0};
  self->tool_colors[WAYTATOR_TOOL_ERASER] = (GdkRGBA){1.0, 1.0, 1.0, 1.0};
}

static void
waytator_window_sync_active_tool_colors(WaytatorWindow *self)
{
  if (self->color_button == NULL || self->fill_color_button == NULL)
    return;

  self->updating_ui = TRUE;
  gtk_color_dialog_button_set_rgba(self->color_button, &self->tool_colors[self->active_tool]);
  gtk_color_dialog_button_set_rgba(self->fill_color_button, &self->tool_fill_colors[self->active_tool]);
  self->updating_ui = FALSE;
}

static gboolean
waytator_window_load_rgba_preference(GKeyFile     *key_file,
                                     const char   *key,
                                     GdkRGBA      *rgba)
{
  g_autofree char *value = NULL;

  if (!g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, key, NULL))
    return FALSE;

  value = g_key_file_get_string(key_file, WAYTATOR_SETTINGS_GROUP, key, NULL);
  if (value == NULL)
    return FALSE;

  return gdk_rgba_parse(rgba, value);
}

static const char *
waytator_window_background_mode_label(WaytatorWindowBackgroundMode mode)
{
  switch (mode) {
  case WAYTATOR_WINDOW_BACKGROUND_FOLLOW_SYSTEM:
    return "Follow system theme";
  case WAYTATOR_WINDOW_BACKGROUND_OPAQUE:
    return "Opaque";
  case WAYTATOR_WINDOW_BACKGROUND_TRANSPARENT:
    return "Transparent";
  default:
    return "Follow system theme";
  }
}

#define WAYTATOR_SHORTCUT_NONE "none"
#define WAYTATOR_SHORTCUT_RIGHT_CLICK "right-click"

#define WAYTATOR_TOOL_SHORTCUT(tool) (WAYTATOR_SHORTCUT_TOOL_FIRST + (tool))
#define WAYTATOR_COLOR_SHORTCUT(index) (WAYTATOR_SHORTCUT_COLOR_FIRST + (index))

//name is what the command line calls it, action is NULL for the ones handled in waytator_window_activate_shortcut
static const struct {
  const char *settings_key;
  const char *name;
  const char *title;
  const char *action;
  const char *default_accel;
} waytator_shortcuts[WAYTATOR_N_SHORTCUTS] = {
  [WAYTATOR_SHORTCUT_CLOSE_WINDOW] = { "close_window_shortcut", "close-window", "Close window", "win.close-window", "Escape" },
  [WAYTATOR_SHORTCUT_COPY] = { "copy_shortcut", "copy", "Copy", "win.copy-buffer", "<Primary>c" },
  [WAYTATOR_SHORTCUT_COPY_KEEP_OPEN] = { "copy_keep_open_shortcut", "copy-without-closing", "Copy without closing", "win.copy-keep-open", "<Primary><Shift>c" },
  [WAYTATOR_SHORTCUT_SAVE] = { "save_shortcut", "save", "Save", "win.save-image", "<Primary>s" },
  [WAYTATOR_SHORTCUT_SAVE_CLOSE] = { "save_close_shortcut", "save-and-close", "Save and close", "win.save-and-close", "" },
  [WAYTATOR_SHORTCUT_SAVE_COPY_CLOSE] = { "save_copy_close_shortcut", "save-copy-and-close", "Save, copy and close", "win.save-copy-and-close", "" },
  [WAYTATOR_SHORTCUT_CAPTURE_AREA] = { "capture_area_shortcut", "capture-area", "Screenshot area", "win.capture-area", "<Primary><Shift>a" },
  [WAYTATOR_SHORTCUT_CAPTURE_SCREEN] = { "capture_screen_shortcut", "capture-screen", "Screenshot screen", "win.capture-screen", "<Primary><Shift>s" },
  [WAYTATOR_TOOL_SHORTCUT(WAYTATOR_TOOL_PAN)] = { "pan_tool_shortcut", NULL, "Pan", NULL, "p" },
  [WAYTATOR_TOOL_SHORTCUT(WAYTATOR_TOOL_CROP)] = { "crop_tool_shortcut", NULL, "Transform", NULL, "c" },
  [WAYTATOR_TOOL_SHORTCUT(WAYTATOR_TOOL_BRUSH)] = { "brush_tool_shortcut", NULL, "Brush", NULL, "b" },
  [WAYTATOR_TOOL_SHORTCUT(WAYTATOR_TOOL_MARKER)] = { "highlighter_tool_shortcut", NULL, "Highlighter", NULL, "h" },
  [WAYTATOR_TOOL_SHORTCUT(WAYTATOR_TOOL_ERASER)] = { "eraser_tool_shortcut", NULL, "Eraser", NULL, "e" },
  [WAYTATOR_TOOL_SHORTCUT(WAYTATOR_TOOL_RECTANGLE)] = { "rectangle_tool_shortcut", NULL, "Rectangle", NULL, "r" },
  [WAYTATOR_TOOL_SHORTCUT(WAYTATOR_TOOL_CIRCLE)] = { "circle_tool_shortcut", NULL, "Circle", NULL, "o" },
  [WAYTATOR_TOOL_SHORTCUT(WAYTATOR_TOOL_LINE)] = { "line_tool_shortcut", NULL, "Line", NULL, "l" },
  [WAYTATOR_TOOL_SHORTCUT(WAYTATOR_TOOL_ARROW)] = { "arrow_tool_shortcut", NULL, "Arrow", NULL, "a" },
  [WAYTATOR_TOOL_SHORTCUT(WAYTATOR_TOOL_OCR)] = { "ocr_tool_shortcut", NULL, "Recognize text", NULL, "g" },
  [WAYTATOR_TOOL_SHORTCUT(WAYTATOR_TOOL_TEXT)] = { "text_tool_shortcut", NULL, "Text", NULL, "t" },
  [WAYTATOR_TOOL_SHORTCUT(WAYTATOR_TOOL_BLUR)] = { "blur_tool_shortcut", NULL, "Blur", NULL, "u" },
  [WAYTATOR_SHORTCUT_SIZE_DECREASE] = { "size_decrease_shortcut", NULL, "Decrease size", NULL, "bracketleft" },
  [WAYTATOR_SHORTCUT_SIZE_INCREASE] = { "size_increase_shortcut", NULL, "Increase size", NULL, "bracketright" },
  [WAYTATOR_COLOR_SHORTCUT(0)] = { "color_1_shortcut", NULL, "Color 1", NULL, "1" },
  [WAYTATOR_COLOR_SHORTCUT(1)] = { "color_2_shortcut", NULL, "Color 2", NULL, "2" },
  [WAYTATOR_COLOR_SHORTCUT(2)] = { "color_3_shortcut", NULL, "Color 3", NULL, "3" },
  [WAYTATOR_COLOR_SHORTCUT(3)] = { "color_4_shortcut", NULL, "Color 4", NULL, "4" },
  [WAYTATOR_COLOR_SHORTCUT(4)] = { "color_5_shortcut", NULL, "Color 5", NULL, "5" },
  [WAYTATOR_COLOR_SHORTCUT(5)] = { "color_6_shortcut", NULL, "Color 6", NULL, "6" },
  [WAYTATOR_COLOR_SHORTCUT(6)] = { "color_7_shortcut", NULL, "Color 7", NULL, "7" },
  [WAYTATOR_COLOR_SHORTCUT(7)] = { "color_8_shortcut", NULL, "Color 8", NULL, "8" },
  [WAYTATOR_COLOR_SHORTCUT(8)] = { "color_9_shortcut", NULL, "Color 9", NULL, "9" },
};

#define WAYTATOR_DEFAULT_N_PALETTE_COLORS 3

//the first few are what a new palette starts with, the rest is what "add" hands out next
static const GdkRGBA waytator_default_palette[WAYTATOR_MAX_PALETTE_COLORS] = {
  { 0.96, 0.2, 0.28, 1.0 },
  { 0.2, 0.78, 0.35, 1.0 },
  { 0.2, 0.52, 0.96, 1.0 },
  { 1.0, 0.91, 0.2, 1.0 },
  { 1.0, 0.55, 0.1, 1.0 },
  { 0.62, 0.36, 0.9, 1.0 },
  { 0.0, 0.0, 0.0, 1.0 },
  { 1.0, 1.0, 1.0, 1.0 },
  { 1.0, 0.45, 0.7, 1.0 },
};

static gboolean
waytator_window_parse_accelerator(const char      *accelerator,
                                  guint           *keyval,
                                  GdkModifierType *modifiers)
{
  guint parsed_keyval = 0;
  GdkModifierType parsed_modifiers = 0;

  if (accelerator == NULL || *accelerator == '\0')
    return FALSE;

  gtk_accelerator_parse(accelerator, &parsed_keyval, &parsed_modifiers);
  if (parsed_keyval == 0)
    return FALSE;

  if (keyval != NULL)
    *keyval = gdk_keyval_to_lower(parsed_keyval);
  if (modifiers != NULL)
    *modifiers = parsed_modifiers & gtk_accelerator_get_default_mod_mask();

  return TRUE;
}

static gboolean
waytator_window_modifier_from_name(const char      *name,
                                   GdkModifierType *modifier)
{
  static const struct {
    const char *name;
    GdkModifierType modifier;
  } modifiers[] = {
    { "ctrl", GDK_CONTROL_MASK },
    { "control", GDK_CONTROL_MASK },
    { "shift", GDK_SHIFT_MASK },
    { "alt", GDK_ALT_MASK },
    { "super", GDK_SUPER_MASK },
    { "win", GDK_SUPER_MASK },
  };

  for (guint i = 0; i < G_N_ELEMENTS(modifiers); i++) {
    if (g_ascii_strcasecmp(name, modifiers[i].name) != 0)
      continue;

    *modifier = modifiers[i].modifier;
    return TRUE;
  }

  return FALSE;
}

static guint
waytator_window_keyval_from_name(const char *name)
{
  static const struct {
    const char *name;
    guint keyval;
  } aliases[] = {
    { "esc", GDK_KEY_Escape },
    { "enter", GDK_KEY_Return },
    { "backspace", GDK_KEY_BackSpace },
    { "del", GDK_KEY_Delete },
    { "ins", GDK_KEY_Insert },
    { "pageup", GDK_KEY_Page_Up },
    { "pagedown", GDK_KEY_Page_Down },
    { "printscreen", GDK_KEY_Print },
  };
  g_autofree char *lower = g_ascii_strdown(name, -1);
  g_autofree char *upper = g_ascii_strup(name, -1);
  g_autofree char *capitalized = g_strdup(lower);
  const char *candidates[4];

  for (guint i = 0; i < G_N_ELEMENTS(aliases); i++) {
    if (g_strcmp0(lower, aliases[i].name) == 0)
      return aliases[i].keyval;
  }

  //keysym names are case sensitive (Escape, F5, space), people typing a flag are not
  capitalized[0] = g_ascii_toupper(capitalized[0]);
  candidates[0] = name;
  candidates[1] = lower;
  candidates[2] = capitalized;
  candidates[3] = upper;
  for (guint i = 0; i < G_N_ELEMENTS(candidates); i++) {
    const guint keyval = gdk_keyval_from_name(candidates[i]);

    if (keyval != 0 && keyval != GDK_KEY_VoidSymbol)
      return keyval;
  }

  if (g_utf8_strlen(name, -1) == 1)
    return gdk_unicode_to_keyval(g_utf8_get_char(name));

  return 0;
}

//takes "ctrl+shift+s" style or gtk accelerator syntax and gives back the stored form, NULL if it makes no sense
static char *
waytator_window_normalize_shortcut(const char *shortcut)
{
  g_autofree char *copy = NULL;
  g_autofree char *accelerator = NULL;
  g_auto(GStrv) tokens = NULL;
  GdkModifierType modifiers = 0;
  guint keyval = 0;
  guint n_tokens;
  guint n_modifiers;

  if (shortcut == NULL || *shortcut == '\0' || g_ascii_strcasecmp(shortcut, WAYTATOR_SHORTCUT_NONE) == 0)
    return g_strdup("");

  if (g_ascii_strcasecmp(shortcut, WAYTATOR_SHORTCUT_RIGHT_CLICK) == 0)
    return g_strdup(WAYTATOR_SHORTCUT_RIGHT_CLICK);

  if (waytator_window_parse_accelerator(shortcut, NULL, NULL))
    return g_strdup(shortcut);

  copy = g_strdup(shortcut);
  if (g_str_has_suffix(copy, "+")) {
    //a trailing plus is the plus key itself, as in "ctrl++"
    keyval = GDK_KEY_plus;
    copy[strlen(copy) - 1] = '\0';
    if (g_str_has_suffix(copy, "+"))
      copy[strlen(copy) - 1] = '\0';
  }

  tokens = g_strsplit(copy, "+", -1);
  n_tokens = g_strv_length(tokens);
  if (keyval == 0 && n_tokens == 0)
    return NULL;

  n_modifiers = keyval != 0 ? n_tokens : n_tokens - 1;

  for (guint i = 0; i < n_modifiers; i++) {
    GdkModifierType modifier;

    if (!waytator_window_modifier_from_name(g_strstrip(tokens[i]), &modifier))
      return NULL;

    modifiers |= modifier;
  }

  if (keyval == 0)
    keyval = waytator_window_keyval_from_name(g_strstrip(tokens[n_tokens - 1]));
  if (keyval == 0)
    return NULL;

  accelerator = gtk_accelerator_name(gdk_keyval_to_lower(keyval), modifiers);
  if (!waytator_window_parse_accelerator(accelerator, NULL, NULL))
    return NULL;

  return g_steal_pointer(&accelerator);
}

gboolean
waytator_window_shortcut_is_valid(const char *shortcut)
{
  g_autofree char *normalized = waytator_window_normalize_shortcut(shortcut);

  return normalized != NULL;
}

static void
waytator_window_apply_shortcut(WaytatorWindow  *self,
                               WaytatorShortcut shortcut,
                               const char      *accelerator)
{
  g_clear_pointer(&self->shortcut_accels[shortcut], g_free);
  self->shortcut_accels[shortcut] = g_strdup(accelerator != NULL ? accelerator : "");
}

const char *
waytator_window_shortcut_accel(WaytatorWindow  *self,
                               WaytatorShortcut shortcut)
{
  return self->shortcut_overrides[shortcut] != NULL
       ? self->shortcut_overrides[shortcut]
       : self->shortcut_accels[shortcut];
}

const char *
waytator_window_shortcut_title(WaytatorShortcut shortcut)
{
  return waytator_shortcuts[shortcut].title;
}

//color slots past the end of the palette keep their key but do nothing
static gboolean
waytator_window_shortcut_exists(WaytatorWindow  *self,
                                WaytatorShortcut shortcut)
{
  return shortcut < WAYTATOR_SHORTCUT_COLOR_FIRST
      || shortcut - WAYTATOR_SHORTCUT_COLOR_FIRST < self->n_palette_colors;
}

gboolean
waytator_window_shortcut_is_active(WaytatorWindow  *self,
                                   WaytatorShortcut shortcut)
{
  const char *accelerator = waytator_window_shortcut_accel(self, shortcut);

  if (accelerator == NULL || *accelerator == '\0' || !waytator_window_shortcut_exists(self, shortcut))
    return FALSE;

  //the toolbar ones need an image, and stay out of the way of a stroke in progress
  if (shortcut >= WAYTATOR_SHORTCUT_TOOL_FIRST)
    return self->texture != NULL && !self->drawing;

  return shortcut != WAYTATOR_SHORTCUT_COPY_KEEP_OPEN || waytator_window_closes_on_copy(self);
}

static gboolean
waytator_window_accelerators_equal(const char *first,
                                   const char *second)
{
  guint first_keyval;
  guint second_keyval;
  GdkModifierType first_modifiers;
  GdkModifierType second_modifiers;

  if (g_strcmp0(first, WAYTATOR_SHORTCUT_RIGHT_CLICK) == 0
      || g_strcmp0(second, WAYTATOR_SHORTCUT_RIGHT_CLICK) == 0)
    return g_strcmp0(first, second) == 0;

  return waytator_window_parse_accelerator(first, &first_keyval, &first_modifiers)
      && waytator_window_parse_accelerator(second, &second_keyval, &second_modifiers)
      && first_keyval == second_keyval
      && first_modifiers == second_modifiers;
}

//the keys bound in main.c, which nothing in the preferences can take over
static const struct {
  const char *action;
  const char *title;
} waytator_fixed_shortcuts[] = {
  { "win.open", "Open" },
  { "win.preferences", "Preferences" },
  { "win.undo", "Undo" },
  { "win.redo", "Redo" },
  { "win.zoom-in", "Zoom in" },
  { "win.zoom-out", "Zoom out" },
  { "win.zoom-fit", "Fit" },
  { "win.zoom-actual-size", "Reset zoom to 100%" },
  { "win.rotate-counter-clockwise", "Rotate counter clockwise" },
  { "win.flip-horizontal", "Flip horizontal" },
  { "win.flip-vertical", "Flip vertical" },
  { "app.quit", "Quit" },
};

//what a key bound in main.c does, or NULL. those come first, a shortcut on the same key never fires
static const char *
waytator_window_fixed_shortcut_title(WaytatorWindow *self,
                                     const char     *accelerator)
{
  GtkApplication *app = gtk_window_get_application(GTK_WINDOW(self));
  g_autofree char *name = NULL;
  g_auto(GStrv) actions = NULL;
  guint keyval;
  GdkModifierType modifiers;

  if (app == NULL || !waytator_window_parse_accelerator(accelerator, &keyval, &modifiers))
    return NULL;

  name = gtk_accelerator_name(keyval, modifiers);
  actions = gtk_application_get_actions_for_accel(app, name);
  for (guint i = 0; actions != NULL && actions[i] != NULL; i++) {
    for (guint j = 0; j < G_N_ELEMENTS(waytator_fixed_shortcuts); j++) {
      if (g_strcmp0(actions[i], waytator_fixed_shortcuts[j].action) == 0)
        return waytator_fixed_shortcuts[j].title;
    }
  }

  return NULL;
}

//the earlier shortcut holding the same key, or -1
int
waytator_window_shortcut_conflict(WaytatorWindow  *self,
                                  WaytatorShortcut shortcut,
                                  gboolean         with_overrides)
{
  const char *accelerator = with_overrides
                          ? waytator_window_shortcut_accel(self, shortcut)
                          : self->shortcut_accels[shortcut];

  for (int i = 0; i < (int) shortcut; i++) {
    const char *other = with_overrides
                      ? waytator_window_shortcut_accel(self, i)
                      : self->shortcut_accels[i];

    if (waytator_window_shortcut_exists(self, i)
        && waytator_window_accelerators_equal(accelerator, other))
      return i;
  }

  return -1;
}

void
waytator_window_activate_shortcut(WaytatorWindow  *self,
                                  WaytatorShortcut shortcut)
{
  if (shortcut >= WAYTATOR_SHORTCUT_TOOL_FIRST && shortcut <= WAYTATOR_SHORTCUT_TOOL_LAST)
    waytator_window_select_tool(self, shortcut - WAYTATOR_SHORTCUT_TOOL_FIRST);
  else if (shortcut == WAYTATOR_SHORTCUT_SIZE_DECREASE)
    waytator_window_step_tool_size(self, -1);
  else if (shortcut == WAYTATOR_SHORTCUT_SIZE_INCREASE)
    waytator_window_step_tool_size(self, 1);
  else if (shortcut >= WAYTATOR_SHORTCUT_COLOR_FIRST && shortcut <= WAYTATOR_SHORTCUT_COLOR_LAST)
    waytator_window_apply_palette_color(self, shortcut - WAYTATOR_SHORTCUT_COLOR_FIRST);
  else
    gtk_widget_activate_action(GTK_WIDGET(self), waytator_shortcuts[shortcut].action, NULL);
}

gboolean
waytator_window_closes_on_copy(WaytatorWindow *self)
{
  return self->close_on_copy_override >= 0
       ? self->close_on_copy_override
       : self->close_on_copy;
}

void
waytator_window_clear_overrides(WaytatorWindow *self)
{
  self->close_on_copy_override = -1;
  for (int i = 0; i < WAYTATOR_N_SHORTCUTS; i++)
    g_clear_pointer(&self->shortcut_overrides[i], g_free);
  waytator_window_update_copy_controls(self);
  waytator_window_update_tool_shortcut_hints(self);
  waytator_window_update_capture_controls(self);
}

void
waytator_window_set_close_on_copy_override(WaytatorWindow *self,
                                           gboolean        close_on_copy)
{
  self->close_on_copy_override = close_on_copy ? 1 : 0;
  waytator_window_update_copy_controls(self);
}

gboolean
waytator_window_set_shortcut_override(WaytatorWindow *self,
                                      const char     *name,
                                      const char     *shortcut)
{
  g_autofree char *normalized = waytator_window_normalize_shortcut(shortcut);

  if (normalized == NULL)
    return FALSE;

  for (int i = 0; i < WAYTATOR_N_SHORTCUTS; i++) {
    if (g_strcmp0(name, waytator_shortcuts[i].name) != 0)
      continue;

    g_free(self->shortcut_overrides[i]);
    self->shortcut_overrides[i] = g_steal_pointer(&normalized);
    waytator_window_update_tool_shortcut_hints(self);
    waytator_window_update_capture_controls(self);
    return TRUE;
  }

  return FALSE;
}

static void
waytator_window_update_shortcut_label(GtkShortcutLabel *label,
                                      const char       *accelerator)
{
  if (g_strcmp0(accelerator, WAYTATOR_SHORTCUT_RIGHT_CLICK) == 0) {
    gtk_shortcut_label_set_disabled_text(label, "Right click");
    gtk_shortcut_label_set_accelerator(label, "");
    return;
  }

  gtk_shortcut_label_set_disabled_text(label, "None");
  gtk_shortcut_label_set_accelerator(label,
                                     accelerator != NULL ? accelerator : "");

  //drop the "+" it puts between keys, the keycaps read fine without it
  if (accelerator == NULL || *accelerator == '\0')
    return;

  for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(label));
       child != NULL;
       child = gtk_widget_get_next_sibling(child))
    gtk_widget_set_visible(child, gtk_widget_has_css_class(child, "keycap"));
}

static const char *
waytator_window_angle_snap_modifier_label(GdkModifierType modifiers)
{
  switch (modifiers & gtk_accelerator_get_default_mod_mask()) {
  case 0:
    return "Disabled";
  case GDK_SHIFT_MASK:
    return "Shift";
  case GDK_CONTROL_MASK:
    return "Ctrl";
  case GDK_ALT_MASK:
    return "Alt";
  case GDK_SUPER_MASK:
    return "Super";
  default:
    return "Shift";
  }
}

static void
waytator_window_load_preferences(WaytatorWindow *self)
{
  g_autofree char *path = waytator_settings_path();
  g_autoptr(GKeyFile) key_file = g_key_file_new();
  g_autoptr(GError) error = NULL;

  if (!g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, &error)) {
    if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
      g_warning("Failed to load preferences from %s: %s", path, error->message);
    return;
  }

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "eraser_style", NULL)) {
    const int eraser_style = g_key_file_get_integer(key_file,
                                                    WAYTATOR_SETTINGS_GROUP,
                                                    "eraser_style",
                                                    NULL);

    if (eraser_style >= WAYTATOR_ERASER_STYLE_DUAL_RING
        && eraser_style <= WAYTATOR_ERASER_STYLE_PATTERN)
      self->eraser_style = eraser_style;
  }

  waytator_settings_read(&self->settings, key_file);

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "window_background_mode", NULL)) {
    const int mode = g_key_file_get_integer(key_file,
                                            WAYTATOR_SETTINGS_GROUP,
                                            "window_background_mode",
                                            NULL);

    if (mode >= WAYTATOR_WINDOW_BACKGROUND_FOLLOW_SYSTEM
        && mode <= WAYTATOR_WINDOW_BACKGROUND_TRANSPARENT)
      self->window_background_mode = mode;
  } else if (g_key_file_has_key(key_file,
                                WAYTATOR_SETTINGS_GROUP,
                                "window_transparency_enabled",
                                NULL)) {
    self->window_background_mode = g_key_file_get_boolean(key_file,
                                                          WAYTATOR_SETTINGS_GROUP,
                                                          "window_transparency_enabled",
                                                          NULL)
                                 ? WAYTATOR_WINDOW_BACKGROUND_TRANSPARENT
                                 : WAYTATOR_WINDOW_BACKGROUND_OPAQUE;
  }

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "window_background_opacity", NULL)) {
    const double opacity = g_key_file_get_double(key_file,
                                                 WAYTATOR_SETTINGS_GROUP,
                                                 "window_background_opacity",
                                                 NULL);

    if (opacity >= 0.1 && opacity <= 1.0)
      self->window_background_opacity = opacity;
  }

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "auto_copy_latest_change", NULL))
    self->auto_copy_latest_change = g_key_file_get_boolean(key_file,
                                                           WAYTATOR_SETTINGS_GROUP,
                                                           "auto_copy_latest_change",
                                                           NULL);

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "close_on_copy", NULL))
    self->close_on_copy = g_key_file_get_boolean(key_file,
                                                 WAYTATOR_SETTINGS_GROUP,
                                                 "close_on_copy",
                                                 NULL);

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "size_slider_visible", NULL))
    self->size_drawer_open = g_key_file_get_boolean(key_file,
                                                    WAYTATOR_SETTINGS_GROUP,
                                                    "size_slider_visible",
                                                    NULL);

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "allow_highlighter_overlap", NULL))
    self->allow_highlighter_overlap = g_key_file_get_boolean(key_file,
                                                             WAYTATOR_SETTINGS_GROUP,
                                                             "allow_highlighter_overlap",
                                                             NULL);

  for (int i = 0; i < WAYTATOR_N_SHORTCUTS; i++) {
    g_autofree char *accelerator = NULL;
    g_autofree char *normalized = NULL;

    if (!g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, waytator_shortcuts[i].settings_key, NULL))
      continue;

    accelerator = g_key_file_get_string(key_file,
                                        WAYTATOR_SETTINGS_GROUP,
                                        waytator_shortcuts[i].settings_key,
                                        NULL);
    normalized = waytator_window_normalize_shortcut(accelerator);
    if (normalized != NULL)
      waytator_window_apply_shortcut(self, i, normalized);
  }

  //these two used to be switches, a switched off one is now just an unassigned shortcut
  if (!g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "close_window_shortcut", NULL)
      && g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "esc_closes_window", NULL)
      && !g_key_file_get_boolean(key_file, WAYTATOR_SETTINGS_GROUP, "esc_closes_window", NULL))
    waytator_window_apply_shortcut(self, WAYTATOR_SHORTCUT_CLOSE_WINDOW, "");

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "copy_shortcut_enabled", NULL)
      && !g_key_file_get_boolean(key_file, WAYTATOR_SETTINGS_GROUP, "copy_shortcut_enabled", NULL))
    waytator_window_apply_shortcut(self, WAYTATOR_SHORTCUT_COPY, "");

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "angle_snap_modifiers", NULL)) {
    const int modifiers = g_key_file_get_integer(key_file,
                                                 WAYTATOR_SETTINGS_GROUP,
                                                 "angle_snap_modifiers",
                                                 NULL);

    switch (modifiers & gtk_accelerator_get_default_mod_mask()) {
    case 0:
    case GDK_SHIFT_MASK:
    case GDK_CONTROL_MASK:
    case GDK_ALT_MASK:
    case GDK_SUPER_MASK:
      self->angle_snap_modifiers = modifiers & gtk_accelerator_get_default_mod_mask();
      break;
    default:
      break;
    }
  }

  waytator_window_load_rgba_preference(key_file,
                                       "default_primary_color",
                                       &self->default_primary_color);
  waytator_window_load_rgba_preference(key_file,
                                       "default_highlighter_color",
                                       &self->default_highlighter_color);
  waytator_window_load_rgba_preference(key_file,
                                       "default_fill_color",
                                       &self->default_fill_color);

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "palette_colors", NULL)) {
    g_auto(GStrv) colors = g_key_file_get_string_list(key_file,
                                                      WAYTATOR_SETTINGS_GROUP,
                                                      "palette_colors",
                                                      NULL,
                                                      NULL);

    self->n_palette_colors = 0;
    for (guint i = 0; colors != NULL && colors[i] != NULL; i++) {
      if (self->n_palette_colors < WAYTATOR_MAX_PALETTE_COLORS
          && gdk_rgba_parse(&self->palette_colors[self->n_palette_colors], colors[i]))
        self->n_palette_colors++;
    }
  }
}

//every window keeps its own copy of the preferences, a stale one would undo a change the next time it saves
void
waytator_window_reload_preferences(GtkApplication *app,
                                   WaytatorWindow *except)
{
  if (app == NULL)
    return;

  for (GList *link = gtk_application_get_windows(app); link != NULL; link = link->next) {
    WaytatorWindow *other;
    gboolean size_drawer_open;

    if (!WAYTATOR_IS_WINDOW(link->data) || link->data == (gpointer) except)
      continue;

    other = WAYTATOR_WINDOW(link->data);
    //whether the drawer is open is saved too, but each window has its own drawer
    size_drawer_open = other->size_drawer_open;
    waytator_window_load_preferences(other);
    other->size_drawer_open = size_drawer_open;
    waytator_window_update_window_background(other);
    waytator_window_update_widget_appearance(other);
    waytator_window_update_copy_controls(other);
    waytator_window_update_tool_shortcut_hints(other);
    waytator_window_update_capture_controls(other);
    gtk_widget_queue_draw(GTK_WIDGET(other->drawing_area));
  }
}

void
waytator_window_save_preferences(WaytatorWindow *self)
{
  g_autofree char *path = waytator_settings_path();
  g_autofree char *directory = g_path_get_dirname(path);
  g_autoptr(GKeyFile) key_file = g_key_file_new();
  g_autofree char *data = NULL;
  g_autofree char *default_primary_color = gdk_rgba_to_string(&self->default_primary_color);
  g_autofree char *default_highlighter_color = gdk_rgba_to_string(&self->default_highlighter_color);
  g_autofree char *default_fill_color = gdk_rgba_to_string(&self->default_fill_color);
  g_autoptr(GKeyFile) previous = g_key_file_new();
  gsize data_length = 0;
  g_autoptr(GError) error = NULL;

  //the video profiles are kept in the file by the preferences that edit them
  if (g_key_file_load_from_file(previous, path, G_KEY_FILE_NONE, NULL))
    waytator_video_profiles_carry_over(previous, key_file);
  g_key_file_set_integer(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "eraser_style",
                         self->eraser_style);
  waytator_settings_write(&self->settings, key_file);
  g_key_file_set_integer(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "window_background_mode",
                         self->window_background_mode);
  g_key_file_set_double(key_file,
                        WAYTATOR_SETTINGS_GROUP,
                        "window_background_opacity",
                        self->window_background_opacity);
  g_key_file_set_boolean(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "auto_copy_latest_change",
                         self->auto_copy_latest_change);
  g_key_file_set_boolean(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "allow_highlighter_overlap",
                         self->allow_highlighter_overlap);
  g_key_file_set_boolean(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "close_on_copy",
                         self->close_on_copy);
  g_key_file_set_boolean(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "size_slider_visible",
                         self->size_drawer_open);
  for (int i = 0; i < WAYTATOR_N_SHORTCUTS; i++)
    g_key_file_set_string(key_file,
                          WAYTATOR_SETTINGS_GROUP,
                          waytator_shortcuts[i].settings_key,
                          self->shortcut_accels[i]);
  g_key_file_set_integer(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "angle_snap_modifiers",
                         self->angle_snap_modifiers);
  g_key_file_set_string(key_file,
                        WAYTATOR_SETTINGS_GROUP,
                        "default_primary_color",
                        default_primary_color);
  g_key_file_set_string(key_file,
                        WAYTATOR_SETTINGS_GROUP,
                        "default_highlighter_color",
                        default_highlighter_color);
  g_key_file_set_string(key_file,
                        WAYTATOR_SETTINGS_GROUP,
                        "default_fill_color",
                        default_fill_color);

  {
    g_autoptr(GStrvBuilder) colors = g_strv_builder_new();
    g_auto(GStrv) color_list = NULL;

    for (guint i = 0; i < self->n_palette_colors; i++)
      g_strv_builder_take(colors, gdk_rgba_to_string(&self->palette_colors[i]));

    color_list = g_strv_builder_end(colors);
    g_key_file_set_string_list(key_file,
                               WAYTATOR_SETTINGS_GROUP,
                               "palette_colors",
                               (const char * const *) color_list,
                               self->n_palette_colors);
  }

  if (g_mkdir_with_parents(directory, 0700) != 0) {
    g_warning("Failed to create preferences directory %s", directory);
    return;
  }

  data = g_key_file_to_data(key_file, &data_length, NULL);
  if (!g_file_set_contents(path, data, data_length, &error)) {
    g_warning("Failed to save preferences to %s: %s", path, error->message);
    return;
  }

  waytator_window_reload_preferences(gtk_window_get_application(GTK_WINDOW(self)), self);
}

static const char *
waytator_window_eraser_style_label(WaytatorEraserStyle style)
{
  switch (style) {
  case WAYTATOR_ERASER_STYLE_DUAL_RING:
    return "Dual ring";
  case WAYTATOR_ERASER_STYLE_DASHED_RING:
    return "Dashed ring";
  case WAYTATOR_ERASER_STYLE_PATTERN:
    return "Pattern fill";
  default:
    return "Dual ring";
  }
}

static void
waytator_window_eraser_style_changed(AdwComboRow    *row,
                                     GParamSpec     *pspec,
                                     WaytatorWindow *self)
{
  (void) pspec;

  self->eraser_style = adw_combo_row_get_selected(row);
  waytator_window_save_preferences(self);
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

static void
waytator_window_window_controls_changed(GListModel     *model,
                                        guint           position,
                                        guint           removed,
                                        guint           added,
                                        WaytatorWindow *self)
{
  (void) model;
  (void) position;
  (void) removed;
  (void) added;

  waytator_window_update_window_controls(self);
}

static void
waytator_window_update_window_controls(WaytatorWindow *self)
{
  const gboolean has_start_controls = self->start_window_controls_children != NULL
                                   && g_list_model_get_n_items(self->start_window_controls_children) > 0;
  const gboolean has_end_controls = self->end_window_controls_children != NULL
                                 && g_list_model_get_n_items(self->end_window_controls_children) > 0;

  gtk_widget_set_visible(GTK_WIDGET(self->start_window_controls), has_start_controls);
  gtk_widget_set_visible(self->start_window_controls_pill, has_start_controls);
  gtk_widget_set_visible(GTK_WIDGET(self->end_window_controls), has_end_controls);
  waytator_window_queue_chrome_layout(self);
}

static void
waytator_window_background_mode_changed(AdwComboRow    *row,
                                        GParamSpec     *pspec,
                                        WaytatorWindow *self)
{
  GtkWidget *opacity_row;

  (void) pspec;

  self->window_background_mode = adw_combo_row_get_selected(row);
  opacity_row = g_object_get_data(G_OBJECT(row), "opacity-row");
  if (opacity_row != NULL)
    gtk_widget_set_sensitive(opacity_row,
                             self->window_background_mode == WAYTATOR_WINDOW_BACKGROUND_TRANSPARENT);
  waytator_window_save_preferences(self);
  waytator_window_update_window_background(self);
}

static void
waytator_window_transparency_opacity_changed(GtkSpinButton  *spin_button,
                                             WaytatorWindow *self)
{
  self->window_background_opacity = gtk_spin_button_get_value(spin_button);
  waytator_window_save_preferences(self);
  waytator_window_update_window_background(self);
}

static void
waytator_window_floating_controls_blur_changed(AdwSwitchRow   *row,
                                               GParamSpec     *pspec,
                                               WaytatorWindow *self)
{
  (void) pspec;

  self->settings.floating_controls_blur = adw_switch_row_get_active(row);
  waytator_window_save_preferences(self);
  waytator_window_update_widget_appearance(self);
}

static void
waytator_window_floating_controls_opacity_changed(GtkSpinButton  *spin_button,
                                                  WaytatorWindow *self)
{
  self->settings.floating_controls_opacity = gtk_spin_button_get_value(spin_button);
  waytator_window_save_preferences(self);
  waytator_window_update_widget_appearance(self);
}

static void
waytator_window_auto_copy_latest_change_changed(AdwSwitchRow   *row,
                                                GParamSpec     *pspec,
                                                WaytatorWindow *self)
{
  (void) pspec;

  self->auto_copy_latest_change = adw_switch_row_get_active(row);
  waytator_window_save_preferences(self);
}

static void
waytator_window_close_on_copy_changed(AdwSwitchRow   *row,
                                      GParamSpec     *pspec,
                                      WaytatorWindow *self)
{
  GtkWidget *shortcut_row;

  (void) pspec;

  self->close_on_copy = adw_switch_row_get_active(row);
  shortcut_row = g_object_get_data(G_OBJECT(row), "shortcut-row");
  if (shortcut_row != NULL)
    gtk_widget_set_visible(shortcut_row, self->close_on_copy || waytator_window_closes_on_copy(self));
  waytator_window_update_copy_controls(self);
  waytator_window_save_preferences(self);
}

static void
waytator_window_highlighter_overlap_changed(AdwSwitchRow   *row,
                                            GParamSpec     *pspec,
                                            WaytatorWindow *self)
{
  (void) pspec;

  self->allow_highlighter_overlap = adw_switch_row_get_active(row);
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  waytator_window_save_preferences(self);
}

static void
waytator_window_angle_snap_modifier_changed(AdwComboRow    *row,
                                            GParamSpec     *pspec,
                                            WaytatorWindow *self)
{
  static const GdkModifierType snap_modifiers[] = {
    0,
    GDK_SHIFT_MASK,
    GDK_CONTROL_MASK,
    GDK_ALT_MASK,
    GDK_SUPER_MASK,
  };
  const guint selected = adw_combo_row_get_selected(row);

  (void) pspec;

  if (selected >= G_N_ELEMENTS(snap_modifiers))
    return;

  self->angle_snap_modifiers = snap_modifiers[selected];
  waytator_window_save_preferences(self);
}

static void
waytator_window_default_primary_color_changed(GObject    *object,
                                              GParamSpec *pspec,
                                              gpointer    user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  const GdkRGBA *rgba;

  (void) pspec;

  rgba = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(object));
  if (rgba == NULL)
    return;

  self->default_primary_color = *rgba;
  waytator_window_apply_default_tool_colors(self);
  waytator_window_sync_active_tool_colors(self);
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  waytator_window_save_preferences(self);
}

static void
waytator_window_default_highlighter_color_changed(GObject    *object,
                                                  GParamSpec *pspec,
                                                  gpointer    user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  const GdkRGBA *rgba;

  (void) pspec;

  rgba = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(object));
  if (rgba == NULL)
    return;

  self->default_highlighter_color = *rgba;
  waytator_window_apply_default_tool_colors(self);
  waytator_window_sync_active_tool_colors(self);
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  waytator_window_save_preferences(self);
}

static void
waytator_window_default_fill_color_changed(GObject    *object,
                                           GParamSpec *pspec,
                                           gpointer    user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  const GdkRGBA *rgba;

  (void) pspec;

  rgba = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(object));
  if (rgba == NULL)
    return;

  self->default_fill_color = *rgba;
  waytator_window_apply_default_tool_colors(self);
  waytator_window_sync_active_tool_colors(self);
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  waytator_window_save_preferences(self);
}

static void
waytator_window_update_shortcut_conflicts(WaytatorWindow *self)
{
  for (int i = 0; i < WAYTATOR_N_SHORTCUTS; i++) {
    const int conflict = waytator_window_shortcut_conflict(self, i, FALSE);
    const char *fixed = waytator_window_fixed_shortcut_title(self, self->shortcut_accels[i]);
    g_autofree char *subtitle = NULL;

    if (self->shortcut_rows[i] == NULL)
      continue;

    if (fixed != NULL || conflict >= 0) {
      subtitle = g_strdup_printf("Already used by %s, which takes priority",
                                 fixed != NULL ? fixed : waytator_shortcuts[conflict].title);
      gtk_widget_add_css_class(GTK_WIDGET(self->shortcut_rows[i]), "warning");
    } else {
      gtk_widget_remove_css_class(GTK_WIDGET(self->shortcut_rows[i]), "warning");
    }

    adw_action_row_set_subtitle(self->shortcut_rows[i], subtitle != NULL ? subtitle : "");
  }
}

static void
waytator_window_end_shortcut_capture(GtkWidget *button)
{
  GtkWidget *shortcut_label = g_object_get_data(G_OBJECT(button), "shortcut-label");

  gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
  gtk_widget_remove_css_class(button, "suggested-action");
  if (shortcut_label != NULL)
    gtk_widget_set_sensitive(shortcut_label, TRUE);
  g_object_set_data(G_OBJECT(button), "capturing", GINT_TO_POINTER(FALSE));
}

static void
waytator_window_begin_shortcut_capture(GtkButton *button,
                                       gpointer   user_data)
{
  GtkWidget *shortcut_label;
  (void) user_data;

  if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "capturing"))) {
    waytator_window_end_shortcut_capture(GTK_WIDGET(button));
    return;
  }

  gtk_button_set_has_frame(button, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(button), "suggested-action");
  shortcut_label = g_object_get_data(G_OBJECT(button), "shortcut-label");
  if (shortcut_label != NULL)
    gtk_widget_set_sensitive(shortcut_label, FALSE);
  g_object_set_data(G_OBJECT(button), "capturing", GINT_TO_POINTER(TRUE));
}

static void
waytator_window_finish_shortcut_capture(GtkWidget  *button,
                                        const char *accelerator)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(g_object_get_data(G_OBJECT(button), "window"));
  GtkShortcutLabel *shortcut_label = GTK_SHORTCUT_LABEL(g_object_get_data(G_OBJECT(button), "shortcut-label"));
  GtkWidget *clear_button = g_object_get_data(G_OBJECT(button), "clear-button");
  const WaytatorShortcut shortcut = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "shortcut-id"));

  waytator_window_apply_shortcut(self, shortcut, accelerator);
  waytator_window_update_shortcut_label(shortcut_label, self->shortcut_accels[shortcut]);
  gtk_widget_set_visible(clear_button, *self->shortcut_accels[shortcut] != '\0');
  waytator_window_update_shortcut_conflicts(self);
  waytator_window_update_tool_shortcut_hints(self);
  waytator_window_update_capture_controls(self);
  waytator_window_save_preferences(self);
  waytator_window_end_shortcut_capture(button);
}

static gboolean
waytator_window_shortcut_capture_key_pressed(GtkEventControllerKey *controller,
                                             guint                  keyval,
                                             guint                  keycode,
                                             GdkModifierType        state,
                                             gpointer               user_data)
{
  GtkWidget *button = GTK_WIDGET(user_data);
  GdkModifierType modifiers;
  g_autofree char *accelerator = NULL;

  (void) controller;
  (void) keycode;

  if (!GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "capturing")))
    return FALSE;

  modifiers = state & gtk_accelerator_get_default_mod_mask();

  if (keyval == GDK_KEY_Shift_L || keyval == GDK_KEY_Shift_R
      || keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R
      || keyval == GDK_KEY_Alt_L || keyval == GDK_KEY_Alt_R
      || keyval == GDK_KEY_Meta_L || keyval == GDK_KEY_Meta_R
      || keyval == GDK_KEY_Super_L || keyval == GDK_KEY_Super_R
      || keyval == GDK_KEY_Hyper_L || keyval == GDK_KEY_Hyper_R)
    return TRUE;

  accelerator = gtk_accelerator_name(keyval, modifiers);
  if (!waytator_window_parse_accelerator(accelerator, NULL, NULL))
    return TRUE;

  waytator_window_finish_shortcut_capture(button, accelerator);
  return TRUE;
}

static void
waytator_window_shortcut_capture_right_clicked(GtkGestureClick *gesture,
                                               int              n_press,
                                               double           x,
                                               double           y,
                                               gpointer         user_data)
{
  GtkWidget *button = GTK_WIDGET(user_data);

  (void) n_press;
  (void) x;
  (void) y;

  if (!GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "capturing")))
    return;

  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  waytator_window_finish_shortcut_capture(button, WAYTATOR_SHORTCUT_RIGHT_CLICK);
}

static void
waytator_window_shortcut_capture_focus_left(GtkEventControllerFocus *controller,
                                            gpointer                 user_data)
{
  (void) controller;

  waytator_window_end_shortcut_capture(GTK_WIDGET(user_data));
}

static void
waytator_window_shortcut_clear_clicked(GtkButton *clear_button,
                                       gpointer   user_data)
{
  (void) clear_button;

  waytator_window_finish_shortcut_capture(GTK_WIDGET(user_data), "");
}

static AdwActionRow *
waytator_window_new_shortcut_row(WaytatorWindow  *self,
                                 WaytatorShortcut shortcut)
{
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
  GtkWidget *shortcut_label = gtk_shortcut_label_new(NULL);
  GtkWidget *button = gtk_button_new();
  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *clear_button = gtk_button_new_from_icon_name("window-close-symbolic");
  GtkEventController *key_controller = gtk_event_controller_key_new();
  GtkEventController *focus_controller = gtk_event_controller_focus_new();
  GtkGesture *right_click = gtk_gesture_click_new();

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), waytator_shortcuts[shortcut].title);
  waytator_window_update_shortcut_label(GTK_SHORTCUT_LABEL(shortcut_label), self->shortcut_accels[shortcut]);
  gtk_widget_set_valign(shortcut_label, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(shortcut_label, GTK_ALIGN_END);
  gtk_widget_set_hexpand(button_box, FALSE);
  gtk_widget_set_halign(button_box, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(button_box), shortcut_label);
  gtk_button_set_child(GTK_BUTTON(button), button_box);
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(button, "Click, then press a key combination or right-click");
  gtk_button_set_has_frame(GTK_BUTTON(clear_button), FALSE);
  gtk_widget_set_valign(clear_button, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(clear_button, "Unassign");
  gtk_widget_set_visible(clear_button, *self->shortcut_accels[shortcut] != '\0');
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
  gtk_widget_add_controller(button, key_controller);
  gtk_widget_add_controller(button, GTK_EVENT_CONTROLLER(right_click));
  gtk_widget_add_controller(button, focus_controller);
  adw_action_row_add_suffix(row, button);
  adw_action_row_add_suffix(row, clear_button);

  g_set_weak_pointer(&self->shortcut_rows[shortcut], row);
  g_object_set_data(G_OBJECT(button), "window", self);
  g_object_set_data(G_OBJECT(button), "shortcut-label", shortcut_label);
  g_object_set_data(G_OBJECT(button), "shortcut-id", GINT_TO_POINTER(shortcut));
  g_object_set_data(G_OBJECT(button), "clear-button", clear_button);
  g_signal_connect(button,
                   "clicked",
                   G_CALLBACK(waytator_window_begin_shortcut_capture),
                   NULL);
  g_signal_connect(key_controller,
                   "key-pressed",
                   G_CALLBACK(waytator_window_shortcut_capture_key_pressed),
                   button);
  g_signal_connect(right_click,
                   "pressed",
                   G_CALLBACK(waytator_window_shortcut_capture_right_clicked),
                   button);
  g_signal_connect(focus_controller,
                   "leave",
                   G_CALLBACK(waytator_window_shortcut_capture_focus_left),
                   button);
  g_signal_connect(clear_button,
                   "clicked",
                   G_CALLBACK(waytator_window_shortcut_clear_clicked),
                   button);

  return row;
}

static void waytator_window_rebuild_palette_rows(WaytatorWindow *self);

static void
waytator_window_palette_color_changed(GObject    *object,
                                      GParamSpec *pspec,
                                      gpointer    user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  const guint index = GPOINTER_TO_UINT(g_object_get_data(object, "palette-index"));
  const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(object));

  (void) pspec;

  if (rgba == NULL || index >= self->n_palette_colors)
    return;

  self->palette_colors[index] = *rgba;
  waytator_window_save_preferences(self);
}

static void
waytator_window_palette_remove_clicked(GtkButton *button,
                                       gpointer   user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  const guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "palette-index"));

  if (index >= self->n_palette_colors)
    return;

  //the keys stay with their position, only the colors move up
  for (guint i = index; i + 1 < self->n_palette_colors; i++)
    self->palette_colors[i] = self->palette_colors[i + 1];
  self->n_palette_colors--;

  waytator_window_rebuild_palette_rows(self);
  waytator_window_save_preferences(self);
}

static void
waytator_window_palette_add_clicked(GtkButton *button,
                                    gpointer   user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) button;

  if (self->n_palette_colors >= WAYTATOR_MAX_PALETTE_COLORS)
    return;

  self->palette_colors[self->n_palette_colors] = waytator_default_palette[self->n_palette_colors];
  self->n_palette_colors++;

  waytator_window_rebuild_palette_rows(self);
  waytator_window_save_preferences(self);
}

static void
waytator_window_rebuild_palette_rows(WaytatorWindow *self)
{
  if (self->palette_group == NULL)
    return;

  for (int i = WAYTATOR_SHORTCUT_COLOR_FIRST; i <= WAYTATOR_SHORTCUT_COLOR_LAST; i++) {
    if (self->shortcut_rows[i] != NULL)
      adw_preferences_group_remove(self->palette_group, GTK_WIDGET(self->shortcut_rows[i]));
  }

  for (guint i = 0; i < self->n_palette_colors; i++) {
    AdwActionRow *row = waytator_window_new_shortcut_row(self, WAYTATOR_COLOR_SHORTCUT(i));
    //the button takes over the dialog
    GtkWidget *color_button = gtk_color_dialog_button_new(gtk_color_dialog_new());
    GtkWidget *remove_button = gtk_button_new_from_icon_name("user-trash-symbolic");

    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(color_button), &self->palette_colors[i]);
    gtk_widget_set_valign(color_button, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(color_button, "Change color");
    gtk_button_set_has_frame(GTK_BUTTON(remove_button), FALSE);
    gtk_widget_set_valign(remove_button, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(remove_button, "Remove color");
    g_object_set_data(G_OBJECT(color_button), "palette-index", GUINT_TO_POINTER(i));
    g_object_set_data(G_OBJECT(remove_button), "palette-index", GUINT_TO_POINTER(i));
    adw_action_row_add_prefix(row, color_button);
    adw_action_row_add_suffix(row, remove_button);
    g_signal_connect(color_button,
                     "notify::rgba",
                     G_CALLBACK(waytator_window_palette_color_changed),
                     self);
    g_signal_connect(remove_button,
                     "clicked",
                     G_CALLBACK(waytator_window_palette_remove_clicked),
                     self);
    adw_preferences_group_add(self->palette_group, GTK_WIDGET(row));
  }

  if (self->palette_add_button != NULL)
    gtk_widget_set_sensitive(self->palette_add_button,
                             self->n_palette_colors < WAYTATOR_MAX_PALETTE_COLORS);
  waytator_window_update_shortcut_conflicts(self);
}

static void
waytator_window_setup_palette_group(WaytatorWindow      *self,
                                    AdwPreferencesGroup *group)
{
  GtkWidget *add_button = gtk_button_new_from_icon_name("list-add-symbolic");

  gtk_button_set_has_frame(GTK_BUTTON(add_button), FALSE);
  gtk_widget_set_valign(add_button, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(add_button, "Add color");
  adw_preferences_group_set_header_suffix(group, add_button);
  g_signal_connect(add_button, "clicked", G_CALLBACK(waytator_window_palette_add_clicked), self);

  g_set_weak_pointer(&self->palette_group, group);
  g_set_weak_pointer(&self->palette_add_button, add_button);
  waytator_window_rebuild_palette_rows(self);
}

static void
waytator_window_update_window_background(WaytatorWindow *self)
{
  g_autofree char *css = NULL;
  char opacity[G_ASCII_DTOSTR_BUF_SIZE];

  if (self->window_css_provider == NULL)
    return;

  if (self->window_background_mode == WAYTATOR_WINDOW_BACKGROUND_FOLLOW_SYSTEM) {
    gtk_css_provider_load_from_string(self->window_css_provider, "");
    return;
  }

  //dont u just love how gtk css requires repeating the same properties for some reason
  if (self->window_background_mode == WAYTATOR_WINDOW_BACKGROUND_OPAQUE) {
    gtk_css_provider_load_from_string(self->window_css_provider,
                                      "window.waytator-window { background: @window_bg_color; background-color: @window_bg_color; }");
    return;
  }

  // printf would use the locale's decimal separator, which is not valid css
  g_ascii_formatd(opacity, sizeof opacity, "%.1f", self->window_background_opacity);
  //gtk asks the compositor to blur only where a backdrop-filter blurs the desktop,
  //so without one here a blur on the controls shrinks the compositor's blur down to them
  css = g_strdup_printf("window.waytator-window { background: alpha(@window_bg_color, %s); background-color: alpha(@window_bg_color, %s);"
                        " backdrop-filter: blur(18px); }",
                        opacity,
                        opacity);
  gtk_css_provider_load_from_string(self->window_css_provider, css);
}

static void
waytator_window_update_widget_appearance(WaytatorWindow *self)
{
  g_autofree char *css = NULL;
  const char *blur = self->settings.floating_controls_blur ? "blur(18px)" : "none";
  char opacity[G_ASCII_DTOSTR_BUF_SIZE];

  if (self->widget_css_provider == NULL)
    return;

  g_ascii_formatd(opacity, sizeof opacity, "%.2f", self->settings.floating_controls_opacity);
  css = g_strdup_printf(
    ".overlay-pill, bottom-sheet > sheet.background {"
    " color: white;"
    " background: alpha(black, %s);"
    " background-color: alpha(black, %s);"
    " border: 1px solid alpha(white, 0.12);"
    " border-radius: 14px;"
    " backdrop-filter: %s;"
    " box-shadow: 0 10px 32px alpha(black, 0.18);"
    " background-image: none;"
    "}"
    ".app-chrome-group { border-color: alpha(white, 0.16); }",
    opacity,
    opacity,
    blur);
  gtk_css_provider_load_from_string(self->widget_css_provider, css);
}

static void
waytator_window_update_screenshot_folder_row(WaytatorWindow *self,
                                             AdwActionRow   *row)
{
  GtkWidget *clear_button = g_object_get_data(G_OBJECT(row), "clear-button");
  const gboolean is_set = *self->settings.screenshot_save_dir != '\0';
  g_autofree char *folder = is_set ? waytator_screenshot_display_path(self->settings.screenshot_save_dir) : NULL;

  adw_action_row_set_subtitle(row, is_set ? folder : "Not set, saving asks where to put each screenshot");
  gtk_widget_set_visible(clear_button, is_set);
}

static void
waytator_window_screenshot_folder_chosen(GObject      *source_object,
                                         GAsyncResult *result,
                                         gpointer      user_data)
{
  g_autoptr(AdwActionRow) row = ADW_ACTION_ROW(user_data);
  WaytatorWindow *self = WAYTATOR_WINDOW(g_object_get_data(G_OBJECT(row), "window"));
  g_autoptr(GFile) folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source_object), result, NULL);
  g_autofree char *path = folder != NULL ? g_file_get_path(folder) : NULL;

  if (path == NULL)
    return;

  g_free(self->settings.screenshot_save_dir);
  self->settings.screenshot_save_dir = g_steal_pointer(&path);
  waytator_window_save_preferences(self);
  waytator_window_update_screenshot_folder_row(self, row);
}

static void
waytator_window_screenshot_folder_choose_clicked(GtkButton    *button,
                                                 AdwActionRow *row)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(g_object_get_data(G_OBJECT(row), "window"));
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();

  (void) button;

  gtk_file_dialog_set_title(dialog, "Screenshot folder");
  if (*self->settings.screenshot_save_dir != '\0') {
    g_autoptr(GFile) folder = g_file_new_for_path(self->settings.screenshot_save_dir);

    gtk_file_dialog_set_initial_folder(dialog, folder);
  }

  gtk_file_dialog_select_folder(dialog,
                                GTK_WINDOW(self),
                                NULL,
                                waytator_window_screenshot_folder_chosen,
                                g_object_ref(row));
}

static void
waytator_window_screenshot_folder_clear_clicked(GtkButton    *button,
                                                AdwActionRow *row)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(g_object_get_data(G_OBJECT(row), "window"));

  (void) button;

  g_free(self->settings.screenshot_save_dir);
  self->settings.screenshot_save_dir = g_strdup("");
  waytator_window_save_preferences(self);
  waytator_window_update_screenshot_folder_row(self, row);
}

static void
waytator_window_screenshot_copy_changed(AdwSwitchRow   *row,
                                        GParamSpec     *pspec,
                                        WaytatorWindow *self)
{
  (void) pspec;

  self->settings.screenshot_copy = adw_switch_row_get_active(row);
  waytator_window_save_preferences(self);
}

static void
waytator_window_screenshot_open_editor_changed(AdwSwitchRow   *row,
                                               GParamSpec     *pspec,
                                               WaytatorWindow *self)
{
  (void) pspec;

  self->settings.screenshot_open_editor = adw_switch_row_get_active(row);
  waytator_window_save_preferences(self);
}

static void
waytator_window_capture_hide_changed(AdwSwitchRow   *row,
                                     GParamSpec     *pspec,
                                     WaytatorWindow *self)
{
  (void) pspec;

  self->settings.capture_hide_waytator = adw_switch_row_get_active(row);
  waytator_window_save_preferences(self);
}

//the speeds the pointer can be slowed to while shift is held, as offered in the preferences
static const double waytator_window_brake_speeds[] = { 0.25, 0.5, 0.75 };

static void
waytator_window_capture_brake_speed_changed(AdwComboRow    *row,
                                            GParamSpec     *pspec,
                                            WaytatorWindow *self)
{
  const guint selected = adw_combo_row_get_selected(row);

  (void) pspec;

  if (selected >= G_N_ELEMENTS(waytator_window_brake_speeds))
    return;

  self->settings.capture_brake_speed = waytator_window_brake_speeds[selected];
  waytator_window_save_preferences(self);
}

static void
waytator_window_update_recording_folder_row(WaytatorWindow *self,
                                            AdwActionRow   *row)
{
  g_autofree char *folder = waytator_screenshot_display_path(self->settings.recording_save_dir);

  adw_action_row_set_subtitle(row, folder);
}

static void
waytator_window_recording_folder_chosen(GObject      *source_object,
                                        GAsyncResult *result,
                                        gpointer      user_data)
{
  g_autoptr(AdwActionRow) row = ADW_ACTION_ROW(user_data);
  WaytatorWindow *self = WAYTATOR_WINDOW(g_object_get_data(G_OBJECT(row), "window"));
  g_autoptr(GFile) folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source_object), result, NULL);
  g_autofree char *path = folder != NULL ? g_file_get_path(folder) : NULL;

  if (path == NULL)
    return;

  g_free(self->settings.recording_save_dir);
  self->settings.recording_save_dir = g_steal_pointer(&path);
  waytator_window_save_preferences(self);
  waytator_window_update_recording_folder_row(self, row);
}

static void
waytator_window_recording_folder_choose_clicked(GtkButton    *button,
                                                AdwActionRow *row)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(g_object_get_data(G_OBJECT(row), "window"));
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
  g_autoptr(GFile) folder = g_file_new_for_path(self->settings.recording_save_dir);

  (void) button;

  gtk_file_dialog_set_title(dialog, "Recording folder");
  gtk_file_dialog_set_initial_folder(dialog, folder);
  gtk_file_dialog_select_folder(dialog,
                                GTK_WINDOW(self),
                                NULL,
                                waytator_window_recording_folder_chosen,
                                g_object_ref(row));
}

static void
waytator_window_recording_copy_changed(AdwSwitchRow   *row,
                                       GParamSpec     *pspec,
                                       WaytatorWindow *self)
{
  (void) pspec;

  self->settings.recording_copy = adw_switch_row_get_active(row);
  waytator_window_save_preferences(self);
}

static void
waytator_window_recording_open_editor_changed(AdwSwitchRow   *row,
                                              GParamSpec     *pspec,
                                              WaytatorWindow *self)
{
  (void) pspec;

  self->settings.recording_open_editor = adw_switch_row_get_active(row);
  waytator_window_save_preferences(self);
}

static void
waytator_window_recording_microphone_changed(AdwComboRow    *row,
                                             GParamSpec     *pspec,
                                             WaytatorWindow *self)
{
  char **names = g_object_get_data(G_OBJECT(row), "microphones");
  const guint selected = adw_combo_row_get_selected(row);

  (void) pspec;

  if (selected >= g_strv_length(names))
    return;

  g_free(self->settings.recording_microphone);
  self->settings.recording_microphone = g_strdup(names[selected]);
  waytator_window_save_preferences(self);
}

//the microphones are asked for when the preferences open, they come and go. the row remembers the node name
//behind each entry, the first being the default one
static void
waytator_window_fill_microphone_row(WaytatorWindow *self,
                                    AdwComboRow    *row)
{
  g_autoptr(GtkStringList) model = gtk_string_list_new(NULL);
  g_autoptr(GStrvBuilder) names = g_strv_builder_new();
  g_autoptr(GPtrArray) microphones = waytator_record_audio_list_microphones(NULL);
  const char *saved = self->settings.recording_microphone;
  guint selected = 0;

  gtk_string_list_append(model, "Default");
  g_strv_builder_add(names, "");

  //without PipeWire there is nothing to choose from, and what was chosen stays until something else is
  for (guint i = 0; microphones != NULL && i < microphones->len; i++) {
    const WaytatorRecordMicrophone *microphone = g_ptr_array_index(microphones, i);

    if (g_strcmp0(microphone->name, saved) == 0)
      selected = i + 1;
    gtk_string_list_append(model, microphone->description);
    g_strv_builder_add(names, microphone->name);
  }

  //one that was unplugged stays in the list, so choosing something else is a choice and not a loss
  if (microphones != NULL && *saved != '\0' && selected == 0) {
    g_autofree char *label = g_strdup_printf("%s (not connected)", saved);

    selected = g_list_model_get_n_items(G_LIST_MODEL(model));
    gtk_string_list_append(model, label);
    g_strv_builder_add(names, saved);
  }

  g_object_set_data_full(G_OBJECT(row), "microphones", g_strv_builder_end(names), (GDestroyNotify) g_strfreev);
  adw_combo_row_set_model(row, G_LIST_MODEL(model));
  adw_combo_row_set_selected(row, selected);
}

static void
waytator_window_recording_show_bar_changed(AdwSwitchRow   *row,
                                           GParamSpec     *pspec,
                                           WaytatorWindow *self)
{
  (void) pspec;

  self->settings.recording_show_bar = adw_switch_row_get_active(row);
  waytator_window_save_preferences(self);
}

//the rates a recording can have, as offered in the preferences
static const int waytator_window_recording_rates[] = { 60, 30 };

static void
waytator_window_recording_rate_changed(AdwComboRow    *row,
                                       GParamSpec     *pspec,
                                       WaytatorWindow *self)
{
  const guint selected = adw_combo_row_get_selected(row);

  (void) pspec;

  if (selected >= G_N_ELEMENTS(waytator_window_recording_rates))
    return;

  self->settings.recording_rate = waytator_window_recording_rates[selected];
  waytator_window_save_preferences(self);
}

static void
waytator_window_recording_show_pointer_changed(AdwSwitchRow   *row,
                                               GParamSpec     *pspec,
                                               WaytatorWindow *self)
{
  (void) pspec;

  self->settings.recording_show_pointer = adw_switch_row_get_active(row);
  waytator_window_save_preferences(self);
}

static AdwPreferencesGroup *
waytator_window_new_recordings_group(WaytatorWindow *self)
{
  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  AdwActionRow *folder_row = ADW_ACTION_ROW(adw_action_row_new());
  AdwSwitchRow *copy_row = ADW_SWITCH_ROW(adw_switch_row_new());
  AdwSwitchRow *editor_row = ADW_SWITCH_ROW(adw_switch_row_new());
  AdwComboRow *microphone_row = ADW_COMBO_ROW(adw_combo_row_new());
  AdwSwitchRow *bar_row = ADW_SWITCH_ROW(adw_switch_row_new());
  AdwComboRow *rate_row = ADW_COMBO_ROW(adw_combo_row_new());
  AdwSwitchRow *pointer_row = ADW_SWITCH_ROW(adw_switch_row_new());
  g_autoptr(GtkStringList) rate_model = gtk_string_list_new((const char *[]) {
    "60 fps",
    "30 fps",
    NULL,
  });
  GtkWidget *choose_button = gtk_button_new_from_icon_name("folder-open-symbolic");

  adw_preferences_group_set_title(group, "Recordings");
  adw_preferences_group_set_description(group, "These settings apply to recordings made by waytator");

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(folder_row), "Folder");
  gtk_widget_set_valign(choose_button, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(choose_button, "Choose a folder");
  adw_action_row_add_suffix(folder_row, choose_button);
  g_object_set_data(G_OBJECT(folder_row), "window", self);
  waytator_window_update_recording_folder_row(self, folder_row);
  g_signal_connect(choose_button, "clicked", G_CALLBACK(waytator_window_recording_folder_choose_clicked), folder_row);

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(copy_row), "Copy to clipboard");
  adw_switch_row_set_active(copy_row, self->settings.recording_copy);
  g_signal_connect(copy_row, "notify::active", G_CALLBACK(waytator_window_recording_copy_changed), self);

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(editor_row), "Open in the editor");
  adw_switch_row_set_active(editor_row, self->settings.recording_open_editor);
  g_signal_connect(editor_row, "notify::active", G_CALLBACK(waytator_window_recording_open_editor_changed), self);

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(microphone_row), "Microphone");
  waytator_window_fill_microphone_row(self, microphone_row);
  g_signal_connect(microphone_row, "notify::selected", G_CALLBACK(waytator_window_recording_microphone_changed), self);

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(bar_row), "Show the recording bar");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(bar_row), "Start and stop with waytator --record when it's off");
  adw_switch_row_set_active(bar_row, self->settings.recording_show_bar);
  g_signal_connect(bar_row, "notify::active", G_CALLBACK(waytator_window_recording_show_bar_changed), self);

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(rate_row), "Frame rate");
  adw_combo_row_set_model(rate_row, G_LIST_MODEL(rate_model));
  adw_combo_row_set_selected(rate_row, self->settings.recording_rate == 30 ? 1 : 0);
  g_signal_connect(rate_row, "notify::selected", G_CALLBACK(waytator_window_recording_rate_changed), self);

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(pointer_row), "Show the pointer");
  adw_switch_row_set_active(pointer_row, self->settings.recording_show_pointer);
  g_signal_connect(pointer_row, "notify::active", G_CALLBACK(waytator_window_recording_show_pointer_changed), self);

  adw_preferences_group_add(group, GTK_WIDGET(folder_row));
  adw_preferences_group_add(group, GTK_WIDGET(copy_row));
  adw_preferences_group_add(group, GTK_WIDGET(editor_row));
  adw_preferences_group_add(group, GTK_WIDGET(microphone_row));
  adw_preferences_group_add(group, GTK_WIDGET(bar_row));
  adw_preferences_group_add(group, GTK_WIDGET(rate_row));
  adw_preferences_group_add(group, GTK_WIDGET(pointer_row));
  return group;
}

static AdwPreferencesPage *
waytator_window_new_recordings_page(WaytatorWindow *self)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());

  adw_preferences_page_set_title(page, "Recordings");
  adw_preferences_page_set_icon_name(page, "camera-video-symbolic");
  adw_preferences_page_add(page, waytator_window_new_recordings_group(self));
  return page;
}

static AdwPreferencesPage *
waytator_window_new_screenshots_page(WaytatorWindow *self)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  AdwActionRow *folder_row = ADW_ACTION_ROW(adw_action_row_new());
  AdwSwitchRow *copy_row = ADW_SWITCH_ROW(adw_switch_row_new());
  AdwSwitchRow *editor_row = ADW_SWITCH_ROW(adw_switch_row_new());
  AdwSwitchRow *hide_row = ADW_SWITCH_ROW(adw_switch_row_new());
  AdwComboRow *brake_row = ADW_COMBO_ROW(adw_combo_row_new());
  g_autoptr(GtkStringList) brake_model = gtk_string_list_new((const char *[]) { "25%", "50%", "75%", NULL });
  guint brake_selected = 0;
  GtkWidget *choose_button = gtk_button_new_from_icon_name("folder-open-symbolic");
  GtkWidget *clear_button = gtk_button_new_from_icon_name("window-close-symbolic");

  adw_preferences_page_set_title(page, "Screenshots");
  adw_preferences_page_set_icon_name(page, "camera-photo-symbolic");
  adw_preferences_group_set_title(group, "Screenshots");
  //GNOME's screenshot UI saves and copies on its own, so the folder and the copy are not waytator's to decide there
  if (waytator_capture_uses_gnome_ui()) {
    adw_preferences_group_set_description(group,
                                          "On GNOME, screenshots are taken with GNOME's screenshot tool, "
                                          "which saves and copies them itself");
    gtk_widget_set_sensitive(GTK_WIDGET(folder_row), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(copy_row), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(brake_row), FALSE);
  } else {
    adw_preferences_group_set_description(group,
                                          "These settings apply to screenshots taken by waytator");
  }

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(folder_row), "Save folder");
  gtk_widget_set_valign(choose_button, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(choose_button, "Choose a folder");
  gtk_button_set_has_frame(GTK_BUTTON(clear_button), FALSE);
  gtk_widget_set_valign(clear_button, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(clear_button, "Stop saving screenshots");
  adw_action_row_add_suffix(folder_row, choose_button);
  adw_action_row_add_suffix(folder_row, clear_button);
  g_object_set_data(G_OBJECT(folder_row), "window", self);
  g_object_set_data(G_OBJECT(folder_row), "clear-button", clear_button);
  waytator_window_update_screenshot_folder_row(self, folder_row);
  g_signal_connect(choose_button, "clicked", G_CALLBACK(waytator_window_screenshot_folder_choose_clicked), folder_row);
  g_signal_connect(clear_button, "clicked", G_CALLBACK(waytator_window_screenshot_folder_clear_clicked), folder_row);

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(copy_row), "Copy to clipboard");
  adw_switch_row_set_active(copy_row, self->settings.screenshot_copy);
  g_signal_connect(copy_row, "notify::active", G_CALLBACK(waytator_window_screenshot_copy_changed), self);

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(editor_row), "Open in the editor");
  adw_switch_row_set_active(editor_row, self->settings.screenshot_open_editor);
  g_signal_connect(editor_row, "notify::active", G_CALLBACK(waytator_window_screenshot_open_editor_changed), self);

  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(hide_row), "Hide waytator");
  adw_switch_row_set_active(hide_row, self->settings.capture_hide_waytator);
  g_signal_connect(hide_row, "notify::active", G_CALLBACK(waytator_window_capture_hide_changed), self);

  //a speed set by hand in the file shows as the nearest one offered
  for (guint i = 1; i < G_N_ELEMENTS(waytator_window_brake_speeds); i++)
    if (fabs(waytator_window_brake_speeds[i] - self->settings.capture_brake_speed)
        < fabs(waytator_window_brake_speeds[brake_selected] - self->settings.capture_brake_speed))
      brake_selected = i;
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(brake_row), "Pointer speed with Shift");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(brake_row), "Holding Shift slows the pointer while selecting an area");
  adw_combo_row_set_model(brake_row, G_LIST_MODEL(brake_model));
  adw_combo_row_set_selected(brake_row, brake_selected);
  g_signal_connect(brake_row, "notify::selected", G_CALLBACK(waytator_window_capture_brake_speed_changed), self);

  adw_preferences_group_add(group, GTK_WIDGET(folder_row));
  adw_preferences_group_add(group, GTK_WIDGET(copy_row));
  adw_preferences_group_add(group, GTK_WIDGET(editor_row));
  adw_preferences_group_add(group, GTK_WIDGET(hide_row));
  adw_preferences_group_add(group, GTK_WIDGET(brake_row));
  adw_preferences_page_add(page, group);
  return page;
}

static void
waytator_window_show_preferences(WaytatorWindow *self)
{
  AdwPreferencesDialog *dialog;
  AdwPreferencesPage *page;
  AdwPreferencesPage *shortcuts_page;
  AdwPreferencesGroup *group;
  AdwPreferencesGroup *appearance_group;
  AdwPreferencesGroup *shortcuts_group;
  AdwPreferencesGroup *tool_shortcuts_group;
  AdwPreferencesGroup *color_shortcuts_group;
  AdwComboRow *row;
  AdwComboRow *background_mode_row;
  AdwSwitchRow *floating_controls_blur_row;
  AdwSwitchRow *auto_copy_latest_change_row;
  AdwSwitchRow *close_on_copy_row;
  AdwSwitchRow *highlighter_overlap_row;
  AdwComboRow *angle_snap_modifier_row;
  AdwActionRow *opacity_row;
  AdwActionRow *floating_controls_opacity_row;
  AdwActionRow *close_window_shortcut_row;
  AdwActionRow *copy_shortcut_row;
  AdwActionRow *copy_keep_open_shortcut_row;
  AdwActionRow *save_shortcut_row;
  AdwActionRow *save_close_shortcut_row;
  AdwActionRow *save_copy_close_shortcut_row;
  AdwActionRow *capture_area_shortcut_row;
  AdwActionRow *capture_screen_shortcut_row;
  AdwActionRow *default_primary_color_row;
  AdwActionRow *default_highlighter_color_row;
  AdwActionRow *default_fill_color_row;
  GtkStringList *model;
  GtkStringList *background_model;
  GtkStringList *angle_snap_model;
  GtkAdjustment *opacity_adjustment;
  GtkSpinButton *opacity_spin_button;
  GtkAdjustment *floating_controls_opacity_adjustment;
  GtkSpinButton *floating_controls_opacity_spin_button;
  GtkWidget *default_primary_color_button;
  GtkWidget *default_highlighter_color_button;
  GtkWidget *default_fill_color_button;
  GtkColorDialog *default_primary_color_dialog;
  GtkColorDialog *default_highlighter_color_dialog;
  GtkColorDialog *default_fill_color_dialog;

  dialog = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
  page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
  group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  appearance_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  shortcuts_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
  shortcuts_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  tool_shortcuts_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  color_shortcuts_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  row = ADW_COMBO_ROW(adw_combo_row_new());
  background_mode_row = ADW_COMBO_ROW(adw_combo_row_new());
  floating_controls_blur_row = ADW_SWITCH_ROW(adw_switch_row_new());
  auto_copy_latest_change_row = ADW_SWITCH_ROW(adw_switch_row_new());
  close_on_copy_row = ADW_SWITCH_ROW(adw_switch_row_new());
  highlighter_overlap_row = ADW_SWITCH_ROW(adw_switch_row_new());
  angle_snap_modifier_row = ADW_COMBO_ROW(adw_combo_row_new());
  opacity_row = ADW_ACTION_ROW(adw_action_row_new());
  floating_controls_opacity_row = ADW_ACTION_ROW(adw_action_row_new());
  close_window_shortcut_row = waytator_window_new_shortcut_row(self, WAYTATOR_SHORTCUT_CLOSE_WINDOW);
  copy_shortcut_row = waytator_window_new_shortcut_row(self, WAYTATOR_SHORTCUT_COPY);
  copy_keep_open_shortcut_row = waytator_window_new_shortcut_row(self, WAYTATOR_SHORTCUT_COPY_KEEP_OPEN);
  save_shortcut_row = waytator_window_new_shortcut_row(self, WAYTATOR_SHORTCUT_SAVE);
  save_close_shortcut_row = waytator_window_new_shortcut_row(self, WAYTATOR_SHORTCUT_SAVE_CLOSE);
  save_copy_close_shortcut_row = waytator_window_new_shortcut_row(self, WAYTATOR_SHORTCUT_SAVE_COPY_CLOSE);
  capture_area_shortcut_row = waytator_window_new_shortcut_row(self, WAYTATOR_SHORTCUT_CAPTURE_AREA);
  capture_screen_shortcut_row = waytator_window_new_shortcut_row(self, WAYTATOR_SHORTCUT_CAPTURE_SCREEN);
  default_primary_color_row = ADW_ACTION_ROW(adw_action_row_new());
  default_highlighter_color_row = ADW_ACTION_ROW(adw_action_row_new());
  default_fill_color_row = ADW_ACTION_ROW(adw_action_row_new());
  opacity_adjustment = gtk_adjustment_new(self->window_background_opacity, 0.1, 1.0, 0.1, 0.1, 0.0);
  opacity_spin_button = GTK_SPIN_BUTTON(gtk_spin_button_new(opacity_adjustment, 0.1, 1));
  floating_controls_opacity_adjustment = gtk_adjustment_new(self->settings.floating_controls_opacity, 0.0, 1.0, 0.1, 0.1, 0.0);
  floating_controls_opacity_spin_button = GTK_SPIN_BUTTON(gtk_spin_button_new(floating_controls_opacity_adjustment, 0.1, 1));
  default_primary_color_dialog = gtk_color_dialog_new();
  default_highlighter_color_dialog = gtk_color_dialog_new();
  default_fill_color_dialog = gtk_color_dialog_new();
  default_primary_color_button = gtk_color_dialog_button_new(default_primary_color_dialog);
  default_highlighter_color_button = gtk_color_dialog_button_new(default_highlighter_color_dialog);
  default_fill_color_button = gtk_color_dialog_button_new(default_fill_color_dialog);
  model = gtk_string_list_new((const char *[]) {
    waytator_window_eraser_style_label(WAYTATOR_ERASER_STYLE_DUAL_RING),
    waytator_window_eraser_style_label(WAYTATOR_ERASER_STYLE_DASHED_RING),
    waytator_window_eraser_style_label(WAYTATOR_ERASER_STYLE_PATTERN),
    NULL,
  });
  //yes this is really how you have to do dropdowns in gtk
  background_model = gtk_string_list_new((const char *[]) {
    waytator_window_background_mode_label(WAYTATOR_WINDOW_BACKGROUND_FOLLOW_SYSTEM),
    waytator_window_background_mode_label(WAYTATOR_WINDOW_BACKGROUND_OPAQUE),
    waytator_window_background_mode_label(WAYTATOR_WINDOW_BACKGROUND_TRANSPARENT),
    NULL,
  });
  angle_snap_model = gtk_string_list_new((const char *[]) {
    waytator_window_angle_snap_modifier_label(0),
    waytator_window_angle_snap_modifier_label(GDK_SHIFT_MASK),
    waytator_window_angle_snap_modifier_label(GDK_CONTROL_MASK),
    waytator_window_angle_snap_modifier_label(GDK_ALT_MASK),
    waytator_window_angle_snap_modifier_label(GDK_SUPER_MASK),
    NULL,
  });

  adw_preferences_page_set_title(page, "General");
  adw_preferences_page_set_icon_name(page, "preferences-system-symbolic");
  adw_preferences_page_set_title(shortcuts_page, "Shortcuts");
  adw_preferences_page_set_icon_name(shortcuts_page, "preferences-desktop-keyboard-shortcuts-symbolic");
  adw_preferences_group_set_title(group, "General");
  adw_preferences_group_set_title(appearance_group, "Appearance");
  adw_preferences_group_set_title(shortcuts_group, "Actions");
  adw_preferences_group_set_title(tool_shortcuts_group, "Tools");
  adw_preferences_group_set_title(color_shortcuts_group, "Stroke color");
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Eraser Styling");
  adw_combo_row_set_model(row, G_LIST_MODEL(model));
  adw_combo_row_set_selected(row, self->eraser_style);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(background_mode_row), "Window background");
  adw_combo_row_set_model(background_mode_row, G_LIST_MODEL(background_model));
  adw_combo_row_set_selected(background_mode_row, self->window_background_mode);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(opacity_row), "Window background opacity");
  gtk_spin_button_set_numeric(opacity_spin_button, TRUE);
  gtk_widget_set_valign(GTK_WIDGET(opacity_spin_button), GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(GTK_WIDGET(opacity_spin_button), 88, -1);
  adw_action_row_add_suffix(opacity_row, GTK_WIDGET(opacity_spin_button));
  adw_action_row_set_activatable_widget(opacity_row, GTK_WIDGET(opacity_spin_button));
  gtk_widget_set_sensitive(GTK_WIDGET(opacity_row),
                           self->window_background_mode == WAYTATOR_WINDOW_BACKGROUND_TRANSPARENT);
  g_object_set_data(G_OBJECT(background_mode_row), "opacity-row", opacity_row);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(floating_controls_blur_row), "Blur the background of controls");
  adw_switch_row_set_active(floating_controls_blur_row, self->settings.floating_controls_blur);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(floating_controls_opacity_row), "Controls background opacity");
  gtk_spin_button_set_numeric(floating_controls_opacity_spin_button, TRUE);
  gtk_widget_set_valign(GTK_WIDGET(floating_controls_opacity_spin_button), GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(GTK_WIDGET(floating_controls_opacity_spin_button), 88, -1);
  adw_action_row_add_suffix(floating_controls_opacity_row, GTK_WIDGET(floating_controls_opacity_spin_button));
  adw_action_row_set_activatable_widget(floating_controls_opacity_row,
                                        GTK_WIDGET(floating_controls_opacity_spin_button));
  gtk_widget_set_visible(GTK_WIDGET(copy_keep_open_shortcut_row),
                         self->close_on_copy || waytator_window_closes_on_copy(self));
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(angle_snap_modifier_row), "Snap modifier");
  adw_combo_row_set_model(angle_snap_modifier_row, G_LIST_MODEL(angle_snap_model));
  switch (self->angle_snap_modifiers) {
  case 0:
    adw_combo_row_set_selected(angle_snap_modifier_row, 0);
    break;
  case GDK_CONTROL_MASK:
    adw_combo_row_set_selected(angle_snap_modifier_row, 2);
    break;
  case GDK_ALT_MASK:
    adw_combo_row_set_selected(angle_snap_modifier_row, 3);
    break;
  case GDK_SUPER_MASK:
    adw_combo_row_set_selected(angle_snap_modifier_row, 4);
    break;
  case GDK_SHIFT_MASK:
  default:
    adw_combo_row_set_selected(angle_snap_modifier_row, 1);
    break;
  }
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(auto_copy_latest_change_row), "Auto-copy latest change");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(auto_copy_latest_change_row),
                              "Auto-copies never close the app; only explicit copies do");
  adw_switch_row_set_active(auto_copy_latest_change_row, self->auto_copy_latest_change);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(close_on_copy_row), "Close on copy");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(close_on_copy_row),
                              "Copying closes the app and adds a “Copy without closing” button");
  adw_switch_row_set_active(close_on_copy_row, self->close_on_copy);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(highlighter_overlap_row), "Allow highlighter strokes to overlap");
  adw_switch_row_set_active(highlighter_overlap_row, self->allow_highlighter_overlap);
  gtk_color_dialog_set_with_alpha(default_fill_color_dialog, TRUE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(default_primary_color_row), "Primary");
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(default_highlighter_color_row), "Highlighter");
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(default_fill_color_row), "Fill");
  adw_action_row_set_subtitle(default_primary_color_row, "Used by most drawing tools");
  adw_action_row_set_subtitle(default_highlighter_color_row, "Used by the highlighter tool");
  adw_action_row_set_subtitle(default_fill_color_row, "Used by tools with a fill option");
  gtk_widget_set_valign(default_primary_color_button, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(default_highlighter_color_button, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(default_fill_color_button, GTK_ALIGN_CENTER);
  gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(default_primary_color_button), &self->default_primary_color);
  gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(default_highlighter_color_button), &self->default_highlighter_color);
  gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(default_fill_color_button), &self->default_fill_color);
  adw_action_row_add_suffix(default_primary_color_row, default_primary_color_button);
  adw_action_row_add_suffix(default_highlighter_color_row, default_highlighter_color_button);
  adw_action_row_add_suffix(default_fill_color_row, default_fill_color_button);
  adw_action_row_set_activatable_widget(default_primary_color_row, default_primary_color_button);
  adw_action_row_set_activatable_widget(default_highlighter_color_row, default_highlighter_color_button);
  adw_action_row_set_activatable_widget(default_fill_color_row, default_fill_color_button);

  adw_preferences_group_add(group, GTK_WIDGET(row));
  adw_preferences_group_add(group, GTK_WIDGET(highlighter_overlap_row));
  adw_preferences_group_add(group, GTK_WIDGET(auto_copy_latest_change_row));
  adw_preferences_group_add(group, GTK_WIDGET(close_on_copy_row));
  adw_preferences_group_add(group, GTK_WIDGET(default_primary_color_row));
  adw_preferences_group_add(group, GTK_WIDGET(default_highlighter_color_row));
  adw_preferences_group_add(group, GTK_WIDGET(default_fill_color_row));
  adw_preferences_group_add(appearance_group, GTK_WIDGET(background_mode_row));
  adw_preferences_group_add(appearance_group, GTK_WIDGET(opacity_row));
  adw_preferences_group_add(appearance_group, GTK_WIDGET(floating_controls_blur_row));
  adw_preferences_group_add(appearance_group, GTK_WIDGET(floating_controls_opacity_row));
  adw_preferences_group_add(shortcuts_group, GTK_WIDGET(close_window_shortcut_row));
  adw_preferences_group_add(shortcuts_group, GTK_WIDGET(copy_shortcut_row));
  adw_preferences_group_add(shortcuts_group, GTK_WIDGET(copy_keep_open_shortcut_row));
  adw_preferences_group_add(shortcuts_group, GTK_WIDGET(save_shortcut_row));
  adw_preferences_group_add(shortcuts_group, GTK_WIDGET(save_close_shortcut_row));
  adw_preferences_group_add(shortcuts_group, GTK_WIDGET(save_copy_close_shortcut_row));
  adw_preferences_group_add(shortcuts_group, GTK_WIDGET(capture_area_shortcut_row));
  adw_preferences_group_add(shortcuts_group, GTK_WIDGET(capture_screen_shortcut_row));
  adw_preferences_group_add(shortcuts_group, GTK_WIDGET(angle_snap_modifier_row));
  adw_preferences_page_add(page, group);
  adw_preferences_page_add(page, appearance_group);
  for (int i = WAYTATOR_SHORTCUT_TOOL_FIRST; i <= WAYTATOR_SHORTCUT_SIZE_INCREASE; i++)
    adw_preferences_group_add(tool_shortcuts_group,
                              GTK_WIDGET(waytator_window_new_shortcut_row(self, i)));
  waytator_window_setup_palette_group(self, color_shortcuts_group);
  adw_preferences_page_add(shortcuts_page, shortcuts_group);
  adw_preferences_page_add(shortcuts_page, tool_shortcuts_group);
  adw_preferences_page_add(shortcuts_page, color_shortcuts_group);
  waytator_window_update_shortcut_conflicts(self);
  adw_preferences_dialog_add(dialog, page);
  adw_preferences_dialog_add(dialog, waytator_window_new_screenshots_page(self));
  adw_preferences_dialog_add(dialog, waytator_window_new_recordings_page(self));
  adw_preferences_dialog_add(dialog, waytator_window_new_videos_page(self));
  adw_preferences_dialog_add(dialog, shortcuts_page);
  adw_dialog_set_title(ADW_DIALOG(dialog), "Preferences");
  adw_preferences_dialog_set_search_enabled(dialog, FALSE);
  adw_dialog_set_content_width(ADW_DIALOG(dialog), 420);

  g_signal_connect(row, "notify::selected", G_CALLBACK(waytator_window_eraser_style_changed), self);
  g_signal_connect(background_mode_row,
                   "notify::selected",
                   G_CALLBACK(waytator_window_background_mode_changed),
                   self);
  g_signal_connect(opacity_spin_button,
                   "value-changed",
                   G_CALLBACK(waytator_window_transparency_opacity_changed),
                   self);
  g_signal_connect(floating_controls_blur_row,
                   "notify::active",
                   G_CALLBACK(waytator_window_floating_controls_blur_changed),
                   self);
  g_signal_connect(floating_controls_opacity_spin_button,
                   "value-changed",
                   G_CALLBACK(waytator_window_floating_controls_opacity_changed),
                   self);
  g_signal_connect(angle_snap_modifier_row,
                   "notify::selected",
                   G_CALLBACK(waytator_window_angle_snap_modifier_changed),
                   self);
  g_signal_connect(auto_copy_latest_change_row,
                   "notify::active",
                   G_CALLBACK(waytator_window_auto_copy_latest_change_changed),
                   self);
  g_signal_connect(close_on_copy_row,
                   "notify::active",
                   G_CALLBACK(waytator_window_close_on_copy_changed),
                   self);
  g_signal_connect(highlighter_overlap_row,
                   "notify::active",
                   G_CALLBACK(waytator_window_highlighter_overlap_changed),
                   self);
  g_signal_connect(default_primary_color_button,
                   "notify::rgba",
                   G_CALLBACK(waytator_window_default_primary_color_changed),
                   self);
  g_signal_connect(default_highlighter_color_button,
                   "notify::rgba",
                   G_CALLBACK(waytator_window_default_highlighter_color_changed),
                   self);
  g_signal_connect(default_fill_color_button,
                   "notify::rgba",
                   G_CALLBACK(waytator_window_default_fill_color_changed),
                   self);
  g_object_set_data(G_OBJECT(close_on_copy_row), "shortcut-row", copy_keep_open_shortcut_row);

  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
  g_object_unref(model);
  g_object_unref(background_model);
  g_object_unref(angle_snap_model);
}

static void
waytator_window_show_about(WaytatorWindow *self)
{
  adw_show_about_dialog(GTK_WIDGET(self),
                        "application-name", "Waytator",
                        "application-icon", "waytator",
                        "version", WAYTATOR_VERSION,
                        "developer-name", "faetalize",
                        "developers", (const char *[]) { "faetalize", NULL },
                        "issue-url", "https://github.com/faetalize/waytator/issues",
                        "license-type", GTK_LICENSE_GPL_3_0,
                        "website", "https://github.com/faetalize/waytator",
                        NULL);
}

static void
waytator_window_preferences_action(GtkWidget  *widget,
                                   const char *action_name,
                                   GVariant   *parameter)
{
  (void) action_name;
  (void) parameter;

  waytator_window_show_preferences(WAYTATOR_WINDOW(widget));
}

static void
waytator_window_about_action(GtkWidget  *widget,
                             const char *action_name,
                             GVariant   *parameter)
{
  (void) action_name;
  (void) parameter;

  waytator_window_show_about(WAYTATOR_WINDOW(widget));
}

GPtrArray *
waytator_window_strokes(WaytatorWindow *self)
{
  return waytator_document_get_strokes(self->document);
}

static void
waytator_window_set_text_view_text(GtkTextView *view,
                                   const char  *text)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);

  gtk_text_buffer_set_text(buffer, text != NULL ? text : "", -1);
}

static char *
waytator_window_build_ocr_all_text(GPtrArray *lines)
{
  GString *text;
  guint i;

  if (lines == NULL || lines->len == 0)
    return g_strdup("");

  text = g_string_new(NULL);

  for (i = 0; i < lines->len; i++) {
    WaytatorOcrLine *line = g_ptr_array_index(lines, i);

    if (line->text == NULL || line->text[0] == '\0')
      continue;

    if (text->len > 0)
      g_string_append_c(text, '\n');
    g_string_append(text, line->text);
  }

  return g_string_free(text, FALSE);
}

static void
waytator_window_clear_ocr_widgets(WaytatorWindow *self)
{
  if (self->ocr_lines != NULL) {
    guint i;

    for (i = 0; i < self->ocr_lines->len; i++) {
      WaytatorOcrLine *line = g_ptr_array_index(self->ocr_lines, i);

      line->button = NULL;
    }
  }

  while (gtk_widget_get_first_child(GTK_WIDGET(self->ocr_overlay)) != NULL)
    gtk_fixed_remove(self->ocr_overlay, gtk_widget_get_first_child(GTK_WIDGET(self->ocr_overlay)));
}

static void
waytator_window_select_ocr_line(WaytatorWindow  *self,
                                WaytatorOcrLine *line)
{
  guint i;

  self->selected_ocr_line = line;

  if (self->ocr_lines != NULL) {
    for (i = 0; i < self->ocr_lines->len; i++) {
      WaytatorOcrLine *candidate = g_ptr_array_index(self->ocr_lines, i);

      if (candidate->button == NULL)
        continue;

      if (candidate == line)
        gtk_widget_add_css_class(candidate->button, "selected");
      else
        gtk_widget_remove_css_class(candidate->button, "selected");
    }
  }

  if (line != NULL) {
    gtk_stack_set_visible_child_name(self->ocr_panel_stack, "selected");
    waytator_window_set_ocr_panel_visible(self, TRUE);
  }

  waytator_window_update_ocr_panel(self);
}

static void
waytator_window_set_ocr_panel_visible(WaytatorWindow *self,
                                      gboolean        visible)
{
  if (!gtk_widget_get_visible(self->ocr_panel_toggle_container) && visible)
    return;

  if (gtk_toggle_button_get_active(self->ocr_panel_toggle_button) != visible)
    gtk_toggle_button_set_active(self->ocr_panel_toggle_button, visible);

  if (adw_bottom_sheet_get_open(self->ocr_panel_bottom_sheet) != visible)
    adw_bottom_sheet_set_open(self->ocr_panel_bottom_sheet, visible);
}

static void
waytator_window_clear_ocr_results(WaytatorWindow *self)
{
  self->ocr_running = FALSE;
  self->ocr_generation++;
  self->selected_ocr_line = NULL;
  waytator_window_clear_ocr_widgets(self);
  g_clear_pointer(&self->ocr_lines, g_ptr_array_unref);
  g_clear_pointer(&self->ocr_all_text, g_free);
  gtk_widget_set_visible(GTK_WIDGET(self->ocr_overlay), FALSE);
  gtk_toggle_button_set_active(self->ocr_panel_toggle_button, FALSE);
  waytator_window_update_ocr_panel(self);
}

static gboolean
waytator_window_ocr_is_visible(WaytatorWindow *self)
{
  return self->active_tool == WAYTATOR_TOOL_OCR
      && self->texture != NULL
      && self->ocr_lines != NULL
      && self->ocr_lines->len > 0;
}

static void
waytator_window_ocr_box_clicked(GtkButton *button,
                                gpointer   user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  guint i;

  if (self->ocr_lines == NULL)
    return;

  for (i = 0; i < self->ocr_lines->len; i++) {
    WaytatorOcrLine *line = g_ptr_array_index(self->ocr_lines, i);

    if (line->button == GTK_WIDGET(button)) {
      waytator_window_select_ocr_line(self, line);
      return;
    }
  }
}

static void
waytator_window_add_ocr_button(WaytatorWindow  *self,
                               WaytatorOcrLine *line)
{
  GtkWidget *button = gtk_button_new();

  gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
  gtk_widget_set_focusable(button, FALSE);
  gtk_widget_add_css_class(button, "ocr-overlay-box");
  gtk_widget_set_tooltip_text(button, line->text);
  g_signal_connect(button, "clicked", G_CALLBACK(waytator_window_ocr_box_clicked), self);

  gtk_fixed_put(self->ocr_overlay, button, 0.0, 0.0);
  line->button = button;
}

void
waytator_window_update_ocr_overlay(WaytatorWindow *self)
{
  const int image_width = self->texture != NULL
                        ? gdk_paintable_get_intrinsic_width(GDK_PAINTABLE(self->texture))
                        : 0;
  const int image_height = self->texture != NULL
                         ? gdk_paintable_get_intrinsic_height(GDK_PAINTABLE(self->texture))
                         : 0;
  const double widget_width = gtk_widget_get_width(GTK_WIDGET(self->drawing_area));
  const double widget_height = gtk_widget_get_height(GTK_WIDGET(self->drawing_area));
  guint i;

  if (!waytator_window_ocr_is_visible(self)
      || widget_width <= 0
      || widget_height <= 0
      || image_width <= 0
      || image_height <= 0) {
    gtk_widget_set_visible(GTK_WIDGET(self->ocr_overlay), FALSE);
    return;
  }

  gtk_widget_set_visible(GTK_WIDGET(self->ocr_overlay), TRUE);

  //the canvas view lays the image out to fill the drawing area exactly
  for (i = 0; i < self->ocr_lines->len; i++) {
    WaytatorOcrLine *line = g_ptr_array_index(self->ocr_lines, i);
    const double scale_x = widget_width / image_width;
    const double scale_y = widget_height / image_height;
    const int x = (int) floor(line->left * scale_x);
    const int y = (int) floor(line->top * scale_y);
    const int width = MAX(18, (int) ceil(line->width * scale_x));
    const int height = MAX(18, (int) ceil(line->height * scale_y));

    if (line->button == NULL)
      waytator_window_add_ocr_button(self, line);

    gtk_widget_set_size_request(line->button, width, height);
    gtk_fixed_move(self->ocr_overlay, line->button, x, y);
  }

  waytator_window_select_ocr_line(self, self->selected_ocr_line);
}

void
waytator_window_update_ocr_panel(WaytatorWindow *self)
{
  const gboolean has_results = self->ocr_lines != NULL && self->ocr_lines->len > 0;
  const gboolean can_show_toggle = self->active_tool == WAYTATOR_TOOL_OCR
                                 && self->texture != NULL
                                 && (self->ocr_running || has_results);
  const gboolean show_panel = can_show_toggle
                           && gtk_toggle_button_get_active(self->ocr_panel_toggle_button);
  const char *selected_text = NULL;
  const char *all_text = NULL;

  if (gtk_widget_get_visible(self->ocr_panel_toggle_container) != can_show_toggle) {
    gtk_widget_set_visible(self->ocr_panel_toggle_container, can_show_toggle);
    //the options above the tools get wider with it
    waytator_window_queue_chrome_layout(self);
  }

  if (self->ocr_running && gtk_toggle_button_get_active(self->ocr_panel_toggle_button)) {
    selected_text = "Recognizing text...";
    all_text = "Recognizing text...";
  } else {
    selected_text = self->selected_ocr_line != NULL
                  ? self->selected_ocr_line->text
                  : "Click a highlighted region to inspect its recognized text.";
    all_text = has_results && self->ocr_all_text != NULL && self->ocr_all_text[0] != '\0'
             ? self->ocr_all_text
             : "No OCR text available.";
  }

  waytator_window_set_text_view_text(self->ocr_selected_text_view, selected_text);
  waytator_window_set_text_view_text(self->ocr_all_text_view, all_text);
  if (adw_bottom_sheet_get_open(self->ocr_panel_bottom_sheet) != show_panel)
    adw_bottom_sheet_set_open(self->ocr_panel_bottom_sheet, show_panel);
}

static void
waytator_window_ocr_panel_toggled(GtkToggleButton *button,
                                  gpointer         user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  if (gtk_toggle_button_get_active(button) && self->selected_ocr_line == NULL)
    gtk_stack_set_visible_child_name(self->ocr_panel_stack, "all");

  waytator_window_update_ocr_panel(self);
}

static void
waytator_window_ocr_panel_open_changed(GObject    *object,
                                       GParamSpec *pspec,
                                       gpointer    user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  const gboolean open = adw_bottom_sheet_get_open(self->ocr_panel_bottom_sheet);

  (void) object;
  (void) pspec;

  if (gtk_toggle_button_get_active(self->ocr_panel_toggle_button) != open)
    gtk_toggle_button_set_active(self->ocr_panel_toggle_button, open);

  if (open && self->selected_ocr_line == NULL)
    gtk_stack_set_visible_child_name(self->ocr_panel_stack, "all");
}

static void
waytator_window_ocr_panel_close_clicked(GtkButton *button,
                                        gpointer   user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) button;

  waytator_window_set_ocr_panel_visible(self, FALSE);
}

static void
waytator_window_ocr_ready(GObject      *source_object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  g_autoptr(GError) error = NULL;
  WaytatorOcrResult *ocr_result;

  (void) source_object;

  ocr_result = g_task_propagate_pointer(G_TASK(result), &error);
  self->ocr_running = FALSE;

  if (ocr_result == NULL) {
    if (self->active_tool == WAYTATOR_TOOL_OCR)
      waytator_window_show_error(self, error->message);
    g_object_unref(self);
    return;
  }

  if (ocr_result->generation != self->ocr_generation) {
    waytator_ocr_result_free(ocr_result);
    g_object_unref(self);
    return;
  }

  waytator_window_clear_ocr_widgets(self);
  g_clear_pointer(&self->ocr_lines, g_ptr_array_unref);
  g_clear_pointer(&self->ocr_all_text, g_free);
  self->ocr_lines = ocr_result->lines;
  self->ocr_all_text = waytator_window_build_ocr_all_text(self->ocr_lines);
  self->selected_ocr_line = NULL;
  ocr_result->lines = NULL;
  waytator_window_update_ocr_overlay(self);
  waytator_window_update_ocr_panel(self);
  waytator_ocr_result_free(ocr_result);
  g_object_unref(self);
}

void
waytator_window_maybe_start_ocr(WaytatorWindow *self)
{
  g_autoptr(GTask) task = NULL;
  WaytatorOcrRequest *request;

  if (self->texture == NULL || self->ocr_running || self->ocr_lines != NULL)
    return;

  request = waytator_ocr_request_new(self->texture, self->ocr_generation);
  if (request == NULL)
    return;

  self->ocr_running = TRUE;
  waytator_window_update_ocr_panel(self);
  task = g_task_new(self, NULL, waytator_window_ocr_ready, g_object_ref(self));
  g_task_set_task_data(task, request, (GDestroyNotify) waytator_ocr_request_free);
  g_task_run_in_thread(task, waytator_ocr_run_task);
}

static void waytator_window_save_copy_ready(GObject *source_object, GAsyncResult *result, gpointer user_data);
static void waytator_window_save_overwrite_action(GtkWidget *widget, const char *action_name, GVariant *parameter);
static void waytator_window_save_copy_action(GtkWidget *widget, const char *action_name, GVariant *parameter);
static void waytator_window_rotate_counter_clockwise_action(GtkWidget *widget, const char *action_name, GVariant *parameter);
static void waytator_window_flip_horizontal_action(GtkWidget *widget, const char *action_name, GVariant *parameter);
static void waytator_window_flip_vertical_action(GtkWidget *widget, const char *action_name, GVariant *parameter);
static void waytator_window_copy_action(GtkWidget *widget, const char *action_name, GVariant *parameter);
static void waytator_window_dismiss_action(GtkWidget *widget, const char *action_name, GVariant *parameter);
static void waytator_window_close_window_action(GtkWidget *widget, const char *action_name, GVariant *parameter);
static void waytator_window_copy_clicked(GtkButton *button, gpointer user_data);
static void waytator_window_open_current_file_action(GtkWidget *widget, const char *action_name, GVariant *parameter);
static void waytator_window_open_containing_folder_action(GtkWidget *widget, const char *action_name, GVariant *parameter);
static void waytator_window_open_parent_folder(WaytatorWindow *self);

static gboolean
waytator_window_restore_copy_button(gpointer user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  self->copy_feedback_timeout_id = 0;
  gtk_stack_set_visible_child(self->copy_icon_stack, GTK_WIDGET(self->copy_default_icon));
  gtk_stack_set_visible_child(self->copy_keep_open_icon_stack, GTK_WIDGET(self->copy_keep_open_default_icon));
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

static void
waytator_window_start_copy(WaytatorWindow *self,
                           gboolean        close_after)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;
  WaytatorExportRequest *request;

  if (self->texture == NULL)
    return;

  if (self->copy_in_progress) {
    if (close_after)
      self->close_copy_pending = TRUE;
    return;
  }

  request = waytator_export_request_new(self->texture,
                                        waytator_window_strokes(self),
                                        WAYTATOR_EXPORT_COPY,
                                        NULL,
                                        "png",
                                        waytator_stroke_copy,
                                        (GDestroyNotify) waytator_stroke_free,
                                        self->allow_highlighter_overlap,
                                        waytator_stroke_render,
                                        &error);
  if (request == NULL) {
    waytator_window_show_error(self, error->message);
    return;
  }

  self->copy_in_progress = TRUE;
  self->close_after_copy = close_after;
  task = g_task_new(self, NULL, waytator_window_copy_export_ready, g_object_ref(self));
  g_task_set_task_data(task, request, (GDestroyNotify) waytator_export_request_free);
  g_task_run_in_thread(task, waytator_export_run_task);
}

void
waytator_window_trigger_copy(WaytatorWindow *self)
{
  waytator_window_start_copy(self, FALSE);
}

void
waytator_window_update_copy_controls(WaytatorWindow *self)
{
  const gboolean closes_on_copy = waytator_window_closes_on_copy(self);

  if (self->copy_button == NULL)
    return;

  gtk_widget_set_visible(GTK_WIDGET(self->copy_keep_open_button), closes_on_copy && self->folds.keep_open);
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->copy_button), closes_on_copy ? "Copy and close" : "Copy");
  gtk_image_set_from_icon_name(self->copy_default_icon,
                               closes_on_copy ? "waytator-copy-close-symbolic" : "waytator-copy-symbolic");
  //a second copy button makes the top bar wider
  waytator_window_queue_chrome_layout(self);
}

void
waytator_window_maybe_auto_copy_latest_change(WaytatorWindow *self)
{
  if (!self->auto_copy_latest_change || self->texture == NULL || !waytator_window_has_unsaved_changes(self))
    return;

  if (self->copy_in_progress) {
    self->auto_copy_pending = TRUE;
    return;
  }

  waytator_window_trigger_copy(self);
}

//the tick goes to the button that copies this way: with close on copy, a copy that stays open is the
//keep-open button's, and where that button is folded into the menu there is none to light up
static void
waytator_window_flash_copy_success(WaytatorWindow *self,
                                   gboolean        close_after)
{
  if (self->copy_feedback_timeout_id != 0) {
    g_source_remove(self->copy_feedback_timeout_id);
    g_object_unref(self);
  }

  gtk_stack_set_visible_child(self->copy_icon_stack, GTK_WIDGET(self->copy_default_icon));
  gtk_stack_set_visible_child(self->copy_keep_open_icon_stack, GTK_WIDGET(self->copy_keep_open_default_icon));
  if (close_after || !waytator_window_closes_on_copy(self))
    gtk_stack_set_visible_child(self->copy_icon_stack, GTK_WIDGET(self->copy_success_icon));
  else
    gtk_stack_set_visible_child(self->copy_keep_open_icon_stack, GTK_WIDGET(self->copy_keep_open_success_icon));
  self->copy_feedback_timeout_id = g_timeout_add(1200,
                                                 waytator_window_restore_copy_button,
                                                 g_object_ref(self));
}

//a capture with a screenshot folder to go to, and no file of its own yet
static gboolean
waytator_window_saves_to_folder(WaytatorWindow *self)
{
  return self->from_capture && self->current_file == NULL && *self->settings.screenshot_save_dir != '\0';
}

//the menu only exists where there is a choice, an image with nowhere obvious to go gets the plain button and a dialog
void
waytator_window_update_save_menu(WaytatorWindow *self)
{
  const gboolean to_folder = waytator_window_saves_to_folder(self);
  const gboolean has_menu = self->current_file != NULL || to_folder;
  g_autofree char *folder = to_folder ? waytator_screenshot_display_path(self->settings.screenshot_save_dir) : NULL;
  g_autofree char *key = g_strdup_printf("%d:%s", self->current_file != NULL, folder != NULL ? folder : "");

  if (self->save_menu == NULL)
    return;

  if (waytator_window_is_video(self)) {
    waytator_window_update_video_save_menu(self);
    return;
  }

  gtk_widget_set_tooltip_text(GTK_WIDGET(self->save_button), "Save");
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->save_as_button), "Save");
  gtk_widget_set_visible(GTK_WIDGET(self->save_button), has_menu && self->folds.save);
  gtk_widget_set_visible(GTK_WIDGET(self->save_as_button), !has_menu && self->folds.save);

  //rebuilding it while nothing changed would close a menu that is open
  if (g_strcmp0(key, self->save_menu_key) == 0)
    return;

  g_free(self->save_menu_key);
  self->save_menu_key = g_steal_pointer(&key);
  //the menu button and the plain one need not be the same width
  waytator_window_queue_chrome_layout(self);
  g_menu_remove_all(self->save_menu);
  if (to_folder) {
    g_autofree char *label = g_strdup_printf("Save to %s", folder);

    g_menu_append(self->save_menu, label, "win.save-image");
    g_menu_append(self->save_menu, "Save as…", "win.save-as");
  } else {
    g_menu_append(self->save_menu, "Save", "win.save");
    g_menu_append(self->save_menu, "Save a copy…", "win.save-copy");
  }
}

void
waytator_window_reset_save_button(WaytatorWindow *self)
{
  const gboolean has_unsaved_changes = waytator_window_has_unsaved_changes(self);
  //a capture or a piped image is worth saving before the first edit, it exists nowhere else
  const gboolean can_save = !self->capture_storing
                          && (has_unsaved_changes || (self->texture != NULL && self->never_saved));

  if (waytator_window_is_video(self)) {
    waytator_window_update_video_save_controls(self);
    return;
  }

  //the spinner and the tick put the plain icon back themselves once they are done
  if (!self->save_in_progress && self->save_spinner_timeout_id == 0 && self->save_feedback_timeout_id == 0)
    gtk_stack_set_visible_child(self->save_icon_stack, GTK_WIDGET(self->save_default_icon));
  gtk_widget_set_sensitive(GTK_WIDGET(self->save_button), can_save);
  gtk_widget_set_sensitive(GTK_WIDGET(self->save_as_button), can_save);
  waytator_window_update_save_menu(self);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "win.save", has_unsaved_changes && self->current_file != NULL);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "win.save-copy", can_save);
}

static gboolean
waytator_window_restore_save_button(gpointer user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  self->save_feedback_timeout_id = 0;
  waytator_window_reset_save_button(self);
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

static gboolean
waytator_window_show_save_success(gpointer user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  self->save_spinner_timeout_id = 0;
  gtk_widget_set_sensitive(GTK_WIDGET(self->save_button), FALSE);
  gtk_stack_set_visible_child(self->save_icon_stack, GTK_WIDGET(self->save_success_icon));
  self->save_feedback_timeout_id = g_timeout_add(1200,
                                                 waytator_window_restore_save_button,
                                                 g_object_ref(self));
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

void
waytator_window_begin_save_feedback(WaytatorWindow *self)
{
  if (self->save_spinner_timeout_id != 0)
    g_source_remove(self->save_spinner_timeout_id);

  if (self->save_feedback_timeout_id != 0)
    g_source_remove(self->save_feedback_timeout_id);

  self->save_spinner_timeout_id = 0;
  self->save_feedback_timeout_id = 0;
  self->save_feedback_started_at = g_get_monotonic_time();
  gtk_widget_set_sensitive(GTK_WIDGET(self->save_as_button), FALSE);

  self->save_in_progress = TRUE;
  gtk_stack_set_visible_child(self->save_icon_stack, GTK_WIDGET(self->save_working_icon));
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "win.save", FALSE);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "win.save-copy", FALSE);
}

void
waytator_window_finish_save_feedback(WaytatorWindow *self)
{
  const gint64 min_spinner_us = 500 * G_TIME_SPAN_MILLISECOND;
  gint64 elapsed = g_get_monotonic_time() - self->save_feedback_started_at;
  guint delay_ms = 0;

  if (self->save_spinner_timeout_id != 0)
    g_source_remove(self->save_spinner_timeout_id);

  if (elapsed < min_spinner_us)
    delay_ms = (guint) ((min_spinner_us - elapsed + 999) / 1000);

  self->save_spinner_timeout_id = g_timeout_add(delay_ms,
                                                waytator_window_show_save_success,
                                                g_object_ref(self));
}


void
waytator_window_show_error(WaytatorWindow *self,
                           const char     *message)
{
  g_autoptr(GtkAlertDialog) dialog = gtk_alert_dialog_new("%s", message);

  gtk_alert_dialog_show(dialog, GTK_WINDOW(self));
}

static char *
waytator_window_make_copy_name(const char *filename)
{
  const char *dot;

  if (filename == NULL || *filename == '\0')
    return g_strdup("image_copy.png");

  dot = strrchr(filename, '.');
  if (dot == NULL || dot == filename)
    return g_strdup_printf("%s_copy", filename);

  return g_strdup_printf("%.*s_copy%s",
                         (int) (dot - filename),
                         filename,
                         dot);
}

gboolean
waytator_window_error_is_user_dismissed(const GError *error)
{
  return g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
      || g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED);
}

static gboolean
waytator_window_has_unsaved_changes(WaytatorWindow *self)
{
  return self->texture != NULL && waytator_document_has_unsaved_changes(self->document);
}

static void
waytator_window_mark_saved(WaytatorWindow *self)
{
  waytator_document_mark_saved(self->document);
}

void
waytator_window_update_history_buttons(WaytatorWindow *self)
{
  const gboolean has_image = self->texture != NULL;

  if (waytator_window_is_video(self)) {
    waytator_window_update_video_history_buttons(self);
    return;
  }

  gtk_widget_set_sensitive(GTK_WIDGET(self->undo_button), has_image && waytator_document_can_undo(self->document));
  gtk_widget_set_sensitive(GTK_WIDGET(self->redo_button), has_image && waytator_document_can_redo(self->document));
}

void
waytator_window_clear_history(WaytatorWindow *self)
{
  waytator_document_clear_history(self->document);
  waytator_window_update_history_buttons(self);
}

void
waytator_window_record_undo_step(WaytatorWindow *self)
{
  waytator_document_record_undo_step(self->document);
  waytator_window_reset_save_button(self);
  waytator_window_update_history_buttons(self);
}

static GBytes *
waytator_window_texture_download_bytes(GdkTexture *texture,
                                       int        *width,
                                       int        *height,
                                       gsize      *stride)
{
  guchar *pixels;
  gsize rowstride;
  int image_width;
  int image_height;

  if (texture == NULL)
    return NULL;

  image_width = gdk_texture_get_width(texture);
  image_height = gdk_texture_get_height(texture);
  if (image_width <= 0 || image_height <= 0)
    return NULL;

  rowstride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, image_width);
  pixels = g_malloc(rowstride * image_height);
  gdk_texture_download(texture, pixels, rowstride);

  if (width != NULL)
    *width = image_width;
  if (height != NULL)
    *height = image_height;
  if (stride != NULL)
    *stride = rowstride;

  return g_bytes_new_take(pixels, rowstride * image_height);
}

static GBytes *
waytator_window_surface_copy_bytes(cairo_surface_t *surface,
                                   int             *width,
                                   int             *height,
                                   gsize           *stride)
{
  const gsize rowstride = cairo_image_surface_get_stride(surface);
  const int image_width = cairo_image_surface_get_width(surface);
  const int image_height = cairo_image_surface_get_height(surface);
  guchar *pixels;

  if (surface == NULL || cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE)
    return NULL;

  cairo_surface_flush(surface);
  pixels = g_memdup2(cairo_image_surface_get_data(surface), rowstride * image_height);

  if (width != NULL)
    *width = image_width;
  if (height != NULL)
    *height = image_height;
  if (stride != NULL)
    *stride = rowstride;

  return g_bytes_new_take(pixels, rowstride * image_height);
}

static void
waytator_window_copy_image_bytes_to_surface(cairo_surface_t *surface,
                                            GBytes          *pixels,
                                            gsize            stride)
{
  const guchar *src = g_bytes_get_data(pixels, NULL);
  guchar *dst;
  const int height = cairo_image_surface_get_height(surface);
  const gsize dst_stride = cairo_image_surface_get_stride(surface);

  if (src == NULL)
    return;

  dst = cairo_image_surface_get_data(surface);

  for (int y = 0; y < height; y++)
    memcpy(dst + y * dst_stride, src + y * stride, MIN(dst_stride, stride));

  cairo_surface_mark_dirty(surface);
}

static gboolean
waytator_window_refresh_image_from_document(WaytatorWindow *self)
{
  g_autoptr(GBytes) pixels = NULL;
  const int old_width = self->texture != NULL ? gdk_texture_get_width(self->texture) : 0;
  const int old_height = self->texture != NULL ? gdk_texture_get_height(self->texture) : 0;
  int width = 0;
  int height = 0;
  gsize stride = 0;

  g_clear_object(&self->texture);
  if (self->image_surface != NULL) {
    cairo_surface_destroy(self->image_surface);
    self->image_surface = NULL;
  }

  if (!waytator_document_get_image(self->document, &pixels, &width, &height, &stride)) {
    gtk_picture_set_paintable(self->picture, NULL);
    return FALSE;
  }

  self->texture = GDK_TEXTURE(gdk_memory_texture_new(width,
                                                     height,
                                                     GDK_MEMORY_DEFAULT,
                                                     pixels,
                                                     stride));
  self->image_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  waytator_window_copy_image_bytes_to_surface(self->image_surface, pixels, stride);
  gtk_picture_set_paintable(self->picture, GDK_PAINTABLE(self->texture));
  //a new image, or one cropped or rotated to a new size, starts out in the middle. anything else, like undoing a
  //stroke, leaves it where it is
  if (width != old_width || height != old_height)
    waytator_canvas_view_centre(self->canvas_view);
  return TRUE;
}

cairo_surface_t *
waytator_window_render_composited_surface(WaytatorWindow *self)
{
  cairo_surface_t *surface;
  cairo_t *cr;
  GPtrArray *strokes = waytator_window_strokes(self);

  if (self->texture == NULL)
    return NULL;

  surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                       gdk_texture_get_width(self->texture),
                                       gdk_texture_get_height(self->texture));
  gdk_texture_download(self->texture,
                       cairo_image_surface_get_data(surface),
                       cairo_image_surface_get_stride(surface));
  cairo_surface_mark_dirty(surface);

  cr = cairo_create(surface);
  waytator_render_strokes(cr,
                          strokes,
                          surface,
                          self->allow_highlighter_overlap,
                          waytator_stroke_render);
  cairo_destroy(cr);
  cairo_surface_flush(surface);
  return surface;
}

void
waytator_window_refresh_document_state(WaytatorWindow *self)
{
  waytator_window_refresh_image_from_document(self);
  self->current_stroke = NULL;
  self->drawing = FALSE;
  self->interaction_has_undo_step = FALSE;
  self->active_touch_draw_sequence = NULL;
  self->cancelled_touch_draw_sequence = NULL;
  self->crop_start_x = 0.0;
  self->crop_start_y = 0.0;
  self->crop_end_x = 0.0;
  self->crop_end_y = 0.0;
  waytator_window_clear_ocr_results(self);
  waytator_window_apply_zoom_mode(self);
  waytator_window_update_zoom_label(self);
  waytator_window_sync_state(self);
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

static void
waytator_window_log_formats(const char      *label,
                            GdkContentFormats *formats)
{
  g_autofree char *description = NULL;

  if (formats == NULL || gdk_content_formats_is_empty(formats)) {
    g_debug("%s: (none)", label);
    return;
  }

  description = gdk_content_formats_to_string(formats);
  g_debug("%s: %s", label, description);
}

static void
waytator_window_notify_and_close(WaytatorWindow *self,
                                 const char     *title,
                                 const char     *body)
{
  GApplication *app = G_APPLICATION(gtk_window_get_application(GTK_WINDOW(self)));

  if (app != NULL) {
    g_autoptr(GNotification) notification = g_notification_new(title);
    GDBusConnection *connection = g_application_get_dbus_connection(app);

    if (body != NULL)
      g_notification_set_body(notification, body);
    g_application_send_notification(app, NULL, notification);
    if (connection != NULL)
      g_dbus_connection_flush_sync(connection, NULL, NULL);
  }

  //wl-copy borrows the keyboard to set the clipboard on GNOME, and gives it back to this window as it closes
  waytator_window_destroy_settled((GtkWindow *[]) { GTK_WINDOW(self) }, 1);
}

//GTK crashes on a tablet pad event for a window that is already gone, and the compositor sends one whenever it
//hands focus to a window, also one that is on its way out. out of sight the window cannot be handed focus any more,
//and after a round trip everything sent before that has found it still there, so only then is it destroyed
void
waytator_window_destroy_settled(GtkWindow *const *windows,
                                guint             n_windows)
{
  if (n_windows == 0)
    return;

  for (guint i = 0; i < n_windows; i++)
    gtk_widget_set_visible(GTK_WIDGET(windows[i]), FALSE);
  gdk_display_sync(gtk_widget_get_display(GTK_WIDGET(windows[0])));
  for (guint i = 0; i < n_windows; i++)
    gtk_window_destroy(windows[i]);
}

static void
waytator_window_clipboard_handed_off(GObject      *source_object,
                                     GAsyncResult *result,
                                     gpointer      user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  GSubprocess *process = G_SUBPROCESS(source_object);
  g_autoptr(GError) error = NULL;

  self->copy_in_progress = FALSE;
  if (!g_subprocess_communicate_finish(process, result, NULL, NULL, &error)
      || !g_subprocess_get_successful(process)) {
    g_autofree char *message = g_strdup_printf("wl-copy failed, so the window was left open%s%s",
                                               error != NULL ? ": " : "",
                                               error != NULL ? error->message : "");

    g_clear_pointer(&self->saved_path, g_free);
    waytator_window_show_error(self, message);
    g_object_unref(self);
    return;
  }

  if (self->saved_path != NULL)
    waytator_window_notify_and_close(self, "Image saved and copied", self->saved_path);
  else
    waytator_window_notify_and_close(self, "Image copied to clipboard", NULL);
  g_object_unref(self);
}

//the clipboard dies with us on wayland, so wl-copy has to hold the image once we exit
static void
waytator_window_hand_off_clipboard_and_close(WaytatorWindow     *self,
                                             WaytatorCopyResult *copy_result)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GSubprocess) process = NULL;

  process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE
                             | G_SUBPROCESS_FLAGS_STDOUT_SILENCE
                             | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                             &error,
                             "wl-copy",
                             "--type",
                             copy_result->mime_type,
                             NULL);
  if (process == NULL) {
    g_autofree char *message = g_strdup_printf("Could not run wl-copy, so the window was left open: %s",
                                               error->message);

    g_clear_pointer(&self->saved_path, g_free);
    waytator_window_show_error(self, message);
    return;
  }

  self->copy_in_progress = TRUE;
  g_subprocess_communicate_async(process,
                                 copy_result->bytes,
                                 NULL,
                                 waytator_window_clipboard_handed_off,
                                 g_object_ref(self));
}

static void
waytator_window_copy_export_ready(GObject      *source_object,
                                  GAsyncResult *result,
                                  gpointer      user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  g_autoptr(GError) error = NULL;
  WaytatorCopyResult *copy_result;
  g_autoptr(GdkContentProvider) provider = NULL;
  g_autoptr(GdkContentProvider) bytes_provider = NULL;
  g_autoptr(GdkContentProvider) texture_provider = NULL;
  g_autoptr(GdkContentFormats) provider_formats = NULL;
  GdkClipboard *clipboard;

  (void) source_object;

  copy_result = g_task_propagate_pointer(G_TASK(result), &error);
  self->copy_in_progress = FALSE;
  if (copy_result == NULL) {
    self->close_after_copy = FALSE;
    self->close_copy_pending = FALSE;
    g_clear_pointer(&self->saved_path, g_free);
    waytator_window_show_error(self, error->message);
    if (self->auto_copy_pending) {
      self->auto_copy_pending = FALSE;
      waytator_window_maybe_auto_copy_latest_change(self);
    }
    g_object_unref(self);
    return;
  }

  clipboard = gdk_display_get_clipboard(gtk_widget_get_display(GTK_WIDGET(self)));
  bytes_provider = gdk_content_provider_new_for_bytes(copy_result->mime_type, copy_result->bytes);

  if (copy_result->texture != NULL) {
    GdkContentProvider *providers[2];

    texture_provider = gdk_content_provider_new_typed(GDK_TYPE_TEXTURE, copy_result->texture);
    providers[0] = g_steal_pointer(&texture_provider);
    providers[1] = g_steal_pointer(&bytes_provider);
    provider = gdk_content_provider_new_union(providers, G_N_ELEMENTS(providers));
  } else {
    provider = g_object_ref(bytes_provider);
  }

  provider_formats = gdk_content_provider_ref_formats(provider);
  waytator_window_log_formats("Clipboard provider formats", provider_formats);

  if (!gdk_clipboard_set_content(clipboard, provider)) {
    self->close_after_copy = FALSE;
    self->close_copy_pending = FALSE;
    g_clear_pointer(&self->saved_path, g_free);
    waytator_copy_result_free(copy_result);
    waytator_window_show_error(self, "Could not copy image to clipboard");
    g_object_unref(self);
    return;
  }

  waytator_window_log_formats("Clipboard accepted formats", gdk_clipboard_get_formats(clipboard));
  waytator_window_flash_copy_success(self, self->close_after_copy);
  if (self->close_after_copy) {
    self->close_after_copy = FALSE;
    waytator_window_hand_off_clipboard_and_close(self, copy_result);
    waytator_copy_result_free(copy_result);
    g_object_unref(self);
    return;
  }

  waytator_copy_result_free(copy_result);
  if (self->close_copy_pending) {
    self->close_copy_pending = FALSE;
    self->auto_copy_pending = FALSE;
    waytator_window_start_copy(self, TRUE);
  } else if (self->auto_copy_pending) {
    self->auto_copy_pending = FALSE;
    waytator_window_maybe_auto_copy_latest_change(self);
  }
  g_object_unref(self);
}

static void
waytator_window_set_saved_path(WaytatorWindow *self,
                               GFile          *file)
{
  g_clear_pointer(&self->saved_path, g_free);
  if (self->after_save != WAYTATOR_AFTER_SAVE_NONE)
    self->saved_path = g_file_get_parse_name(file);
}

static void
waytator_window_finish_after_save(WaytatorWindow *self)
{
  const WaytatorAfterSave after_save = self->after_save;

  self->after_save = WAYTATOR_AFTER_SAVE_NONE;
  if (after_save == WAYTATOR_AFTER_SAVE_CLOSE)
    waytator_window_notify_and_close(self, "Image saved", self->saved_path);
  else if (after_save == WAYTATOR_AFTER_SAVE_COPY_CLOSE)
    waytator_window_start_copy(self, TRUE);
}

//the first save of an image that had no file makes that file the one being edited
static void
waytator_window_adopt_saved_file(WaytatorWindow *self)
{
  if (self->adopt_file == NULL)
    return;

  g_clear_object(&self->current_file);
  self->current_file = g_steal_pointer(&self->adopt_file);
  self->adopt_file_reserved = FALSE;
  g_free(self->source_name);
  self->source_name = g_file_get_basename(self->current_file);
  gtk_label_set_text(self->file_label, self->source_name);
  waytator_window_sync_state(self);
}

static void
waytator_window_save_export_ready(GObject      *source_object,
                                  GAsyncResult *result,
                                  gpointer      user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  g_autoptr(GError) error = NULL;

  (void) source_object;

  self->save_in_progress = FALSE;
  if (!g_task_propagate_boolean(G_TASK(result), &error)) {
    if (self->save_spinner_timeout_id != 0)
      g_source_remove(self->save_spinner_timeout_id);

    if (self->save_feedback_timeout_id != 0)
      g_source_remove(self->save_feedback_timeout_id);

    self->save_spinner_timeout_id = 0;
    self->save_feedback_timeout_id = 0;
    self->after_save = WAYTATOR_AFTER_SAVE_NONE;
    g_clear_pointer(&self->saved_path, g_free);
    //an empty file waytator made to hold the name, not one that was there before
    if (self->adopt_file_reserved)
      g_file_delete(self->adopt_file, NULL, NULL);
    self->adopt_file_reserved = FALSE;
    g_clear_object(&self->adopt_file);
    self->from_capture = FALSE;
    waytator_window_reset_save_button(self);
    waytator_window_show_error(self, error->message);
    g_object_unref(self);
    return;
  }

  waytator_window_mark_saved(self);
  self->never_saved = FALSE;
  //the tick goes up first, so taking on the new file does not put the plain icon back in between
  waytator_window_finish_save_feedback(self);
  waytator_window_adopt_saved_file(self);
  waytator_window_finish_after_save(self);
  g_object_unref(self);
}

//where every save that is not an overwrite ends up, a first save also makes the file the one being edited
static gboolean
waytator_window_save_to_file(WaytatorWindow *self,
                             GFile          *file)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;
  WaytatorExportRequest *request;

  request = waytator_export_request_new(self->texture,
                                        waytator_window_strokes(self),
                                        WAYTATOR_EXPORT_SAVE,
                                        file,
                                        NULL,
                                        waytator_stroke_copy,
                                        (GDestroyNotify) waytator_stroke_free,
                                        self->allow_highlighter_overlap,
                                        waytator_stroke_render,
                                        &error);
  if (request == NULL) {
    self->after_save = WAYTATOR_AFTER_SAVE_NONE;
    waytator_window_show_error(self, error->message);
    return FALSE;
  }

  g_clear_object(&self->adopt_file);
  self->adopt_file_reserved = FALSE;
  if (self->current_file == NULL)
    self->adopt_file = g_object_ref(file);

  waytator_window_set_saved_path(self, file);
  waytator_window_begin_save_feedback(self);
  task = g_task_new(self, NULL, waytator_window_save_export_ready, g_object_ref(self));
  g_task_set_task_data(task, request, (GDestroyNotify) waytator_export_request_free);
  g_task_run_in_thread(task, waytator_export_run_task);
  return TRUE;
}

static void
waytator_window_save_copy_ready(GObject      *source_object,
                                GAsyncResult *result,
                                gpointer      user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;

  file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source_object), result, &error);
  if (file == NULL) {
    self->after_save = WAYTATOR_AFTER_SAVE_NONE;
    if (!waytator_window_error_is_user_dismissed(error))
      waytator_window_show_error(self, error->message);

    g_object_unref(self);
    return;
  }

  waytator_window_save_to_file(self, file);
  g_object_unref(self);
}

static gboolean
waytator_window_save_overwrite(WaytatorWindow *self)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;
  WaytatorExportRequest *request;

  if (self->current_file == NULL)
    return FALSE;

  request = waytator_export_request_new(self->texture,
                                        waytator_window_strokes(self),
                                        WAYTATOR_EXPORT_SAVE,
                                        self->current_file,
                                        NULL,
                                        waytator_stroke_copy,
                                        (GDestroyNotify) waytator_stroke_free,
                                        self->allow_highlighter_overlap,
                                        waytator_stroke_render,
                                        &error);
  if (request == NULL) {
    waytator_window_show_error(self, error->message);
    return FALSE;
  }

  waytator_window_set_saved_path(self, self->current_file);
  waytator_window_begin_save_feedback(self);
  task = g_task_new(self, NULL, waytator_window_save_export_ready, g_object_ref(self));
  g_task_set_task_data(task, request, (GDestroyNotify) waytator_export_request_free);
  g_task_run_in_thread(task, waytator_export_run_task);
  return TRUE;
}

static void
waytator_window_save_overwrite_action(GtkWidget  *widget,
                                      const char *action_name,
                                      GVariant   *parameter)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(widget);

  (void) action_name;
  (void) parameter;

  self->after_save = WAYTATOR_AFTER_SAVE_NONE;
  waytator_window_save_overwrite(self);
}

//a capture already has somewhere to go when a screenshot folder is set, so there is nothing to ask
static gboolean
waytator_window_save_capture_to_folder(WaytatorWindow *self)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;

  if (!self->from_capture || self->current_file != NULL || *self->settings.screenshot_save_dir == '\0')
    return FALSE;

  file = waytator_screenshot_reserve_file(self->settings.screenshot_save_dir, self->source_name, &error);
  if (file == NULL) {
    g_autofree char *reason = waytator_screenshot_describe_save_error(self->settings.screenshot_save_dir, error);
    g_autofree char *message = g_strdup_printf("%s Save again to choose another place.", reason);

    //the next attempt goes through the dialog, the folder is not going to work this time
    self->from_capture = FALSE;
    self->after_save = WAYTATOR_AFTER_SAVE_NONE;
    waytator_window_reset_save_button(self);
    waytator_window_show_error(self, message);
    return TRUE;
  }

  //the reserved file is empty, and a save that never started or failed would leave it behind
  if (!waytator_window_save_to_file(self, file))
    g_file_delete(file, NULL, NULL);
  else
    self->adopt_file_reserved = self->adopt_file != NULL;
  return TRUE;
}

static void
waytator_window_show_save_dialog(WaytatorWindow *self)
{
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
  g_autofree char *basename = NULL;
  g_autofree char *copy_name = NULL;


  //without a file there is no original for this to be a copy of
  if (self->current_file != NULL) {
    basename = g_file_get_basename(self->current_file);
    copy_name = waytator_window_make_copy_name(basename);
  } else {
    copy_name = g_strdup(self->source_name);
  }

  gtk_file_dialog_set_title(dialog, self->current_file != NULL ? "Save a copy" : "Save image");
  gtk_file_dialog_set_initial_name(dialog, copy_name);

  gtk_file_dialog_save(dialog,
                       GTK_WINDOW(self),
                       NULL,
                       waytator_window_save_copy_ready,
                       g_object_ref(self));
}

//the first save of an image with no file: the screenshot folder when it applies, a dialog otherwise
static void
waytator_window_save_new(WaytatorWindow *self)
{
  if (!waytator_window_save_capture_to_folder(self))
    waytator_window_show_save_dialog(self);
}

//always asks, for a capture that should not end up in the screenshot folder
static void
waytator_window_save_as_action(GtkWidget  *widget,
                               const char *action_name,
                               GVariant   *parameter)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(widget);

  (void) action_name;
  (void) parameter;

  //a video asks the same questions however its export starts
  if (waytator_window_is_video(self)) {
    waytator_window_video_save(self, NULL, NULL, FALSE, FALSE);
    return;
  }

  if (self->texture == NULL || self->save_in_progress || self->capture_storing
      || self->after_save != WAYTATOR_AFTER_SAVE_NONE)
    return;

  waytator_window_show_save_dialog(self);
}

static void
waytator_window_save_copy_action(GtkWidget  *widget,
                                 const char *action_name,
                                 GVariant   *parameter)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(widget);

  (void) action_name;
  (void) parameter;

  if (self->capture_storing)
    return;

  self->after_save = WAYTATOR_AFTER_SAVE_NONE;
  waytator_window_show_save_dialog(self);
}

//unlike win.save this also works for images that have no file yet, by asking where to put them
static void
waytator_window_save_image_action(GtkWidget  *widget,
                                  const char *action_name,
                                  GVariant   *parameter)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(widget);

  (void) action_name;
  (void) parameter;

  if (waytator_window_is_video(self)) {
    waytator_window_video_save(self, NULL, NULL, FALSE, FALSE);
    return;
  }

  if (self->texture == NULL || self->save_in_progress || self->capture_storing
      || self->after_save != WAYTATOR_AFTER_SAVE_NONE)
    return;

  if (self->current_file == NULL)
    waytator_window_save_new(self);
  else if (waytator_window_has_unsaved_changes(self))
    waytator_window_save_overwrite(self);
}

static void
waytator_window_save_then(WaytatorWindow   *self,
                          WaytatorAfterSave after_save)
{
  if (self->texture == NULL || self->save_in_progress || self->capture_storing
      || self->after_save != WAYTATOR_AFTER_SAVE_NONE)
    return;

  self->after_save = after_save;
  g_clear_pointer(&self->saved_path, g_free);

  if (self->current_file == NULL) {
    waytator_window_save_new(self);
    return;
  }

  if (!waytator_window_has_unsaved_changes(self)) {
    waytator_window_set_saved_path(self, self->current_file);
    waytator_window_finish_after_save(self);
    return;
  }

  if (!waytator_window_save_overwrite(self))
    self->after_save = WAYTATOR_AFTER_SAVE_NONE;
}

static void
waytator_window_save_and_close_action(GtkWidget  *widget,
                                      const char *action_name,
                                      GVariant   *parameter)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(widget);

  (void) action_name;
  (void) parameter;

  if (waytator_window_is_video(self))
    waytator_window_video_save(self, NULL, NULL, FALSE, TRUE);
  else
    waytator_window_save_then(self, WAYTATOR_AFTER_SAVE_CLOSE);
}

static void
waytator_window_save_copy_and_close_action(GtkWidget  *widget,
                                           const char *action_name,
                                           GVariant   *parameter)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(widget);

  (void) action_name;
  (void) parameter;

  if (waytator_window_is_video(self))
    waytator_window_video_save(self, NULL, NULL, TRUE, TRUE);
  else
    waytator_window_save_then(self, WAYTATOR_AFTER_SAVE_COPY_CLOSE);
}

static void
waytator_window_commit_transformed_surface(WaytatorWindow  *self,
                                           cairo_surface_t *surface)
{
  g_autoptr(GBytes) pixels = NULL;
  int width = 0;
  int height = 0;
  gsize stride = 0;

  pixels = waytator_window_surface_copy_bytes(surface, &width, &height, &stride);
  if (pixels == NULL)
    return;

  waytator_document_set_image(self->document, pixels, width, height, stride);
  waytator_document_clear_annotations(self->document);
  waytator_window_restore_strokes(self, waytator_window_strokes(self));
}

void
waytator_window_apply_crop(WaytatorWindow *self,
                           int             left,
                           int             top,
                           int             width,
                           int             height)
{
  cairo_surface_t *source_surface;
  cairo_surface_t *result_surface;
  cairo_t *cr;

  if (self->texture == NULL || width <= 0 || height <= 0)
    return;

  source_surface = waytator_window_render_composited_surface(self);
  if (source_surface == NULL)
    return;

  result_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cr = cairo_create(result_surface);
  cairo_set_source_surface(cr, source_surface, -left, -top);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(source_surface);

  waytator_window_record_undo_step(self);
  waytator_window_commit_transformed_surface(self, result_surface);
  cairo_surface_destroy(result_surface);
}

static void
waytator_window_rotate_counter_clockwise_action(GtkWidget  *widget,
                                                const char *action_name,
                                                GVariant   *parameter)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(widget);
  cairo_surface_t *source_surface;
  cairo_surface_t *result_surface;
  cairo_t *cr;
  const int width = self->texture != NULL ? gdk_texture_get_width(self->texture) : 0;
  const int height = self->texture != NULL ? gdk_texture_get_height(self->texture) : 0;

  (void) action_name;
  (void) parameter;

  if (width <= 0 || height <= 0)
    return;

  source_surface = waytator_window_render_composited_surface(self);
  if (source_surface == NULL)
    return;

  result_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, height, width);
  cr = cairo_create(result_surface);
  cairo_translate(cr, 0.0, width);
  cairo_rotate(cr, -G_PI / 2.0);
  cairo_set_source_surface(cr, source_surface, 0.0, 0.0);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(source_surface);

  waytator_window_record_undo_step(self);
  waytator_window_commit_transformed_surface(self, result_surface);
  cairo_surface_destroy(result_surface);
}

static void
waytator_window_flip_horizontal_action(GtkWidget  *widget,
                                       const char *action_name,
                                       GVariant   *parameter)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(widget);
  cairo_surface_t *source_surface;
  cairo_surface_t *result_surface;
  cairo_t *cr;
  const int width = self->texture != NULL ? gdk_texture_get_width(self->texture) : 0;
  const int height = self->texture != NULL ? gdk_texture_get_height(self->texture) : 0;

  (void) action_name;
  (void) parameter;

  if (width <= 0 || height <= 0)
    return;

  source_surface = waytator_window_render_composited_surface(self);
  if (source_surface == NULL)
    return;

  result_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cr = cairo_create(result_surface);
  cairo_translate(cr, width, 0.0);
  cairo_scale(cr, -1.0, 1.0);
  cairo_set_source_surface(cr, source_surface, 0.0, 0.0);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(source_surface);

  waytator_window_record_undo_step(self);
  waytator_window_commit_transformed_surface(self, result_surface);
  cairo_surface_destroy(result_surface);
}

static void
waytator_window_flip_vertical_action(GtkWidget  *widget,
                                     const char *action_name,
                                     GVariant   *parameter)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(widget);
  cairo_surface_t *source_surface;
  cairo_surface_t *result_surface;
  cairo_t *cr;
  const int width = self->texture != NULL ? gdk_texture_get_width(self->texture) : 0;
  const int height = self->texture != NULL ? gdk_texture_get_height(self->texture) : 0;

  (void) action_name;
  (void) parameter;

  if (width <= 0 || height <= 0)
    return;

  source_surface = waytator_window_render_composited_surface(self);
  if (source_surface == NULL)
    return;

  result_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cr = cairo_create(result_surface);
  cairo_translate(cr, 0.0, height);
  cairo_scale(cr, 1.0, -1.0);
  cairo_set_source_surface(cr, source_surface, 0.0, 0.0);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(source_surface);

  waytator_window_record_undo_step(self);
  waytator_window_commit_transformed_surface(self, result_surface);
  cairo_surface_destroy(result_surface);
}

static void
waytator_window_copy_action(GtkWidget  *widget,
                            const char *action_name,
                            GVariant   *parameter)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(widget);

  (void) action_name;
  (void) parameter;

  if (!gtk_widget_is_sensitive(GTK_WIDGET(self->copy_button)))
    return;

  gtk_widget_activate(GTK_WIDGET(self->copy_button));
}

static void
waytator_window_dismiss_action(GtkWidget  *widget,
                               const char *action_name,
                               GVariant   *parameter)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(widget);

  (void) action_name;
  (void) parameter;

  if (adw_bottom_sheet_get_open(self->ocr_panel_bottom_sheet))
    waytator_window_set_ocr_panel_visible(self, FALSE);
}

static void
waytator_window_close_window_action(GtkWidget  *widget,
                                    const char *action_name,
                                    GVariant   *parameter)
{
  (void) action_name;
  (void) parameter;

  //the way the close button goes, so an unsaved video edit is asked about first
  gtk_window_close(GTK_WINDOW(widget));
}

static void
waytator_window_copy_clicked(GtkButton *button,
                             gpointer   user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) button;

  if (waytator_window_is_video(self)) {
    waytator_window_video_save(self, NULL, NULL, TRUE, waytator_window_closes_on_copy(self));
    return;
  }

  g_clear_pointer(&self->saved_path, g_free);
  waytator_window_start_copy(self, waytator_window_closes_on_copy(self));
}

static void
waytator_window_copy_keep_open_clicked(GtkButton *button,
                                       gpointer   user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) button;

  if (waytator_window_is_video(self))
    waytator_window_video_save(self, NULL, NULL, TRUE, FALSE);
  else
    waytator_window_start_copy(self, FALSE);
}

static void
waytator_window_copy_keep_open_action(GtkWidget  *widget,
                                      const char *action_name,
                                      GVariant   *parameter)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(widget);

  (void) action_name;
  (void) parameter;

  if (!gtk_widget_is_sensitive(GTK_WIDGET(self->copy_button)))
    return;

  if (waytator_window_is_video(self))
    waytator_window_video_save(self, NULL, NULL, TRUE, FALSE);
  else
    waytator_window_start_copy(self, FALSE);
}

static void
waytator_window_clear_annotations(WaytatorWindow *self)
{
  waytator_document_clear_annotations(self->document);
  self->current_stroke = NULL;
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
  waytator_window_update_history_buttons(self);
  waytator_window_maybe_auto_copy_latest_change(self);
}

static void
waytator_window_clear_image(WaytatorWindow *self)
{
  g_clear_object(&self->current_file);
  g_clear_object(&self->texture);
  if (self->image_surface != NULL) {
    cairo_surface_destroy(self->image_surface);
    self->image_surface = NULL;
  }
  g_clear_pointer(&self->source_name, g_free);
  g_clear_object(&self->adopt_file);
  self->adopt_file_reserved = FALSE;
  self->never_saved = FALSE;
  self->from_capture = FALSE;
  self->capture_storing = FALSE;
  self->image_serial++;
  self->crop_start_x = 0.0;
  self->crop_start_y = 0.0;
  self->crop_end_x = 0.0;
  self->crop_end_y = 0.0;
  waytator_document_set_image(self->document, NULL, 0, 0, 0);
  waytator_window_clear_ocr_results(self);
  waytator_window_clear_history(self);
  waytator_window_clear_annotations(self);
  waytator_window_mark_saved(self);
  gtk_picture_set_paintable(self->picture, NULL);
  self->fit_mode = TRUE;
  self->opening_zoom = FALSE;
  gtk_widget_queue_allocate(GTK_WIDGET(self->canvas_view));
  waytator_window_sync_state(self);
}

static void
waytator_window_set_image(WaytatorWindow *self,
                          GdkTexture     *texture,
                          GFile          *file,
                          const char     *display_name)
{
  g_autoptr(GBytes) pixels = NULL;
  int width = 0;
  int height = 0;
  gsize stride = 0;

  waytator_window_clear_image(self);

  self->current_file = file != NULL ? g_object_ref(file) : NULL;
  pixels = waytator_window_texture_download_bytes(texture, &width, &height, &stride);
  if (pixels == NULL)
    return;

  waytator_document_set_image(self->document, pixels, width, height, stride);
  waytator_window_refresh_image_from_document(self);

  self->source_name = g_strdup(display_name != NULL ? display_name : "image.png");

  gtk_label_set_text(self->file_label, self->source_name);
  self->zoom = 1.0;
  self->fit_mode = FALSE;
  self->opening_zoom = TRUE;
  self->drawing = FALSE;
  self->current_stroke = NULL;
  self->ocr_running = FALSE;
  self->never_saved = file == NULL;
  waytator_window_mark_saved(self);

  waytator_window_sync_state(self);
  waytator_window_queue_opening_zoom(self);
  gtk_widget_queue_draw(GTK_WIDGET(self->drawing_area));
}

gboolean
waytator_window_open_file(WaytatorWindow *self,
                          GFile          *file,
                          GError        **error)
{
  g_autofree char *basename = NULL;
  g_autoptr(GdkTexture) texture = NULL;

  g_return_val_if_fail(WAYTATOR_IS_WINDOW(self), FALSE);
  g_return_val_if_fail(file == NULL || G_IS_FILE(file), FALSE);

  if (file == NULL) {
    waytator_window_clear_image(self);
    return TRUE;
  }

  if (waytator_video_editor_handles_file(file))
    return waytator_window_open_video(self, file);

  texture = gdk_texture_new_from_file(file, error);
  if (texture == NULL)
    return FALSE;

  basename = g_file_get_basename(file);
  waytator_window_set_image(self, texture, file, basename);
  return TRUE;
}

gboolean
waytator_window_open_bytes(WaytatorWindow *self,
                           GBytes         *bytes,
                           const char     *display_name,
                           GError        **error)
{
  g_autoptr(GdkTexture) texture = NULL;

  g_return_val_if_fail(WAYTATOR_IS_WINDOW(self), FALSE);
  g_return_val_if_fail(bytes != NULL, FALSE);

  texture = gdk_texture_new_from_bytes(bytes, error);
  if (texture == NULL)
    return FALSE;

  waytator_window_set_image(self,
                            texture,
                            NULL,
                            display_name != NULL ? display_name : "stdin.png");
  return TRUE;
}

//a screenshot that is not on disk yet. with to_folder saving it starts out in the screenshot folder,
//without it saving asks where, for when the folder just failed to take this very capture
void
waytator_window_open_capture(WaytatorWindow *self,
                             GdkTexture     *texture,
                             const char     *display_name,
                             gboolean        to_folder)
{
  g_return_if_fail(WAYTATOR_IS_WINDOW(self));
  g_return_if_fail(GDK_IS_TEXTURE(texture));

  waytator_window_set_image(self, texture, NULL, display_name);
  self->from_capture = to_folder;
  waytator_window_reset_save_button(self);
}

//nothing to lose here, a new image can use this window rather than one of its own
gboolean
waytator_window_is_empty(WaytatorWindow *self)
{
  g_return_val_if_fail(WAYTATOR_IS_WINDOW(self), FALSE);

  return self->texture == NULL && !self->capture_in_progress && !waytator_window_is_video(self);
}

//a new image never replaces one that is being worked on, it gets the first empty window or a fresh one
WaytatorWindow *
waytator_window_for_new_image(GtkApplication *app)
{
  for (GList *link = gtk_application_get_windows(app); link != NULL; link = link->next) {
    if (WAYTATOR_IS_WINDOW(link->data) && waytator_window_is_empty(WAYTATOR_WINDOW(link->data)))
      return WAYTATOR_WINDOW(link->data);
  }

  return waytator_window_new(ADW_APPLICATION(app));
}

static void
waytator_window_open_ready(GObject      *source_object,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  WaytatorWindow *target;

  file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source_object), result, &error);

  if (file == NULL) {
    if (!waytator_window_error_is_user_dismissed(error))
      g_printerr("Error opening file: %s\n", error->message);

    g_object_unref(self);
    return;
  }

  target = waytator_window_for_new_image(gtk_window_get_application(GTK_WINDOW(self)));
  if (waytator_window_open_file(target, file, &error)) {
    gtk_window_present(GTK_WINDOW(target));
  } else {
    //a window made just for this file has no reason to stay
    if (target != self && !gtk_widget_get_visible(GTK_WIDGET(target)))
      gtk_window_destroy(GTK_WINDOW(target));
    waytator_window_show_error(self, error->message);
  }

  g_object_unref(self);
}

static void
waytator_window_open(WaytatorWindow *self)
{
  static const char *const image_types[] = {
    "image/png", "image/jpeg", "image/webp", "image/gif", "image/bmp", "image/svg+xml",
  };
  g_autoptr(GtkFileDialog) dialog = NULL;
  g_autoptr(GtkFileFilter) everything = NULL;
  g_autoptr(GtkFileFilter) images = NULL;
  g_autoptr(GtkFileFilter) videos = NULL;
  g_autoptr(GListStore) filters = NULL;

  dialog = gtk_file_dialog_new();
  everything = gtk_file_filter_new();
  images = gtk_file_filter_new();
  videos = gtk_file_filter_new();
  filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

  gtk_file_filter_set_name(everything, "Images and videos");
  for (guint i = 0; i < G_N_ELEMENTS(image_types); i++)
    gtk_file_filter_add_mime_type(everything, image_types[i]);
  gtk_file_filter_add_mime_type(everything, "video/*");
  g_list_store_append(filters, everything);

  gtk_file_filter_set_name(videos, "Videos");
  gtk_file_filter_add_mime_type(videos, "video/*");

  gtk_file_filter_set_name(images, "Images");
  gtk_file_filter_add_mime_type(images, "image/png");
  gtk_file_filter_add_mime_type(images, "image/jpeg");
  gtk_file_filter_add_mime_type(images, "image/webp");
  gtk_file_filter_add_mime_type(images, "image/gif");
  gtk_file_filter_add_mime_type(images, "image/bmp");
  gtk_file_filter_add_mime_type(images, "image/svg+xml");
  g_list_store_append(filters, images);
  g_list_store_append(filters, videos);

  gtk_file_dialog_set_title(dialog, "Open");
  gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
  gtk_file_dialog_set_default_filter(dialog, everything);

  gtk_file_dialog_open(dialog,
                       GTK_WINDOW(self),
                       NULL,
                       waytator_window_open_ready,
                       g_object_ref(self));
}

static void
waytator_window_open_action(GtkWidget  *widget,
                            const char *action_name,
                            GVariant   *parameter)
{
  (void) action_name;
  (void) parameter;

  waytator_window_open(WAYTATOR_WINDOW(widget));
}

static void
waytator_window_update_capture_controls(WaytatorWindow *self)
{
  const WaytatorShortcut shortcut = WAYTATOR_SHORTCUT_CAPTURE_AREA;
  const char *accelerator;
  g_autofree char *label = NULL;
  g_autofree char *tooltip = NULL;
  guint keyval;
  GdkModifierType modifiers;

  if (self->capture_button == NULL)
    return;

  //a key that an earlier shortcut already took would be a lie
  accelerator = waytator_window_shortcut_accel(self, shortcut);
  if (waytator_window_parse_accelerator(accelerator, &keyval, &modifiers)
      && waytator_window_shortcut_conflict(self, shortcut, TRUE) < 0
      && waytator_window_fixed_shortcut_title(self, accelerator) == NULL)
    label = gtk_accelerator_get_label(gdk_keyval_to_upper(keyval), modifiers);

  //capturing always starts with an area, the overlay is where a screen can be picked instead
  gtk_label_set_text(self->capture_area_hint, label != NULL ? label : "");
  tooltip = label != NULL
          ? g_strdup_printf("%s (%s)", waytator_shortcuts[shortcut].title, label)
          : g_strdup(waytator_shortcuts[shortcut].title);
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->capture_button), tooltip);
}

//every waytator window, so none of them ends up in the picture. hidden collects them, most recently used first
gboolean
waytator_window_hide_all(GtkApplication *app,
                         GPtrArray      *hidden)
{
  g_autoptr(GList) windows = g_list_copy(gtk_application_get_windows(app));
  gboolean hid = FALSE;

  for (GList *link = windows; link != NULL; link = link->next) {
    if (!WAYTATOR_IS_WINDOW(link->data) || !gtk_widget_get_visible(GTK_WIDGET(link->data)))
      continue;

    g_ptr_array_add(hidden, g_object_ref(link->data));
    gtk_widget_set_visible(GTK_WIDGET(link->data), FALSE);
    hid = TRUE;
  }

  return hid;
}

gboolean
waytator_window_restore_all(GPtrArray *hidden)
{
  const gboolean restored = hidden->len > 0;

  //they were hidden most recently used first, so that one comes back last and ends up on top
  for (guint i = hidden->len; i > 0; i--)
    gtk_window_present(g_ptr_array_index(hidden, i - 1));

  g_ptr_array_set_size(hidden, 0);
  return restored;
}

//whether a capture would have waytator in it, which is when hiding it is worth offering
gboolean
waytator_window_any_visible(GtkApplication *app)
{
  for (GList *link = gtk_application_get_windows(app); link != NULL; link = link->next) {
    if (WAYTATOR_IS_WINDOW(link->data) && gtk_widget_get_visible(GTK_WIDGET(link->data)))
      return TRUE;
  }

  return FALSE;
}

gboolean
waytator_window_capture_busy(GtkApplication *app)
{
  if (waytator_screenshot_is_capturing() || waytator_recording_is_starting())
    return TRUE;

  for (GList *link = gtk_application_get_windows(app); link != NULL; link = link->next) {
    if (WAYTATOR_IS_WINDOW(link->data) && WAYTATOR_WINDOW(link->data)->capture_in_progress)
      return TRUE;
  }

  return FALSE;
}

static gboolean
waytator_window_hide_for_capture(WaytatorWindow *self)
{
  GtkApplication *app = gtk_window_get_application(GTK_WINDOW(self));

  if (app == NULL || self->capture_hidden_windows == NULL)
    return FALSE;

  return waytator_window_hide_all(app, self->capture_hidden_windows);
}

static gboolean
waytator_window_restore_after_capture(WaytatorWindow *self)
{
  //a capture can still finish after its window was closed
  if (self->capture_hidden_windows == NULL)
    return FALSE;

  return waytator_window_restore_all(self->capture_hidden_windows);
}

typedef struct {
  WaytatorWindow *window;
  guint serial;
} WaytatorCaptureStore;

static void
waytator_window_capture_stored(GFile      *file,
                               const char *save_error,
                               gboolean    copied,
                               gpointer    user_data)
{
  WaytatorCaptureStore *store = user_data;
  WaytatorWindow *self = store->window;

  (void) copied;

  //the window has moved on to another image since, this file is not its business any more
  if (store->serial == self->image_serial) {
    self->capture_storing = FALSE;
    if (file != NULL) {
      g_clear_object(&self->current_file);
      self->current_file = g_object_ref(file);
      g_free(self->source_name);
      self->source_name = g_file_get_basename(file);
      gtk_label_set_text(self->file_label, self->source_name);
      self->never_saved = FALSE;
    } else if (save_error != NULL) {
      g_autofree char *message = g_strdup_printf("%s Save to choose another place.", save_error);

      //the folder is not going to work this time, so saving asks instead
      self->from_capture = FALSE;
      waytator_window_show_error(self, message);
    }
    waytator_window_sync_state(self);
  }

  g_object_unref(self);
  g_free(store);
}

//every capture lands in the screenshot folder right away, saving afterwards updates that file
static void
waytator_window_store_capture(WaytatorWindow *self,
                              GdkTexture     *texture,
                              const char     *name)
{
  WaytatorCaptureStore *store;

  if (*self->settings.screenshot_save_dir == '\0' && !self->settings.screenshot_copy)
    return;

  store = g_new0(WaytatorCaptureStore, 1);
  store->window = g_object_ref(self);
  store->serial = self->image_serial;
  //a save now would race the one in the background for the name
  self->capture_storing = *self->settings.screenshot_save_dir != '\0';
  waytator_window_reset_save_button(self);
  waytator_screenshot_store(texture, name, &self->settings, waytator_window_capture_stored, store);
}

static void waytator_window_begin_capture(WaytatorWindow       *self,
                                          WaytatorCaptureMode   mode,
                                          const char           *output_name,
                                          guint                 delay_seconds,
                                          WaytatorCaptureEarly *early);

//a capture the portal turned down for want of a permission, kept to be taken again once it is given
typedef struct {
  WaytatorWindow *window;
  WaytatorCaptureMode mode;
  char *output_name;
  //the window only exists for this capture, as with --screenshot, and has nothing else to show
  gboolean temporary;
} WaytatorCapturePermission;

static void
waytator_capture_permission_free(WaytatorCapturePermission *permission)
{
  g_object_unref(permission->window);
  g_free(permission->output_name);
  g_free(permission);
}

static void
waytator_window_capture_allowed(GObject      *source_object,
                                GAsyncResult *result,
                                gpointer      user_data)
{
  WaytatorCapturePermission *permission = user_data;
  WaytatorWindow *self = permission->window;
  g_autoptr(GError) error = NULL;

  (void) source_object;

  if (self->disposed) {
    waytator_capture_permission_free(permission);
    return;
  }

  if (!waytator_capture_allow_finish(result, &error)) {
    g_autofree char *message = g_strdup_printf("Could not allow screenshots: %s", error->message);

    waytator_window_show_error(self, message);
    waytator_capture_permission_free(permission);
    return;
  }

  //back out of sight, so the capture goes the way it would have the first time
  if (permission->temporary)
    gtk_widget_set_visible(GTK_WIDGET(self), FALSE);
  waytator_window_begin_capture(self, permission->mode, permission->output_name, 0, NULL);
  waytator_capture_permission_free(permission);
}

static void
waytator_window_capture_permission_response(AdwAlertDialog            *dialog,
                                            const char                *response,
                                            WaytatorCapturePermission *permission)
{
  (void) dialog;

  if (g_strcmp0(response, "allow") == 0) {
    waytator_capture_allow_async(waytator_window_capture_allowed, permission);
    return;
  }

  if (permission->temporary && !permission->window->disposed)
    gtk_window_destroy(GTK_WINDOW(permission->window));
  waytator_capture_permission_free(permission);
}

static void
waytator_window_ask_capture_permission(WaytatorWindow     *self,
                                       WaytatorCaptureMode mode,
                                       const char         *output_name,
                                       gboolean            temporary)
{
  AdwDialog *dialog = adw_alert_dialog_new("Allow Screenshots?", NULL);
  WaytatorCapturePermission *permission = g_new0(WaytatorCapturePermission, 1);

  permission->window = g_object_ref(self);
  permission->mode = mode;
  permission->output_name = g_strdup(output_name);
  permission->temporary = temporary;

  adw_alert_dialog_set_body(ADW_ALERT_DIALOG(dialog),
                            "Waytator needs a one-time permission to take screenshots. "
                            "Once it is allowed, the screenshot is taken again.");
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", "Not Now", "allow", "Allow", NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "allow", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "allow");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  //every way of closing the dialog ends in a response, which is what lets go of the permission
  g_signal_connect(dialog, "response", G_CALLBACK(waytator_window_capture_permission_response), permission);

  gtk_window_present(GTK_WINDOW(self));
  adw_dialog_present(dialog, GTK_WIDGET(self));
}

//a screenshot from the window that skipped the editor, kept for Edit on its toast
typedef struct {
  WaytatorWindow *window;
  GdkTexture *texture;
  //set once it is saved
  GFile *file;
  char *name;
} WaytatorQuietCapture;

static void
waytator_quiet_capture_free(gpointer user_data)
{
  WaytatorQuietCapture *capture = user_data;

  g_object_unref(capture->window);
  g_object_unref(capture->texture);
  g_clear_object(&capture->file);
  g_free(capture->name);
  g_free(capture);
}

//the window that asked keeps its image, a capture only takes its place when it has none
static WaytatorWindow *
waytator_window_target_for_capture(WaytatorWindow *self)
{
  if (self->texture != NULL)
    return waytator_window_for_new_image(gtk_window_get_application(GTK_WINDOW(self)));

  return self;
}

static void
waytator_window_quiet_capture_edit(AdwToast *toast,
                                   gpointer  user_data)
{
  WaytatorQuietCapture *capture = user_data;
  WaytatorWindow *target = waytator_window_target_for_capture(capture->window);
  g_autoptr(GError) error = NULL;

  (void) toast;

  //the saved file may be gone by now, the capture itself is still around
  if (capture->file == NULL || !waytator_window_open_file(target, capture->file, &error))
    waytator_window_open_capture(target, capture->texture, capture->name, TRUE);
  gtk_window_present(GTK_WINDOW(target));
}

static void
waytator_window_show_quiet_capture(WaytatorQuietCapture *capture,
                                   const char           *title)
{
  AdwToast *toast = adw_toast_new(title);

  adw_toast_set_button_label(toast, "Edit");
  //goes when the toast does
  g_object_set_data_full(G_OBJECT(toast), "capture", capture, waytator_quiet_capture_free);
  g_signal_connect(toast, "button-clicked", G_CALLBACK(waytator_window_quiet_capture_edit), capture);
  adw_toast_overlay_add_toast(capture->window->toast_overlay, toast);
}

static void
waytator_window_quiet_capture_stored(GFile      *file,
                                     const char *save_error,
                                     gboolean    copied,
                                     gpointer    user_data)
{
  WaytatorQuietCapture *capture = user_data;
  WaytatorWindow *self = capture->window;

  if (self->disposed) {
    waytator_quiet_capture_free(capture);
    return;
  }

  //it ended up nowhere, so it goes to the editor after all. a folder that failed is not offered again
  if (file == NULL && !copied) {
    WaytatorWindow *target = waytator_window_target_for_capture(self);

    waytator_window_open_capture(target, capture->texture, capture->name, save_error == NULL);
    gtk_window_present(GTK_WINDOW(target));
    if (save_error != NULL) {
      g_autofree char *message = g_strdup_printf("%s Save to choose another place.", save_error);

      waytator_window_show_error(target, message);
    }
    waytator_quiet_capture_free(capture);
    return;
  }

  //copied, but the folder did not take it
  if (save_error != NULL)
    waytator_window_show_error(self, save_error);

  capture->file = file != NULL ? g_object_ref(file) : NULL;
  waytator_window_show_quiet_capture(capture, file != NULL ? "Screenshot saved" : "Screenshot copied");
}

//saved and copied as the preferences say, with the window left on what it shows. file is set when
//GNOME's screenshot UI already did both. FALSE when the screenshot would end up nowhere, so it needs the editor
static gboolean
waytator_window_capture_quietly(WaytatorWindow *self,
                                GdkTexture     *texture,
                                GFile          *file,
                                const char     *name)
{
  WaytatorQuietCapture *capture;

  if (file == NULL && *self->settings.screenshot_save_dir == '\0' && !self->settings.screenshot_copy)
    return FALSE;

  capture = g_new0(WaytatorQuietCapture, 1);
  capture->window = g_object_ref(self);
  capture->texture = g_object_ref(texture);
  capture->name = g_strdup(name);

  if (file != NULL) {
    capture->file = g_object_ref(file);
    waytator_window_show_quiet_capture(capture, "Screenshot saved");
  } else {
    waytator_screenshot_store(texture, name, &self->settings, waytator_window_quiet_capture_stored, capture);
  }

  return TRUE;
}

//file is set when GNOME's screenshot UI took the capture and already saved and copied it
static void
waytator_window_finish_capture(WaytatorWindow *self,
                               GdkTexture     *texture,
                               GFile          *file,
                               const GError   *error)
{
  WaytatorWindow *target = self;

  //closed while the capture was on screen, the capture goes with it
  if (self->disposed)
    return;

  self->capture_in_progress = FALSE;
  waytator_window_restore_after_capture(self);

  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)) {
    waytator_window_ask_capture_permission(self,
                                           self->capture_mode,
                                           self->capture_output_name,
                                           !self->capture_window_was_visible && self->texture == NULL);
    g_clear_pointer(&self->capture_output_name, g_free);
    return;
  }

  g_clear_pointer(&self->capture_output_name, g_free);

  if (texture == NULL && error == NULL && !self->capture_window_was_visible && self->texture == NULL) {
    //cancelled, and the window only exists because the command line asked for a capture
    gtk_window_destroy(GTK_WINDOW(self));
    return;
  }

  if (texture != NULL) {
    g_autofree char *name = file != NULL ? g_file_get_basename(file) : waytator_screenshot_make_name();

    //a window that only exists for this capture has nothing to stay on, it goes to the editor whatever the switch says
    if (!self->settings.screenshot_open_editor
        && self->capture_window_was_visible
        && waytator_window_capture_quietly(self, texture, file, name))
      return;

    target = waytator_window_target_for_capture(self);

    if (file != NULL) {
      //GNOME saved and copied it already, saving edits updates its file
      waytator_window_set_image(target, texture, file, name);
      waytator_window_reset_save_button(target);
    } else {
      waytator_window_open_capture(target, texture, name, TRUE);
    }
    gtk_window_present(GTK_WINDOW(target));
    if (file == NULL)
      waytator_window_store_capture(target, texture, name);
  }

  if (error != NULL) {
    gtk_window_present(GTK_WINDOW(self));
    waytator_window_show_error(self, error->message);
  }
}

static void
waytator_window_capture_overlay_done(GdkTexture         *texture,
                                     WaytatorCaptureMode mode,
                                     gpointer            user_data)
{
  g_autoptr(WaytatorWindow) self = WAYTATOR_WINDOW(user_data);

  (void) mode;

  waytator_window_finish_capture(self, texture, NULL, NULL);
}

static void waytator_window_capture_retake(gboolean            hide_waytator,
                                           WaytatorCaptureMode mode,
                                           guint               delay_seconds,
                                           gpointer            user_data);

static void
waytator_window_capture_ready(GObject      *source_object,
                              GAsyncResult *result,
                              gpointer      user_data)
{
  g_autoptr(WaytatorWindow) self = WAYTATOR_WINDOW(user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) outputs = waytator_capture_outputs_finish(result, &error);
  WaytatorCaptureOutput *output = NULL;

  (void) source_object;

  if (self->disposed)
    return;

  if (outputs == NULL) {
    //the portal reports a declined permission prompt as a cancellation
    waytator_window_finish_capture(self,
                                   NULL,
                                   NULL,
                                   g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ? NULL : error);
    return;
  }

  //a retake comes from the overlay, which should come back rather than turn into a capture
  if (!self->capture_retake)
    output = waytator_capture_pick_output(outputs, self->capture_mode, self->capture_output_name, &error);
  if (error != NULL) {
    waytator_window_finish_capture(self, NULL, NULL, error);
    return;
  }

  if (output != NULL) {
    g_autoptr(GdkTexture) texture = waytator_capture_output_create_texture(output);

    waytator_window_finish_capture(self, texture, NULL, NULL);
    return;
  }

  //the box only means something when there is a waytator window that would be in the picture
  if (waytator_capture_overlay_show(g_steal_pointer(&outputs),
                                    self->capture_mode,
                                    self->capture_hidden_windows->len > 0
                                      || waytator_window_any_visible(gtk_window_get_application(GTK_WINDOW(self))),
                                    self->capture_hide,
                                    waytator_window_capture_retake,
                                    waytator_window_capture_overlay_done,
                                    self)) {
    g_object_ref(self);
    return;
  }

  g_set_error_literal(&error,
                      G_IO_ERROR,
                      G_IO_ERROR_FAILED,
                      "Could not match the capture to a screen");
  waytator_window_finish_capture(self, NULL, NULL, error);
}

static void
waytator_window_gnome_capture_ready(GObject      *source_object,
                                    GAsyncResult *result,
                                    gpointer      user_data)
{
  g_autoptr(WaytatorWindow) self = WAYTATOR_WINDOW(user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GdkTexture) texture = waytator_capture_gnome_ui_finish(result, &file, &error);

  (void) source_object;

  if (self->disposed)
    return;

  //closing GNOME's UI without taking anything is a cancel like any other
  waytator_window_finish_capture(self,
                                 texture,
                                 file,
                                 g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ? NULL : error);
}

static gboolean
waytator_window_capture_after_wait(gpointer user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  self->capture_delay_id = 0;
  if (waytator_capture_uses_gnome_ui()) {
    waytator_capture_gnome_ui_async(waytator_window_gnome_capture_ready, g_object_ref(self));
  } else if (self->capture_early != NULL) {
    waytator_capture_early_take_async(self->capture_early, waytator_window_capture_ready, g_object_ref(self));
    g_clear_pointer(&self->capture_early, waytator_capture_early_unref);
  } else {
    waytator_capture_outputs_async(NULL, waytator_window_capture_ready, g_object_ref(self));
  }
  return G_SOURCE_REMOVE;
}

static void
waytator_window_capture_counted(gboolean finished,
                                gpointer user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  self->capture_countdown = NULL;
  if (finished)
    waytator_window_capture_after_wait(self);
  else
    waytator_window_finish_capture(self, NULL, NULL, NULL);
}

//wait is for whatever else has to leave the screen first, like the overlay of a retake,
//the delay is the one someone asked for and is long enough for that on its own
static void
waytator_window_take_capture(WaytatorWindow *self,
                             gboolean        wait,
                             guint           delay_seconds)
{
  guint delay_ms = delay_seconds * 1000;

  if (self->capture_hide)
    wait |= waytator_window_hide_for_capture(self);
  else
    wait |= waytator_window_restore_after_capture(self);

  if (wait)
    delay_ms = MAX(delay_ms, WAYTATOR_CAPTURE_SETTLE_MS);

  //taken before anything left the screen, so it would show what the wait is for
  if (delay_ms != 0)
    g_clear_pointer(&self->capture_early, waytator_capture_early_unref);

  if (delay_ms == 0) {
    waytator_window_capture_after_wait(self);
    return;
  }

  //a delay someone asked for counts down on screen, and is gone from it before the frame is taken
  if (delay_seconds > 0) {
    self->capture_countdown = waytator_countdown_show(NULL, NULL, delay_seconds, waytator_window_capture_counted, self);
    return;
  }

  self->capture_delay_id = g_timeout_add(delay_ms, waytator_window_capture_after_wait, self);
}

static void
waytator_window_capture_retake(gboolean            hide_waytator,
                               WaytatorCaptureMode mode,
                               guint               delay_seconds,
                               gpointer            user_data)
{
  g_autoptr(WaytatorWindow) self = WAYTATOR_WINDOW(user_data);

  if (self->disposed)
    return;

  //the box is for this capture only, the preference stays as it was
  self->capture_hide = hide_waytator;
  self->capture_mode = mode;
  self->capture_retake = TRUE;
  waytator_window_take_capture(self, TRUE, delay_seconds);
}

static void
waytator_window_begin_capture(WaytatorWindow       *self,
                              WaytatorCaptureMode   mode,
                              const char           *output_name,
                              guint                 delay_seconds,
                              WaytatorCaptureEarly *early)
{
  GtkApplication *app = gtk_window_get_application(GTK_WINDOW(self));

  if (self->capture_in_progress || (app != NULL && waytator_window_capture_busy(app))) {
    if (early != NULL)
      waytator_capture_early_unref(early);
    return;
  }

  self->capture_in_progress = TRUE;
  self->capture_window_was_visible = gtk_widget_get_visible(GTK_WIDGET(self));
  self->capture_hide = self->settings.capture_hide_waytator;
  self->capture_retake = FALSE;
  self->capture_output_name = g_strdup(output_name);
  self->capture_mode = mode;
  self->capture_early = early;
  waytator_window_take_capture(self, FALSE, delay_seconds);
}

//for the command line
void
waytator_window_start_capture(WaytatorWindow       *self,
                              WaytatorCaptureMode   mode,
                              const char           *output_name,
                              guint                 delay_seconds,
                              WaytatorCaptureEarly *early)
{
  g_return_if_fail(WAYTATOR_IS_WINDOW(self));

  waytator_window_begin_capture(self, mode, output_name, delay_seconds, early);
}

static void
waytator_window_capture_action(GtkWidget  *widget,
                               const char *action_name,
                               GVariant   *parameter)
{
  (void) parameter;

  //a delay is picked in the capture overlay, or given with --delay
  waytator_window_begin_capture(WAYTATOR_WINDOW(widget),
                                g_strcmp0(action_name, "win.capture-screen") == 0
                                  ? WAYTATOR_CAPTURE_SCREEN
                                  : WAYTATOR_CAPTURE_AREA,
                                NULL,
                                0,
                                NULL);
}

//the finished recording opens in a window of its own, the one that asked for it may be busy with something else
static void
waytator_window_recording_edit(GFile   *file,
                               gpointer user_data)
{
  WaytatorWindow *window = waytator_window_for_new_image(GTK_APPLICATION(user_data));
  g_autoptr(GError) error = NULL;

  if (!waytator_window_open_file(window, file, &error)) {
    g_printerr("Error opening recording: %s\n", error->message);
    //a window made just for this file has no reason to stay
    if (!gtk_widget_get_visible(GTK_WIDGET(window)))
      gtk_window_destroy(GTK_WINDOW(window));
    return;
  }

  gtk_window_present(GTK_WINDOW(window));
}

//a recording started from a window, for what its end leads to
typedef struct {
  GtkApplication *app;
  GWeakRef window;
  gboolean open_editor;
} WaytatorWindowRecording;

static void
waytator_window_recording_free(gpointer user_data)
{
  WaytatorWindowRecording *recording = user_data;

  g_object_unref(recording->app);
  g_weak_ref_clear(&recording->window);
  g_free(recording);
}

static void
waytator_window_recording_toast_edit(AdwToast       *toast,
                                     WaytatorWindow *self)
{
  waytator_window_recording_edit(g_object_get_data(G_OBJECT(toast), "file"),
                                 gtk_window_get_application(GTK_WINDOW(self)));
}

//with the editor switched off the window that asked says where it went, and once it is closed nothing does
static void
waytator_window_recording_done(GFile   *file,
                               gpointer user_data)
{
  WaytatorWindowRecording *recording = user_data;
  g_autoptr(WaytatorWindow) self = NULL;
  AdwToast *toast;

  if (recording->open_editor) {
    waytator_window_recording_edit(file, recording->app);
    return;
  }

  self = g_weak_ref_get(&recording->window);
  if (self == NULL || self->disposed)
    return;

  toast = adw_toast_new("Recording saved");
  adw_toast_set_button_label(toast, "Edit");
  g_object_set_data_full(G_OBJECT(toast), "file", g_object_ref(file), g_object_unref);
  g_signal_connect(toast, "button-clicked", G_CALLBACK(waytator_window_recording_toast_edit), self);
  adw_toast_overlay_add_toast(self->toast_overlay, toast);
}

//also what stops a recording, the record button and the folded menu offer the same action then
static void
waytator_window_record_action(GtkWidget  *widget,
                              const char *action_name,
                              GVariant   *parameter)
{
  GApplication *app = G_APPLICATION(gtk_window_get_application(GTK_WINDOW(widget)));
  WaytatorRecordingRequest request = {
    .mode = WAYTATOR_CAPTURE_AREA,
  };
  WaytatorWindowRecording *recording;

  (void) action_name;
  (void) parameter;

  //stopping one is always fine, starting one over a capture still on screen is not
  if (!waytator_recording_is_active() && waytator_window_capture_busy(GTK_APPLICATION(app)))
    return;

  recording = g_new0(WaytatorWindowRecording, 1);
  recording->app = GTK_APPLICATION(g_object_ref(app));
  g_weak_ref_init(&recording->window, widget);
  recording->open_editor = WAYTATOR_WINDOW(widget)->settings.recording_open_editor;
  waytator_recording_toggle(app, &request, waytator_window_recording_done, recording, waytator_window_recording_free);
}

static void
waytator_window_open_current_file_ready(GObject      *source_object,
                                        GAsyncResult *result,
                                        gpointer      user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  g_autoptr(GError) error = NULL;

  if (!gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(source_object), result, &error)
      && !waytator_window_error_is_user_dismissed(error))
    waytator_window_show_error(self, error->message);

  g_object_unref(self);
}

static void
waytator_window_open_current_file_action(GtkWidget  *widget,
                                         const char *action_name,
                                         GVariant   *parameter)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(widget);
  GFile *file = waytator_window_get_shown_file(self);
  g_autoptr(GtkFileLauncher) launcher = NULL;

  (void) action_name;
  (void) parameter;

  if (file == NULL)
    return;

  launcher = gtk_file_launcher_new(file);
  gtk_file_launcher_launch(launcher,
                           GTK_WINDOW(self),
                           NULL,
                           waytator_window_open_current_file_ready,
                           g_object_ref(self));
}

static void
waytator_window_open_parent_folder(WaytatorWindow *self)
{
  GFile *file = waytator_window_get_shown_file(self);
  g_autoptr(GtkFileLauncher) launcher = NULL;
  g_autoptr(GFile) parent = NULL;

  if (file == NULL)
    return;

  parent = g_file_get_parent(file);
  if (parent == NULL)
    return;

  launcher = gtk_file_launcher_new(parent);
  gtk_file_launcher_launch(launcher,
                           GTK_WINDOW(self),
                           NULL,
                           waytator_window_open_current_file_ready,
                           g_object_ref(self));
}

static void
waytator_window_open_containing_folder_action(GtkWidget  *widget,
                                              const char *action_name,
                                              GVariant   *parameter)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(widget);
  GFile *file = waytator_window_get_shown_file(self);
  g_autoptr(GDBusConnection) bus = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *uri = NULL;

  (void) action_name;
  (void) parameter;

  if (file == NULL)
    return;

  uri = g_file_get_uri(file);
  if (uri == NULL)
    return;

  bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  if (bus == NULL) {
    g_clear_error(&error);
    waytator_window_open_parent_folder(self);
    return;
  }
  //if a file manager supporting the FileManager1 D-Bus interface is available
  //use it to show the file in its containing folder, otherwise
  //fall back to opening the parent folder without highlighting the file
  if (!g_dbus_connection_call_sync(bus,
                                   "org.freedesktop.FileManager1",
                                   "/org/freedesktop/FileManager1",
                                   "org.freedesktop.FileManager1",
                                   "ShowItems",
                                   g_variant_new("(^ass)", (const char *[]) {uri, NULL}, ""),
                                   NULL,
                                   G_DBUS_CALL_FLAGS_NONE,
                                   -1,
                                   NULL,
                                   &error)) {
    g_clear_error(&error);
    waytator_window_open_parent_folder(self);
  }
}

static void
waytator_window_dispose(GObject *object)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(object);

  //a capture or a background save can still call back, this tells them the window is gone
  self->disposed = TRUE;
  self->image_serial++;
  self->capture_in_progress = FALSE;
  self->capture_storing = FALSE;
  //whatever this capture hid should not stay hidden with nobody left to bring it back
  for (guint i = 0; self->capture_hidden_windows != NULL && i < self->capture_hidden_windows->len; i++) {
    GtkWindow *window = g_ptr_array_index(self->capture_hidden_windows, i);

    if (window != GTK_WINDOW(self))
      gtk_window_present(window);
  }

  g_clear_object(&self->current_file);
  g_clear_pointer(&self->source_name, g_free);
  g_clear_object(&self->texture);
  if (self->image_surface != NULL) {
    cairo_surface_destroy(self->image_surface);
    self->image_surface = NULL;
  }
  waytator_window_clear_ocr_results(self);
  waytator_window_clear_annotations(self);
  if (self->copy_feedback_timeout_id != 0)
    g_source_remove(self->copy_feedback_timeout_id);
  if (self->save_spinner_timeout_id != 0)
    g_source_remove(self->save_spinner_timeout_id);
  if (self->save_feedback_timeout_id != 0)
    g_source_remove(self->save_feedback_timeout_id);
  if (self->capture_delay_id != 0)
    g_source_remove(self->capture_delay_id);
  self->capture_delay_id = 0;
  g_clear_pointer(&self->capture_countdown, waytator_countdown_cancel);
  g_clear_pointer(&self->capture_early, waytator_capture_early_unref);
  g_clear_pointer(&self->capture_output_name, g_free);
  g_clear_pointer(&self->capture_hidden_windows, g_ptr_array_unref);
  g_clear_object(&self->adopt_file);
  g_clear_object(&self->save_menu);
  g_clear_object(&self->capture_menu);
  g_clear_handle_id(&self->record_tick_id, g_source_remove);
  g_clear_pointer(&self->save_menu_key, g_free);
  g_clear_handle_id(&self->chrome_layout_id, g_source_remove);
  g_clear_handle_id(&self->zoom_osd_timeout_id, g_source_remove);
  g_clear_object(&self->app_menu);
  g_clear_object(&self->zoom_menu_row);
  g_clear_pointer(&self->app_menu_key, g_free);
  waytator_settings_clear(&self->settings);
  waytator_window_clear_history(self);
  g_clear_pointer(&self->document, waytator_document_free);
  g_clear_object(&self->start_window_controls_children);
  g_clear_object(&self->end_window_controls_children);
  if (self->window_css_provider != NULL && gdk_display_get_default() != NULL)
    gtk_style_context_remove_provider_for_display(gdk_display_get_default(),
                                                  GTK_STYLE_PROVIDER(self->window_css_provider));
  g_clear_object(&self->window_css_provider);
  if (self->widget_css_provider != NULL && gdk_display_get_default() != NULL)
    gtk_style_context_remove_provider_for_display(gdk_display_get_default(),
                                                  GTK_STYLE_PROVIDER(self->widget_css_provider));
  g_clear_object(&self->widget_css_provider);
  for (int i = 0; i < WAYTATOR_N_SHORTCUTS; i++) {
    g_clear_pointer(&self->shortcut_accels[i], g_free);
    g_clear_pointer(&self->shortcut_overrides[i], g_free);
    g_clear_weak_pointer(&self->shortcut_rows[i]);
  }
  g_clear_weak_pointer(&self->palette_group);
  g_clear_weak_pointer(&self->size_focus_before);
  g_clear_weak_pointer(&self->palette_add_button);
  g_clear_pointer(&self->saved_path, g_free);
  g_clear_pointer(&self->active_touch_sequences, g_hash_table_unref);
  g_clear_pointer(&self->touch_tap_points, g_hash_table_unref);

  G_OBJECT_CLASS(waytator_window_parent_class)->dispose(object);
}

static void
waytator_window_bind_template_children(GtkWidgetClass *widget_class)
{
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, canvas_stack);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, canvas_view);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, empty_page);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, canvas_surface);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, picture);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, drawing_area);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_overlay);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_panel_bottom_sheet);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_panel);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_panel_toggle_container);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_panel_toggle_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_panel_close_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_panel_stack);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_panel_tabs);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_selected_page);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_all_page);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_selected_text_view);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_all_text_view);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, start_actions);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, start_window_controls_pill);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, start_window_controls);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, open_actions);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, capture_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, record_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, record_icon);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, record_time);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, capture_area_hint);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, open_hint);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, file_group);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, file_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, file_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, zoom_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, end_actions);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, top_bar);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, bottom_bar);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, dock_box);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, options_row);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, chrome_watcher);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, open_label_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, open_menu_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, open_separator);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, crop_actions);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, zoom_out_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, zoom_in_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, zoom_drawer);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, zoom_drawer_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, zoom_drawer_fit_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, zoom_osd);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, tool_group);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, pan_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, crop_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, brush_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, highlighter_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, eraser_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, shapes_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, shapes_popover);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, rectangle_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, circle_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, line_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, arrow_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, ocr_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, text_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, blur_tool_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, rotate_counter_clockwise_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, flip_horizontal_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, flip_vertical_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, history_actions);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, undo_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, redo_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, document_actions);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, save_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, save_as_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, save_icon_stack);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, save_default_icon);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, save_working_icon);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, save_success_icon);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, copy_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, copy_keep_open_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, app_menu_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, end_window_controls);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, copy_icon_stack);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, copy_default_icon);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, copy_success_icon);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, copy_keep_open_icon_stack);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, copy_keep_open_default_icon);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, copy_keep_open_success_icon);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, zoom_group);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, fit_zoom_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, settings_group);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, color_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, fill_color_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, width_scale);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, size_drawer);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, size_presets);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, size_readout);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, size_value_box);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, size_value_stack);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, size_value_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, size_value_entry);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, size_drawer_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, text_size_spin);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, blur_type_dropdown);
}

static void
waytator_window_install_actions(GtkWidgetClass *widget_class)
{
  gtk_widget_class_install_action(widget_class, "win.open", NULL, waytator_window_open_action);
  gtk_widget_class_install_action(widget_class, "win.capture-area", NULL, waytator_window_capture_action);
  gtk_widget_class_install_action(widget_class, "win.capture-screen", NULL, waytator_window_capture_action);
  gtk_widget_class_install_action(widget_class, "win.record-area", NULL, waytator_window_record_action);
  gtk_widget_class_install_action(widget_class, "win.open-current-file", NULL, waytator_window_open_current_file_action);
  gtk_widget_class_install_action(widget_class, "win.open-containing-folder", NULL, waytator_window_open_containing_folder_action);
  gtk_widget_class_install_action(widget_class, "win.copy-buffer", NULL, waytator_window_copy_action);
  gtk_widget_class_install_action(widget_class, "win.copy-keep-open", NULL, waytator_window_copy_keep_open_action);
  gtk_widget_class_install_action(widget_class, "win.dismiss", NULL, waytator_window_dismiss_action);
  gtk_widget_class_install_action(widget_class, "win.close-window", NULL, waytator_window_close_window_action);
  gtk_widget_class_install_action(widget_class, "win.save", NULL, waytator_window_save_overwrite_action);
  gtk_widget_class_install_action(widget_class, "win.save-copy", NULL, waytator_window_save_copy_action);
  gtk_widget_class_install_action(widget_class, "win.save-as", NULL, waytator_window_save_as_action);
  gtk_widget_class_install_action(widget_class, "win.save-image", NULL, waytator_window_save_image_action);
  gtk_widget_class_install_action(widget_class, "win.save-and-close", NULL, waytator_window_save_and_close_action);
  gtk_widget_class_install_action(widget_class, "win.save-copy-and-close", NULL, waytator_window_save_copy_and_close_action);
  gtk_widget_class_install_action(widget_class, "win.rotate-counter-clockwise", NULL, waytator_window_rotate_counter_clockwise_action);
  gtk_widget_class_install_action(widget_class, "win.flip-horizontal", NULL, waytator_window_flip_horizontal_action);
  gtk_widget_class_install_action(widget_class, "win.flip-vertical", NULL, waytator_window_flip_vertical_action);
  gtk_widget_class_install_action(widget_class, "win.preferences", NULL, waytator_window_preferences_action);
  gtk_widget_class_install_action(widget_class, "win.about", NULL, waytator_window_about_action);
  waytator_window_install_canvas_actions(widget_class);
}

static void
waytator_window_init_state(WaytatorWindow *self)
{
  self->zoom = 1.0;
  self->fit_mode = TRUE;
  //nothing folded until the window has a size to fold for
  self->folds = (WaytatorChromeFolds) {
    .title = TRUE,
    .zoom = WAYTATOR_ZOOM_FULL,
    .save = TRUE,
    .keep_open = TRUE,
  };
  self->chrome_widths_stale = TRUE;
  self->active_tool = WAYTATOR_TOOL_BRUSH;
  self->drawing = FALSE;
  self->document = waytator_document_new();
  self->active_touch_sequences = g_hash_table_new(g_direct_hash, g_direct_equal);
  self->touch_tap_points = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
  self->ocr_lines = NULL;
  self->selected_ocr_line = NULL;
  self->ocr_all_text = NULL;
  self->pinch_start_zoom = 1.0;
  self->pointer_in = FALSE;
  self->eraser_style = WAYTATOR_ERASER_STYLE_DUAL_RING;
  self->window_background_mode = WAYTATOR_WINDOW_BACKGROUND_OPAQUE;
  self->window_background_opacity = 0.8;
  self->angle_snap_modifiers = GDK_SHIFT_MASK;
  self->allow_highlighter_overlap = TRUE;
  self->auto_copy_latest_change = FALSE;
  self->default_primary_color = (GdkRGBA){0.96, 0.2, 0.28, 1.0};
  self->default_highlighter_color = (GdkRGBA){1.0, 0.91, 0.2, 1.0};
  self->default_fill_color = (GdkRGBA){0.96, 0.2, 0.28, 0.0};
  self->close_on_copy = FALSE;
  self->close_on_copy_override = -1;
  self->n_palette_colors = WAYTATOR_DEFAULT_N_PALETTE_COLORS;
  for (guint i = 0; i < self->n_palette_colors; i++)
    self->palette_colors[i] = waytator_default_palette[i];
  for (int i = 0; i < WAYTATOR_N_SHORTCUTS; i++)
    waytator_window_apply_shortcut(self, i, waytator_shortcuts[i].default_accel);
  waytator_settings_init(&self->settings);
  self->capture_hidden_windows = g_ptr_array_new_with_free_func(g_object_unref);
  waytator_window_load_preferences(self);

  for (int i = 0; i <= WAYTATOR_TOOL_BLUR; i++)
    self->tool_widths[i] = waytator_tool_width(i);

  waytator_window_apply_default_tool_colors(self);
}

static void
waytator_window_setup_window_background(WaytatorWindow *self)
{
  self->window_css_provider = gtk_css_provider_new();
  self->widget_css_provider = gtk_css_provider_new();
  gtk_widget_add_css_class(GTK_WIDGET(self), "waytator-window");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                             GTK_STYLE_PROVIDER(self->window_css_provider),
                                             WAYTATOR_WINDOW_STYLE_PROVIDER_PRIORITY); 
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                             GTK_STYLE_PROVIDER(self->widget_css_provider),
                                             WAYTATOR_WINDOW_STYLE_PROVIDER_PRIORITY);
  waytator_window_update_window_background(self);
  waytator_window_update_widget_appearance(self);
}

static void
waytator_window_ensure_icons_registered(void)
{
  static gboolean icons_registered = FALSE;

  if (!icons_registered) {
    GtkIconTheme *icon_theme = gtk_icon_theme_get_for_display(gdk_display_get_default());

    gtk_icon_theme_add_resource_path(icon_theme, WAYTATOR_RESOURCE_PREFIX "/icons/hicolor");
    icons_registered = TRUE;
  }
}

//the capture overlay can show without any window, as with --notify, and its icons are waytator's too
void
waytator_window_ensure_css_loaded(void)
{
  static gboolean css_loaded = FALSE;

  waytator_window_ensure_icons_registered();

  if (!css_loaded) {
    GtkCssProvider *provider = gtk_css_provider_new();

    gtk_css_provider_load_from_resource(provider, WAYTATOR_RESOURCE_PREFIX "/ui/style.css");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    css_loaded = TRUE;
  }
}

static void
waytator_window_setup_ocr_panel(WaytatorWindow *self)
{
  gtk_text_view_set_monospace(self->ocr_selected_text_view, FALSE);
  gtk_text_view_set_monospace(self->ocr_all_text_view, FALSE);
  gtk_stack_page_set_name(gtk_stack_get_page(self->ocr_panel_stack, GTK_WIDGET(self->ocr_selected_page)), "selected");
  gtk_stack_page_set_title(gtk_stack_get_page(self->ocr_panel_stack, GTK_WIDGET(self->ocr_selected_page)), "Selected region");
  gtk_stack_page_set_name(gtk_stack_get_page(self->ocr_panel_stack, GTK_WIDGET(self->ocr_all_page)), "all");
  gtk_stack_page_set_title(gtk_stack_get_page(self->ocr_panel_stack, GTK_WIDGET(self->ocr_all_page)), "All detected text");
  gtk_stack_set_visible_child_name(self->ocr_panel_stack, "all");
}

static void
waytator_window_setup_window_controls(WaytatorWindow *self)
{
  self->start_window_controls_children = gtk_widget_observe_children(GTK_WIDGET(self->start_window_controls));
  self->end_window_controls_children = gtk_widget_observe_children(GTK_WIDGET(self->end_window_controls));

  g_signal_connect(self->start_window_controls_children,
                   "items-changed",
                   G_CALLBACK(waytator_window_window_controls_changed),
                   self);
  g_signal_connect(self->end_window_controls_children,
                   "items-changed",
                   G_CALLBACK(waytator_window_window_controls_changed),
                   self);

  waytator_window_update_window_controls(self);
}

static void
waytator_window_setup_menu_button_popover(GtkMenuButton   *button,
                                          GtkArrowType     direction,
                                          GtkPositionType  position)
{
  GtkPopover *popover;

  gtk_menu_button_set_direction(button, direction);

  popover = gtk_menu_button_get_popover(button);
  if (popover == NULL) {
    GMenuModel *model = gtk_menu_button_get_menu_model(button);

    if (model != NULL) {
      GtkWidget *popover_widget = gtk_popover_menu_new_from_model(model);

      gtk_menu_button_set_popover(button, popover_widget);
      popover = GTK_POPOVER(popover_widget);
    }
  }

  if (popover == NULL)
    return;

  gtk_popover_set_has_arrow(popover, FALSE);
  gtk_popover_set_position(popover, position);
}

//not bound to the action in the template, the action being enabled would make the button sensitive again
static void
waytator_window_save_as_clicked(GtkButton *button,
                                gpointer   user_data)
{
  (void) button;

  gtk_widget_activate_action(GTK_WIDGET(user_data), "win.save-image", NULL);
}

//a menu item can't change its label in place, so while a recording is under way Stop recording takes the
//place of Record. it is the same action, which stops a recording that is going
static void
waytator_window_update_capture_menu(WaytatorWindow *self)
{
  g_menu_remove_all(self->capture_menu);
  g_menu_append(self->capture_menu, "Screenshot", "win.capture-area");
  g_menu_append(self->capture_menu,
                waytator_recording_is_active() ? "Stop recording" : "Record",
                "win.record-area");
}

//the menu is brought up to date as it opens, it doesn't follow a recording that starts or ends while it is open
static void
waytator_window_open_menu_opened(GtkMenuButton  *button,
                                 GParamSpec     *pspec,
                                 WaytatorWindow *self)
{
  (void) pspec;

  if (gtk_menu_button_get_active(button))
    waytator_window_update_capture_menu(self);
}

static gboolean
waytator_window_record_tick(gpointer user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  g_autofree char *text = waytator_recording_bar_format_time(g_get_monotonic_time()
                                                             - waytator_recording_get_start_time());
  //the time only grows a digit now and then, which is when the bar has to make room for it
  const gboolean longer = strlen(text) != strlen(gtk_label_get_text(self->record_time));

  gtk_label_set_text(self->record_time, text);
  if (longer)
    waytator_window_queue_chrome_layout(self);
  return G_SOURCE_CONTINUE;
}

//while a recording is under way the record button stops it, and says for how long it has been going
static void
waytator_window_update_recording(WaytatorWindow *self)
{
  const gboolean active = waytator_recording_is_active();
  const gboolean recording = waytator_recording_get_start_time() != 0;
  //once it is being saved there is nothing left to stop
  const gboolean saving = active && !recording && !waytator_recording_is_starting();

  gtk_image_set_from_icon_name(self->record_icon, active ? "media-playback-stop-symbolic" : "camera-video-symbolic");
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->record_button), active ? "Stop recording" : "Record");
  gtk_widget_set_sensitive(GTK_WIDGET(self->record_button), !saving);
  if (active)
    gtk_widget_add_css_class(GTK_WIDGET(self->record_button), "recording");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(self->record_button), "recording");

  if (recording && self->record_tick_id == 0) {
    gtk_label_set_text(self->record_time, "");
    self->record_tick_id = g_timeout_add(500, waytator_window_record_tick, self);
    waytator_window_record_tick(self);
  } else if (!recording) {
    g_clear_handle_id(&self->record_tick_id, g_source_remove);
  }
  //a recording that is being saved keeps the time it stopped at
  gtk_widget_set_visible(GTK_WIDGET(self->record_time), recording || saving);

  waytator_window_queue_chrome_layout(self);
}

void
waytator_window_update_recording_all(GtkApplication *app)
{
  for (GList *link = gtk_application_get_windows(app); link != NULL; link = link->next) {
    if (WAYTATOR_IS_WINDOW(link->data) && !WAYTATOR_WINDOW(link->data)->disposed)
      waytator_window_update_recording(WAYTATOR_WINDOW(link->data));
  }
}

//what a restart would lose: edits, an image or video that exists nowhere else, or a save or export under way
static gboolean
waytator_window_has_progress(WaytatorWindow *self)
{
  if (waytator_window_is_video(self)) {
    WaytatorVideoEditor *editor = WAYTATOR_VIDEO_EDITOR(self->video_editor);

    return waytator_video_editor_has_unsaved_changes(editor)
        || waytator_video_editor_is_exporting(editor)
        || waytator_video_editor_get_file(editor) == NULL;
  }

  return self->texture != NULL
      && (waytator_window_has_unsaved_changes(self)
          || self->never_saved
          || self->save_in_progress
          || self->capture_storing);
}

//the new version opens what this window shows, and takes over from this process
static void
waytator_window_restart(WaytatorWindow *self)
{
  g_autoptr(GError) error = NULL;

  if (!waytator_instance_restart(waytator_window_get_shown_file(self), &error)) {
    g_autofree char *message = g_strdup_printf("Could not start the new version: %s", error->message);

    waytator_window_show_error(self, message);
  }
}

static void
waytator_window_restart_response(AdwAlertDialog *dialog,
                                 const char     *response,
                                 gpointer        user_data)
{
  (void) dialog;

  if (g_strcmp0(response, "restart") == 0)
    waytator_window_restart(WAYTATOR_WINDOW(user_data));
}

//every window goes with the restart, so any of them with something to lose is asked about first
static void
waytator_window_restart_clicked(AdwBanner *banner,
                                gpointer   user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  GtkApplication *app = gtk_window_get_application(GTK_WINDOW(self));
  gboolean progress = FALSE;
  AdwDialog *dialog;

  (void) banner;

  for (GList *link = app != NULL ? gtk_application_get_windows(app) : NULL; link != NULL; link = link->next) {
    if (WAYTATOR_IS_WINDOW(link->data) && waytator_window_has_progress(WAYTATOR_WINDOW(link->data)))
      progress = TRUE;
  }

  if (!progress) {
    waytator_window_restart(self);
    return;
  }

  dialog = adw_alert_dialog_new("This won't save your progress", NULL);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog),
                                 "cancel", "_Cancel",
                                 "restart", "_Restart",
                                 NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "restart", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  g_signal_connect(dialog, "response", G_CALLBACK(waytator_window_restart_response), self);
  adw_dialog_present(dialog, GTK_WIDGET(self));
}

static void
waytator_window_update_out_of_date(WaytatorWindow *self)
{
  //a recording or capture under way would be cut off, so the restart waits for it
  adw_banner_set_button_label(self->out_of_date_banner, waytator_instance_can_restart() ? "Restart" : NULL);
  adw_banner_set_revealed(self->out_of_date_banner, waytator_instance_is_out_of_date());
}

void
waytator_window_update_out_of_date_all(GtkApplication *app)
{
  for (GList *link = gtk_application_get_windows(app); link != NULL; link = link->next) {
    if (WAYTATOR_IS_WINDOW(link->data) && !WAYTATOR_WINDOW(link->data)->disposed)
      waytator_window_update_out_of_date(WAYTATOR_WINDOW(link->data));
  }
}

//what the Open, screenshot and record buttons do, for when the bar is too short for them. the capture overlay
//has the rest, so there is no picking of a screen, a window or a delay here
static void
waytator_window_setup_open_menu(WaytatorWindow *self)
{
  g_autoptr(GMenu) open_menu = g_menu_new();
  g_autoptr(GMenu) open_section = g_menu_new();

  self->capture_menu = g_menu_new();
  waytator_window_update_capture_menu(self);
  g_menu_append(open_section, "Open…", "win.open");
  g_menu_append_section(open_menu, NULL, G_MENU_MODEL(open_section));
  g_menu_append_section(open_menu, NULL, G_MENU_MODEL(self->capture_menu));
  gtk_menu_button_set_menu_model(self->open_menu_button, G_MENU_MODEL(open_menu));
  g_signal_connect(self->open_menu_button, "notify::active", G_CALLBACK(waytator_window_open_menu_opened), self);
  waytator_window_setup_menu_button_popover(self->open_menu_button, GTK_ARROW_DOWN, GTK_POS_BOTTOM);
  waytator_window_update_recording(self);
}

static void
waytator_window_setup_capture_button(WaytatorWindow *self)
{
  //this one is bound in main.c and cannot be reassigned
  g_autofree char *open_hint = gtk_accelerator_get_label(GDK_KEY_O, GDK_CONTROL_MASK);

  gtk_label_set_text(self->open_hint, open_hint);
  waytator_window_update_capture_controls(self);
  waytator_window_setup_open_menu(self);
}

static void
waytator_window_setup_popovers(WaytatorWindow *self)
{
  //the tools sit in the dock at the bottom
  waytator_window_setup_menu_button_popover(self->shapes_tool_button,
                                            GTK_ARROW_UP,
                                            GTK_POS_TOP);
  waytator_window_setup_menu_button_popover(self->save_button,
                                            GTK_ARROW_DOWN,
                                            GTK_POS_BOTTOM);
  waytator_window_setup_menu_button_popover(self->file_button,
                                            GTK_ARROW_DOWN,
                                            GTK_POS_BOTTOM);
}

//an edited video gets a chance to stay, its edits exist nowhere else
static gboolean
waytator_window_close_request(GtkWindow *window)
{
  if (waytator_window_video_close_request(WAYTATOR_WINDOW(window)))
    return TRUE;

  return GTK_WINDOW_CLASS(waytator_window_parent_class)->close_request(window);
}

static void
waytator_window_class_init(WaytatorWindowClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = waytator_window_dispose;
  GTK_WINDOW_CLASS(klass)->close_request = waytator_window_close_request;

  g_type_ensure(WAYTATOR_TYPE_CANVAS_VIEW);
  g_type_ensure(WAYTATOR_TYPE_PASSTHROUGH_BOX);
  g_type_ensure(WAYTATOR_TYPE_PASSTHROUGH_CENTER_BOX);
  gtk_widget_class_set_template_from_resource(widget_class, WAYTATOR_RESOURCE_PREFIX "/ui/window.ui");

  waytator_window_bind_template_children(widget_class);
  waytator_window_install_actions(widget_class);
}

//put in here rather than the template, where it would push all of the window's content one level deeper
static void
waytator_window_setup_toast_overlay(WaytatorWindow *self)
{
  GtkWidget *content = g_object_ref(adw_application_window_get_content(ADW_APPLICATION_WINDOW(self)));

  self->toast_overlay = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(self), NULL);
  adw_toast_overlay_set_child(self->toast_overlay, content);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(self), GTK_WIDGET(self->toast_overlay));
  g_object_unref(content);
}

//above the content rather than over it, so it never covers the bars
static void
waytator_window_setup_out_of_date_banner(WaytatorWindow *self)
{
  GtkWidget *content = g_object_ref(adw_toast_overlay_get_child(self->toast_overlay));
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  self->out_of_date_banner = ADW_BANNER(adw_banner_new("This version of Waytator is out of date"));
  g_signal_connect(self->out_of_date_banner, "button-clicked", G_CALLBACK(waytator_window_restart_clicked), self);
  adw_toast_overlay_set_child(self->toast_overlay, NULL);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->out_of_date_banner));
  gtk_box_append(GTK_BOX(box), content);
  adw_toast_overlay_set_child(self->toast_overlay, box);
  g_object_unref(content);
  waytator_window_update_out_of_date(self);
}

static void
waytator_window_init(WaytatorWindow *self)
{
  waytator_window_init_state(self);
  waytator_window_ensure_icons_registered();

  gtk_widget_init_template(GTK_WIDGET(self));
  waytator_window_setup_toast_overlay(self);
  waytator_window_setup_out_of_date_banner(self);
  waytator_window_setup_window_background(self);
  waytator_window_ensure_css_loaded();

  gtk_drawing_area_set_draw_func(self->drawing_area,
                                 waytator_window_drawing_area_draw,
                                 self,
                                 NULL);
  waytator_window_setup_ocr_panel(self);
  waytator_window_setup_video(self);
  waytator_window_setup_window_controls(self);
  //built here rather than in the template, its entries depend on where the image can be saved
  self->save_menu = g_menu_new();
  gtk_menu_button_set_menu_model(self->save_button, G_MENU_MODEL(self->save_menu));
  waytator_window_setup_popovers(self);
  waytator_window_setup_capture_button(self);
  waytator_window_setup_tool_shortcut_hints(self);
  waytator_window_setup_controllers(self);
  waytator_window_setup_signals(self);
  waytator_window_setup_chrome_layout(self);
  g_signal_connect(self->copy_button, "clicked", G_CALLBACK(waytator_window_copy_clicked), self);
  g_signal_connect(self->save_as_button, "clicked", G_CALLBACK(waytator_window_save_as_clicked), self);
  g_signal_connect(self->copy_keep_open_button, "clicked", G_CALLBACK(waytator_window_copy_keep_open_clicked), self);
  waytator_window_update_copy_controls(self);
  g_signal_connect(self->ocr_panel_toggle_button, "toggled", G_CALLBACK(waytator_window_ocr_panel_toggled), self);
  g_signal_connect(self->ocr_panel_close_button, "clicked", G_CALLBACK(waytator_window_ocr_panel_close_clicked), self);
  g_signal_connect(self->ocr_panel_bottom_sheet, "notify::open", G_CALLBACK(waytator_window_ocr_panel_open_changed), self);

  waytator_window_update_size_controls(self);
  waytator_window_update_tool_ui(self);
  waytator_window_sync_state(self);
}

WaytatorWindow *
waytator_window_new(AdwApplication *app)
{
  return g_object_new(WAYTATOR_TYPE_WINDOW,
                      "application", app,
                      NULL);
}
