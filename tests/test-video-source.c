#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>

#include "waytator-video-source.h"
#include "waytator-video-thumbnails.h"

//the test pictures are a grid of black and white blocks spelling out the frame number. the top left is
//always white and the bottom left always black, so a picture turned the wrong way shows
#define TEST_BLOCK 64
#define TEST_COLUMNS 4
#define TEST_ROWS 3
#define TEST_WIDTH (TEST_BLOCK * TEST_COLUMNS)
#define TEST_HEIGHT (TEST_BLOCK * TEST_ROWS)
#define TEST_GOP 12

static const int test_bits[][2] = {
  { 1, 0 }, { 2, 0 }, { 3, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 }, { 1, 2 }, { 2, 2 }, { 3, 2 },
};

typedef enum {
  TEST_CONSTANT,
  TEST_VARIABLE,
  TEST_ROTATED,
  TEST_AUDIO_ONLY,
  //gops longer than the gpu frames the decoder keeps
  TEST_LONG_GOP,
  N_TEST_VIDEOS,
} TestKind;

typedef struct {
  char *path;
  guint n_frames;
  //from the first frame, in microseconds
  gint64 *pts;
  guint8 *keyframe;
  gboolean h264;
} TestVideo;

static char *test_dir;
static TestVideo *test_videos[N_TEST_VIDEOS];
static GdkDisplay *test_display;
static GskRenderer *test_renderer;
static gboolean test_display_tried;

typedef struct {
  const guint8 *data;
  gsize stride;
  //bytes from one pixel to the next
  int step;
  int width;
  int height;
} TestPicture;

static void
test_draw(AVFrame *frame,
          guint    number)
{
  g_assert_cmpint(av_frame_make_writable(frame), >=, 0);
  for (int y = 0; y < frame->height; y++)
    memset(frame->data[0] + (gsize) y * frame->linesize[0], 16, frame->width);
  for (int y = 0; y < frame->height / 2; y++) {
    memset(frame->data[1] + (gsize) y * frame->linesize[1], 128, frame->width / 2);
    memset(frame->data[2] + (gsize) y * frame->linesize[2], 128, frame->width / 2);
  }

  for (int bit = -1; bit < (int) G_N_ELEMENTS(test_bits); bit++) {
    const int column = bit < 0 ? 0 : test_bits[bit][0];
    const int row = bit < 0 ? 0 : test_bits[bit][1];

    if (bit >= 0 && !(number & (1u << bit)))
      continue;
    for (int y = row * TEST_BLOCK; y < (row + 1) * TEST_BLOCK; y++)
      memset(frame->data[0] + (gsize) y * frame->linesize[0] + column * TEST_BLOCK, 235, TEST_BLOCK);
  }
}

static gboolean
test_block_is_white(const TestPicture *picture,
                    int                column,
                    int                row,
                    int                turns)
{
  //where the middle of the block was in the picture as it was encoded
  const int encoded_width = turns % 2 ? picture->height : picture->width;
  const int encoded_height = turns % 2 ? picture->width : picture->height;
  const int ex = (2 * column + 1) * encoded_width / (2 * TEST_COLUMNS);
  const int ey = (2 * row + 1) * encoded_height / (2 * TEST_ROWS);
  const int radius = MAX(1, MIN(picture->width, picture->height) / (TEST_ROWS * 8));
  int x;
  int y;
  guint sum = 0;
  guint n = 0;

  switch (turns) {
  case 1:
    x = encoded_height - 1 - ey;
    y = ex;
    break;
  case 2:
    x = encoded_width - 1 - ex;
    y = encoded_height - 1 - ey;
    break;
  case 3:
    x = ey;
    y = encoded_width - 1 - ex;
    break;
  default:
    x = ex;
    y = ey;
    break;
  }

  for (int dy = -radius; dy <= radius; dy++) {
    for (int dx = -radius; dx <= radius; dx++) {
      sum += picture->data[(gsize) (y + dy) * picture->stride + (gsize) (x + dx) * picture->step];
      n++;
    }
  }

  return sum / n > 128;
}

//the frame number in the picture, -1 when the corners say it isn't one
static int
test_read_number(const TestPicture *picture,
                 int                turns)
{
  int number = 0;

  if (!test_block_is_white(picture, 0, 0, turns) || test_block_is_white(picture, 0, 2, turns))
    return -1;

  for (guint bit = 0; bit < G_N_ELEMENTS(test_bits); bit++) {
    if (test_block_is_white(picture, test_bits[bit][0], test_bits[bit][1], turns))
      number |= 1 << bit;
  }

  return number;
}

static int
test_av_frame_number(const AVFrame *frame)
{
  AVFrame *memory = NULL;
  TestPicture picture;
  int number;

  if (frame->hw_frames_ctx != NULL) {
    memory = av_frame_alloc();
    g_assert_cmpint(av_hwframe_transfer_data(memory, frame, 0), >=, 0);
    frame = memory;
  }

  g_assert_true(frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_NV12);
  picture = (TestPicture) {
    .data = frame->data[0],
    .stride = frame->linesize[0],
    .step = 1,
    .width = frame->width,
    .height = frame->height,
  };
  number = test_read_number(&picture, 0);
  av_frame_free(&memory);
  return number;
}

static int
test_frame_number(WaytatorVideoFrame *frame)
{
  return test_av_frame_number(waytator_video_frame_get_av_frame(frame));
}

//green, which is bright for white and dark for black whatever the colours do
static int
test_texture_number(GdkTexture *texture,
                    int         turns)
{
  const int width = gdk_texture_get_width(texture);
  const int height = gdk_texture_get_height(texture);
  g_autofree guint8 *pixels = g_malloc((gsize) width * 4 * height);
  TestPicture picture = {
    .data = pixels + 1,
    .stride = (gsize) width * 4,
    .step = 4,
    .width = width,
    .height = height,
  };

  gdk_texture_download(texture, pixels, (gsize) width * 4);
  return test_read_number(&picture, turns);
}

static void
test_write_packets(AVCodecContext  *encoder,
                   AVFormatContext *output,
                   AVStream        *stream,
                   TestVideo       *video,
                   const gint64    *input_pts)
{
  AVPacket *packet = av_packet_alloc();

  while (avcodec_receive_packet(encoder, packet) == 0) {
    if (packet->flags & AV_PKT_FLAG_KEY) {
      for (guint i = 0; i < video->n_frames; i++) {
        if (input_pts[i] == packet->pts)
          video->keyframe[i] = TRUE;
      }
    }
    av_packet_rescale_ts(packet, encoder->time_base, stream->time_base);
    packet->stream_index = stream->index;
    g_assert_cmpint(av_interleaved_write_frame(output, packet), >=, 0);
  }
  av_packet_free(&packet);
}

static void
test_add_display_matrix(AVStream *stream)
{
  int32_t *matrix;

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 30, 100)
  AVPacketSideData *side_data = av_packet_side_data_new(&stream->codecpar->coded_side_data,
                                                        &stream->codecpar->nb_coded_side_data,
                                                        AV_PKT_DATA_DISPLAYMATRIX,
                                                        9 * sizeof(int32_t),
                                                        0);

  g_assert_nonnull(side_data);
  matrix = (int32_t *) side_data->data;
#else
  matrix = (int32_t *) av_stream_new_side_data(stream, AV_PKT_DATA_DISPLAYMATRIX, 9 * sizeof(int32_t));
  g_assert_nonnull(matrix);
#endif
  //a quarter turn clockwise, like a phone held upright
  av_display_rotation_set(matrix, 90);
}

static AVStream *
test_add_audio(AVFormatContext *output)
{
  AVStream *stream = avformat_new_stream(output, NULL);

  stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
  stream->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
  stream->codecpar->sample_rate = 48000;
  stream->codecpar->format = AV_SAMPLE_FMT_S16;
  stream->codecpar->bits_per_coded_sample = 16;
  stream->codecpar->block_align = 4;
  av_channel_layout_default(&stream->codecpar->ch_layout, 2);
  stream->time_base = (AVRational) { 1, 48000 };
  return stream;
}

static void
test_write_audio(AVFormatContext *output,
                 AVStream        *stream,
                 gint64           duration)
{
  const int samples = 1024;
  AVPacket *packet = av_packet_alloc();

  for (gint64 start = 0; start < duration * 48000 / G_USEC_PER_SEC; start += samples) {
    g_assert_cmpint(av_new_packet(packet, samples * 4), >=, 0);
    memset(packet->data, 0, samples * 4);
    packet->pts = packet->dts = av_rescale_q(start, (AVRational) { 1, 48000 }, stream->time_base);
    packet->duration = av_rescale_q(samples, (AVRational) { 1, 48000 }, stream->time_base);
    packet->stream_index = stream->index;
    packet->flags |= AV_PKT_FLAG_KEY;
    g_assert_cmpint(av_interleaved_write_frame(output, packet), >=, 0);
  }
  av_packet_free(&packet);
}

static TestVideo *
test_video_write(TestKind kind)
{
  //a recording that only has frames for the moments the screen changed
  static const gint64 variable_ms[] = { 16, 17, 50, 500, 33, 16, 250, 17 };
  const gboolean variable = kind == TEST_VARIABLE;
  const gboolean has_video = kind != TEST_AUDIO_ONLY;
  const char *format_name = kind == TEST_VARIABLE || kind == TEST_AUDIO_ONLY ? "matroska" : "mp4";
  const char *names[] = { "constant.mp4", "variable.mkv", "rotated.mp4", "audio.mkv", "long-gop.mp4" };
  const int gop = kind == TEST_LONG_GOP ? 150 : TEST_GOP;
  TestVideo *video = g_new0(TestVideo, 1);
  g_autofree gint64 *input_pts = NULL;
  AVFormatContext *output = NULL;
  AVCodecContext *encoder = NULL;
  AVStream *stream = NULL;
  AVStream *audio = NULL;
  AVFrame *frame = NULL;
  gint64 length = 2 * G_USEC_PER_SEC;

  video->path = g_build_filename(test_dir, names[kind], NULL);
  video->n_frames = kind == TEST_CONSTANT || kind == TEST_LONG_GOP ? 300 : kind == TEST_VARIABLE ? 60 : kind == TEST_ROTATED ? 24 : 0;
  video->pts = g_new0(gint64, MAX(video->n_frames, 1));
  video->keyframe = g_new0(guint8, MAX(video->n_frames, 1));
  input_pts = g_new0(gint64, MAX(video->n_frames, 1));

  g_assert_cmpint(avformat_alloc_output_context2(&output, NULL, format_name, video->path), >=, 0);

  if (has_video) {
    const AVCodec *codec = avcodec_find_encoder_by_name("libx264");

    video->h264 = codec != NULL;
    if (codec == NULL)
      codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    g_assert_nonnull(codec);

    stream = avformat_new_stream(output, NULL);
    encoder = avcodec_alloc_context3(codec);
    encoder->width = TEST_WIDTH;
    encoder->height = TEST_HEIGHT;
    encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder->time_base = variable ? (AVRational) { 1, 1000 } : (AVRational) { 1, 30 };
    encoder->framerate = variable ? (AVRational) { 60, 1 } : (AVRational) { 30, 1 };
    encoder->gop_size = gop;
    encoder->max_b_frames = 2;
    if (output->oformat->flags & AVFMT_GLOBALHEADER)
      encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (video->h264) {
      av_opt_set(encoder->priv_data, "preset", "veryfast", 0);
      av_opt_set(encoder->priv_data, "crf", "18", 0);
      g_autofree char *parameters = g_strdup_printf("keyint=%d:min-keyint=%d:scenecut=0:bframes=2", gop, gop);

      av_opt_set(encoder->priv_data, "x264-params", parameters, 0);
    } else {
      encoder->bit_rate = 2000000;
      av_opt_set(encoder->priv_data, "sc_threshold", "1000000000", 0);
    }
    g_assert_cmpint(avcodec_open2(encoder, codec, NULL), >=, 0);
    g_assert_cmpint(avcodec_parameters_from_context(stream->codecpar, encoder), >=, 0);
    stream->time_base = encoder->time_base;
    if (kind == TEST_ROTATED)
      test_add_display_matrix(stream);

    for (guint i = 0; i < video->n_frames; i++) {
      if (!variable)
        input_pts[i] = i;
      else if (i == 0)
        input_pts[i] = 100;
      else
        input_pts[i] = input_pts[i - 1] + variable_ms[(i - 1) % G_N_ELEMENTS(variable_ms)];
      video->pts[i] = av_rescale_q(input_pts[i] - input_pts[0], encoder->time_base, (AVRational) { 1, G_USEC_PER_SEC });
    }
    length = av_rescale_q(input_pts[video->n_frames - 1] + 1, encoder->time_base, (AVRational) { 1, G_USEC_PER_SEC });
  }
  if (kind == TEST_VARIABLE || kind == TEST_AUDIO_ONLY)
    audio = test_add_audio(output);

  g_assert_cmpint(avio_open(&output->pb, video->path, AVIO_FLAG_WRITE), >=, 0);
  g_assert_cmpint(avformat_write_header(output, NULL), >=, 0);

  if (audio != NULL)
    test_write_audio(output, audio, length);

  if (has_video) {
    frame = av_frame_alloc();
    frame->format = encoder->pix_fmt;
    frame->width = encoder->width;
    frame->height = encoder->height;
    g_assert_cmpint(av_frame_get_buffer(frame, 0), >=, 0);
    for (guint i = 0; i < video->n_frames; i++) {
      test_draw(frame, i);
      frame->pts = input_pts[i];
      g_assert_cmpint(avcodec_send_frame(encoder, frame), >=, 0);
      test_write_packets(encoder, output, stream, video, input_pts);
    }
    g_assert_cmpint(avcodec_send_frame(encoder, NULL), >=, 0);
    test_write_packets(encoder, output, stream, video, input_pts);
    av_frame_free(&frame);
    avcodec_free_context(&encoder);
  }

  g_assert_cmpint(av_write_trailer(output), >=, 0);
  avio_closep(&output->pb);
  avformat_free_context(output);
  return video;
}

static TestVideo *
test_video_get(TestKind kind)
{
  if (test_videos[kind] == NULL)
    test_videos[kind] = test_video_write(kind);

  return test_videos[kind];
}

static WaytatorVideoSource *
test_open(TestKind kind)
{
  g_autoptr(GError) error = NULL;
  WaytatorVideoSource *source = waytator_video_source_open(test_video_get(kind)->path, NULL, &error);

  g_assert_no_error(error);
  g_assert_nonnull(source);
  return source;
}

static void
test_check_frame(WaytatorVideoDecoder *decoder,
                 guint                 index)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(WaytatorVideoFrame) frame = waytator_video_decoder_get(decoder, index, NULL, &error);

  g_assert_no_error(error);
  g_assert_nonnull(frame);
  g_assert_cmpuint(waytator_video_frame_get_index(frame), ==, index);
  g_assert_cmpint(test_frame_number(frame), ==, index);
}

static void
test_shuffle(guint *order,
             guint  n)
{
  for (guint i = 0; i < n; i++)
    order[i] = i;
  for (guint i = n - 1; i > 0; i--) {
    const guint j = g_test_rand_int_range(0, i + 1);
    const guint swap = order[i];

    order[i] = order[j];
    order[j] = swap;
  }
}

static void
test_index(void)
{
  const TestVideo *video = test_video_get(TEST_CONSTANT);
  g_autoptr(WaytatorVideoSource) source = test_open(TEST_CONSTANT);
  const WaytatorVideoFrames *frames = waytator_video_source_get_frames(source);

  g_assert_cmpuint(frames->n_frames, ==, video->n_frames);
  for (guint i = 0; i < frames->n_frames; i++) {
    g_assert_cmpint(ABS(frames->pts[i] - video->pts[i]), <=, 1);
    g_assert_cmpint(frames->keyframe[i] != 0, ==, video->keyframe[i] != 0);
  }
  //one keyframe a gop, wherever the encoder put it
  g_assert_cmpuint(waytator_video_frames_keyframe_before(frames, video->n_frames - 1), >=, video->n_frames - TEST_GOP);
  g_assert_cmpint(ABS(frames->duration - (gint64) video->n_frames * G_USEC_PER_SEC / 30), <=, 2);
  g_assert_cmpint(frames->rate_num, ==, 30);
  g_assert_cmpint(frames->rate_den, ==, 1);
  g_assert_cmpint(waytator_video_source_get_width(source), ==, TEST_WIDTH);
  g_assert_cmpint(waytator_video_source_get_height(source), ==, TEST_HEIGHT);
  g_assert_cmpint(waytator_video_source_get_orientation(source), ==, 0);
  g_assert_false(waytator_video_source_has_audio(source));
  g_assert_cmpstr(waytator_video_source_get_codec_name(source), ==, video->h264 ? "h264" : "mpeg4");
}

static void
test_variable_rate(void)
{
  const TestVideo *video = test_video_get(TEST_VARIABLE);
  g_autoptr(WaytatorVideoSource) source = test_open(TEST_VARIABLE);
  g_autoptr(WaytatorVideoDecoder) decoder = NULL;
  const WaytatorVideoFrames *frames = waytator_video_source_get_frames(source);
  g_autoptr(GError) error = NULL;
  g_autofree guint *order = g_new(guint, video->n_frames);

  g_assert_cmpuint(frames->n_frames, ==, video->n_frames);
  for (guint i = 0; i < frames->n_frames; i++) {
    g_assert_cmpint(frames->pts[i], ==, video->pts[i]);
    g_assert_cmpint(frames->keyframe[i] != 0, ==, video->keyframe[i] != 0);
    g_assert_cmpuint(waytator_video_frames_find(frames, frames->pts[i] + 1), ==, i);
  }
  g_assert_cmpint(frames->duration, >, frames->pts[frames->n_frames - 1]);
  g_assert_cmpint(frames->rate_num, >, 0);
  g_assert_cmpint(frames->rate_den, >, 0);
  g_assert_cmpfloat((double) frames->rate_num / frames->rate_den, <=, 240.0);

  //the first frame is 100 ms into the file and the sound starts at its very beginning
  g_assert_cmpint(waytator_video_source_get_start_time(source), ==, 100000);
  g_assert_true(waytator_video_source_has_audio(source));
  g_assert_cmpint(waytator_video_source_get_audio_rate(source), ==, 48000);
  g_assert_cmpint(waytator_video_source_get_audio_channels(source), ==, 2);
  g_assert_cmpint(waytator_video_source_get_audio_offset(source), ==, -100000);

  decoder = waytator_video_decoder_new(source, 0, &error);
  g_assert_no_error(error);
  test_shuffle(order, video->n_frames);
  for (guint i = 0; i < video->n_frames; i++)
    test_check_frame(decoder, order[i]);
}

static void
test_rotation(void)
{
  g_autoptr(WaytatorVideoSource) source = test_open(TEST_ROTATED);
  g_autoptr(WaytatorVideoDecoder) decoder = NULL;
  g_autoptr(GError) error = NULL;

  g_assert_cmpint(waytator_video_source_get_orientation(source), ==, 1);
  g_assert_cmpint(waytator_video_source_get_width(source), ==, TEST_HEIGHT);
  g_assert_cmpint(waytator_video_source_get_height(source), ==, TEST_WIDTH);

  //frames come out the way they were encoded
  decoder = waytator_video_decoder_new(source, 0, &error);
  g_assert_no_error(error);
  test_check_frame(decoder, 13);
  test_check_frame(decoder, 2);
}

static void
test_not_video(void)
{
  g_autofree char *text = g_build_filename(test_dir, "notes.wtx", NULL);
  g_autoptr(GError) error = NULL;
  WaytatorVideoSource *source;

  g_assert_true(g_file_set_contents(text, "a shopping list, not a video\nmilk\neggs\n", -1, NULL));
  source = waytator_video_source_open(text, NULL, &error);
  g_assert_null(source);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_test_message("%s", error->message);
  g_clear_error(&error);
  g_unlink(text);

  source = waytator_video_source_open(test_video_get(TEST_AUDIO_ONLY)->path, NULL, &error);
  g_assert_null(source);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&error);

  source = waytator_video_source_open("/nonexistent/video.mp4", NULL, &error);
  g_assert_null(source);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
}

static void
test_opened(GObject      *object,
            GAsyncResult *result,
            gpointer      data)
{
  GAsyncResult **out = data;

  (void) object;

  *out = g_object_ref(result);
}

static void
test_open_async(void)
{
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  g_autoptr(WaytatorVideoSource) source = NULL;
  g_autoptr(GError) error = NULL;
  GAsyncResult *result = NULL;

  waytator_video_source_open_async(test_video_get(TEST_CONSTANT)->path, NULL, test_opened, &result);
  while (result == NULL)
    g_main_context_iteration(NULL, TRUE);
  source = waytator_video_source_open_finish(result, &error);
  g_clear_object(&result);
  g_assert_no_error(error);
  g_assert_cmpuint(waytator_video_source_get_frames(source)->n_frames, ==, test_video_get(TEST_CONSTANT)->n_frames);

  g_cancellable_cancel(cancellable);
  waytator_video_source_open_async(test_video_get(TEST_CONSTANT)->path, cancellable, test_opened, &result);
  while (result == NULL)
    g_main_context_iteration(NULL, TRUE);
  g_assert_null(waytator_video_source_open_finish(result, &error));
  g_clear_object(&result);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

static void
test_random_access(void)
{
  const TestVideo *video = test_video_get(TEST_CONSTANT);
  g_autoptr(WaytatorVideoSource) source = test_open(TEST_CONSTANT);
  g_autoptr(WaytatorVideoDecoder) decoder = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree guint *order = g_new(guint, video->n_frames);

  decoder = waytator_video_decoder_new(source, 0, &error);
  g_assert_no_error(error);
  g_assert_false(waytator_video_decoder_is_hardware(decoder));

  test_shuffle(order, video->n_frames);
  for (guint i = 0; i < video->n_frames; i++)
    test_check_frame(decoder, order[i]);
  g_clear_pointer(&decoder, waytator_video_decoder_free);

  //a fresh decoder each way, so nothing comes from what an earlier pass kept
  decoder = waytator_video_decoder_new(source, 0, &error);
  g_assert_no_error(error);
  for (guint i = 0; i < video->n_frames; i++)
    test_check_frame(decoder, i);
  g_clear_pointer(&decoder, waytator_video_decoder_free);

  decoder = waytator_video_decoder_new(source, 0, &error);
  g_assert_no_error(error);
  for (guint i = video->n_frames; i > 0; i--)
    test_check_frame(decoder, i - 1);
}

static void
test_keyframe(void)
{
  const TestVideo *video = test_video_get(TEST_CONSTANT);
  g_autoptr(WaytatorVideoSource) source = test_open(TEST_CONSTANT);
  const WaytatorVideoFrames *frames = waytator_video_source_get_frames(source);
  g_autoptr(WaytatorVideoDecoder) decoder = NULL;
  g_autoptr(GError) error = NULL;
  const guint asked[] = { 100, 5, 0, 37, 119, 12, 64 };

  decoder = waytator_video_decoder_new(source, 0, &error);
  g_assert_no_error(error);
  for (guint i = 0; i < G_N_ELEMENTS(asked); i++) {
    g_autoptr(WaytatorVideoFrame) frame = waytator_video_decoder_get_keyframe(decoder, asked[i], NULL, &error);
    const guint keyframe = waytator_video_frames_keyframe_before(frames, asked[i]);

    g_assert_no_error(error);
    g_assert_cmpuint(waytator_video_frame_get_index(frame), ==, keyframe);
    g_assert_true(video->keyframe[keyframe]);
    g_assert_cmpint(test_frame_number(frame), ==, keyframe);
  }

  //an exact frame after a keyframe still decodes right
  test_check_frame(decoder, 70);
  test_check_frame(decoder, 71);
}

static void
test_cache(void)
{
  const TestVideo *video = test_video_get(TEST_CONSTANT);
  g_autoptr(WaytatorVideoSource) source = test_open(TEST_CONSTANT);
  const WaytatorVideoFrames *frames = waytator_video_source_get_frames(source);
  g_autoptr(WaytatorVideoDecoder) decoder = NULL;
  g_autoptr(GError) error = NULL;
  guint last = video->n_frames / 2;
  guint keyframe;
  guint previous;

  //the last frame of a gop in the middle
  while (last + 1 < video->n_frames && !frames->keyframe[last + 1])
    last++;
  keyframe = waytator_video_frames_keyframe_before(frames, last);
  g_assert_cmpuint(keyframe, >, 0);

  decoder = waytator_video_decoder_new(source, 0, &error);
  g_assert_no_error(error);
  g_assert_null(waytator_video_decoder_peek(decoder, last));
  test_check_frame(decoder, last);

  //everything on the way there is kept, so stepping back decodes nothing
  for (guint i = last + 1; i > keyframe; i--) {
    g_autoptr(WaytatorVideoFrame) peeked = waytator_video_decoder_peek(decoder, i - 1);
    g_autoptr(WaytatorVideoFrame) frame = NULL;

    g_assert_nonnull(peeked);
    frame = waytator_video_decoder_get(decoder, i - 1, NULL, &error);
    g_assert_no_error(error);
    g_assert_true(frame == peeked);
    g_assert_cmpint(test_frame_number(frame), ==, i - 1);
  }
  g_assert_null(waytator_video_decoder_peek(decoder, keyframe - 1));

  //one step back past the keyframe decodes the whole gop before it
  test_check_frame(decoder, keyframe - 1);
  previous = waytator_video_frames_keyframe_before(frames, keyframe - 1);
  for (guint i = previous; i < keyframe; i++) {
    g_autoptr(WaytatorVideoFrame) peeked = waytator_video_decoder_peek(decoder, i);

    g_assert_nonnull(peeked);
    g_assert_cmpint(test_frame_number(peeked), ==, i);
  }
}

static WaytatorVideoDecoder *
test_hardware_decoder(WaytatorVideoSource      *source,
                      WaytatorVideoDecoderFlags flags)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(WaytatorVideoDecoder) decoder = NULL;
  g_autoptr(WaytatorVideoFrame) frame = NULL;

  decoder = waytator_video_decoder_new(source, WAYTATOR_VIDEO_DECODER_HARDWARE | flags, &error);
  g_assert_no_error(error);
  //the first frame tells whether the gpu really takes it
  frame = waytator_video_decoder_get(decoder, 0, NULL, &error);
  g_assert_no_error(error);
  g_assert_cmpint(test_frame_number(frame), ==, 0);
  if (!waytator_video_decoder_is_hardware(decoder))
    return NULL;

  return g_steal_pointer(&decoder);
}

static void
test_hardware(void)
{
  const TestVideo *video = test_video_get(TEST_CONSTANT);
  g_autoptr(WaytatorVideoSource) source = test_open(TEST_CONSTANT);
  g_autoptr(WaytatorVideoDecoder) decoder = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree guint *order = g_new(guint, video->n_frames);
  g_autoptr(WaytatorVideoSource) long_gop = NULL;
  const WaytatorVideoFrames *long_frames;
  guint n_hardware = 0;
  guint n_memory = 0;

  decoder = test_hardware_decoder(source, 0);
  if (decoder == NULL) {
    g_test_skip("the GPU can't decode the test video here");
    return;
  }

  test_shuffle(order, video->n_frames);
  for (guint i = 0; i < 40; i++) {
    g_autoptr(WaytatorVideoFrame) frame = waytator_video_decoder_get(decoder, order[i], NULL, &error);

    g_assert_no_error(error);
    g_assert_cmpuint(waytator_video_frame_get_index(frame), ==, order[i]);
    g_assert_cmpint(test_frame_number(frame), ==, order[i]);
  }

  //a gop longer than the gpu frames kept. playing on lets the oldest go, stepping back into the gop keeps
  //what it fills in behind itself, in memory past the cap
  g_clear_pointer(&decoder, waytator_video_decoder_free);
  long_gop = test_open(TEST_LONG_GOP);
  long_frames = waytator_video_source_get_frames(long_gop);
  g_assert_true(long_frames->keyframe[150]);
  decoder = test_hardware_decoder(long_gop, 0);
  g_assert_nonnull(decoder);
  for (guint i = 0; i < long_frames->n_frames; i++) {
    g_autoptr(WaytatorVideoFrame) frame = waytator_video_decoder_get(decoder, i, NULL, &error);

    g_assert_no_error(error);
    g_assert_nonnull(waytator_video_frame_get_av_frame(frame)->hw_frames_ctx);
    g_assert_cmpint(test_frame_number(frame), ==, i);
  }
  g_assert_null(waytator_video_decoder_peek(decoder, 1));
  for (guint i = 151; i > 0; i--)
    test_check_frame(decoder, i - 1);
  for (guint i = 0; i < 150; i++) {
    g_autoptr(WaytatorVideoFrame) frame = waytator_video_decoder_peek(decoder, i);

    if (frame == NULL)
      continue;
    g_assert_cmpint(test_frame_number(frame), ==, i);
    if (waytator_video_frame_get_av_frame(frame)->hw_frames_ctx != NULL)
      n_hardware++;
    else
      n_memory++;
  }
  g_assert_cmpuint(n_hardware, <=, 128);
  g_assert_cmpuint(n_memory, >, 0);

  //frames off the gpu in memory
  g_clear_pointer(&decoder, waytator_video_decoder_free);
  decoder = test_hardware_decoder(source, WAYTATOR_VIDEO_DECODER_MEMORY);
  for (guint i = 0; i < 20; i++) {
    g_autoptr(WaytatorVideoFrame) frame = waytator_video_decoder_get(decoder, order[i], NULL, &error);
    g_autoptr(WaytatorVideoFrame) keyframe = NULL;

    g_assert_no_error(error);
    g_assert_null(waytator_video_frame_get_av_frame(frame)->hw_frames_ctx);
    g_assert_cmpint(test_frame_number(frame), ==, order[i]);

    keyframe = waytator_video_decoder_get_keyframe(decoder, order[i], NULL, &error);
    g_assert_no_error(error);
    g_assert_null(waytator_video_frame_get_av_frame(keyframe)->hw_frames_ctx);
    g_assert_cmpint(test_frame_number(keyframe), ==, waytator_video_frame_get_index(keyframe));
  }
}

static void
test_cancel(void)
{
  g_autoptr(WaytatorVideoSource) source = test_open(TEST_CONSTANT);
  g_autoptr(WaytatorVideoDecoder) decoder = NULL;
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  g_autoptr(GError) error = NULL;

  decoder = waytator_video_decoder_new(source, 0, &error);
  g_assert_no_error(error);
  g_cancellable_cancel(cancellable);
  g_assert_null(waytator_video_decoder_get(decoder, 50, cancellable, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&error);

  //the decoder carries on fine afterwards
  test_check_frame(decoder, 50);
}

//offscreen only: a renderer on the display, never a window or a surface
static GskRenderer *
test_get_renderer(void)
{
  g_autoptr(GError) error = NULL;

  if (test_display_tried)
    return test_renderer;

  test_display_tried = TRUE;
  if (g_getenv("WAYLAND_DISPLAY") == NULL)
    return NULL;

  if (!gtk_init_check())
    return NULL;
  test_display = gdk_display_get_default();

  test_renderer = gsk_gl_renderer_new();
  if (!gsk_renderer_realize_for_display(test_renderer, test_display, &error)) {
    g_test_message("no renderer: %s", error->message);
    g_clear_object(&test_renderer);
  }

  return test_renderer;
}

static int
test_rendered_number(GdkTexture *texture)
{
  const int width = gdk_texture_get_width(texture);
  const int height = gdk_texture_get_height(texture);
  const graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width, height);
  GskRenderNode *node = gsk_texture_node_new(texture, &bounds);
  g_autoptr(GdkTexture) rendered = gsk_renderer_render_texture(test_renderer, node, &bounds);
  const int number = test_texture_number(rendered, 0);

  gsk_render_node_unref(node);
  return number;
}

static void
test_texture(void)
{
  g_autoptr(WaytatorVideoSource) source = NULL;
  g_autoptr(WaytatorVideoDecoder) decoder = NULL;
  g_autoptr(GError) error = NULL;

  if (test_get_renderer() == NULL) {
    g_test_skip("no display to render on");
    return;
  }

  source = test_open(TEST_CONSTANT);
  decoder = waytator_video_decoder_new(source, 0, &error);
  g_assert_no_error(error);
  for (guint i = 0; i < 3; i++) {
    g_autoptr(WaytatorVideoFrame) frame = waytator_video_decoder_get(decoder, 41 + i * 23, NULL, &error);
    GdkTexture *texture;

    g_assert_no_error(error);
    texture = waytator_video_frame_get_texture(frame);
    g_assert_nonnull(texture);
    g_assert_true(waytator_video_frame_get_texture(frame) == texture);
    g_assert_cmpint(gdk_texture_get_width(texture), ==, TEST_WIDTH);
    g_assert_cmpint(gdk_texture_get_height(texture), ==, TEST_HEIGHT);
    g_assert_cmpint(test_rendered_number(texture), ==, 41 + i * 23);
  }
  g_clear_pointer(&decoder, waytator_video_decoder_free);

  decoder = test_hardware_decoder(source, 0);
  if (decoder == NULL) {
    g_test_skip("the GPU can't decode the test video here");
    return;
  }
  for (guint i = 0; i < 3; i++) {
    g_autoptr(WaytatorVideoFrame) frame = waytator_video_decoder_get(decoder, 7 + i * 31, NULL, &error);
    GdkTexture *texture;

    g_assert_no_error(error);
    texture = waytator_video_frame_get_texture(frame);
    g_assert_nonnull(texture);
    if (!GDK_IS_DMABUF_TEXTURE(texture))
      g_test_message("the gpu frame went through memory");
    g_assert_cmpint(test_rendered_number(texture), ==, 7 + i * 31);
  }
}

static gboolean
test_set_flag(gpointer data)
{
  *(gboolean *) data = TRUE;
  return G_SOURCE_REMOVE;
}

static void
test_count(gpointer data)
{
  (*(guint *) data)++;
}

static gboolean
test_has_thumbnail(WaytatorVideoThumbnails *thumbnails,
                   guint                    frame)
{
  guint shown = G_MAXUINT;
  g_autoptr(GdkTexture) texture = waytator_video_thumbnails_lookup(thumbnails, frame, &shown);

  return texture != NULL && shown == frame;
}

static void
test_wait_for_thumbnails(WaytatorVideoThumbnails *thumbnails,
                         guint                    first,
                         guint                    last,
                         guint                    step)
{
  gboolean timed_out = FALSE;
  const guint timeout = g_timeout_add_seconds(20, test_set_flag, &timed_out);

  for (;;) {
    gboolean done = TRUE;

    for (guint i = first; i <= last && done; i += step)
      done = test_has_thumbnail(thumbnails, i);
    if (done)
      break;
    g_assert_false(timed_out);
    g_main_context_iteration(NULL, TRUE);
  }
  g_source_remove(timeout);
}

static void
test_thumbnails(void)
{
  g_autoptr(WaytatorVideoSource) source = test_open(TEST_ROTATED);
  const WaytatorVideoFrames *frames = waytator_video_source_get_frames(source);
  g_autoptr(WaytatorVideoThumbnails) thumbnails = waytator_video_thumbnails_new(source, 32);
  guint updates = 0;

  g_signal_connect_swapped(thumbnails, "updated", G_CALLBACK(test_count), &updates);

  //keyframes come on their own
  test_wait_for_thumbnails(thumbnails, 0, 0, 1);
  for (guint i = 0; i < frames->n_frames; i++) {
    if (frames->keyframe[i])
      test_wait_for_thumbnails(thumbnails, i, i, 1);
  }
  g_assert_cmpuint(updates, >, 0);

  for (guint i = 0; i < frames->n_frames; i++) {
    guint shown = G_MAXUINT;
    g_autoptr(GdkTexture) texture = waytator_video_thumbnails_lookup(thumbnails, i, &shown);

    g_assert_nonnull(texture);
    g_assert_cmpuint(shown, <=, i);
    if (frames->keyframe[i])
      g_assert_cmpuint(shown, ==, i);
    //upright, the source is turned a quarter
    g_assert_cmpint(gdk_texture_get_width(texture), ==, 24);
    g_assert_cmpint(gdk_texture_get_height(texture), ==, 32);
    g_assert_cmpint(test_texture_number(texture, 1), ==, shown);
  }

  waytator_video_thumbnails_request(thumbnails, 1, frames->n_frames - 1, 2);
  test_wait_for_thumbnails(thumbnails, 1, frames->n_frames - 1, 2);
  for (guint i = 1; i < frames->n_frames; i += 2) {
    guint shown = G_MAXUINT;
    g_autoptr(GdkTexture) texture = waytator_video_thumbnails_lookup(thumbnails, i, &shown);

    g_assert_cmpuint(shown, ==, i);
    g_assert_cmpint(test_texture_number(texture, 1), ==, i);
  }
}

static void
test_thumbnails_dispose(void)
{
  g_autoptr(WaytatorVideoSource) source = test_open(TEST_CONSTANT);

  //going away in the middle of the work
  for (guint i = 0; i < 3; i++) {
    WaytatorVideoThumbnails *thumbnails = waytator_video_thumbnails_new(source, 48);

    waytator_video_thumbnails_request(thumbnails, 0, 119, 1);
    if (i > 0)
      g_usleep(i * 20 * G_TIME_SPAN_MILLISECOND);
    waytator_video_thumbnails_request(thumbnails, 50, 60, 1);
    g_object_unref(thumbnails);
  }
  while (g_main_context_iteration(NULL, FALSE));
}

static void
test_cleanup(void)
{
  for (guint i = 0; i < N_TEST_VIDEOS; i++) {
    TestVideo *video = test_videos[i];

    if (video == NULL)
      continue;
    g_unlink(video->path);
    g_free(video->path);
    g_free(video->pts);
    g_free(video->keyframe);
    g_free(video);
  }
  g_rmdir(test_dir);
  g_free(test_dir);
}

//mesa's egl keeps a little it never lets go of, read by the leak checker when there is one
const char *__lsan_default_suppressions(void);

const char *
__lsan_default_suppressions(void)
{
  return "leak:libEGL_mesa.so\n";
}

int
main(int   argc,
     char *argv[])
{
  g_autoptr(GError) error = NULL;
  int result;

  g_test_init(&argc, &argv, NULL);
  av_log_set_level(AV_LOG_ERROR);
  //the display is only ever used offscreen, and only on wayland, without asking portals or the
  //accessibility bus for anything
  gdk_set_allowed_backends("wayland");
  g_setenv("GDK_DEBUG", "no-portals,default-settings", TRUE);
  g_setenv("GTK_A11Y", "none", TRUE);
  test_dir = g_dir_make_tmp("waytator-video-source-XXXXXX", &error);
  g_assert_no_error(error);

  g_test_add_func("/video-source/index", test_index);
  g_test_add_func("/video-source/variable-rate", test_variable_rate);
  g_test_add_func("/video-source/rotation", test_rotation);
  g_test_add_func("/video-source/not-video", test_not_video);
  g_test_add_func("/video-source/open-async", test_open_async);
  g_test_add_func("/video-source/random-access", test_random_access);
  g_test_add_func("/video-source/keyframe", test_keyframe);
  g_test_add_func("/video-source/cache", test_cache);
  g_test_add_func("/video-source/cancel", test_cancel);
  g_test_add_func("/video-source/hardware", test_hardware);
  g_test_add_func("/video-source/texture", test_texture);
  g_test_add_func("/video-thumbnails/thumbnails", test_thumbnails);
  g_test_add_func("/video-thumbnails/dispose", test_thumbnails_dispose);

  result = g_test_run();
  test_cleanup();
  return result;
}
