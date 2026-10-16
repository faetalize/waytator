#include <glib/gstdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <math.h>
#include <unistd.h>

#include "waytator-video-edit.h"
#include "waytator-video-export.h"
#include "waytator-video-source.h"

#define WIDTH 320
#define HEIGHT 240
#define N_FRAMES 30

//the four quarters of every test frame, by where they sit in the picture
enum {
  TOP_LEFT,
  TOP_RIGHT,
  BOTTOM_LEFT,
  BOTTOM_RIGHT,
};

static const guint8 quarter_colours[4][3] = {
  [TOP_LEFT] = { 230, 30, 30 },
  [TOP_RIGHT] = { 30, 200, 30 },
  [BOTTOM_LEFT] = { 30, 30, 230 },
  [BOTTOM_RIGHT] = { 240, 240, 240 },
};

//a short video of four coloured quarters, tagged as needing turns quarter turns clockwise to be upright,
//with a tone along with it when sound is set
static char *
make_video_with(int      turns,
                gboolean sound)
{
  g_autofree char *path = g_build_filename(g_get_tmp_dir(), "waytator-test-export-XXXXXX.mp4", NULL);
  const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
  AVFormatContext *muxer = NULL;
  AVCodecContext *encoder;
  AVStream *stream;
  AVFrame *rgb = av_frame_alloc();
  AVFrame *yuv = av_frame_alloc();
  AVPacket *packet = av_packet_alloc();
  struct SwsContext *scaler;
  AVCodecContext *sound_encoder = NULL;
  AVStream *sound_stream = NULL;
  int fd = g_mkstemp(path);

  g_assert_cmpint(fd, >=, 0);
  close(fd);
  if (codec == NULL)
    codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
  g_assert_nonnull(codec);

  g_assert_cmpint(avformat_alloc_output_context2(&muxer, NULL, "mp4", path), >=, 0);
  encoder = avcodec_alloc_context3(codec);
  encoder->width = WIDTH;
  encoder->height = HEIGHT;
  encoder->pix_fmt = AV_PIX_FMT_YUV420P;
  encoder->time_base = (AVRational) { 1, 30 };
  encoder->framerate = (AVRational) { 30, 1 };
  encoder->gop_size = 10;
  encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  g_assert_cmpint(avcodec_open2(encoder, codec, NULL), >=, 0);
  stream = avformat_new_stream(muxer, NULL);
  stream->time_base = encoder->time_base;
  avcodec_parameters_from_context(stream->codecpar, encoder);
  if (turns != 0) {
    int32_t matrix[9];
    AVPacketSideData *side;

    //set takes the turn clockwise, the same way the picture needs turning to be upright
    av_display_rotation_set(matrix, 90.0 * turns);
    side = av_packet_side_data_new(&stream->codecpar->coded_side_data,
                                   &stream->codecpar->nb_coded_side_data,
                                   AV_PKT_DATA_DISPLAYMATRIX, sizeof matrix, 0);
    memcpy(side->data, matrix, sizeof matrix);
  }
  if (sound) {
    const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;

    sound_encoder = avcodec_alloc_context3(avcodec_find_encoder(AV_CODEC_ID_AAC));
    sound_encoder->sample_rate = 48000;
    sound_encoder->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_copy(&sound_encoder->ch_layout, &stereo);
    sound_encoder->time_base = (AVRational) { 1, 48000 };
    sound_encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    g_assert_cmpint(avcodec_open2(sound_encoder, sound_encoder->codec, NULL), >=, 0);
    sound_stream = avformat_new_stream(muxer, NULL);
    sound_stream->time_base = sound_encoder->time_base;
    avcodec_parameters_from_context(sound_stream->codecpar, sound_encoder);
  }
  g_assert_cmpint(avio_open(&muxer->pb, path, AVIO_FLAG_WRITE), >=, 0);
  g_assert_cmpint(avformat_write_header(muxer, NULL), >=, 0);

  rgb->format = AV_PIX_FMT_RGB24;
  rgb->width = WIDTH;
  rgb->height = HEIGHT;
  av_frame_get_buffer(rgb, 0);
  yuv->format = AV_PIX_FMT_YUV420P;
  yuv->width = WIDTH;
  yuv->height = HEIGHT;
  av_frame_get_buffer(yuv, 0);
  for (int y = 0; y < HEIGHT; y++) {
    for (int x = 0; x < WIDTH; x++) {
      const int quarter = (y >= HEIGHT / 2 ? 2 : 0) + (x >= WIDTH / 2 ? 1 : 0);

      memcpy(rgb->data[0] + y * rgb->linesize[0] + x * 3, quarter_colours[quarter], 3);
    }
  }
  scaler = sws_getContext(WIDTH, HEIGHT, AV_PIX_FMT_RGB24, WIDTH, HEIGHT, AV_PIX_FMT_YUV420P,
                          SWS_BILINEAR, NULL, NULL, NULL);
  sws_scale(scaler, (const uint8_t *const *) rgb->data, rgb->linesize, 0, HEIGHT, yuv->data, yuv->linesize);

  for (int i = 0; i <= N_FRAMES; i++) {
    yuv->pts = i;
    g_assert_cmpint(avcodec_send_frame(encoder, i < N_FRAMES ? yuv : NULL), >=, 0);
    while (avcodec_receive_packet(encoder, packet) >= 0) {
      av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
      packet->stream_index = stream->index;
      av_interleaved_write_frame(muxer, packet);
    }
  }
  if (sound) {
    //a 440 Hz tone as long as the picture
    const int total = 48000 * N_FRAMES / 30;
    AVFrame *samples = av_frame_alloc();

    for (int start = 0; start <= total; start += sound_encoder->frame_size) {
      if (start < total) {
        av_frame_unref(samples);
        samples->format = AV_SAMPLE_FMT_FLTP;
        samples->sample_rate = 48000;
        samples->nb_samples = MIN(sound_encoder->frame_size, total - start);
        av_channel_layout_copy(&samples->ch_layout, &sound_encoder->ch_layout);
        av_frame_get_buffer(samples, 0);
        for (int i = 0; i < samples->nb_samples; i++) {
          const float value = 0.3f * sinf(2.0f * G_PI * 440.0f * (start + i) / 48000.0f);

          ((float *) samples->data[0])[i] = value;
          ((float *) samples->data[1])[i] = value;
        }
        samples->pts = start;
      }
      g_assert_cmpint(avcodec_send_frame(sound_encoder, start < total ? samples : NULL), >=, 0);
      while (avcodec_receive_packet(sound_encoder, packet) >= 0) {
        av_packet_rescale_ts(packet, sound_encoder->time_base, sound_stream->time_base);
        packet->stream_index = sound_stream->index;
        av_interleaved_write_frame(muxer, packet);
      }
    }
    av_frame_free(&samples);
    avcodec_free_context(&sound_encoder);
  }
  av_write_trailer(muxer);

  sws_freeContext(scaler);
  av_frame_free(&rgb);
  av_frame_free(&yuv);
  av_packet_free(&packet);
  avcodec_free_context(&encoder);
  avio_closep(&muxer->pb);
  avformat_free_context(muxer);
  return g_steal_pointer(&path);
}

static char *
make_video(int turns)
{
  return make_video_with(turns, FALSE);
}

//the first frame of a file as RGB
static AVFrame *
read_first_frame(const char *path)
{
  AVFormatContext *demuxer = NULL;
  const AVCodec *codec = NULL;
  AVCodecContext *decoder;
  AVPacket *packet = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  AVFrame *rgb = av_frame_alloc();
  struct SwsContext *scaler;
  int index;
  gboolean got = FALSE;

  g_assert_cmpint(avformat_open_input(&demuxer, path, NULL, NULL), >=, 0);
  avformat_find_stream_info(demuxer, NULL);
  index = av_find_best_stream(demuxer, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
  g_assert_cmpint(index, >=, 0);
  decoder = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(decoder, demuxer->streams[index]->codecpar);
  g_assert_cmpint(avcodec_open2(decoder, codec, NULL), >=, 0);

  while (!got && av_read_frame(demuxer, packet) >= 0) {
    if (packet->stream_index == index) {
      avcodec_send_packet(decoder, packet);
      got = avcodec_receive_frame(decoder, frame) >= 0;
    }
    av_packet_unref(packet);
  }
  if (!got) {
    avcodec_send_packet(decoder, NULL);
    got = avcodec_receive_frame(decoder, frame) >= 0;
  }
  g_assert_true(got);

  rgb->format = AV_PIX_FMT_RGB24;
  rgb->width = frame->width;
  rgb->height = frame->height;
  av_frame_get_buffer(rgb, 0);
  scaler = sws_getContext(frame->width, frame->height, frame->format, rgb->width, rgb->height, AV_PIX_FMT_RGB24,
                          SWS_BILINEAR, NULL, NULL, NULL);
  sws_scale(scaler, (const uint8_t *const *) frame->data, frame->linesize, 0, frame->height, rgb->data, rgb->linesize);

  sws_freeContext(scaler);
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&decoder);
  avformat_close_input(&demuxer);
  return rgb;
}

//which test colour is closest to the pixel a quarter of the way in from a corner
static int
quarter_at(const AVFrame *rgb,
           int            corner)
{
  const int x = (corner == TOP_LEFT || corner == BOTTOM_LEFT) ? rgb->width / 4 : rgb->width * 3 / 4;
  const int y = (corner == TOP_LEFT || corner == TOP_RIGHT) ? rgb->height / 4 : rgb->height * 3 / 4;
  const guint8 *pixel = rgb->data[0] + y * rgb->linesize[0] + x * 3;
  int best = -1;
  int best_distance = G_MAXINT;

  for (int i = 0; i < 4; i++) {
    const int distance = abs(pixel[0] - quarter_colours[i][0])
                       + abs(pixel[1] - quarter_colours[i][1])
                       + abs(pixel[2] - quarter_colours[i][2]);

    if (distance < best_distance) {
      best_distance = distance;
      best = i;
    }
  }

  return best;
}

//where a corner of the picture ends up after turning it clockwise and then flipping it, worked out by
//following the corner around rather than the way the export does it
static int
expected_corner(int      corner,
                int      turns,
                gboolean flip_horizontal,
                gboolean flip_vertical)
{
  int column = corner % 2;
  int row = corner / 2;

  for (int i = 0; i < (turns & 3); i++) {
    //a clockwise quarter turn takes the top left to the top right
    const int turned_column = 1 - row;

    row = column;
    column = turned_column;
  }
  if (flip_horizontal)
    column = 1 - column;
  if (flip_vertical)
    row = 1 - row;
  return row * 2 + column;
}

static WaytatorVideoProfile *
test_profile(void)
{
  WaytatorVideoProfile *profile = g_new0(WaytatorVideoProfile, 1);

  profile->id = g_strdup("test");
  profile->name = g_strdup("Test");
  profile->codec = WAYTATOR_VIDEO_CODEC_H264;
  profile->encoder = WAYTATOR_VIDEO_ENCODER_AUTO;
  profile->quality = 80;
  profile->sound = WAYTATOR_VIDEO_SOUND_NONE;
  return profile;
}

static void
check_transform(const char  *input,
                int          source_turns,
                int          rotation,
                gboolean     flip_horizontal,
                gboolean     flip_vertical,
                WaytatorVideoEncoder encoder)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(WaytatorVideoSource) source = waytator_video_source_open(input, NULL, &error);
  g_autoptr(WaytatorVideoEdit) edit = NULL;
  g_autoptr(WaytatorVideoState) state = NULL;
  g_autoptr(WaytatorVideoProfile) profile = test_profile();
  g_autofree char *output = g_build_filename(g_get_tmp_dir(), "waytator-test-export-out-XXXXXX.mp4", NULL);
  const WaytatorVideoSegment values = { .flip_horizontal = flip_horizontal, .flip_vertical = flip_vertical };
  AVFrame *result;
  int width;
  int height;
  int fd;

  g_assert_no_error(error);
  g_assert_cmpint(waytator_video_source_get_orientation(source), ==, source_turns);
  edit = waytator_video_edit_new(waytator_video_source_get_frames(source),
                                 waytator_video_source_get_width(source),
                                 waytator_video_source_get_height(source),
                                 FALSE);
  waytator_video_edit_set_rotation(edit, rotation);
  waytator_video_edit_set(edit, 0, G_MAXINT64, &values,
                          WAYTATOR_VIDEO_FIELD_FLIP_HORIZONTAL | WAYTATOR_VIDEO_FIELD_FLIP_VERTICAL);
  state = waytator_video_edit_get_state(edit);
  profile->encoder = encoder;

  fd = g_mkstemp(output);
  close(fd);
  g_assert_true(waytator_video_export(source, state, profile, output, NULL, NULL, NULL, &error));
  g_assert_no_error(error);

  result = read_first_frame(output);
  waytator_video_state_get_output_size(state, &width, &height);
  g_assert_cmpint(result->width, ==, width);
  g_assert_cmpint(result->height, ==, height);
  for (int corner = 0; corner < 4; corner++) {
    //the source's own turn comes first, it is how the picture is meant to be seen
    const int upright = expected_corner(corner, source_turns, FALSE, FALSE);

    g_assert_cmpint(quarter_at(result, expected_corner(upright, rotation, flip_horizontal, flip_vertical)), ==, corner);
  }

  av_frame_free(&result);
  g_unlink(output);
}

static void
test_transforms(gconstpointer data)
{
  const WaytatorVideoEncoder encoder = GPOINTER_TO_INT(data);

  for (int source_turns = 0; source_turns < 4; source_turns += 3) {
    g_autofree char *input = make_video(source_turns);

    for (int rotation = 0; rotation < 4; rotation++) {
      for (int flips = 0; flips < 4; flips++)
        check_transform(input, source_turns, rotation, flips & 1, (flips & 2) != 0, encoder);
    }
    g_unlink(input);
  }
}

static void
test_crop_and_speed(void)
{
  g_autofree char *input = make_video(0);
  g_autoptr(GError) error = NULL;
  g_autoptr(WaytatorVideoSource) source = waytator_video_source_open(input, NULL, &error);
  g_autoptr(WaytatorVideoEdit) edit = NULL;
  g_autoptr(WaytatorVideoState) state = NULL;
  g_autoptr(WaytatorVideoProfile) profile = test_profile();
  g_autofree char *output = g_build_filename(g_get_tmp_dir(), "waytator-test-export-crop-XXXXXX.mp4", NULL);
  const WaytatorVideoCrop crop = { WIDTH / 2, 0, WIDTH / 2, HEIGHT / 2 };
  const WaytatorVideoSegment values = { .speed = 2.0 };
  AVFormatContext *demuxer = NULL;
  AVFrame *result;
  int fd;

  g_assert_no_error(error);
  edit = waytator_video_edit_new(waytator_video_source_get_frames(source), WIDTH, HEIGHT, FALSE);
  waytator_video_edit_set_crop(edit, &crop);
  waytator_video_edit_set(edit, 0, G_MAXINT64, &values, WAYTATOR_VIDEO_FIELD_SPEED);
  state = waytator_video_edit_get_state(edit);
  fd = g_mkstemp(output);
  close(fd);
  g_assert_true(waytator_video_export(source, state, profile, output, NULL, NULL, NULL, &error));
  g_assert_no_error(error);

  //the top right quarter only, at half the length
  result = read_first_frame(output);
  g_assert_cmpint(result->width, ==, WIDTH / 2);
  g_assert_cmpint(result->height, ==, HEIGHT / 2);
  for (int corner = 0; corner < 4; corner++)
    g_assert_cmpint(quarter_at(result, corner), ==, TOP_RIGHT);
  av_frame_free(&result);

  g_assert_cmpint(avformat_open_input(&demuxer, output, NULL, NULL), >=, 0);
  avformat_find_stream_info(demuxer, NULL);
  g_assert_cmpfloat(fabs(demuxer->duration / (double) AV_TIME_BASE - N_FRAMES / 30.0 / 2.0), <, 0.1);
  avformat_close_input(&demuxer);
  g_unlink(output);
  g_unlink(input);
}

//how long a file's streams run, in seconds, and whether it has sound
static void
probe(const char *path,
      double     *picture,
      double     *sound,
      int        *width,
      int        *height,
      double     *rate)
{
  AVFormatContext *demuxer = NULL;

  *sound = -1;
  g_assert_cmpint(avformat_open_input(&demuxer, path, NULL, NULL), >=, 0);
  avformat_find_stream_info(demuxer, NULL);
  for (unsigned i = 0; i < demuxer->nb_streams; i++) {
    const AVStream *stream = demuxer->streams[i];
    const double length = stream->duration != AV_NOPTS_VALUE
                        ? stream->duration * av_q2d(stream->time_base)
                        : demuxer->duration / (double) AV_TIME_BASE;

    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      *picture = length;
      *width = stream->codecpar->width;
      *height = stream->codecpar->height;
      *rate = av_q2d(stream->avg_frame_rate);
    } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      *sound = length;
    }
  }
  avformat_close_input(&demuxer);
}

static gboolean
export_to(WaytatorVideoSource        *source,
          WaytatorVideoState         *state,
          const WaytatorVideoProfile *profile,
          const char                 *extension,
          char                      **output)
{
  g_autoptr(GError) error = NULL;
  g_autofree char *template = g_strdup_printf("waytator-test-export-out-XXXXXX.%s", extension);
  int fd;

  *output = g_build_filename(g_get_tmp_dir(), template, NULL);
  fd = g_mkstemp(*output);
  close(fd);
  if (!waytator_video_export(source, state, profile, *output, NULL, NULL, NULL, &error)) {
    g_test_message("export failed: %s", error->message);
    return FALSE;
  }
  return TRUE;
}

static void
test_sound(gconstpointer data)
{
  const WaytatorVideoSound sound = GPOINTER_TO_INT(data);
  g_autofree char *input = make_video_with(0, TRUE);
  g_autoptr(GError) error = NULL;
  g_autoptr(WaytatorVideoSource) source = waytator_video_source_open(input, NULL, &error);
  g_autoptr(WaytatorVideoEdit) edit = NULL;
  g_autoptr(WaytatorVideoState) state = NULL;
  g_autoptr(WaytatorVideoProfile) profile = test_profile();
  g_autofree char *output = NULL;
  const WaytatorVideoSegment values = { .mute = TRUE, .speed = 1.5 };
  double picture = 0;
  double heard = 0;
  int width;
  int height;
  double rate;

  g_assert_no_error(error);
  g_assert_true(waytator_video_source_has_audio(source));
  edit = waytator_video_edit_new(waytator_video_source_get_frames(source), WIDTH, HEIGHT, TRUE);
  //half of it muted and faster, so the sound has cuts, silence and a stretch in it
  waytator_video_edit_set(edit, 500000, G_MAXINT64, &values, WAYTATOR_VIDEO_FIELD_MUTE | WAYTATOR_VIDEO_FIELD_SPEED);
  state = waytator_video_edit_get_state(edit);
  profile->sound = sound;
  profile->sound_bitrate = 96;

  g_assert_true(export_to(source, state, profile, "mp4", &output));
  probe(output, &picture, &heard, &width, &height, &rate);
  g_assert_cmpfloat(fabs(picture - waytator_video_state_get_duration(state) / 1e6), <, 0.05);
  g_assert_cmpfloat(heard, >, 0);
  //the sound runs as long as the picture, give or take an encoder frame
  g_assert_cmpfloat(fabs(heard - picture), <, 0.05);
  g_unlink(output);
  g_unlink(input);
}

static void
test_size_limit(void)
{
  g_autofree char *input = make_video_with(0, TRUE);
  g_autoptr(GError) error = NULL;
  g_autoptr(WaytatorVideoSource) source = waytator_video_source_open(input, NULL, &error);
  g_autoptr(WaytatorVideoEdit) edit = NULL;
  g_autoptr(WaytatorVideoState) state = NULL;
  g_autoptr(WaytatorVideoProfile) profile = test_profile();
  g_autofree char *output = NULL;
  GStatBuf info;

  g_assert_no_error(error);
  edit = waytator_video_edit_new(waytator_video_source_get_frames(source), WIDTH, HEIGHT, TRUE);
  state = waytator_video_edit_get_state(edit);
  profile->encoder = WAYTATOR_VIDEO_ENCODER_SOFTWARE;
  profile->size_limit = 0.04;
  profile->sound = WAYTATOR_VIDEO_SOUND_AAC;
  profile->sound_bitrate = 64;

  g_assert_true(export_to(source, state, profile, "mp4", &output));
  g_assert_cmpint(g_stat(output, &info), ==, 0);
  g_test_message("size limited to %d bytes, came to %d", (int) (profile->size_limit * 1e6), (int) info.st_size);
  g_assert_cmpint(info.st_size, <=, (gint64) (profile->size_limit * 1000 * 1000));
  g_unlink(output);

  //far too long for the limit says so
  profile->size_limit = 0.002;
  g_clear_pointer(&output, g_free);
  g_assert_false(export_to(source, state, profile, "mp4", &output));
  g_unlink(output);
  g_unlink(input);
}

static void
test_gif(void)
{
  g_autofree char *input = make_video(0);
  g_autoptr(GError) error = NULL;
  g_autoptr(WaytatorVideoSource) source = waytator_video_source_open(input, NULL, &error);
  g_autoptr(WaytatorVideoEdit) edit = NULL;
  g_autoptr(WaytatorVideoState) state = NULL;
  g_autoptr(WaytatorVideoProfile) profile = test_profile();
  g_autofree char *output = NULL;
  AVFrame *result;
  double picture = 0;
  double heard = 0;
  int width = 0;
  int height = 0;
  double rate = 0;

  g_assert_no_error(error);
  edit = waytator_video_edit_new(waytator_video_source_get_frames(source), WIDTH, HEIGHT, FALSE);
  state = waytator_video_edit_get_state(edit);
  profile->codec = WAYTATOR_VIDEO_CODEC_GIF;
  profile->max_size = 120;
  profile->max_rate = 10;

  g_assert_true(export_to(source, state, profile, "gif", &output));
  probe(output, &picture, &heard, &width, &height, &rate);
  g_assert_cmpint(width, ==, 160);
  g_assert_cmpint(height, ==, 120);
  g_assert_cmpfloat(heard, <, 0);
  result = read_first_frame(output);
  for (int corner = 0; corner < 4; corner++)
    g_assert_cmpint(quarter_at(result, corner), ==, corner);
  av_frame_free(&result);
  g_unlink(output);
  g_unlink(input);
}

int
main(int   argc,
     char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_data_func("/video-export/transforms/software",
                       GINT_TO_POINTER(WAYTATOR_VIDEO_ENCODER_SOFTWARE), test_transforms);
  g_test_add_data_func("/video-export/transforms/automatic",
                       GINT_TO_POINTER(WAYTATOR_VIDEO_ENCODER_AUTO), test_transforms);
  g_test_add_func("/video-export/crop-and-speed", test_crop_and_speed);
  g_test_add_data_func("/video-export/sound/aac", GINT_TO_POINTER(WAYTATOR_VIDEO_SOUND_AAC), test_sound);
  g_test_add_data_func("/video-export/sound/opus", GINT_TO_POINTER(WAYTATOR_VIDEO_SOUND_OPUS), test_sound);
  g_test_add_func("/video-export/size-limit", test_size_limit);
  g_test_add_func("/video-export/gif", test_gif);

  return g_test_run();
}
