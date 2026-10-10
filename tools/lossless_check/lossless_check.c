/**
 * @file tools/lossless_check/lossless_check.c
 * @brief Check lossless refinement in a stream recorded with `SUNSHINE_DEBUG_VIDEO_DUMP`.
 *
 * For H.264 it reports which macroblock rows are coded with QP 0, which is lossless with
 * `qpprime_y_zero_transform_bypass_flag`. FFmpeg doesn't export HEVC block QPs, so for HEVC it only reports
 * `transquant_bypass_enabled_flag` and frame sizes.
 *
 * Not built by CMake:
 * @code{.sh}
 * cc -O2 -o lossless_check lossless_check.c $(pkg-config --cflags --libs libavformat libavcodec libavutil)
 * lossless_check dump.h264
 * @endcode
 */
// standard includes
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// lib includes
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/video_enc_params.h>

static int hevc = 0;  ///< Whether the stream is HEVC, otherwise H.264.
static int bypass_flag = -1;  ///< Transform bypass flag of the stream, -1 until its parameter set was traced.
static int parameter_set_seen = 0;  ///< Whether the parameter set that codes the bypass flag was traced.
static int deblocking_idc = 0;  ///< H.264 `disable_deblocking_filter_idc` of the traced slices.
static int tracing = 0;  ///< Whether log lines come from the header tracer.

/**
 * @brief Parse the value at the end of a header trace line.
 *
 * @param line Trace line ending in `= value`.
 * @param value Variable to store the value in.
 */
static void parse_trace_value(const char *line, int *value) {
  const char *equals = strrchr(line, '=');
  if (equals) {
    *value = atoi(equals + 1);
  }
}

/**
 * @brief Capture the bypass flag and deblocking mode from header traces and forward warnings and errors.
 *
 * @param avcl Logging context.
 * @param level Log level.
 * @param fmt Format string.
 * @param vl Format arguments.
 */
static void log_callback(void *avcl, int level, const char *fmt, va_list vl) {
  if (!tracing) {
    if (level <= AV_LOG_WARNING) {
      av_log_default_callback(avcl, level, fmt, vl);
    }
    return;
  }

  char line[1024];
  int print_prefix = 1;
  av_log_format_line(avcl, level, fmt, vl, line, sizeof(line), &print_prefix);
  // First field of every PPS (HEVC) or SPS (H.264)
  if (strstr(line, hevc ? "pps_pic_parameter_set_id" : "profile_idc")) {
    parameter_set_seen = 1;
  }
  if (strstr(line, hevc ? "transquant_bypass_enabled_flag" : "qpprime_y_zero_transform_bypass_flag")) {
    parse_trace_value(line, &bypass_flag);
  } else if (!hevc && strstr(line, "disable_deblocking_filter_idc")) {
    parse_trace_value(line, &deblocking_idc);
  }
}

/**
 * @brief Trace the headers of packets until the bypass flag is known.
 *
 * @param bsf Initialized `trace_headers` bitstream filter.
 * @param pkt Packet to trace.
 */
static void trace_packet(AVBSFContext *bsf, const AVPacket *pkt) {
  if (bypass_flag >= 0) {
    return;
  }

  AVPacket *copy = av_packet_clone(pkt);
  if (!copy) {
    return;
  }

  tracing = 1;
  if (av_bsf_send_packet(bsf, copy) >= 0) {
    while (av_bsf_receive_packet(bsf, copy) >= 0) {
      av_packet_unref(copy);
    }
  }
  tracing = 0;
  av_packet_free(&copy);

  // H.264 only codes the flag in High 4:4:4 profiles
  if (parameter_set_seen && bypass_flag < 0) {
    bypass_flag = 0;
  }
}

/**
 * @brief Print the type, size and, for H.264, the QP 0 macroblock rows of a frame.
 *
 * @param frame_number Decoded frame number.
 * @param frame Decoded frame with exported encoding parameters.
 * @param packet_size Size of the packet the frame was decoded from.
 * @return Nonzero when at least one macroblock row is entirely QP 0.
 */
static int print_frame(int frame_number, const AVFrame *frame, int packet_size) {
  printf("%6d  %c  %9.1f", frame_number, av_get_picture_type_char(frame->pict_type), packet_size / 1000.0);
  if (hevc) {
    printf("\n");
    return 0;
  }

  const AVFrameSideData *side_data = av_frame_get_side_data(frame, AV_FRAME_DATA_VIDEO_ENC_PARAMS);
  if (!side_data) {
    printf("  (no encoding parameters)\n");
    return 0;
  }

  const AVVideoEncParams *params = (const AVVideoEncParams *) side_data->data;
  const int mb_rows = (frame->height + 15) / 16;
  int *zero = calloc(mb_rows, sizeof(int));
  int *total = calloc(mb_rows, sizeof(int));
  char *map = calloc(mb_rows + 1, 1);
  if (!zero || !total || !map) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }

  for (unsigned i = 0; i < params->nb_blocks; i++) {
    const AVVideoBlockParams *block = av_video_enc_params_block((AVVideoEncParams *) params, i);
    const int row = block->src_y / 16;
    if (row < 0 || row >= mb_rows) {
      continue;
    }
    total[row]++;
    if (params->qp + block->delta_qp == 0) {
      zero[row]++;
    }
  }

  int lossless_rows = 0;
  for (int row = 0; row < mb_rows; row++) {
    if (total[row] == 0 || 2 * zero[row] < total[row]) {
      map[row] = '.';
    } else if (zero[row] == total[row]) {
      map[row] = '#';
      lossless_rows++;
    } else {
      map[row] = '+';
    }
  }

  printf("  %s\n", map);

  free(zero);
  free(total);
  free(map);
  return lossless_rows > 0;
}

/**
 * @brief Send a packet to the decoder and print every frame it produces.
 *
 * @param decoder Opened H.264 or HEVC decoder.
 * @param pkt Packet to decode, or `NULL` to drain the decoder.
 * @param frame Reusable decoded frame.
 * @param frame_number Number of the next frame, advanced for every printed frame.
 * @param frames_with_lossless_rows Number of frames with entirely QP 0 rows, advanced as needed.
 */
static void decode_packet(AVCodecContext *decoder, const AVPacket *pkt, AVFrame *frame, int *frame_number, int *frames_with_lossless_rows) {
  const int packet_size = pkt ? pkt->size : 0;
  if (avcodec_send_packet(decoder, pkt) < 0) {
    return;
  }
  while (avcodec_receive_frame(decoder, frame) >= 0) {
    if (print_frame((*frame_number)++, frame, packet_size)) {
      (*frames_with_lossless_rows)++;
    }
    av_frame_unref(frame);
  }
}

/**
 * @brief Decode a raw H.264 or HEVC stream and report lossless coding per frame.
 *
 * @param argc Argument count.
 * @param argv Arguments, the stream path is the only one.
 * @return Zero on success.
 */
int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s dump.h264|dump.h265\n", argv[0]);
    return 2;
  }

  av_log_set_callback(log_callback);

  AVFormatContext *format = NULL;
  if (avformat_open_input(&format, argv[1], NULL, NULL) < 0 || avformat_find_stream_info(format, NULL) < 0) {
    fprintf(stderr, "can't open %s\n", argv[1]);
    return 1;
  }
  const AVCodecParameters *codecpar = format->streams[0]->codecpar;
  if (codecpar->codec_id != AV_CODEC_ID_H264 && codecpar->codec_id != AV_CODEC_ID_HEVC) {
    fprintf(stderr, "%s is neither raw H.264 nor HEVC\n", argv[1]);
    return 1;
  }
  hevc = codecpar->codec_id == AV_CODEC_ID_HEVC;
  const char *codec_name = hevc ? "HEVC" : "H.264";

  const AVBitStreamFilter *trace_headers = av_bsf_get_by_name("trace_headers");
  AVBSFContext *bsf = NULL;
  if (!trace_headers || av_bsf_alloc(trace_headers, &bsf) < 0 || avcodec_parameters_copy(bsf->par_in, codecpar) < 0 || av_bsf_init(bsf) < 0) {
    fprintf(stderr, "can't initialize trace_headers\n");
    return 1;
  }

  AVCodecContext *decoder = avcodec_alloc_context3(avcodec_find_decoder(codecpar->codec_id));
  if (!decoder || avcodec_parameters_to_context(decoder, codecpar) < 0) {
    fprintf(stderr, "can't create %s decoder\n", codec_name);
    return 1;
  }
  decoder->thread_count = 1;
  decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
  av_opt_set(decoder, "export_side_data", "venc_params", 0);
  if (avcodec_open2(decoder, NULL, NULL) < 0) {
    fprintf(stderr, "can't open %s decoder\n", codec_name);
    return 1;
  }

  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  int frame_number = 0;
  int frames_with_lossless_rows = 0;
  int header_printed = 0;

  while (av_read_frame(format, pkt) >= 0) {
    trace_packet(bsf, pkt);
    if (!header_printed && bypass_flag >= 0) {
      if (hevc) {
        printf("profile %d, transquant_bypass_enabled_flag %d%s\n", codecpar->profile, bypass_flag, bypass_flag ? "" : " (blocks are NOT lossless in this stream)");
        printf(" frame  type  size kB\n");
      } else {
        printf("profile_idc %d, qpprime_y_zero_transform_bypass_flag %d%s\n", codecpar->profile, bypass_flag, bypass_flag ? "" : " (QP 0 is NOT lossless in this stream)");
        printf("disable_deblocking_filter_idc %d%s\n", deblocking_idc, deblocking_idc == 1 ? "" : " (deblocking can alter lossless pixels next to lossy ones)");
        printf(" frame  type  size kB  macroblock rows ('#' all QP 0, '+' mostly QP 0, '.' lossy)\n");
      }
      header_printed = 1;
    }

    decode_packet(decoder, pkt, frame, &frame_number, &frames_with_lossless_rows);
    av_packet_unref(pkt);
  }
  decode_packet(decoder, NULL, frame, &frame_number, &frames_with_lossless_rows);

  if (hevc) {
    printf("%d frames, per-block QPs are only available for H.264\n", frame_number);
  } else {
    printf("%d frames, %d with entirely QP 0 macroblock rows\n", frame_number, frames_with_lossless_rows);
  }

  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&decoder);
  av_bsf_free(&bsf);
  avformat_close_input(&format);
  return 0;
}
