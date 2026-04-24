#include "waytator-window.h"
#include "waytator-window-private.h"
#include "waytator-config.h"

#include "waytator-export.h"
#include "waytator-ocr.h"
#include "waytator-render.h"
#include "waytator-stroke.h"

#include <cairo.h>
#include <string.h>

G_DEFINE_FINAL_TYPE(WaytatorWindow, waytator_window, ADW_TYPE_APPLICATION_WINDOW)

#define WAYTATOR_SETTINGS_GROUP "preferences"
#define WAYTATOR_SETTINGS_FILE "waytator/settings.ini"
#define WAYTATOR_WINDOW_STYLE_PROVIDER_PRIORITY (GTK_STYLE_PROVIDER_PRIORITY_USER + 1)
#define WAYTATOR_RESOURCE_PREFIX "/dev/faetalize/waytator"

static void waytator_window_clear_ocr_results(WaytatorWindow *self);
static void waytator_window_set_ocr_panel_visible(WaytatorWindow *self,
                                                  gboolean        visible);
static void waytator_window_ocr_panel_open_changed(GObject    *object,
                                                   GParamSpec *pspec,
                                                   gpointer    user_data);
static void waytator_window_show_error(WaytatorWindow *self,
                                       const char     *message);
static gboolean waytator_window_has_unsaved_changes(WaytatorWindow *self);
static void waytator_window_copy_export_ready(GObject      *source_object,
                                              GAsyncResult *result,
                                              gpointer      user_data);
static gboolean waytator_window_parse_accelerator(const char       *accelerator,
                                                  guint            *keyval,
                                                  GdkModifierType  *modifiers);
static void waytator_window_apply_copy_shortcut(WaytatorWindow *self,
                                                const char     *accelerator);
static void waytator_window_update_shortcut_label(GtkShortcutLabel *label,
                                                  const char       *accelerator);
static const char *waytator_window_angle_snap_modifier_label(GdkModifierType modifiers);
static void waytator_window_highlighter_overlap_changed(AdwSwitchRow   *row,
                                                        GParamSpec     *pspec,
                                                        WaytatorWindow *self);

static void waytator_window_update_window_controls(WaytatorWindow *self);
static void waytator_window_update_window_background(WaytatorWindow *self);
static void waytator_window_update_widget_appearance(WaytatorWindow *self);
static void waytator_window_save_preferences(WaytatorWindow *self);

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

static char *
waytator_window_preferences_path(void)
{
  return g_build_filename(g_get_user_config_dir(), WAYTATOR_SETTINGS_FILE, NULL);
}

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

static void
waytator_window_apply_copy_shortcut(WaytatorWindow *self,
                                    const char     *accelerator)
{
  g_clear_pointer(&self->copy_shortcut_accel, g_free);
  self->copy_shortcut_accel = g_strdup(accelerator);
}

static void
waytator_window_update_shortcut_label(GtkShortcutLabel *label,
                                      const char       *accelerator)
{
  gtk_shortcut_label_set_accelerator(label,
                                     accelerator != NULL ? accelerator : "");
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
  g_autofree char *path = waytator_window_preferences_path();
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

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "floating_controls_blur", NULL))
    self->floating_controls_blur = g_key_file_get_boolean(key_file,
                                                          WAYTATOR_SETTINGS_GROUP,
                                                          "floating_controls_blur",
                                                          NULL);

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "floating_controls_opacity", NULL)) {
    const double opacity = g_key_file_get_double(key_file,
                                                 WAYTATOR_SETTINGS_GROUP,
                                                 "floating_controls_opacity",
                                                 NULL);

    if (opacity >= 0.0 && opacity <= 1.0)
      self->floating_controls_opacity = opacity;
  }

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "esc_closes_window", NULL))
    self->esc_closes_window = g_key_file_get_boolean(key_file,
                                                     WAYTATOR_SETTINGS_GROUP,
                                                     "esc_closes_window",
                                                     NULL);

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "copy_shortcut_enabled", NULL))
    self->copy_shortcut_enabled = g_key_file_get_boolean(key_file,
                                                         WAYTATOR_SETTINGS_GROUP,
                                                         "copy_shortcut_enabled",
                                                         NULL);

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "auto_copy_latest_change", NULL))
    self->auto_copy_latest_change = g_key_file_get_boolean(key_file,
                                                           WAYTATOR_SETTINGS_GROUP,
                                                           "auto_copy_latest_change",
                                                           NULL);

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "allow_highlighter_overlap", NULL))
    self->allow_highlighter_overlap = g_key_file_get_boolean(key_file,
                                                             WAYTATOR_SETTINGS_GROUP,
                                                             "allow_highlighter_overlap",
                                                             NULL);

  if (g_key_file_has_key(key_file, WAYTATOR_SETTINGS_GROUP, "copy_shortcut", NULL)) {
    g_autofree char *accelerator = g_key_file_get_string(key_file,
                                                         WAYTATOR_SETTINGS_GROUP,
                                                         "copy_shortcut",
                                                         NULL);

    if (waytator_window_parse_accelerator(accelerator, NULL, NULL))
      waytator_window_apply_copy_shortcut(self, accelerator);
  }

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
}

static void
waytator_window_save_preferences(WaytatorWindow *self)
{
  g_autofree char *path = waytator_window_preferences_path();
  g_autofree char *directory = g_path_get_dirname(path);
  g_autoptr(GKeyFile) key_file = g_key_file_new();
  g_autofree char *data = NULL;
  gsize data_length = 0;
  g_autoptr(GError) error = NULL;

  g_key_file_set_integer(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "eraser_style",
                         self->eraser_style);
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
                         "floating_controls_blur",
                         self->floating_controls_blur);
  g_key_file_set_double(key_file,
                        WAYTATOR_SETTINGS_GROUP,
                        "floating_controls_opacity",
                        self->floating_controls_opacity);
  g_key_file_set_boolean(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "esc_closes_window",
                         self->esc_closes_window);
  g_key_file_set_boolean(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "copy_shortcut_enabled",
                         self->copy_shortcut_enabled);
  g_key_file_set_boolean(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "auto_copy_latest_change",
                         self->auto_copy_latest_change);
  g_key_file_set_boolean(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "allow_highlighter_overlap",
                         self->allow_highlighter_overlap);
  g_key_file_set_string(key_file,
                        WAYTATOR_SETTINGS_GROUP,
                        "copy_shortcut",
                        self->copy_shortcut_accel);
  g_key_file_set_integer(key_file,
                         WAYTATOR_SETTINGS_GROUP,
                         "angle_snap_modifiers",
                         self->angle_snap_modifiers);

  if (g_mkdir_with_parents(directory, 0700) != 0) {
    g_warning("Failed to create preferences directory %s", directory);
    return;
  }

  data = g_key_file_to_data(key_file, &data_length, NULL);
  if (!g_file_set_contents(path, data, data_length, &error))
    g_warning("Failed to save preferences to %s: %s", path, error->message);
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

  self->floating_controls_blur = adw_switch_row_get_active(row);
  waytator_window_save_preferences(self);
  waytator_window_update_widget_appearance(self);
}

static void
waytator_window_floating_controls_opacity_changed(GtkSpinButton  *spin_button,
                                                  WaytatorWindow *self)
{
  self->floating_controls_opacity = gtk_spin_button_get_value(spin_button);
  waytator_window_save_preferences(self);
  waytator_window_update_widget_appearance(self);
}

static void
waytator_window_esc_closes_window_changed(AdwSwitchRow   *row,
                                          GParamSpec     *pspec,
                                          WaytatorWindow *self)
{
  (void) pspec;

  self->esc_closes_window = adw_switch_row_get_active(row);
  waytator_window_save_preferences(self);
}

static void
waytator_window_copy_shortcut_enabled_changed(AdwSwitchRow   *row,
                                              GParamSpec     *pspec,
                                              WaytatorWindow *self)
{
  GtkWidget *shortcut_row;

  (void) pspec;

  self->copy_shortcut_enabled = adw_switch_row_get_active(row);
  shortcut_row = g_object_get_data(G_OBJECT(row), "shortcut-row");
  if (shortcut_row != NULL)
    gtk_widget_set_sensitive(shortcut_row, self->copy_shortcut_enabled);
  waytator_window_save_preferences(self);
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
waytator_window_apply_copy_shortcut_row(GtkButton *button,
                                        gpointer   user_data)
{
  GtkWidget *shortcut_label;
  (void) user_data;

  gtk_button_set_has_frame(button, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(button), "suggested-action");
  shortcut_label = g_object_get_data(G_OBJECT(button), "shortcut-label");
  if (shortcut_label != NULL)
    gtk_widget_set_sensitive(shortcut_label, FALSE);
  g_object_set_data(G_OBJECT(button), "capturing", GINT_TO_POINTER(TRUE));
}

static gboolean
waytator_window_copy_shortcut_capture_key_pressed(GtkEventControllerKey *controller,
                                                  guint                  keyval,
                                                  guint                  keycode,
                                                  GdkModifierType        state,
                                                  gpointer               user_data)
{
  GtkWidget *button = GTK_WIDGET(user_data);
  WaytatorWindow *self = WAYTATOR_WINDOW(g_object_get_data(G_OBJECT(button), "window"));
  GtkShortcutLabel *shortcut_label = GTK_SHORTCUT_LABEL(g_object_get_data(G_OBJECT(button), "shortcut-label"));
  GdkModifierType modifiers;
  g_autofree char *accelerator = NULL;

  (void) controller;
  (void) keycode;

  if (!GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "capturing")))
    return FALSE;

  modifiers = state & gtk_accelerator_get_default_mod_mask();

  if (keyval == GDK_KEY_Escape && modifiers == 0) {
    gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
    gtk_widget_remove_css_class(button, "suggested-action");
    gtk_widget_set_sensitive(GTK_WIDGET(shortcut_label), TRUE);
    g_object_set_data(G_OBJECT(button), "capturing", GINT_TO_POINTER(FALSE));
    return TRUE;
  }

  if (keyval == GDK_KEY_BackSpace || keyval == GDK_KEY_Delete) {
    waytator_window_apply_copy_shortcut(self, "");
    waytator_window_update_shortcut_label(shortcut_label, "");
  } else {
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

    waytator_window_apply_copy_shortcut(self, accelerator);
    waytator_window_update_shortcut_label(shortcut_label, self->copy_shortcut_accel);
  }

  waytator_window_save_preferences(self);
  gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
  gtk_widget_remove_css_class(button, "suggested-action");
  gtk_widget_set_sensitive(GTK_WIDGET(shortcut_label), TRUE);
  g_object_set_data(G_OBJECT(button), "capturing", GINT_TO_POINTER(FALSE));

  return TRUE;
}

static void
waytator_window_update_window_background(WaytatorWindow *self)
{
  g_autofree char *css = NULL;

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

  css = g_strdup_printf("window.waytator-window { background: alpha(@window_bg_color, %.1f); background-color: alpha(@window_bg_color, %.1f); }",
                        self->window_background_opacity,
                        self->window_background_opacity);
  gtk_css_provider_load_from_string(self->window_css_provider, css);
}

static void
waytator_window_update_widget_appearance(WaytatorWindow *self)
{
  g_autofree char *css = NULL;
  const char *blur = self->floating_controls_blur ? "blur(18px)" : "none";

  if (self->widget_css_provider == NULL)
    return;

  css = g_strdup_printf(
    ".overlay-pill, bottom-sheet > sheet.background {"
    " color: white;"
    " background: alpha(black, %.2f);"
    " background-color: alpha(black, %.2f);"
    " border: 1px solid alpha(white, 0.12);"
    " border-radius: 14px;"
    " backdrop-filter: %s;"
    " box-shadow: 0 10px 32px alpha(black, 0.18);"
    " background-image: none;"
    "}"
    ".app-chrome-group { border-color: alpha(white, 0.16); }",
    self->floating_controls_opacity,
    self->floating_controls_opacity,
    blur);
  gtk_css_provider_load_from_string(self->widget_css_provider, css);
}

static void
waytator_window_show_preferences(WaytatorWindow *self)
{
  AdwPreferencesDialog *dialog;
  AdwPreferencesPage *page;
  AdwPreferencesGroup *group;
  AdwPreferencesGroup *appearance_group;
  AdwPreferencesGroup *shortcuts_group;
  AdwComboRow *row;
  AdwComboRow *background_mode_row;
  AdwSwitchRow *floating_controls_blur_row;
  AdwSwitchRow *esc_closes_window_row;
  AdwSwitchRow *copy_shortcut_enabled_row;
  AdwSwitchRow *auto_copy_latest_change_row;
  AdwSwitchRow *highlighter_overlap_row;
  AdwComboRow *angle_snap_modifier_row;
  AdwActionRow *opacity_row;
  AdwActionRow *floating_controls_opacity_row;
  AdwActionRow *copy_shortcut_row;
  GtkStringList *model;
  GtkStringList *background_model;
  GtkStringList *angle_snap_model;
  GtkAdjustment *opacity_adjustment;
  GtkSpinButton *opacity_spin_button;
  GtkAdjustment *floating_controls_opacity_adjustment;
  GtkSpinButton *floating_controls_opacity_spin_button;
  GtkWidget *copy_shortcut_label;
  GtkWidget *copy_shortcut_button;
  GtkWidget *copy_shortcut_button_box;
  GtkEventController *copy_shortcut_key_controller;

  dialog = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
  page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
  group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  appearance_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  shortcuts_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  row = ADW_COMBO_ROW(adw_combo_row_new());
  background_mode_row = ADW_COMBO_ROW(adw_combo_row_new());
  floating_controls_blur_row = ADW_SWITCH_ROW(adw_switch_row_new());
  esc_closes_window_row = ADW_SWITCH_ROW(adw_switch_row_new());
  copy_shortcut_enabled_row = ADW_SWITCH_ROW(adw_switch_row_new());
  auto_copy_latest_change_row = ADW_SWITCH_ROW(adw_switch_row_new());
  highlighter_overlap_row = ADW_SWITCH_ROW(adw_switch_row_new());
  angle_snap_modifier_row = ADW_COMBO_ROW(adw_combo_row_new());
  opacity_row = ADW_ACTION_ROW(adw_action_row_new());
  floating_controls_opacity_row = ADW_ACTION_ROW(adw_action_row_new());
  copy_shortcut_row = ADW_ACTION_ROW(adw_action_row_new());
  opacity_adjustment = gtk_adjustment_new(self->window_background_opacity, 0.1, 1.0, 0.1, 0.1, 0.0);
  opacity_spin_button = GTK_SPIN_BUTTON(gtk_spin_button_new(opacity_adjustment, 0.1, 1));
  floating_controls_opacity_adjustment = gtk_adjustment_new(self->floating_controls_opacity, 0.0, 1.0, 0.1, 0.1, 0.0);
  floating_controls_opacity_spin_button = GTK_SPIN_BUTTON(gtk_spin_button_new(floating_controls_opacity_adjustment, 0.1, 1));
  copy_shortcut_label = gtk_shortcut_label_new(NULL);
  copy_shortcut_button = gtk_button_new();
  copy_shortcut_button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  copy_shortcut_key_controller = gtk_event_controller_key_new();
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
  adw_preferences_group_set_title(group, "General");
  adw_preferences_group_set_title(appearance_group, "Appearance");
  adw_preferences_group_set_title(shortcuts_group, "Shortcuts");
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
  adw_switch_row_set_active(floating_controls_blur_row, self->floating_controls_blur);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(floating_controls_opacity_row), "Controls background opacity");
  gtk_spin_button_set_numeric(floating_controls_opacity_spin_button, TRUE);
  gtk_widget_set_valign(GTK_WIDGET(floating_controls_opacity_spin_button), GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(GTK_WIDGET(floating_controls_opacity_spin_button), 88, -1);
  adw_action_row_add_suffix(floating_controls_opacity_row, GTK_WIDGET(floating_controls_opacity_spin_button));
  adw_action_row_set_activatable_widget(floating_controls_opacity_row,
                                        GTK_WIDGET(floating_controls_opacity_spin_button));
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(esc_closes_window_row), "Escape closes window");
  adw_switch_row_set_active(esc_closes_window_row, self->esc_closes_window);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(copy_shortcut_enabled_row), "Enable copy shortcut");
  adw_switch_row_set_active(copy_shortcut_enabled_row, self->copy_shortcut_enabled);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(copy_shortcut_row), "Copy shortcut");
  waytator_window_update_shortcut_label(GTK_SHORTCUT_LABEL(copy_shortcut_label), self->copy_shortcut_accel);
  gtk_widget_set_valign(copy_shortcut_label, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(copy_shortcut_label, GTK_ALIGN_END);
  gtk_widget_set_hexpand(copy_shortcut_button_box, FALSE);
  gtk_widget_set_halign(copy_shortcut_button_box, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(copy_shortcut_button_box), copy_shortcut_label);
  gtk_button_set_child(GTK_BUTTON(copy_shortcut_button), copy_shortcut_button_box);
  gtk_widget_set_valign(copy_shortcut_button, GTK_ALIGN_CENTER);
  gtk_widget_add_controller(copy_shortcut_button, copy_shortcut_key_controller);
  adw_action_row_add_suffix(copy_shortcut_row, copy_shortcut_button);
  gtk_widget_set_sensitive(GTK_WIDGET(copy_shortcut_row), self->copy_shortcut_enabled);
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
  adw_switch_row_set_active(auto_copy_latest_change_row, self->auto_copy_latest_change);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(highlighter_overlap_row), "Allow highlighter strokes to overlap");
  adw_switch_row_set_active(highlighter_overlap_row, self->allow_highlighter_overlap);

  adw_preferences_group_add(group, GTK_WIDGET(row));
  adw_preferences_group_add(group, GTK_WIDGET(highlighter_overlap_row));
  adw_preferences_group_add(group, GTK_WIDGET(auto_copy_latest_change_row));
  adw_preferences_group_add(appearance_group, GTK_WIDGET(background_mode_row));
  adw_preferences_group_add(appearance_group, GTK_WIDGET(opacity_row));
  adw_preferences_group_add(appearance_group, GTK_WIDGET(floating_controls_blur_row));
  adw_preferences_group_add(appearance_group, GTK_WIDGET(floating_controls_opacity_row));
  adw_preferences_group_add(shortcuts_group, GTK_WIDGET(esc_closes_window_row));
  adw_preferences_group_add(shortcuts_group, GTK_WIDGET(copy_shortcut_enabled_row));
  adw_preferences_group_add(shortcuts_group, GTK_WIDGET(copy_shortcut_row));
  adw_preferences_group_add(shortcuts_group, GTK_WIDGET(angle_snap_modifier_row));
  adw_preferences_page_add(page, group);
  adw_preferences_page_add(page, appearance_group);
  adw_preferences_page_add(page, shortcuts_group);
  adw_preferences_dialog_add(dialog, page);
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
  g_signal_connect(esc_closes_window_row,
                   "notify::active",
                   G_CALLBACK(waytator_window_esc_closes_window_changed),
                   self);
  g_signal_connect(copy_shortcut_enabled_row,
                   "notify::active",
                   G_CALLBACK(waytator_window_copy_shortcut_enabled_changed),
                   self);
  g_signal_connect(angle_snap_modifier_row,
                   "notify::selected",
                   G_CALLBACK(waytator_window_angle_snap_modifier_changed),
                   self);
  g_signal_connect(auto_copy_latest_change_row,
                   "notify::active",
                   G_CALLBACK(waytator_window_auto_copy_latest_change_changed),
                   self);
  g_signal_connect(highlighter_overlap_row,
                   "notify::active",
                   G_CALLBACK(waytator_window_highlighter_overlap_changed),
                   self);
  g_object_set_data(G_OBJECT(copy_shortcut_enabled_row), "shortcut-row", copy_shortcut_row);
  g_object_set_data(G_OBJECT(copy_shortcut_button), "window", self);
  g_object_set_data(G_OBJECT(copy_shortcut_button), "shortcut-label", copy_shortcut_label);
  g_signal_connect(copy_shortcut_button,
                   "clicked",
                   G_CALLBACK(waytator_window_apply_copy_shortcut_row),
                   NULL);
  g_signal_connect(copy_shortcut_key_controller,
                   "key-pressed",
                   G_CALLBACK(waytator_window_copy_shortcut_capture_key_pressed),
                   copy_shortcut_button);

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
  gtk_widget_set_visible(self->ocr_panel_toggle_container, FALSE);
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
  double display_x;
  double display_y;
  double display_width;
  double display_height;
  guint i;

  if (!waytator_window_ocr_is_visible(self)) {
    gtk_widget_set_visible(GTK_WIDGET(self->ocr_overlay), FALSE);
    return;
  }

  if (!waytator_window_get_display_rect(self,
                                        widget_width,
                                        widget_height,
                                        &display_x,
                                        &display_y,
                                        &display_width,
                                        &display_height)
      || image_width <= 0
      || image_height <= 0) {
    gtk_widget_set_visible(GTK_WIDGET(self->ocr_overlay), FALSE);
    return;
  }

  gtk_widget_set_visible(GTK_WIDGET(self->ocr_overlay), TRUE);

  for (i = 0; i < self->ocr_lines->len; i++) {
    WaytatorOcrLine *line = g_ptr_array_index(self->ocr_lines, i);
    const double scale_x = display_width / image_width;
    const double scale_y = display_height / image_height;
    const int x = (int) floor(display_x + line->left * scale_x);
    const int y = (int) floor(display_y + line->top * scale_y);
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

  gtk_widget_set_visible(self->ocr_panel_toggle_container, can_show_toggle);

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

static gboolean
waytator_window_restore_copy_button(gpointer user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  self->copy_feedback_timeout_id = 0;
  gtk_stack_set_visible_child(self->copy_icon_stack, GTK_WIDGET(self->copy_default_icon));
  g_object_unref(self);
  return G_SOURCE_REMOVE;
}

void
waytator_window_trigger_copy(WaytatorWindow *self)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;
  WaytatorExportRequest *request;

  if (self->texture == NULL || self->copy_in_progress)
    return;

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
  task = g_task_new(self, NULL, waytator_window_copy_export_ready, g_object_ref(self));
  g_task_set_task_data(task, request, (GDestroyNotify) waytator_export_request_free);
  g_task_run_in_thread(task, waytator_export_run_task);
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

static void
waytator_window_flash_copy_success(WaytatorWindow *self)
{
  if (self->copy_feedback_timeout_id != 0)
    g_source_remove(self->copy_feedback_timeout_id);

  gtk_stack_set_visible_child(self->copy_icon_stack, GTK_WIDGET(self->copy_success_icon));
  self->copy_feedback_timeout_id = g_timeout_add(1200,
                                                 waytator_window_restore_copy_button,
                                                 g_object_ref(self));
}

void
waytator_window_reset_save_button(WaytatorWindow *self)
{
  const gboolean has_unsaved_changes = waytator_window_has_unsaved_changes(self);

  gtk_stack_set_visible_child(self->save_icon_stack, GTK_WIDGET(self->save_default_icon));
  gtk_widget_set_sensitive(GTK_WIDGET(self->save_button), has_unsaved_changes);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "win.save", has_unsaved_changes && self->current_file != NULL);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "win.save-copy", has_unsaved_changes);
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

static void
waytator_window_begin_save_feedback(WaytatorWindow *self)
{
  if (self->save_spinner_timeout_id != 0)
    g_source_remove(self->save_spinner_timeout_id);

  if (self->save_feedback_timeout_id != 0)
    g_source_remove(self->save_feedback_timeout_id);

  self->save_spinner_timeout_id = 0;
  self->save_feedback_timeout_id = 0;
  self->save_feedback_started_at = g_get_monotonic_time();

  gtk_stack_set_visible_child(self->save_icon_stack, GTK_WIDGET(self->save_working_icon));
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "win.save", FALSE);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "win.save-copy", FALSE);
}

static void
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


static void
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

static gboolean
waytator_window_error_is_user_dismissed(const GError *error)
{
  if (error == NULL)
    return FALSE;

  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return TRUE;

  return error->message != NULL
      && g_ascii_strcasecmp(error->message, "Dismissed by user") == 0;
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
    waytator_copy_result_free(copy_result);
    waytator_window_show_error(self, "Could not copy image to clipboard");
    g_object_unref(self);
    return;
  }

  waytator_window_log_formats("Clipboard accepted formats", gdk_clipboard_get_formats(clipboard));
  waytator_window_flash_copy_success(self);
  waytator_copy_result_free(copy_result);
  if (self->auto_copy_pending) {
    self->auto_copy_pending = FALSE;
    waytator_window_maybe_auto_copy_latest_change(self);
  }
  g_object_unref(self);
}

static void
waytator_window_save_export_ready(GObject      *source_object,
                                  GAsyncResult *result,
                                  gpointer      user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  g_autoptr(GError) error = NULL;

  (void) source_object;

  if (!g_task_propagate_boolean(G_TASK(result), &error)) {
    if (self->save_spinner_timeout_id != 0)
      g_source_remove(self->save_spinner_timeout_id);

    if (self->save_feedback_timeout_id != 0)
      g_source_remove(self->save_feedback_timeout_id);

    self->save_spinner_timeout_id = 0;
    self->save_feedback_timeout_id = 0;
    waytator_window_reset_save_button(self);
    waytator_window_show_error(self, error->message);
    g_object_unref(self);
    return;
  }

  waytator_window_mark_saved(self);
  waytator_window_finish_save_feedback(self);
  g_object_unref(self);
}

static void
waytator_window_save_copy_ready(GObject      *source_object,
                                GAsyncResult *result,
                                gpointer      user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GTask) task = NULL;
  WaytatorExportRequest *request;

  file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source_object), result, &error);
  if (file == NULL) {
    if (!waytator_window_error_is_user_dismissed(error))
      waytator_window_show_error(self, error->message);

    g_object_unref(self);
    return;
  }

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
    waytator_window_show_error(self, error->message);
    g_object_unref(self);
    return;
  }

  waytator_window_begin_save_feedback(self);
  task = g_task_new(self, NULL, waytator_window_save_export_ready, g_object_ref(self));
  g_task_set_task_data(task, request, (GDestroyNotify) waytator_export_request_free);
  g_task_run_in_thread(task, waytator_export_run_task);

  g_object_unref(self);
}

static void
waytator_window_save_overwrite_action(GtkWidget  *widget,
                                      const char *action_name,
                                      GVariant   *parameter)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(widget);
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = NULL;
  WaytatorExportRequest *request;

  (void) action_name;
  (void) parameter;

  if (self->current_file == NULL)
    return;

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
    return;
  }

  waytator_window_begin_save_feedback(self);
  task = g_task_new(self, NULL, waytator_window_save_export_ready, g_object_ref(self));
  g_task_set_task_data(task, request, (GDestroyNotify) waytator_export_request_free);
  g_task_run_in_thread(task, waytator_export_run_task);
}

static void
waytator_window_save_copy_action(GtkWidget  *widget,
                                 const char *action_name,
                                 GVariant   *parameter)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(widget);
  g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
  g_autofree char *basename = NULL;
  g_autofree char *copy_name = NULL;

  (void) action_name;
  (void) parameter;

  if (self->current_file != NULL)
    basename = g_file_get_basename(self->current_file);

  copy_name = waytator_window_make_copy_name(basename != NULL ? basename : self->source_name);

  gtk_file_dialog_set_title(dialog, "Save as copy");
  gtk_file_dialog_set_initial_name(dialog, copy_name);

  gtk_file_dialog_save(dialog,
                       GTK_WINDOW(self),
                       NULL,
                       waytator_window_save_copy_ready,
                       g_object_ref(self));
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

  gtk_window_destroy(GTK_WINDOW(widget));
}

static void
waytator_window_copy_clicked(GtkButton *button,
                             gpointer   user_data)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  (void) button;

  waytator_window_trigger_copy(self);
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
  waytator_window_update_picture_size(self);
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
  self->fit_mode = TRUE;
  self->drawing = FALSE;
  self->current_stroke = NULL;
  self->ocr_running = FALSE;
  waytator_window_mark_saved(self);

  waytator_window_sync_state(self);
  waytator_window_queue_fit_zoom(self);
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

static void
waytator_window_open_ready(GObject      *source_object,
                           GAsyncResult *result,
                           gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) file = NULL;
  WaytatorWindow *self = WAYTATOR_WINDOW(user_data);

  file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source_object), result, &error);

  if (file == NULL) {
    if (!waytator_window_error_is_user_dismissed(error))
      g_printerr("Error opening file: %s\n", error->message);

    g_object_unref(self);
    return;
  }

  if (!waytator_window_open_file(self, file, &error))
    waytator_window_show_error(self, error->message);

  g_object_unref(self);
}

static void
waytator_window_open(WaytatorWindow *self)
{
  g_autoptr(GtkFileDialog) dialog = NULL;
  g_autoptr(GtkFileFilter) images = NULL;
  g_autoptr(GListStore) filters = NULL;

  dialog = gtk_file_dialog_new();
  images = gtk_file_filter_new();
  filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

  gtk_file_filter_set_name(images, "Images");
  gtk_file_filter_add_mime_type(images, "image/png");
  gtk_file_filter_add_mime_type(images, "image/jpeg");
  gtk_file_filter_add_mime_type(images, "image/webp");
  gtk_file_filter_add_mime_type(images, "image/gif");
  gtk_file_filter_add_mime_type(images, "image/bmp");
  gtk_file_filter_add_mime_type(images, "image/svg+xml");
  g_list_store_append(filters, images);

  gtk_file_dialog_set_title(dialog, "Open image");
  gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
  gtk_file_dialog_set_default_filter(dialog, images);

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
  g_autoptr(GtkFileLauncher) launcher = NULL;

  (void) action_name;
  (void) parameter;

  if (self->current_file == NULL)
    return;

  launcher = gtk_file_launcher_new(self->current_file);
  gtk_file_launcher_launch(launcher,
                           GTK_WINDOW(self),
                           NULL,
                           waytator_window_open_current_file_ready,
                           g_object_ref(self));
}

static void
waytator_window_dispose(GObject *object)
{
  WaytatorWindow *self = WAYTATOR_WINDOW(object);

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
  g_clear_pointer(&self->copy_shortcut_accel, g_free);

  G_OBJECT_CLASS(waytator_window_parent_class)->dispose(object);
}

static void
waytator_window_bind_template_children(GtkWidgetClass *widget_class)
{
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, canvas_stack);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, canvas_scroller);
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
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, file_group);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, file_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, file_label);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, zoom_label);
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
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, save_icon_stack);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, save_default_icon);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, save_working_icon);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, save_success_icon);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, copy_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, app_menu_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, end_window_controls);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, copy_icon_stack);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, copy_default_icon);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, copy_success_icon);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, zoom_group);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, fit_zoom_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, settings_group);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, color_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, fill_color_button);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, width_scale);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, text_size_spin);
  gtk_widget_class_bind_template_child(widget_class, WaytatorWindow, blur_type_dropdown);
}

static void
waytator_window_install_actions(GtkWidgetClass *widget_class)
{
  gtk_widget_class_install_action(widget_class, "win.open", NULL, waytator_window_open_action);
  gtk_widget_class_install_action(widget_class, "win.open-current-file", NULL, waytator_window_open_current_file_action);
  gtk_widget_class_install_action(widget_class, "win.copy-buffer", NULL, waytator_window_copy_action);
  gtk_widget_class_install_action(widget_class, "win.dismiss", NULL, waytator_window_dismiss_action);
  gtk_widget_class_install_action(widget_class, "win.close-window", NULL, waytator_window_close_window_action);
  gtk_widget_class_install_action(widget_class, "win.save", NULL, waytator_window_save_overwrite_action);
  gtk_widget_class_install_action(widget_class, "win.save-copy", NULL, waytator_window_save_copy_action);
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
  int i;

  self->zoom = 1.0;
  self->fit_mode = TRUE;
  self->active_tool = WAYTATOR_TOOL_BRUSH;
  self->drawing = FALSE;
  self->document = waytator_document_new();
  self->ocr_lines = NULL;
  self->selected_ocr_line = NULL;
  self->ocr_all_text = NULL;
  self->pinch_start_zoom = 1.0;
  self->pointer_in = FALSE;
  self->eraser_style = WAYTATOR_ERASER_STYLE_DUAL_RING;
  self->window_background_mode = WAYTATOR_WINDOW_BACKGROUND_OPAQUE;
  self->window_background_opacity = 0.8;
  self->esc_closes_window = TRUE;
  self->copy_shortcut_enabled = TRUE;
  self->angle_snap_modifiers = GDK_SHIFT_MASK;
  self->allow_highlighter_overlap = TRUE;
  self->floating_controls_blur = TRUE;
  self->auto_copy_latest_change = FALSE;
  self->floating_controls_opacity = 0.7;
  waytator_window_apply_copy_shortcut(self, "<Primary>c");
  waytator_window_load_preferences(self);

  for (i = 0; i <= WAYTATOR_TOOL_BLUR; i++) {
    self->tool_widths[i] = waytator_tool_width(i);
    self->tool_colors[i] = (GdkRGBA){0.96, 0.2, 0.28, 1.0};
    self->tool_fill_colors[i] = (GdkRGBA){0.96, 0.2, 0.28, 0.0};
  }
  self->tool_colors[WAYTATOR_TOOL_MARKER] = (GdkRGBA){1.0, 0.91, 0.2, 1.0};
  self->tool_colors[WAYTATOR_TOOL_BLUR] = (GdkRGBA){0.0, 0.0, 0.0, 1.0};
  self->tool_colors[WAYTATOR_TOOL_ERASER] = (GdkRGBA){1.0, 1.0, 1.0, 1.0};
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

static void
waytator_window_ensure_css_loaded(void)
{
  static gboolean css_loaded = FALSE;

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
waytator_window_class_init(WaytatorWindowClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = waytator_window_dispose;

  gtk_widget_class_set_template_from_resource(widget_class, WAYTATOR_RESOURCE_PREFIX "/ui/window.ui");

  waytator_window_bind_template_children(widget_class);
  waytator_window_install_actions(widget_class);
}

static void
waytator_window_init(WaytatorWindow *self)
{
  waytator_window_init_state(self);
  waytator_window_ensure_icons_registered();

  gtk_widget_init_template(GTK_WIDGET(self));
  waytator_window_setup_window_background(self);
  waytator_window_ensure_css_loaded();

  gtk_drawing_area_set_draw_func(self->drawing_area,
                                 waytator_window_drawing_area_draw,
                                 self,
                                 NULL);
  waytator_window_setup_ocr_panel(self);
  waytator_window_setup_window_controls(self);
  waytator_window_setup_controllers(self);
  waytator_window_setup_signals(self);
  g_signal_connect(self->copy_button, "clicked", G_CALLBACK(waytator_window_copy_clicked), self);
  g_signal_connect(self->ocr_panel_toggle_button, "toggled", G_CALLBACK(waytator_window_ocr_panel_toggled), self);
  g_signal_connect(self->ocr_panel_close_button, "clicked", G_CALLBACK(waytator_window_ocr_panel_close_clicked), self);
  g_signal_connect(self->ocr_panel_bottom_sheet, "notify::open", G_CALLBACK(waytator_window_ocr_panel_open_changed), self);

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
