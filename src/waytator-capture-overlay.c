#include "waytator-capture-overlay.h"
#include "waytator-pointer-brake.h"
#include "waytator-settings.h"
#include "waytator-window.h"

#include <gtk4-layer-shell.h>
#include <math.h>

//in logical pixels, anything smaller is a stray click rather than a selection
#define WAYTATOR_CAPTURE_MIN_SELECTION 3.0
//the magnifier shows this many of the monitor's pixels across, odd so that one sits right under the pointer
#define WAYTATOR_MAGNIFIER_PIXELS 15
//how big each of them is drawn, in logical pixels
#define WAYTATOR_MAGNIFIER_ZOOM 10.0f
//keeps the magnifier off the cursor
#define WAYTATOR_MAGNIFIER_GAP 24.0f
//the closest the bar comes to the edges of the screen
#define WAYTATOR_CAPTURE_PILL_MARGIN 8
//how long the parts of the bar take to open and close, and the mode highlight to slide
#define WAYTATOR_CAPTURE_PILL_TRANSITION_MS 200

typedef struct _WaytatorCaptureOverlay WaytatorCaptureOverlay;

#define WAYTATOR_TYPE_CAPTURE_VIEW (waytator_capture_view_get_type())

G_DECLARE_FINAL_TYPE(WaytatorCaptureView, waytator_capture_view, WAYTATOR, CAPTURE_VIEW, GtkWidget)

//one per monitor, shows that monitor's frozen frame with the selection drawn on top
struct _WaytatorCaptureView {
  GtkWidget parent_instance;

  WaytatorCaptureOverlay *overlay;
  WaytatorCaptureOutput *output;
  GdkTexture *texture;
  GtkWidget *pill;
  GtkToggleButton *area_button;
  GtkToggleButton *screen_button;
  GtkToggleButton *window_button;
  GtkToggleButton *magnifier_button;
  GtkToggleButton *delay_button;
  GtkToggleButton *system_sound_button;
  GtkToggleButton *microphone_button;
  GtkLabel *delay_label;
  GtkRevealer *delay_choices;
  GtkRevealer *shift_hint;
  //where the bar was dragged to, in the view's logical pixels, and where in it the pointer took hold of it
  gboolean pill_moved;
  double pill_x;
  double pill_y;
  double grab_x;
  double grab_y;
  gboolean hovered;
  gboolean selecting;
  //where the cursor is, or the one drawn in its place while braking
  double pointer_x;
  double pointer_y;
  //where GDK last saw the pointer, and whether braking has since moved the cursor away from there
  double seen_x;
  double seen_y;
  gboolean detached;
  double start_x;
  double start_y;
  double end_x;
  double end_y;
};

struct _WaytatorCaptureOverlay {
  GPtrArray *outputs;
  GPtrArray *windows;
  GPtrArray *views;
  GtkCssProvider *pill_style;
  WaytatorCaptureMode mode;
  gboolean updating;
  gboolean closed;
  gboolean offer_hide;
  gboolean hide_waytator;
  gboolean magnifier;
  //a recording: the bar has what to record and the countdown, and the callback gets the choice
  gboolean record;
  gboolean system_sound;
  gboolean microphone;
  guint delay_seconds;
  double brake_speed;
  //held while shift is, on the view the pointer was over when it went down
  WaytatorPointerBrake *brake;
  WaytatorCaptureView *brake_view;
  WaytatorCaptureOverlayRetakeCallback retake;
  WaytatorCaptureOverlayCallback callback;
  WaytatorCaptureOverlayRecordCallback record_callback;
  gpointer user_data;
};

G_DEFINE_FINAL_TYPE(WaytatorCaptureView, waytator_capture_view, GTK_TYPE_WIDGET)

//cairo leaves the spare byte of RGB24 undefined, and a screenshot is opaque either way
static GdkTexture *
waytator_capture_texture_new(cairo_surface_t *surface,
                             int              x,
                             int              y,
                             int              width,
                             int              height)
{
  const guchar *src;
  gsize src_stride;
  const gsize stride = (gsize) width * 4;
  guint32 *pixels;
  g_autoptr(GBytes) bytes = NULL;

  cairo_surface_flush(surface);
  src = cairo_image_surface_get_data(surface);
  src_stride = cairo_image_surface_get_stride(surface);
  pixels = g_malloc(stride * height);

  for (int row = 0; row < height; row++) {
    const guint32 *src_row = (const guint32 *) (src + (gsize) (y + row) * src_stride) + x;
    guint32 *dst_row = pixels + (gsize) row * width;

    for (int column = 0; column < width; column++)
      dst_row[column] = src_row[column] | 0xff000000u;
  }

  bytes = g_bytes_new_take(pixels, stride * height);
  return gdk_memory_texture_new(width, height, GDK_MEMORY_DEFAULT, bytes, stride);
}

//the whole frame, shared with the surface rather than copied: the texture format skips the spare byte
//just as RGB24 does, and a copy of a whole screen held up the overlay and the editor alike
GdkTexture *
waytator_capture_output_create_texture(WaytatorCaptureOutput *output)
{
  cairo_surface_t *surface = output->surface;
  const int height = cairo_image_surface_get_height(surface);
  const gsize stride = cairo_image_surface_get_stride(surface);
  g_autoptr(GBytes) bytes = NULL;

  cairo_surface_flush(surface);
  bytes = g_bytes_new_with_free_func(cairo_image_surface_get_data(surface),
                                     stride * (gsize) height,
                                     (GDestroyNotify) cairo_surface_destroy,
                                     cairo_surface_reference(surface));
  return gdk_memory_texture_new(cairo_image_surface_get_width(surface),
                                height,
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
                                GDK_MEMORY_B8G8R8X8,
#else
                                GDK_MEMORY_X8R8G8B8,
#endif
                                bytes,
                                stride);
}

//the screen a capture is about when there is nothing to ask, NULL when the overlay has to
WaytatorCaptureOutput *
waytator_capture_pick_output(GPtrArray          *outputs,
                             WaytatorCaptureMode mode,
                             const char         *output_name,
                             GError            **error)
{
  if (mode != WAYTATOR_CAPTURE_SCREEN)
    return NULL;

  if (output_name == NULL)
    return outputs->len == 1 ? g_ptr_array_index(outputs, 0) : NULL;

  for (guint i = 0; i < outputs->len; i++) {
    WaytatorCaptureOutput *output = g_ptr_array_index(outputs, i);

    if (g_strcmp0(output->name, output_name) == 0)
      return output;
  }

  g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "There is no screen named %s", output_name);
  return NULL;
}

void
waytator_record_choice_free(WaytatorRecordChoice *choice)
{
  if (choice == NULL)
    return;

  g_free(choice->output_name);
  g_free(choice);
}

//the pill is one of the floating controls, read from the settings since there may be no window to ask
GtkCssProvider *
waytator_capture_overlay_new_pill_style(const WaytatorSettings *settings)
{
  g_autofree char *css = NULL;
  GtkCssProvider *provider = gtk_css_provider_new();
  char opacity_text[G_ASCII_DTOSTR_BUF_SIZE];

  //printf would use the locale's decimal separator, which is not valid css
  g_ascii_formatd(opacity_text, sizeof opacity_text, "%.2f", settings->floating_controls_opacity);
  css = g_strdup_printf(".capture-pill { background: alpha(black, %s); background-color: alpha(black, %s);"
                        " backdrop-filter: %s; }",
                        opacity_text,
                        opacity_text,
                        settings->floating_controls_blur ? "blur(18px)" : "none");
  gtk_css_provider_load_from_string(provider, css);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                             GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
  return provider;
}

static gboolean
waytator_capture_overlay_free(gpointer user_data)
{
  WaytatorCaptureOverlay *overlay = user_data;

  g_ptr_array_unref(overlay->outputs);
  g_ptr_array_unref(overlay->windows);
  g_ptr_array_unref(overlay->views);
  gtk_style_context_remove_provider_for_display(gdk_display_get_default(),
                                                GTK_STYLE_PROVIDER(overlay->pill_style));
  g_object_unref(overlay->pill_style);
  g_free(overlay);
  return G_SOURCE_REMOVE;
}

static void
waytator_capture_view_set_selection_end(WaytatorCaptureView *self,
                                        double               x,
                                        double               y);

//the compositor is asked to put the real cursor where the drawn one ended up
static void
waytator_capture_overlay_stop_brake(WaytatorCaptureOverlay *overlay)
{
  WaytatorCaptureView *view = overlay->brake_view;

  if (overlay->brake == NULL)
    return;

  waytator_pointer_brake_release(overlay->brake, view->pointer_x, view->pointer_y);
  overlay->brake = NULL;
  overlay->brake_view = NULL;
  gtk_widget_set_cursor_from_name(GTK_WIDGET(view),
                                  overlay->mode == WAYTATOR_CAPTURE_AREA ? "crosshair" : "default");
  gtk_widget_queue_draw(GTK_WIDGET(view));
}

static void
waytator_capture_overlay_brake_moved(double   dx,
                                     double   dy,
                                     gpointer user_data)
{
  WaytatorCaptureOverlay *overlay = user_data;
  WaytatorCaptureView *view = overlay->brake_view;
  const double x = CLAMP(view->pointer_x + dx * overlay->brake_speed, 0.0, gtk_widget_get_width(GTK_WIDGET(view)));
  const double y = CLAMP(view->pointer_y + dy * overlay->brake_speed, 0.0, gtk_widget_get_height(GTK_WIDGET(view)));

  view->detached = TRUE;
  if (view->selecting) {
    waytator_capture_view_set_selection_end(view, x, y);
    return;
  }

  view->pointer_x = x;
  view->pointer_y = y;
  gtk_widget_queue_draw(GTK_WIDGET(view));
}

static void
waytator_capture_overlay_brake_lost(gpointer user_data)
{
  waytator_capture_overlay_stop_brake(user_data);
}

//the real cursor stays put and one drawn by the view moves at a fraction of its speed instead
static void
waytator_capture_overlay_start_brake(WaytatorCaptureOverlay *overlay)
{
  if (overlay->brake != NULL || overlay->mode != WAYTATOR_CAPTURE_AREA)
    return;

  for (guint i = 0; i < overlay->views->len; i++) {
    WaytatorCaptureView *view = g_ptr_array_index(overlay->views, i);

    if (!view->hovered)
      continue;

    overlay->brake = waytator_pointer_brake_new(GTK_WIDGET(view),
                                                waytator_capture_overlay_brake_moved,
                                                waytator_capture_overlay_brake_lost,
                                                overlay);
    if (overlay->brake == NULL)
      return;

    overlay->brake_view = view;
    gtk_widget_set_cursor_from_name(GTK_WIDGET(view), "none");
    gtk_widget_queue_draw(GTK_WIDGET(view));
    return;
  }
}

//FALSE when it already closed, events can still arrive while the windows go away
static gboolean
waytator_capture_overlay_close(WaytatorCaptureOverlay *overlay)
{
  if (overlay->closed)
    return FALSE;

  overlay->closed = TRUE;
  waytator_capture_overlay_stop_brake(overlay);
  //with one window per screen, closing the focused one hands focus to the next one, which is closing too
  waytator_window_destroy_settled((GtkWindow *const *) overlay->windows->pdata, overlay->windows->len);

  //this usually runs inside a handler of one of the views, which still points at the overlay
  g_idle_add(waytator_capture_overlay_free, overlay);
  return TRUE;
}

//NULL when it was cancelled
static void
waytator_capture_overlay_finish_record(WaytatorCaptureOverlay *overlay,
                                       WaytatorRecordChoice   *choice)
{
  if (choice != NULL) {
    choice->system_sound = overlay->system_sound;
    choice->microphone = overlay->microphone;
    choice->delay_seconds = overlay->delay_seconds;
  }

  if (waytator_capture_overlay_close(overlay))
    overlay->record_callback(choice, overlay->user_data);
  else
    waytator_record_choice_free(choice);
}

static void
waytator_capture_overlay_finish(WaytatorCaptureOverlay *overlay,
                                GdkTexture             *texture)
{
  //a screen pick hands over the view's own texture, which goes with the windows
  g_autoptr(GdkTexture) kept = texture != NULL ? g_object_ref(texture) : NULL;

  if (overlay->record) {
    waytator_capture_overlay_finish_record(overlay, NULL);
    return;
  }

  if (waytator_capture_overlay_close(overlay))
    overlay->callback(kept, overlay->mode, overlay->user_data);
}

static void
waytator_capture_overlay_hide_toggled(GtkToggleButton        *button,
                                      WaytatorCaptureOverlay *overlay)
{
  if (waytator_capture_overlay_close(overlay))
    overlay->retake(gtk_toggle_button_get_active(button), overlay->mode, 0, overlay->user_data);
}

//a recording counts down after the pick, so the overlay stays and the choice is only marked
static void
waytator_capture_overlay_set_delay(WaytatorCaptureOverlay *overlay,
                                   guint                   seconds)
{
  overlay->delay_seconds = seconds;

  for (guint i = 0; i < overlay->views->len; i++) {
    WaytatorCaptureView *view = g_ptr_array_index(overlay->views, i);
    GtkWidget *choices = gtk_revealer_get_child(view->delay_choices);
    g_autofree char *text = g_strdup_printf("%us", seconds);

    for (GtkWidget *choice = gtk_widget_get_first_child(choices);
         choice != NULL;
         choice = gtk_widget_get_next_sibling(choice)) {
      if (GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(choice), "seconds")) == seconds)
        gtk_widget_add_css_class(choice, "capture-delay-picked");
      else
        gtk_widget_remove_css_class(choice, "capture-delay-picked");
    }

    gtk_label_set_text(view->delay_label, text);
    gtk_widget_set_visible(GTK_WIDGET(view->delay_label), seconds > 0);
    gtk_toggle_button_set_active(view->delay_button, FALSE);
  }
}

//the frozen frame is from before the wait, so the overlay goes and a new one is taken once it is over
static void
waytator_capture_overlay_delay_clicked(GtkButton           *button,
                                       WaytatorCaptureView *view)
{
  WaytatorCaptureOverlay *overlay = view->overlay;
  const guint seconds = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "seconds"));

  if (overlay->record) {
    waytator_capture_overlay_set_delay(overlay, seconds == overlay->delay_seconds ? 0 : seconds);
    return;
  }

  if (waytator_capture_overlay_close(overlay))
    overlay->retake(overlay->hide_waytator, overlay->mode, seconds, overlay->user_data);
}

static void
waytator_capture_overlay_delay_toggled(GtkToggleButton     *button,
                                       WaytatorCaptureView *view)
{
  gtk_revealer_set_reveal_child(view->delay_choices, gtk_toggle_button_get_active(button));
}

static void
waytator_capture_overlay_set_pills_visible(WaytatorCaptureOverlay *overlay,
                                           gboolean                visible)
{
  for (guint i = 0; i < overlay->views->len; i++) {
    WaytatorCaptureView *view = g_ptr_array_index(overlay->views, i);

    gtk_widget_set_visible(view->pill, visible);
  }
}

static void
waytator_capture_overlay_set_mode(WaytatorCaptureOverlay *overlay,
                                  WaytatorCaptureMode     mode)
{
  const gboolean area = mode == WAYTATOR_CAPTURE_AREA;

  waytator_capture_overlay_stop_brake(overlay);
  overlay->mode = mode;
  overlay->updating = TRUE;

  for (guint i = 0; i < overlay->views->len; i++) {
    WaytatorCaptureView *view = g_ptr_array_index(overlay->views, i);

    gtk_toggle_button_set_active(area ? view->area_button : view->screen_button, TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(view->magnifier_button), area);
    gtk_revealer_set_reveal_child(view->shift_hint, area);
    gtk_widget_set_cursor_from_name(GTK_WIDGET(view), area ? "crosshair" : "default");
    view->selecting = FALSE;
    gtk_widget_queue_draw(GTK_WIDGET(view));
  }

  overlay->updating = FALSE;
}

static void
waytator_capture_overlay_set_magnifier(WaytatorCaptureOverlay *overlay,
                                       gboolean                magnifier)
{
  overlay->magnifier = magnifier;
  overlay->updating = TRUE;

  for (guint i = 0; i < overlay->views->len; i++) {
    WaytatorCaptureView *view = g_ptr_array_index(overlay->views, i);

    gtk_toggle_button_set_active(view->magnifier_button, magnifier);
    gtk_widget_queue_draw(GTK_WIDGET(view));
  }

  overlay->updating = FALSE;
}

//the widget is laid out in logical pixels, the frame is in the monitor's real ones
static void
waytator_capture_view_pixel_at(WaytatorCaptureView *self,
                               double               x,
                               double               y,
                               int                 *pixel_x,
                               int                 *pixel_y)
{
  const int surface_width = cairo_image_surface_get_width(self->output->surface);
  const int surface_height = cairo_image_surface_get_height(self->output->surface);
  const double scale_x = (double) surface_width / MAX(gtk_widget_get_width(GTK_WIDGET(self)), 1);
  const double scale_y = (double) surface_height / MAX(gtk_widget_get_height(GTK_WIDGET(self)), 1);

  *pixel_x = CLAMP((int) floor(x * scale_x), 0, surface_width - 1);
  *pixel_y = CLAMP((int) floor(y * scale_y), 0, surface_height - 1);
}

//both corners take in the whole pixel under them, so the one the magnifier points at is the one that gets captured
static gboolean
waytator_capture_view_get_selection_pixels(WaytatorCaptureView *self,
                                           int                 *x,
                                           int                 *y,
                                           int                 *width,
                                           int                 *height)
{
  int start_x;
  int start_y;
  int end_x;
  int end_y;

  if (!self->selecting)
    return FALSE;

  waytator_capture_view_pixel_at(self, self->start_x, self->start_y, &start_x, &start_y);
  waytator_capture_view_pixel_at(self, self->end_x, self->end_y, &end_x, &end_y);
  *x = MIN(start_x, end_x);
  *y = MIN(start_y, end_y);
  *width = ABS(end_x - start_x) + 1;
  *height = ABS(end_y - start_y) + 1;
  return TRUE;
}

//the selection as drawn, lined up with the pixels it takes in
static gboolean
waytator_capture_view_get_selection(WaytatorCaptureView *self,
                                    graphene_rect_t     *selection)
{
  const double scale_x = gtk_widget_get_width(GTK_WIDGET(self))
                         / (double) cairo_image_surface_get_width(self->output->surface);
  const double scale_y = gtk_widget_get_height(GTK_WIDGET(self))
                         / (double) cairo_image_surface_get_height(self->output->surface);
  int x;
  int y;
  int width;
  int height;

  if (!waytator_capture_view_get_selection_pixels(self, &x, &y, &width, &height))
    return FALSE;

  graphene_rect_init(selection, x * scale_x, y * scale_y, width * scale_x, height * scale_y);
  return TRUE;
}

static void
waytator_capture_view_snapshot_label(GtkSnapshot *snapshot,
                                     PangoLayout *layout,
                                     float        x,
                                     float        y)
{
  const GdkRGBA background = { 0.0, 0.0, 0.0, 0.75 };
  const GdkRGBA foreground = { 1.0, 1.0, 1.0, 1.0 };
  GskRoundedRect box;
  int text_width;
  int text_height;

  pango_layout_get_pixel_size(layout, &text_width, &text_height);
  gsk_rounded_rect_init_from_rect(&box,
                                  &GRAPHENE_RECT_INIT(x, y, text_width + 12.0f, text_height + 6.0f),
                                  6.0f);
  gtk_snapshot_push_rounded_clip(snapshot, &box);
  gtk_snapshot_append_color(snapshot, &background, &box.bounds);
  gtk_snapshot_pop(snapshot);

  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(x + 6.0f, y + 3.0f));
  gtk_snapshot_append_layout(snapshot, layout, &foreground);
  gtk_snapshot_restore(snapshot);
}

static void
waytator_capture_view_snapshot_size_label(WaytatorCaptureView   *self,
                                          GtkSnapshot           *snapshot,
                                          const graphene_rect_t *selection,
                                          float                  view_width,
                                          float                  view_height)
{
  g_autoptr(PangoLayout) layout = NULL;
  g_autofree char *text = NULL;
  int x;
  int y;
  int width;
  int height;
  int text_width;
  int text_height;
  float box_x;
  float box_y;

  waytator_capture_view_get_selection_pixels(self, &x, &y, &width, &height);
  text = g_strdup_printf("%d × %d", width, height);
  layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), text);
  pango_layout_get_pixel_size(layout, &text_width, &text_height);

  box_x = selection->origin.x;
  box_y = selection->origin.y + selection->size.height + 8.0f;
  if (box_y + text_height + 6.0f > view_height)
    box_y = selection->origin.y + selection->size.height - text_height - 14.0f;
  box_x = MAX(MIN(box_x, view_width - text_width - 12.0f), 0.0f);

  waytator_capture_view_snapshot_label(snapshot, layout, box_x, box_y);
}

static void
waytator_capture_view_snapshot_outline(GtkSnapshot           *snapshot,
                                       const graphene_rect_t *rect,
                                       float                  thickness)
{
  const GdkRGBA white = { 1.0, 1.0, 1.0, 1.0 };
  GskRoundedRect outline;

  gsk_rounded_rect_init_from_rect(&outline, rect, 0.0f);
  gtk_snapshot_append_border(snapshot,
                             &outline,
                             (float[4]) { thickness, thickness, thickness, thickness },
                             (GdkRGBA[4]) { white, white, white, white });
}

static void
waytator_capture_view_snapshot_ring(GtkSnapshot          *snapshot,
                                    const GskRoundedRect *shape,
                                    float                 thickness,
                                    const GdkRGBA        *color)
{
  gtk_snapshot_append_border(snapshot,
                             shape,
                             (float[4]) { thickness, thickness, thickness, thickness },
                             (GdkRGBA[4]) { *color, *color, *color, *color });
}

//beside the pointer, on the side facing away from the selection so it never covers what is being picked, and
//over on the other side wherever the screen runs out
static void
waytator_capture_view_place_magnifier(WaytatorCaptureView   *self,
                                      const graphene_rect_t *selection,
                                      float                  width,
                                      float                  height,
                                      float                  view_width,
                                      float                  view_height,
                                      graphene_point_t      *origin)
{
  const float x = self->pointer_x;
  const float y = self->pointer_y;
  const gboolean prefer_left = self->selecting && self->end_x < self->start_x;
  const gboolean prefer_up = self->selecting && self->end_y < self->start_y;
  graphene_rect_t avoid;
  gboolean found_fallback = FALSE;
  graphene_point_t fallback = { 0.0f, 0.0f };

  //with the outline around it
  if (selection != NULL)
    graphene_rect_inset_r(selection, -2.0f, -2.0f, &avoid);

  for (guint i = 0; i < 4; i++) {
    const gboolean left = prefer_left != ((i & 1) != 0);
    const gboolean up = prefer_up != ((i & 2) != 0);
    const graphene_rect_t box = GRAPHENE_RECT_INIT(left ? x - WAYTATOR_MAGNIFIER_GAP - width : x + WAYTATOR_MAGNIFIER_GAP,
                                                   up ? y - WAYTATOR_MAGNIFIER_GAP - height : y + WAYTATOR_MAGNIFIER_GAP,
                                                   width,
                                                   height);

    if (box.origin.x < 0.0f
        || box.origin.y < 0.0f
        || box.origin.x + width > view_width
        || box.origin.y + height > view_height)
      continue;

    if (selection == NULL || !graphene_rect_intersection(&box, &avoid, NULL)) {
      *origin = box.origin;
      return;
    }

    if (!found_fallback) {
      fallback = box.origin;
      found_fallback = TRUE;
    }
  }

  if (found_fallback) {
    *origin = fallback;
    return;
  }

  //a screen too small for it anywhere
  origin->x = CLAMP(x + WAYTATOR_MAGNIFIER_GAP, 0.0f, MAX(view_width - width, 0.0f));
  origin->y = CLAMP(y + WAYTATOR_MAGNIFIER_GAP, 0.0f, MAX(view_height - height, 0.0f));
}

//the monitor's own pixels around the pointer, blown up on a grid, with the one under it marked
static void
waytator_capture_view_snapshot_magnifier(WaytatorCaptureView   *self,
                                         GtkSnapshot           *snapshot,
                                         const graphene_rect_t *selection,
                                         float                  view_width,
                                         float                  view_height)
{
  const GdkRGBA backdrop = { 0.1, 0.1, 0.1, 1.0 };
  const GdkRGBA grid = { 0.0, 0.0, 0.0, 0.25 };
  const GdkRGBA crosshair = { 0.55, 0.78, 1.0, 0.4 };
  const GdkRGBA black = { 0.0, 0.0, 0.0, 1.0 };
  const GdkRGBA white = { 1.0, 1.0, 1.0, 1.0 };
  const GdkRGBA edge = { 0.0, 0.0, 0.0, 0.5 };
  const GdkRGBA shadow = { 0.0, 0.0, 0.0, 0.35 };
  const float zoom = WAYTATOR_MAGNIFIER_ZOOM;
  const float size = WAYTATOR_MAGNIFIER_PIXELS * zoom;
  const int reach = WAYTATOR_MAGNIFIER_PIXELS / 2;
  const float middle = reach * zoom;
  const int surface_width = cairo_image_surface_get_width(self->output->surface);
  const int surface_height = cairo_image_surface_get_height(self->output->surface);
  g_autoptr(PangoLayout) layout = NULL;
  g_autoptr(GdkTexture) pixels = NULL;
  g_autofree char *text = NULL;
  graphene_point_t origin;
  GskRoundedRect circle;
  GskRoundedRect border;
  int pixel_x;
  int pixel_y;
  int text_width;
  int text_height;
  int left;
  int top;
  int right;
  int bottom;
  float block_width;
  float x;
  float y;

  waytator_capture_view_pixel_at(self, self->pointer_x, self->pointer_y, &pixel_x, &pixel_y);
  text = g_strdup_printf("X: %d  Y: %d", pixel_x, pixel_y);
  layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), text);
  pango_layout_get_pixel_size(layout, &text_width, &text_height);

  block_width = MAX(size, text_width + 12.0f);
  waytator_capture_view_place_magnifier(self,
                                        selection,
                                        block_width,
                                        size + 6.0f + text_height + 6.0f,
                                        view_width,
                                        view_height,
                                        &origin);
  x = origin.x + (block_width - size) / 2.0f;
  y = origin.y;
  gsk_rounded_rect_init_from_rect(&circle, &GRAPHENE_RECT_INIT(x, y, size, size), size / 2.0f);

  gtk_snapshot_append_outset_shadow(snapshot, &circle, &shadow, 0.0f, 4.0f, 0.0f, 16.0f);
  gtk_snapshot_push_rounded_clip(snapshot, &circle);
  gtk_snapshot_append_color(snapshot, &backdrop, &circle.bounds);

  //only what it shows is cut out of the frame, past the edge of the screen it stays dark
  left = MAX(pixel_x - reach, 0);
  top = MAX(pixel_y - reach, 0);
  right = MIN(pixel_x + reach + 1, surface_width);
  bottom = MIN(pixel_y + reach + 1, surface_height);
  pixels = waytator_capture_texture_new(self->output->surface, left, top, right - left, bottom - top);
  gtk_snapshot_append_scaled_texture(snapshot,
                                     pixels,
                                     GSK_SCALING_FILTER_NEAREST,
                                     &GRAPHENE_RECT_INIT(x + middle + (left - pixel_x) * zoom,
                                                         y + middle + (top - pixel_y) * zoom,
                                                         (right - left) * zoom,
                                                         (bottom - top) * zoom));

  for (int i = 1; i < WAYTATOR_MAGNIFIER_PIXELS; i++) {
    gtk_snapshot_append_color(snapshot, &grid, &GRAPHENE_RECT_INIT(x + i * zoom - 0.5f, y, 1.0f, size));
    gtk_snapshot_append_color(snapshot, &grid, &GRAPHENE_RECT_INIT(x, y + i * zoom - 0.5f, size, 1.0f));
  }

  //the row and column through the pointer, short of the pixel itself so its colour stays readable
  gtk_snapshot_append_color(snapshot, &crosshair, &GRAPHENE_RECT_INIT(x + middle, y, zoom, middle));
  gtk_snapshot_append_color(snapshot, &crosshair, &GRAPHENE_RECT_INIT(x + middle, y + middle + zoom, zoom, middle));
  gtk_snapshot_append_color(snapshot, &crosshair, &GRAPHENE_RECT_INIT(x, y + middle, middle, zoom));
  gtk_snapshot_append_color(snapshot, &crosshair, &GRAPHENE_RECT_INIT(x + middle + zoom, y + middle, middle, zoom));

  gsk_rounded_rect_init_from_rect(&border, &GRAPHENE_RECT_INIT(x + middle, y + middle, zoom, zoom), 0.0f);
  waytator_capture_view_snapshot_ring(snapshot, &border, 1.0f, &black);
  gsk_rounded_rect_init_from_rect(&border, &GRAPHENE_RECT_INIT(x + middle - 1.0f, y + middle - 1.0f, zoom + 2.0f, zoom + 2.0f), 0.0f);
  waytator_capture_view_snapshot_ring(snapshot, &border, 1.0f, &white);
  gtk_snapshot_pop(snapshot);

  waytator_capture_view_snapshot_ring(snapshot, &circle, 2.0f, &white);
  gsk_rounded_rect_init_from_rect(&border, &GRAPHENE_RECT_INIT(x - 1.0f, y - 1.0f, size + 2.0f, size + 2.0f), size / 2.0f + 1.0f);
  waytator_capture_view_snapshot_ring(snapshot, &border, 1.0f, &edge);

  waytator_capture_view_snapshot_label(snapshot,
                                       layout,
                                       origin.x + (block_width - text_width - 12.0f) / 2.0f,
                                       y + size + 6.0f);
}

//the real cursor is held still while braking, this stands in for it
static void
waytator_capture_view_snapshot_cursor(WaytatorCaptureView *self,
                                      GtkSnapshot         *snapshot)
{
  const GdkRGBA black = { 0.0, 0.0, 0.0, 0.8 };
  const GdkRGBA white = { 1.0, 1.0, 1.0, 1.0 };
  const float x = self->pointer_x;
  const float y = self->pointer_y;

  gtk_snapshot_append_color(snapshot, &black, &GRAPHENE_RECT_INIT(x - 10.5f, y - 1.5f, 21.0f, 3.0f));
  gtk_snapshot_append_color(snapshot, &black, &GRAPHENE_RECT_INIT(x - 1.5f, y - 10.5f, 3.0f, 21.0f));
  gtk_snapshot_append_color(snapshot, &white, &GRAPHENE_RECT_INIT(x - 9.5f, y - 0.5f, 19.0f, 1.0f));
  gtk_snapshot_append_color(snapshot, &white, &GRAPHENE_RECT_INIT(x - 0.5f, y - 9.5f, 1.0f, 19.0f));
}

static void
waytator_capture_view_snapshot(GtkWidget   *widget,
                               GtkSnapshot *snapshot)
{
  WaytatorCaptureView *self = WAYTATOR_CAPTURE_VIEW(widget);
  const GdkRGBA dim = { 0.0, 0.0, 0.0, 0.45 };
  const float width = gtk_widget_get_width(widget);
  const float height = gtk_widget_get_height(widget);
  const graphene_rect_t bounds = GRAPHENE_RECT_INIT(0.0f, 0.0f, width, height);
  graphene_rect_t selection;

  gtk_snapshot_append_texture(snapshot, self->texture, &bounds);

  if (self->overlay->mode == WAYTATOR_CAPTURE_SCREEN) {
    if (self->hovered)
      waytator_capture_view_snapshot_outline(snapshot, &bounds, 3.0f);
    else
      gtk_snapshot_append_color(snapshot, &dim, &bounds);
    return;
  }

  if (!waytator_capture_view_get_selection(self, &selection)) {
    gtk_snapshot_append_color(snapshot, &dim, &bounds);
    if (self->overlay->magnifier && self->hovered)
      waytator_capture_view_snapshot_magnifier(self, snapshot, NULL, width, height);
    if (self->overlay->brake_view == self)
      waytator_capture_view_snapshot_cursor(self, snapshot);
    return;
  }

  //everything but the selection
  gtk_snapshot_append_color(snapshot,
                            &dim,
                            &GRAPHENE_RECT_INIT(0.0f, 0.0f, width, selection.origin.y));
  gtk_snapshot_append_color(snapshot,
                            &dim,
                            &GRAPHENE_RECT_INIT(0.0f,
                                                selection.origin.y + selection.size.height,
                                                width,
                                                height - selection.origin.y - selection.size.height));
  gtk_snapshot_append_color(snapshot,
                            &dim,
                            &GRAPHENE_RECT_INIT(0.0f, selection.origin.y, selection.origin.x, selection.size.height));
  gtk_snapshot_append_color(snapshot,
                            &dim,
                            &GRAPHENE_RECT_INIT(selection.origin.x + selection.size.width,
                                                selection.origin.y,
                                                width - selection.origin.x - selection.size.width,
                                                selection.size.height));

  //drawn just outside, so the line never covers what is being captured
  waytator_capture_view_snapshot_outline(snapshot,
                                         &GRAPHENE_RECT_INIT(selection.origin.x - 1.0f,
                                                             selection.origin.y - 1.0f,
                                                             selection.size.width + 2.0f,
                                                             selection.size.height + 2.0f),
                                         1.0f);
  waytator_capture_view_snapshot_size_label(self, snapshot, &selection, width, height);

  if (self->overlay->magnifier && self->hovered)
    waytator_capture_view_snapshot_magnifier(self, snapshot, &selection, width, height);
  if (self->overlay->brake_view == self)
    waytator_capture_view_snapshot_cursor(self, snapshot);
}

//GDK hears nothing of the pointer while it is held, nor after it is let go until it really moves, and in the
//meantime it repeats where it last saw it whenever the widgets under it change, which is where braking began
static gboolean
waytator_capture_view_is_stale(WaytatorCaptureView *self,
                               double               x,
                               double               y)
{
  return self->detached && x == self->seen_x && y == self->seen_y;
}

//FALSE when it is only GDK repeating itself
static gboolean
waytator_capture_view_pointer_seen(WaytatorCaptureView *self,
                                   double               x,
                                   double               y)
{
  if (waytator_capture_view_is_stale(self, x, y))
    return FALSE;

  self->detached = FALSE;
  self->seen_x = x;
  self->seen_y = y;
  self->pointer_x = x;
  self->pointer_y = y;
  return TRUE;
}

static void
waytator_capture_view_set_selection_end(WaytatorCaptureView *self,
                                        double               x,
                                        double               y)
{
  self->end_x = CLAMP(x, 0.0, gtk_widget_get_width(GTK_WIDGET(self)));
  self->end_y = CLAMP(y, 0.0, gtk_widget_get_height(GTK_WIDGET(self)));
  //a touch drag moves no pointer, the magnifier follows the selection instead
  self->pointer_x = self->end_x;
  self->pointer_y = self->end_y;
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void
waytator_capture_view_drag_begin(GtkGestureDrag      *gesture,
                                 double               x,
                                 double               y,
                                 WaytatorCaptureView *self)
{
  (void) gesture;

  if (self->overlay->mode != WAYTATOR_CAPTURE_AREA)
    return;

  //the press comes with where braking began, it belongs to the drawn cursor
  if (self->detached) {
    x = self->pointer_x;
    y = self->pointer_y;
  }

  self->selecting = TRUE;
  self->start_x = CLAMP(x, 0.0, gtk_widget_get_width(GTK_WIDGET(self)));
  self->start_y = CLAMP(y, 0.0, gtk_widget_get_height(GTK_WIDGET(self)));
  waytator_capture_view_set_selection_end(self, x, y);
  //the pill would otherwise sit on top of what is being selected
  waytator_capture_overlay_set_pills_visible(self->overlay, FALSE);
}

static void
waytator_capture_view_drag_update(GtkGestureDrag      *gesture,
                                  double               offset_x,
                                  double               offset_y,
                                  WaytatorCaptureView *self)
{
  double start_x;
  double start_y;

  //while braking the selection follows the drawn cursor, and until the pointer moves again after that
  if (!self->selecting
      || !gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y)
      || waytator_capture_view_is_stale(self, start_x + offset_x, start_y + offset_y))
    return;

  waytator_capture_view_set_selection_end(self, start_x + offset_x, start_y + offset_y);
}

//what a screenshot would have taken, in the screen's logical pixels, a screen being all of it. a window has no
//area, only the screen it was picked on, which is where its countdown goes
static WaytatorRecordChoice *
waytator_capture_view_new_choice(WaytatorCaptureView *self,
                                 WaytatorCaptureMode  mode)
{
  WaytatorRecordChoice *choice = g_new0(WaytatorRecordChoice, 1);
  graphene_rect_t selection;

  choice->mode = mode;
  choice->output_name = g_strdup(self->output->name);
  choice->output_x = self->output->x;
  choice->output_y = self->output->y;
  choice->output_width = self->output->width;
  choice->output_height = self->output->height;

  if (mode == WAYTATOR_CAPTURE_SCREEN) {
    choice->width = self->output->width;
    choice->height = self->output->height;
  } else if (mode == WAYTATOR_CAPTURE_AREA && waytator_capture_view_get_selection(self, &selection)) {
    const int right = MIN((int) ceil(selection.origin.x + selection.size.width), self->output->width);
    const int bottom = MIN((int) ceil(selection.origin.y + selection.size.height), self->output->height);

    choice->x = MAX((int) floor(selection.origin.x), 0);
    choice->y = MAX((int) floor(selection.origin.y), 0);
    choice->width = MAX(right - choice->x, 1);
    choice->height = MAX(bottom - choice->y, 1);
  }

  return choice;
}

static void
waytator_capture_view_drag_end(GtkGestureDrag      *gesture,
                               double               offset_x,
                               double               offset_y,
                               WaytatorCaptureView *self)
{
  g_autoptr(GdkTexture) texture = NULL;
  int x;
  int y;
  int width;
  int height;

  if (self->overlay->mode == WAYTATOR_CAPTURE_SCREEN) {
    if (self->overlay->record)
      waytator_capture_overlay_finish_record(self->overlay,
                                             waytator_capture_view_new_choice(self, WAYTATOR_CAPTURE_SCREEN));
    else
      waytator_capture_overlay_finish(self->overlay, self->texture);
    return;
  }

  if (!self->selecting)
    return;

  waytator_capture_view_drag_update(gesture, offset_x, offset_y, self);
  waytator_capture_view_get_selection_pixels(self, &x, &y, &width, &height);

  if (fabs(self->end_x - self->start_x) < WAYTATOR_CAPTURE_MIN_SELECTION
      || fabs(self->end_y - self->start_y) < WAYTATOR_CAPTURE_MIN_SELECTION) {
    self->selecting = FALSE;
    waytator_capture_overlay_set_pills_visible(self->overlay, TRUE);
    gtk_widget_queue_draw(GTK_WIDGET(self));
    return;
  }

  if (self->overlay->record) {
    waytator_capture_overlay_finish_record(self->overlay,
                                           waytator_capture_view_new_choice(self, WAYTATOR_CAPTURE_AREA));
    return;
  }

  texture = waytator_capture_texture_new(self->output->surface, x, y, width, height);
  waytator_capture_overlay_finish(self->overlay, texture);
}

static void
waytator_capture_view_drag_cancelled(GtkGesture          *gesture,
                                     GdkEventSequence    *sequence,
                                     WaytatorCaptureView *self)
{
  (void) gesture;
  (void) sequence;

  if (!self->selecting)
    return;

  self->selecting = FALSE;
  waytator_capture_overlay_set_pills_visible(self->overlay, TRUE);
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void
waytator_capture_view_secondary_pressed(GtkGestureClick     *gesture,
                                        int                  n_press,
                                        double               x,
                                        double               y,
                                        WaytatorCaptureView *self)
{
  (void) gesture;
  (void) n_press;
  (void) x;
  (void) y;

  waytator_capture_overlay_finish(self->overlay, NULL);
}

static void
waytator_capture_view_pointer_entered(GtkEventControllerMotion *controller,
                                      double                    x,
                                      double                    y,
                                      WaytatorCaptureView      *self)
{
  (void) controller;

  self->hovered = TRUE;
  waytator_capture_view_pointer_seen(self, x, y);
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void
waytator_capture_view_pointer_moved(GtkEventControllerMotion *controller,
                                    double                    x,
                                    double                    y,
                                    WaytatorCaptureView      *self)
{
  (void) controller;

  if (waytator_capture_view_pointer_seen(self, x, y) && self->overlay->magnifier)
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void
waytator_capture_view_pointer_left(GtkEventControllerMotion *controller,
                                   WaytatorCaptureView      *self)
{
  (void) controller;

  self->hovered = FALSE;
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void
waytator_capture_view_dispose(GObject *object)
{
  WaytatorCaptureView *self = WAYTATOR_CAPTURE_VIEW(object);

  g_clear_object(&self->texture);

  G_OBJECT_CLASS(waytator_capture_view_parent_class)->dispose(object);
}

static void
waytator_capture_view_class_init(WaytatorCaptureViewClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = waytator_capture_view_dispose;
  widget_class->snapshot = waytator_capture_view_snapshot;
}

static void
waytator_capture_view_init(WaytatorCaptureView *self)
{
  GtkGesture *drag = gtk_gesture_drag_new();
  GtkGesture *secondary = gtk_gesture_click_new();
  GtkEventController *motion = gtk_event_controller_motion_new();

  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
  g_signal_connect(drag, "drag-begin", G_CALLBACK(waytator_capture_view_drag_begin), self);
  g_signal_connect(drag, "drag-update", G_CALLBACK(waytator_capture_view_drag_update), self);
  g_signal_connect(drag, "drag-end", G_CALLBACK(waytator_capture_view_drag_end), self);
  g_signal_connect(drag, "cancel", G_CALLBACK(waytator_capture_view_drag_cancelled), self);
  gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(drag));

  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary), GDK_BUTTON_SECONDARY);
  g_signal_connect(secondary, "pressed", G_CALLBACK(waytator_capture_view_secondary_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(secondary));

  g_signal_connect(motion, "enter", G_CALLBACK(waytator_capture_view_pointer_entered), self);
  g_signal_connect(motion, "leave", G_CALLBACK(waytator_capture_view_pointer_left), self);
  g_signal_connect(motion, "motion", G_CALLBACK(waytator_capture_view_pointer_moved), self);
  gtk_widget_add_controller(GTK_WIDGET(self), motion);
}

static void
waytator_capture_overlay_mode_toggled(GtkToggleButton     *button,
                                      WaytatorCaptureView *view)
{
  if (view->overlay->updating || !gtk_toggle_button_get_active(button))
    return;

  //the desktop has the window picked in its own list, there is nothing left to pick here
  if (button == view->window_button) {
    waytator_capture_overlay_finish_record(view->overlay,
                                           waytator_capture_view_new_choice(view, WAYTATOR_CAPTURE_WINDOW));
    return;
  }

  waytator_capture_overlay_set_mode(view->overlay,
                                    button == view->area_button
                                      ? WAYTATOR_CAPTURE_AREA
                                      : WAYTATOR_CAPTURE_SCREEN);
}

//the bar's buttons never take the keyboard, keys belong to the overlay
static void
waytator_capture_overlay_init_pill_button(GtkWidget  *button,
                                          const char *tooltip)
{
  gtk_widget_set_focusable(button, FALSE);
  gtk_widget_add_css_class(button, "flat");
  if (tooltip != NULL)
    gtk_widget_set_tooltip_text(button, tooltip);
}

#define WAYTATOR_TYPE_CAPTURE_MODES (waytator_capture_modes_get_type())

G_DECLARE_FINAL_TYPE(WaytatorCaptureModes, waytator_capture_modes, WAYTATOR, CAPTURE_MODES, GtkWidget)

//Area and Screen side by side, with one highlight that slides over to whichever is picked
struct _WaytatorCaptureModes {
  GtkWidget parent_instance;

  GtkWidget *selected;
  //where the highlight set off from
  graphene_rect_t from;
  AdwAnimation *slide;
};

G_DEFINE_FINAL_TYPE(WaytatorCaptureModes, waytator_capture_modes, GTK_TYPE_WIDGET)

static gboolean
waytator_capture_modes_get_highlight(WaytatorCaptureModes *self,
                                     graphene_rect_t      *highlight)
{
  graphene_rect_t to;

  if (self->selected == NULL || !gtk_widget_compute_bounds(self->selected, GTK_WIDGET(self), &to))
    return FALSE;

  if (adw_animation_get_state(self->slide) != ADW_ANIMATION_PLAYING) {
    *highlight = to;
    return TRUE;
  }

  graphene_rect_interpolate(&self->from, &to, adw_animation_get_value(self->slide), highlight);
  return TRUE;
}

static void
waytator_capture_modes_snapshot(GtkWidget   *widget,
                                GtkSnapshot *snapshot)
{
  WaytatorCaptureModes *self = WAYTATOR_CAPTURE_MODES(widget);
  const GdkRGBA color = { 1.0, 1.0, 1.0, 0.16 };
  graphene_rect_t highlight;
  GskRoundedRect shape;

  if (waytator_capture_modes_get_highlight(self, &highlight)) {
    gsk_rounded_rect_init_from_rect(&shape, &highlight, 9.0f);
    gtk_snapshot_push_rounded_clip(snapshot, &shape);
    gtk_snapshot_append_color(snapshot, &color, &highlight);
    gtk_snapshot_pop(snapshot);
  }

  GTK_WIDGET_CLASS(waytator_capture_modes_parent_class)->snapshot(widget, snapshot);
}

static void
waytator_capture_modes_slide_step(double   value,
                                  gpointer user_data)
{
  (void) value;

  gtk_widget_queue_draw(GTK_WIDGET(user_data));
}

static void
waytator_capture_modes_button_toggled(GtkToggleButton      *button,
                                      WaytatorCaptureModes *self)
{
  graphene_rect_t from;

  if (!gtk_toggle_button_get_active(button) || GTK_WIDGET(button) == self->selected)
    return;

  //the first pick, or one made before the bar is on screen, has nowhere to slide from
  if (!gtk_widget_get_mapped(GTK_WIDGET(self)) || !waytator_capture_modes_get_highlight(self, &from)) {
    self->selected = GTK_WIDGET(button);
    adw_animation_skip(self->slide);
    gtk_widget_queue_draw(GTK_WIDGET(self));
    return;
  }

  //from wherever the highlight is, which may be halfway through the last slide
  self->from = from;
  self->selected = GTK_WIDGET(button);
  adw_animation_play(self->slide);
}

static void
waytator_capture_modes_add(WaytatorCaptureModes *self,
                           GtkWidget            *button)
{
  gtk_widget_set_parent(button, GTK_WIDGET(self));
  g_signal_connect(button, "toggled", G_CALLBACK(waytator_capture_modes_button_toggled), self);
}

static void
waytator_capture_modes_dispose(GObject *object)
{
  WaytatorCaptureModes *self = WAYTATOR_CAPTURE_MODES(object);
  GtkWidget *child;

  g_clear_object(&self->slide);
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self))) != NULL)
    gtk_widget_unparent(child);

  G_OBJECT_CLASS(waytator_capture_modes_parent_class)->dispose(object);
}

static void
waytator_capture_modes_class_init(WaytatorCaptureModesClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = waytator_capture_modes_dispose;
  widget_class->snapshot = waytator_capture_modes_snapshot;
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BOX_LAYOUT);
}

static void
waytator_capture_modes_init(WaytatorCaptureModes *self)
{
  AdwAnimationTarget *target = adw_callback_animation_target_new(waytator_capture_modes_slide_step, self, NULL);

  self->slide = adw_timed_animation_new(GTK_WIDGET(self), 0.0, 1.0, WAYTATOR_CAPTURE_PILL_TRANSITION_MS, target);
  adw_timed_animation_set_easing(ADW_TIMED_ANIMATION(self->slide), ADW_EASE_OUT_CUBIC);
  gtk_widget_add_css_class(GTK_WIDGET(self), "capture-modes");
}

static GtkToggleButton *
waytator_capture_overlay_new_mode_button(WaytatorCaptureView  *view,
                                         WaytatorCaptureModes *modes,
                                         const char           *label)
{
  GtkWidget *button = gtk_toggle_button_new_with_label(label);

  waytator_capture_overlay_init_pill_button(button, NULL);
  g_signal_connect(button, "toggled", G_CALLBACK(waytator_capture_overlay_mode_toggled), view);
  waytator_capture_modes_add(modes, button);
  return GTK_TOGGLE_BUTTON(button);
}

static void
waytator_capture_overlay_magnifier_toggled(GtkToggleButton     *button,
                                           WaytatorCaptureView *view)
{
  const gboolean magnifier = gtk_toggle_button_get_active(button);

  if (view->overlay->updating)
    return;

  waytator_capture_overlay_set_magnifier(view->overlay, magnifier);
  //it stays the way it was left for the next capture
  if (waytator_settings_save_boolean("capture_magnifier", magnifier))
    waytator_window_reload_preferences(GTK_APPLICATION(g_application_get_default()), NULL);
}

//both stay the way they were left for the next recording
static void
waytator_capture_overlay_sound_toggled(GtkToggleButton     *button,
                                       WaytatorCaptureView *view)
{
  WaytatorCaptureOverlay *overlay = view->overlay;
  const gboolean microphone = button == view->microphone_button;
  const gboolean active = gtk_toggle_button_get_active(button);

  if (overlay->updating)
    return;

  if (microphone)
    overlay->microphone = active;
  else
    overlay->system_sound = active;

  overlay->updating = TRUE;
  for (guint i = 0; i < overlay->views->len; i++) {
    WaytatorCaptureView *other = g_ptr_array_index(overlay->views, i);

    gtk_toggle_button_set_active(microphone ? other->microphone_button : other->system_sound_button, active);
  }
  overlay->updating = FALSE;

  if (waytator_settings_save_boolean(microphone ? "record_microphone" : "record_system_sound", active))
    waytator_window_reload_preferences(GTK_APPLICATION(g_application_get_default()), NULL);
}

static GtkWidget *
waytator_capture_overlay_append_toggle(WaytatorCaptureView *view,
                                       const char          *icon_name,
                                       const char          *tooltip,
                                       gboolean             active)
{
  GtkWidget *button = gtk_toggle_button_new();

  gtk_button_set_icon_name(GTK_BUTTON(button), icon_name);
  waytator_capture_overlay_init_pill_button(button, tooltip);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), active);
  gtk_box_append(GTK_BOX(view->pill), button);
  return button;
}

static void
waytator_capture_overlay_append_separator(WaytatorCaptureView *view)
{
  GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_VERTICAL);

  gtk_widget_set_margin_start(separator, 4);
  gtk_widget_set_margin_end(separator, 4);
  gtk_box_append(GTK_BOX(view->pill), separator);
}

//the choices open inside the pill rather than in a popover: closing the overlay while a popover held the keyboard
//sent focus back to a window that was already gone, and GTK crashes on tablet pad events that come with it
static void
waytator_capture_overlay_append_delay(WaytatorCaptureView *view)
{
  static const guint delays[] = { 1, 3, 5, 10 };
  GtkWidget *button = waytator_capture_overlay_append_toggle(view, "alarm-symbolic", "Delay", FALSE);
  GtkWidget *choices = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

  view->delay_button = GTK_TOGGLE_BUTTON(button);
  g_signal_connect(button, "toggled", G_CALLBACK(waytator_capture_overlay_delay_toggled), view);

  //a recording keeps the delay it was given, so the button says which one it is
  if (view->overlay->record) {
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

    view->delay_label = GTK_LABEL(gtk_label_new(NULL));
    gtk_widget_add_css_class(GTK_WIDGET(view->delay_label), "capture-delay-value");
    gtk_box_append(GTK_BOX(content), gtk_image_new_from_icon_name("alarm-symbolic"));
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(view->delay_label));
    gtk_button_set_child(GTK_BUTTON(button), content);
  }

  for (guint i = 0; i < G_N_ELEMENTS(delays); i++) {
    g_autofree char *label = g_strdup_printf("%us", delays[i]);
    GtkWidget *choice = gtk_button_new_with_label(label);

    waytator_capture_overlay_init_pill_button(choice, NULL);
    gtk_widget_add_css_class(choice, "capture-delay-choice");
    g_object_set_data(G_OBJECT(choice), "seconds", GUINT_TO_POINTER(delays[i]));
    g_signal_connect(choice, "clicked", G_CALLBACK(waytator_capture_overlay_delay_clicked), view);
    gtk_box_append(GTK_BOX(choices), choice);
  }

  view->delay_choices = GTK_REVEALER(gtk_revealer_new());
  gtk_revealer_set_transition_type(view->delay_choices, GTK_REVEALER_TRANSITION_TYPE_SLIDE_RIGHT);
  gtk_revealer_set_transition_duration(view->delay_choices, WAYTATOR_CAPTURE_PILL_TRANSITION_MS);
  gtk_revealer_set_child(view->delay_choices, choices);
  gtk_box_append(GTK_BOX(view->pill), GTK_WIDGET(view->delay_choices));
}

//bottom centre until it is dragged somewhere, and always whole on the screen, even while the delay choices widen it
static void
waytator_capture_overlay_get_pill_rect(WaytatorCaptureView *view,
                                       GdkRectangle        *rect)
{
  GtkWidget *stack = gtk_widget_get_parent(view->pill);
  const int width = gtk_widget_get_width(stack);
  const int height = gtk_widget_get_height(stack);
  double x;
  double y;

  gtk_widget_measure(view->pill, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &rect->width, NULL, NULL);
  gtk_widget_measure(view->pill, GTK_ORIENTATION_VERTICAL, rect->width, NULL, &rect->height, NULL, NULL);
  x = view->pill_moved ? view->pill_x : (width - rect->width) / 2.0;
  y = view->pill_moved ? view->pill_y : height - rect->height - 32.0;

  rect->x = CLAMP((int) round(x),
                  WAYTATOR_CAPTURE_PILL_MARGIN,
                  MAX(width - rect->width - WAYTATOR_CAPTURE_PILL_MARGIN, WAYTATOR_CAPTURE_PILL_MARGIN));
  rect->y = CLAMP((int) round(y),
                  WAYTATOR_CAPTURE_PILL_MARGIN,
                  MAX(height - rect->height - WAYTATOR_CAPTURE_PILL_MARGIN, WAYTATOR_CAPTURE_PILL_MARGIN));
}

static gboolean
waytator_capture_overlay_place_pill(GtkOverlay          *stack,
                                    GtkWidget           *widget,
                                    GdkRectangle        *allocation,
                                    WaytatorCaptureView *view)
{
  (void) stack;

  if (widget != view->pill)
    return FALSE;

  waytator_capture_overlay_get_pill_rect(view, allocation);
  return TRUE;
}

static void
waytator_capture_overlay_handle_drag_begin(GtkGestureDrag      *gesture,
                                           double               x,
                                           double               y,
                                           WaytatorCaptureView *view)
{
  GtkWidget *handle = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  graphene_rect_t in_pill;

  if (!gtk_widget_compute_bounds(handle, view->pill, &in_pill))
    return;

  view->grab_x = in_pill.origin.x + x;
  view->grab_y = in_pill.origin.y + y;
  gtk_widget_set_cursor_from_name(handle, "grabbing");
}

//worked out from where the pointer is on the screen, the handle itself moves along under it
static void
waytator_capture_overlay_handle_drag_update(GtkGestureDrag      *gesture,
                                            double               offset_x,
                                            double               offset_y,
                                            WaytatorCaptureView *view)
{
  GtkWidget *handle = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  GtkWidget *stack = gtk_widget_get_parent(view->pill);
  graphene_rect_t on_screen;
  double start_x;
  double start_y;

  if (!gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y)
      || !gtk_widget_compute_bounds(handle, stack, &on_screen))
    return;

  view->pill_moved = TRUE;
  view->pill_x = on_screen.origin.x + start_x + offset_x - view->grab_x;
  view->pill_y = on_screen.origin.y + start_y + offset_y - view->grab_y;
  gtk_widget_queue_allocate(stack);
}

static void
waytator_capture_overlay_handle_drag_end(GtkGestureDrag      *gesture,
                                         double               offset_x,
                                         double               offset_y,
                                         WaytatorCaptureView *view)
{
  GtkWidget *handle = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  GdkRectangle placed;

  waytator_capture_overlay_handle_drag_update(gesture, offset_x, offset_y, view);
  gtk_widget_set_cursor_from_name(handle, "grab");

  //dragged past an edge, it stays where it stopped rather than where the pointer let go
  if (view->pill_moved) {
    waytator_capture_overlay_get_pill_rect(view, &placed);
    view->pill_x = placed.x;
    view->pill_y = placed.y;
  }
}

//so it is clear that this one records
static void
waytator_capture_overlay_append_record_mark(WaytatorCaptureView *view)
{
  GtkWidget *mark = gtk_image_new_from_icon_name("media-record-symbolic");

  gtk_widget_add_css_class(mark, "capture-record-mark");
  gtk_box_append(GTK_BOX(view->pill), mark);
}

static void
waytator_capture_overlay_append_handle(WaytatorCaptureView *view)
{
  GtkWidget *handle = gtk_image_new_from_icon_name("list-drag-handle-symbolic");
  GtkGesture *drag = gtk_gesture_drag_new();

  gtk_widget_add_css_class(handle, "capture-handle");
  gtk_widget_set_tooltip_text(handle, "Move");
  gtk_widget_set_cursor_from_name(handle, "grab");
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
  g_signal_connect(drag, "drag-begin", G_CALLBACK(waytator_capture_overlay_handle_drag_begin), view);
  g_signal_connect(drag, "drag-update", G_CALLBACK(waytator_capture_overlay_handle_drag_update), view);
  g_signal_connect(drag, "drag-end", G_CALLBACK(waytator_capture_overlay_handle_drag_end), view);
  gtk_widget_add_controller(handle, GTK_EVENT_CONTROLLER(drag));
  gtk_box_append(GTK_BOX(view->pill), handle);
}

//the one control there is nothing else to show for
static void
waytator_capture_overlay_append_shift_hint(WaytatorCaptureView *view)
{
  GtkWidget *key = gtk_label_new("Shift");
  GtkWidget *hint = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

  gtk_widget_add_css_class(hint, "capture-shortcut");
  gtk_widget_add_css_class(key, "capture-keycap");
  gtk_widget_set_valign(key, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(hint), gtk_label_new("Hold"));
  gtk_box_append(GTK_BOX(hint), key);
  gtk_box_append(GTK_BOX(hint), gtk_label_new("to slow the cursor"));

  //only Area has a use for it, and the bar eases to its new width rather than jumping when it goes
  view->shift_hint = GTK_REVEALER(gtk_revealer_new());
  gtk_revealer_set_transition_type(view->shift_hint, GTK_REVEALER_TRANSITION_TYPE_SLIDE_LEFT);
  gtk_revealer_set_transition_duration(view->shift_hint, WAYTATOR_CAPTURE_PILL_TRANSITION_MS);
  gtk_revealer_set_child(view->shift_hint, hint);
  gtk_box_append(GTK_BOX(view->pill), GTK_WIDGET(view->shift_hint));
}

static void
waytator_capture_overlay_cancel_clicked(GtkButton           *button,
                                        WaytatorCaptureView *view)
{
  (void) button;

  waytator_capture_overlay_finish(view->overlay, NULL);
}

static void
waytator_capture_overlay_build_pill(WaytatorCaptureView *view)
{
  WaytatorCaptureModes *modes = g_object_new(WAYTATOR_TYPE_CAPTURE_MODES, NULL);
  GtkWidget *cancel = gtk_button_new_from_icon_name("window-close-symbolic");
  GtkWidget *button;

  view->pill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_add_css_class(view->pill, "capture-pill");

  waytator_capture_overlay_append_handle(view);
  if (view->overlay->record)
    waytator_capture_overlay_append_record_mark(view);

  view->area_button = waytator_capture_overlay_new_mode_button(view, modes, "Area");
  view->screen_button = waytator_capture_overlay_new_mode_button(view, modes, "Screen");
  gtk_toggle_button_set_group(view->screen_button, view->area_button);
  if (view->overlay->record) {
    view->window_button = waytator_capture_overlay_new_mode_button(view, modes, "Window");
    gtk_toggle_button_set_group(view->window_button, view->area_button);
  }
  gtk_box_append(GTK_BOX(view->pill), GTK_WIDGET(modes));
  waytator_capture_overlay_append_separator(view);

  button = waytator_capture_overlay_append_toggle(view,
                                                  "waytator-magnifier-symbolic",
                                                  "Magnifier",
                                                  view->overlay->magnifier);
  g_signal_connect(button, "toggled", G_CALLBACK(waytator_capture_overlay_magnifier_toggled), view);
  view->magnifier_button = GTK_TOGGLE_BUTTON(button);

  if (view->overlay->offer_hide) {
    button = waytator_capture_overlay_append_toggle(view,
                                                    "view-conceal-symbolic",
                                                    "Hide waytator",
                                                    view->overlay->hide_waytator);
    g_signal_connect(button, "toggled", G_CALLBACK(waytator_capture_overlay_hide_toggled), view->overlay);
  }

  waytator_capture_overlay_append_delay(view);
  waytator_capture_overlay_append_separator(view);

  if (view->overlay->record) {
    button = waytator_capture_overlay_append_toggle(view,
                                                    "audio-volume-high-symbolic",
                                                    "System sound",
                                                    view->overlay->system_sound);
    g_signal_connect(button, "toggled", G_CALLBACK(waytator_capture_overlay_sound_toggled), view);
    view->system_sound_button = GTK_TOGGLE_BUTTON(button);

    button = waytator_capture_overlay_append_toggle(view,
                                                    "audio-input-microphone-symbolic",
                                                    "Microphone",
                                                    view->overlay->microphone);
    g_signal_connect(button, "toggled", G_CALLBACK(waytator_capture_overlay_sound_toggled), view);
    view->microphone_button = GTK_TOGGLE_BUTTON(button);
    waytator_capture_overlay_append_separator(view);
  }

  waytator_capture_overlay_append_shift_hint(view);

  waytator_capture_overlay_init_pill_button(cancel, "Cancel (Esc)");
  gtk_widget_add_css_class(cancel, "capture-cancel");
  g_signal_connect(cancel, "clicked", G_CALLBACK(waytator_capture_overlay_cancel_clicked), view);
  gtk_box_append(GTK_BOX(view->pill), cancel);
}

static gboolean
waytator_capture_overlay_key_pressed(GtkEventControllerKey  *controller,
                                     guint                   keyval,
                                     guint                   keycode,
                                     GdkModifierType         state,
                                     WaytatorCaptureOverlay *overlay)
{
  (void) controller;
  (void) keycode;
  (void) state;

  if (keyval == GDK_KEY_Shift_L || keyval == GDK_KEY_Shift_R) {
    waytator_capture_overlay_start_brake(overlay);
    return GDK_EVENT_STOP;
  }

  if (keyval != GDK_KEY_Escape)
    return GDK_EVENT_PROPAGATE;

  waytator_capture_overlay_finish(overlay, NULL);
  return GDK_EVENT_STOP;
}

static void
waytator_capture_overlay_key_released(GtkEventControllerKey  *controller,
                                      guint                   keyval,
                                      guint                   keycode,
                                      GdkModifierType         state,
                                      WaytatorCaptureOverlay *overlay)
{
  (void) controller;
  (void) keycode;
  (void) state;

  if (keyval == GDK_KEY_Shift_L || keyval == GDK_KEY_Shift_R)
    waytator_capture_overlay_stop_brake(overlay);
}

static gboolean
waytator_capture_overlay_close_requested(GtkWindow              *window,
                                         WaytatorCaptureOverlay *overlay)
{
  (void) window;

  waytator_capture_overlay_finish(overlay, NULL);
  return GDK_EVENT_STOP;
}

static GdkMonitor *
waytator_capture_overlay_find_monitor(WaytatorCaptureOutput *output)
{
  GListModel *monitors = gdk_display_get_monitors(gdk_display_get_default());
  GdkMonitor *same_geometry = NULL;

  for (guint i = 0; i < g_list_model_get_n_items(monitors); i++) {
    g_autoptr(GdkMonitor) monitor = g_list_model_get_item(monitors, i);
    GdkRectangle geometry;

    if (output->name != NULL && g_strcmp0(gdk_monitor_get_connector(monitor), output->name) == 0)
      return monitor;

    gdk_monitor_get_geometry(monitor, &geometry);
    if (geometry.x == output->x
        && geometry.y == output->y
        && geometry.width == output->width
        && geometry.height == output->height)
      same_geometry = monitor;
  }

  return same_geometry;
}

static void
waytator_capture_overlay_add_window(WaytatorCaptureOverlay *overlay,
                                    WaytatorCaptureOutput  *output,
                                    GdkMonitor             *monitor)
{
  static const GtkLayerShellEdge edges[] = {
    GTK_LAYER_SHELL_EDGE_LEFT,
    GTK_LAYER_SHELL_EDGE_RIGHT,
    GTK_LAYER_SHELL_EDGE_TOP,
    GTK_LAYER_SHELL_EDGE_BOTTOM,
  };
  GtkWindow *window = GTK_WINDOW(gtk_window_new());
  GtkWidget *stack = gtk_overlay_new();
  GtkEventController *keys = gtk_event_controller_key_new();
  WaytatorCaptureView *view = g_object_new(WAYTATOR_TYPE_CAPTURE_VIEW, NULL);

  view->overlay = overlay;
  view->output = output;
  view->texture = waytator_capture_output_create_texture(output);
  waytator_capture_overlay_build_pill(view);

  gtk_overlay_set_child(GTK_OVERLAY(stack), GTK_WIDGET(view));
  gtk_overlay_add_overlay(GTK_OVERLAY(stack), view->pill);
  g_signal_connect(stack, "get-child-position", G_CALLBACK(waytator_capture_overlay_place_pill), view);
  gtk_window_set_child(window, stack);
  gtk_window_set_decorated(window, FALSE);
  gtk_widget_add_css_class(GTK_WIDGET(window), "capture-overlay");

  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(waytator_capture_overlay_key_pressed), overlay);
  g_signal_connect(keys, "key-released", G_CALLBACK(waytator_capture_overlay_key_released), overlay);
  gtk_widget_add_controller(GTK_WIDGET(window), keys);
  g_signal_connect(window, "close-request", G_CALLBACK(waytator_capture_overlay_close_requested), overlay);

  //gnome has no layer shell, a fullscreen window is the closest thing there
  if (gtk_layer_is_supported()) {
    gtk_layer_init_for_window(window);
    gtk_layer_set_namespace(window, "waytator-capture");
    gtk_layer_set_layer(window, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_monitor(window, monitor);
    for (guint i = 0; i < G_N_ELEMENTS(edges); i++)
      gtk_layer_set_anchor(window, edges[i], TRUE);
    gtk_layer_set_exclusive_zone(window, -1);
    gtk_layer_set_keyboard_mode(window, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);
  } else {
    gtk_window_fullscreen_on_monitor(window, monitor);
  }

  g_ptr_array_add(overlay->windows, window);
  g_ptr_array_add(overlay->views, view);
}

static WaytatorCaptureOverlay *
waytator_capture_overlay_new(GPtrArray                           *outputs,
                             gboolean                             offer_hide,
                             gboolean                             hide_waytator,
                             WaytatorCaptureOverlayRetakeCallback retake,
                             gpointer                             user_data)
{
  WaytatorCaptureOverlay *overlay = g_new0(WaytatorCaptureOverlay, 1);
  WaytatorSettings settings;

  waytator_window_ensure_css_loaded();
  waytator_settings_load(&settings);

  overlay->outputs = outputs;
  overlay->windows = g_ptr_array_new();
  overlay->views = g_ptr_array_new();
  overlay->pill_style = waytator_capture_overlay_new_pill_style(&settings);
  overlay->magnifier = settings.capture_magnifier;
  overlay->system_sound = settings.record_system_sound;
  overlay->microphone = settings.record_microphone;
  overlay->brake_speed = settings.capture_brake_speed;
  overlay->offer_hide = offer_hide;
  overlay->hide_waytator = hide_waytator;
  overlay->retake = retake;
  overlay->user_data = user_data;
  waytator_settings_clear(&settings);
  return overlay;
}

//FALSE when none of the outputs is on a known monitor, the overlay is gone then
static gboolean
waytator_capture_overlay_present(WaytatorCaptureOverlay *overlay,
                                 WaytatorCaptureMode     mode)
{
  for (guint i = 0; i < overlay->outputs->len; i++) {
    WaytatorCaptureOutput *output = g_ptr_array_index(overlay->outputs, i);
    GdkMonitor *monitor = waytator_capture_overlay_find_monitor(output);

    if (monitor != NULL)
      waytator_capture_overlay_add_window(overlay, output, monitor);
  }

  if (overlay->windows->len == 0) {
    waytator_capture_overlay_free(overlay);
    return FALSE;
  }

  waytator_capture_overlay_set_mode(overlay, mode);
  if (overlay->record)
    waytator_capture_overlay_set_delay(overlay, overlay->delay_seconds);
  for (guint i = 0; i < overlay->windows->len; i++)
    gtk_window_present(g_ptr_array_index(overlay->windows, i));

  return TRUE;
}

//takes the outputs, and returns FALSE without calling back when none of them is on a known monitor
gboolean
waytator_capture_overlay_show(GPtrArray                           *outputs,
                              WaytatorCaptureMode                  mode,
                              gboolean                             offer_hide,
                              gboolean                             hide_waytator,
                              WaytatorCaptureOverlayRetakeCallback retake,
                              WaytatorCaptureOverlayCallback       callback,
                              gpointer                             user_data)
{
  WaytatorCaptureOverlay *overlay = waytator_capture_overlay_new(outputs, offer_hide, hide_waytator, retake, user_data);

  overlay->callback = callback;
  return waytator_capture_overlay_present(overlay, mode);
}

//a window is picked in the desktop's own list, so it is never what the overlay starts in
gboolean
waytator_capture_overlay_show_record(GPtrArray                           *outputs,
                                     WaytatorCaptureMode                  mode,
                                     guint                                delay_seconds,
                                     gboolean                             offer_hide,
                                     gboolean                             hide_waytator,
                                     WaytatorCaptureOverlayRetakeCallback retake,
                                     WaytatorCaptureOverlayRecordCallback callback,
                                     gpointer                             user_data)
{
  WaytatorCaptureOverlay *overlay = waytator_capture_overlay_new(outputs, offer_hide, hide_waytator, retake, user_data);

  overlay->record = TRUE;
  overlay->delay_seconds = delay_seconds;
  overlay->record_callback = callback;
  return waytator_capture_overlay_present(overlay,
                                          mode == WAYTATOR_CAPTURE_WINDOW ? WAYTATOR_CAPTURE_AREA : mode);
}
