#pragma once

#include <glib.h>

G_BEGIN_DECLS

//puts the sound of several sources onto one timeline that starts at a time given and runs at exactly
//WAYTATOR_RECORD_AUDIO_RATE frames a second, whatever the clocks of the sources do. it knows nothing of
//pipewire and takes the time as an argument, so it can be tested with made up sound. one thread at a
//time, except for set_muted
typedef struct _WaytatorRecordMixer WaytatorRecordMixer;

//the next n_frames frames of the mix, interleaved stereo floats. time is when the first of them belongs, in
//the same microseconds as the start, exactly start plus the frames emitted before them over the rate
typedef void (*WaytatorRecordMixerFunc)(const float *samples,
                                        guint        n_frames,
                                        gint64       time,
                                        gpointer     user_data);

//the output starts at start, a g_get_monotonic_time() time. func is called from emit, in order and
//without gaps
WaytatorRecordMixer *waytator_record_mixer_new(guint                   n_sources,
                                               gint64                  start,
                                               WaytatorRecordMixerFunc func,
                                               gpointer                user_data);
void waytator_record_mixer_free(WaytatorRecordMixer *mixer);
//n_frames frames of a source, following on from its last call. time is the estimate of when the first of
//them was heard. the mixer smooths those estimates, drops or repeats the odd frame when the source's clock
//runs away from the timeline and puts the sound back after a stall. nothing comes out until emit
void waytator_record_mixer_push(WaytatorRecordMixer *mixer,
                                guint                source,
                                const float         *samples,
                                guint                n_frames,
                                gint64               time);
//hands func everything that is ready as of now: the stretch every source has delivered, and anything
//older than the latency bound whatever a slow source has delivered, which counts as silence there. call it
//after every push and now and then besides, so the output keeps flowing when the sources go quiet
void waytator_record_mixer_emit(WaytatorRecordMixer *mixer,
                                gint64               now);
//a muted source goes on being taken in and kept in place but adds nothing. any thread
void waytator_record_mixer_set_muted(WaytatorRecordMixer *mixer,
                                     guint                source,
                                     gboolean             muted);
//a source that will deliver no more, which then no longer holds the output back. what it delivered still
//comes out
void waytator_record_mixer_finish(WaytatorRecordMixer *mixer,
                                  guint                source);

G_END_DECLS
