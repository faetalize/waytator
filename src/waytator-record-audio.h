#pragma once

#include <glib.h>

G_BEGIN_DECLS

//the sound of a recording comes out at this rate, in stereo, as interleaved floats
#define WAYTATOR_RECORD_AUDIO_RATE 48000
#define WAYTATOR_RECORD_AUDIO_CHANNELS 2

typedef enum {
  //everything the computer plays, through its default output
  WAYTATOR_RECORD_AUDIO_SYSTEM = 1 << 0,
  WAYTATOR_RECORD_AUDIO_MICROPHONE = 1 << 1,
} WaytatorRecordAudioSources;

//n_frames frames of sound, each WAYTATOR_RECORD_AUDIO_CHANNELS floats, following on from the last call
//without a gap. time is when the first of them was heard, in g_get_monotonic_time()'s microseconds. called
//on the capture's own thread, never the main one, and never again once waytator_record_audio_free returns
typedef void (*WaytatorRecordAudioFunc)(const float *samples,
                                        guint        n_frames,
                                        gint64       time,
                                        gpointer     user_data);

typedef struct _WaytatorRecordAudio WaytatorRecordAudio;

//starts taking in the sources, mixed into one, from now on. microphone is the node.name of the microphone
//to use, NULL or empty for the default one, and is ignored without WAYTATOR_RECORD_AUDIO_MICROPHONE. a
//microphone that isn't there any more falls back to the default one. sources can't be 0
WaytatorRecordAudio *waytator_record_audio_new(WaytatorRecordAudioSources sources,
                                               const char                *microphone,
                                               WaytatorRecordAudioFunc    func,
                                               gpointer                   user_data,
                                               GError                   **error);
//silences the microphone in the mix while muted, the rest carries on. any thread
void waytator_record_audio_set_microphone_muted(WaytatorRecordAudio *audio,
                                                gboolean             muted);
//stops taking in sound, waiting for the capture thread so func is never called after this returns
void waytator_record_audio_free(WaytatorRecordAudio *audio);

//one microphone the system has
typedef struct {
  //PipeWire's node.name, which stays the same from one start to the next, for the settings
  char *name;
  //what to show for it, e.g. "Blue Yeti Analog Stereo"
  char *description;
} WaytatorRecordMicrophone;

void waytator_record_microphone_free(WaytatorRecordMicrophone *microphone);
//the microphones there are right now, as WaytatorRecordMicrophone. blocks for a moment while PipeWire
//answers, so call it when the list is about to be shown rather than all the time
GPtrArray *waytator_record_audio_list_microphones(GError **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaytatorRecordAudio, waytator_record_audio_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaytatorRecordMicrophone, waytator_record_microphone_free)

G_END_DECLS
