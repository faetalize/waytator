#pragma once

#include <gtk/gtk.h>

#include "waytator-video-edit.h"
#include "waytator-video-source.h"

G_BEGIN_DECLS

//shows a frame the way the edit turns it out, cropped, rotated and flipped, fitted to the widget. while
//cropping it shows the whole picture with the crop on top, to drag into shape
#define WAYTATOR_TYPE_VIDEO_VIEW (waytator_video_view_get_type())
G_DECLARE_FINAL_TYPE(WaytatorVideoView, waytator_video_view, WAYTATOR, VIDEO_VIEW, GtkWidget)

GtkWidget *waytator_video_view_new(void);
//the edit the crop goes into. the view follows its crop and rotation
void waytator_video_view_set_edit(WaytatorVideoView *self,
                                  WaytatorVideoEdit *edit);
//quarter turns clockwise that decoded frames need to be upright
void waytator_video_view_set_orientation(WaytatorVideoView *self,
                                         int                orientation);
//a picture in decoder orientation, of the source's size or smaller. segment says which segment's flips it
//shows with
void waytator_video_view_set_texture(WaytatorVideoView *self,
                                     GdkTexture        *texture,
                                     guint              segment);
void waytator_video_view_set_cropping(WaytatorVideoView *self,
                                      gboolean           cropping);
gboolean waytator_video_view_get_cropping(WaytatorVideoView *self);

G_END_DECLS
