#pragma once

#include <glib-object.h>

#include "waytator-video-edit.h"
#include "waytator-video-source.h"

G_BEGIN_DECLS

//the edited sound always comes out as interleaved float samples, two channels at 48 kHz
#define WAYTATOR_VIDEO_AUDIO_RATE 48000
#define WAYTATOR_VIDEO_AUDIO_CHANNELS 2

//plays the sound of an edit from a timeline time on: each segment's stretch of the source with its speed,
//pitch and mute, joined with short fades so cuts never click. the same code makes the preview and the
//export, so they sound the same. one thread at a time
typedef struct _WaytatorVideoAudio WaytatorVideoAudio;

WaytatorVideoAudio *waytator_video_audio_new(WaytatorVideoSource *source,
                                             WaytatorVideoState  *state,
                                             GError             **error);
void waytator_video_audio_free(WaytatorVideoAudio *audio);
//picks the edit up again at the timeline time it had reached
void waytator_video_audio_set_state(WaytatorVideoAudio *audio,
                                    WaytatorVideoState *state);
//how fast the preview plays the timeline, 1 being normal. it is silent where that would be too fast to
//make out, and going backwards
void waytator_video_audio_set_rate(WaytatorVideoAudio *audio,
                                   double              rate);
void waytator_video_audio_seek(WaytatorVideoAudio *audio,
                               gint64              time);
//up to n_frames frames of samples, fewer only at the end. time says where on the timeline the first one
//belongs. 0 once the timeline is over
gsize waytator_video_audio_read(WaytatorVideoAudio *audio,
                                float              *samples,
                                gsize               n_frames,
                                gint64             *time,
                                GError            **error);

//sends sound to the speakers through pipewire, reading it from an audio on a thread of its own
typedef struct _WaytatorVideoAudioOutput WaytatorVideoAudioOutput;

//fails when there is no pipewire to play to, the video then plays without sound
WaytatorVideoAudioOutput *waytator_video_audio_output_new(GError **error);
void waytator_video_audio_output_free(WaytatorVideoAudioOutput *output);
//plays from wherever the audio is now. nothing else may touch the audio until stop returns
void waytator_video_audio_output_start(WaytatorVideoAudioOutput *output,
                                       WaytatorVideoAudio       *audio);
//silences it straight away and lets go of the audio, dropping whatever was still queued
void waytator_video_audio_output_stop(WaytatorVideoAudioOutput *output);
//the timeline time of the sound reaching the speakers right now, the clock the picture keeps up with.
//FALSE until the first sample gets there
gboolean waytator_video_audio_output_get_time(WaytatorVideoAudioOutput *output,
                                              gint64                   *time);
//whether everything read from the audio has been played
gboolean waytator_video_audio_output_is_drained(WaytatorVideoAudioOutput *output);

//the loudness of the source over time for the audio rail, found on a thread of its own
#define WAYTATOR_TYPE_VIDEO_WAVEFORM (waytator_video_waveform_get_type())
G_DECLARE_FINAL_TYPE(WaytatorVideoWaveform, waytator_video_waveform, WAYTATOR, VIDEO_WAVEFORM, GObject)

//emits "updated" on the main thread as more of the source is read, and once it is all done
WaytatorVideoWaveform *waytator_video_waveform_new(WaytatorVideoSource *source);
//the lowest and highest sample, from -1 to 1, in each of n equal parts of a stretch of the source, the
//channels mixed together. parts not read yet come back as 0. FALSE when the source has no sound
gboolean waytator_video_waveform_get_peaks(WaytatorVideoWaveform *self,
                                           gint64                 source_start,
                                           gint64                 source_end,
                                           guint                  n,
                                           float                 *minimum,
                                           float                 *maximum);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaytatorVideoAudio, waytator_video_audio_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaytatorVideoAudioOutput, waytator_video_audio_output_free)

G_END_DECLS
