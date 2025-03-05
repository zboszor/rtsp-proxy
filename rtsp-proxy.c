/*
 * RSTP proxy with dropout handling
 *
 * Theory of operation:
 * 1. The source feed must produce a specific resolution video.
 *    Since we can't know this resolution in advance,
 *    command line argument --res WxH must be passed to produce
 * 2. Initially produce blank image frames of this specific resolution
 *    and feed it to the destination.
 *    The destination is usually an RTSP multiplexing server,
 *    provided by e.g. rtsp-simple-server or mediamtx.
 * 3. When we can connect to the source feed, start reading frames
 *    and feed it to the destination instead of the blank frame.
 * 4. When the source feed drops out (detected by ffmpeg), use the
 *    last received frame to keep feeding  it to the destination.
 *    Reconnection to the source feed will be attempted.
 *
 * DISCLAIMER:
 * Most of this code is copied / adapted from various FFMPEG
 * source code examples.
 *
 * Compile with:
 * gcc -Wall -g -o rtsp-proxy rtsp-proxy.c $(pkg-config --cflags --libs libavformat libavcodec libavutil libswscale) -lpthread
 *
 * Usage:
 * rtsp-proxy \
 *    --src rtsp://192.168.2.132/Preview_01_main \
 *    --dst rtsp://127.0.0.1/new_feed_name \
 *    --res 1024x768 --fps 15
 *
 * Defaults:
 * --res 800x600
 * --fps 15
 *
 * Source and destination URLs must be specified.
 * The same parameters can be set from an ini file, see README.md
 */

#include <config.h>

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <pthread.h>
#if defined(HAVE_INIPARSER_INIPARSER_H)
#include <iniparser/iniparser.h>
#elif defined(HAVE_INIPARSER_H)
#include <iniparser.h>
#endif
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/parseutils.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#if LIBAVCODEC_VERSION_MAJOR < 61
#error "This software requires FFmpeg 7.0 or newer"
#endif

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61,19,0) /* FFmpeg 7.1 */
#define FFMPEG_7_1 1
#endif

static volatile int quit_program = 0;

static char *dst_url;
static char *src_url;
static char *src_accel;
static char *fps;
static char *res;
static char *src_delay;

#define NFRAMES (2)
AVFrame *frames[NFRAMES];
volatile int frame_idx; /* 0 or 1 */

static AVBufferRef *hw_enc_ctx = NULL;
static enum AVPixelFormat pixelformat = AV_PIX_FMT_YUV420P;
static enum AVPixelFormat hwpixelformat;
static enum AVHWDeviceType hwtype;
static int dst_width;
static int dst_height;
static double fpsval;
static long delay;
static bool try_hwaccel;

static void sighandler(int signum) {
	quit_program = 1;
}

typedef struct OutputStream {
	AVFormatContext *oc;
	AVStream *st;
	AVCodecContext *enc;
} OutputStream;

static void add_stream(OutputStream *ost, AVFormatContext *oc, const AVCodec **codec, enum AVCodecID codec_id) {
	AVCodecContext *c;
	int i, ret;

	/* find the encoder */
	*codec = avcodec_find_encoder(codec_id);
	if (!(*codec)) {
		fprintf(stderr, "Could not find encoder for '%s'\n", avcodec_get_name(codec_id));
		exit(1);
	}

	if (!ost->st)
		ost->st = avformat_new_stream(oc, NULL);
	if (!ost->st) {
		fprintf(stderr, "Could not allocate stream\n");
		exit(1);
	}

	ost->st->id = oc->nb_streams - 1;
	c = avcodec_alloc_context3(*codec);
	if (!c) {
		fprintf(stderr, "Could not alloc an encoding context\n");
		exit(1);
	}

	ost->enc = c;

	const enum AVSampleFormat *sample_fmts = NULL;
	const int *supported_samplerates = NULL;

	switch ((*codec)->type) {
	case AVMEDIA_TYPE_AUDIO:
#if FFMPEG_7_1
		ret = avcodec_get_supported_config(c, NULL, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, (const void**)&sample_fmts, NULL);
#else
		ret = 0;
		sample_fmts = (*codec)->sample_fmts;
#endif
		c->sample_fmt  = (ret >= 0 && sample_fmts) ? sample_fmts[0] : AV_SAMPLE_FMT_FLTP;

		c->bit_rate    = 64000;
		c->sample_rate = 44100;

#if FFMPEG_7_1
		ret = avcodec_get_supported_config(c, NULL, AV_CODEC_CONFIG_SAMPLE_RATE, 0, (const void**)&supported_samplerates, NULL);
#else
		ret = 0;
		supported_samplerates = (*codec)->supported_samplerates;
#endif
		if (supported_samplerates) {
			c->sample_rate = supported_samplerates[0];
			for (i = 0; supported_samplerates[i]; i++) {
				if (supported_samplerates[i] == 44100)
					c->sample_rate = 44100;
			}
		}
		av_channel_layout_copy(&c->ch_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);
		ost->st->time_base = (AVRational){ 1, c->sample_rate };
		break;

	case AVMEDIA_TYPE_VIDEO:
		c->codec_id = codec_id;

		c->bit_rate = 400000;
		/* Resolution must be a multiple of two. */
		c->width    = dst_width;
		c->height   = dst_height;
		/* timebase: This is the fundamental unit of time (in seconds) in terms
		 * of which frame timestamps are represented. For fixed-fps content,
		 * timebase should be 1/framerate and timestamp increments should be
		 * identical to 1. */
		ost->st->time_base = (AVRational){ 1, fpsval };
		c->time_base       = ost->st->time_base;

		c->gop_size      = 12; /* emit one intra frame every twelve frames at most */
		c->pix_fmt       = pixelformat;
		if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
			/* just for testing, we also add B-frames */
			c->max_b_frames = 2;
		}
		if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
			/* Needed to avoid using macroblocks in which some coeffs overflow.
			 * This does not happen with normal video, it just happens here as
			 * the motion of the chroma plane does not match the luma plane. */
			c->mb_decision = 2;
		}
		break;

	default:
		break;
	}

	/* Some formats want stream headers to be separate. */
	if (oc->oformat->flags & AVFMT_GLOBALHEADER)
		c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}

static int write_frame(OutputStream *ost, AVFrame *frame, AVPacket *pkt) {
	int ret;

	/* send the frame to the encoder */
	ret = avcodec_send_frame(ost->enc, frame);
	if (ret < 0) {
		printf("error sending a frame (format code %d vs %d) to the encoder: %s\n", pixelformat, frame->format, av_err2str(ret));
		exit(1);
	}

	while (ret >= 0) {
		ret = avcodec_receive_packet(ost->enc, pkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		else if (ret < 0) {
			fprintf(stderr, "Error encoding a frame: %s\n", av_err2str(ret));
			exit(1);
		}

		/* rescale output packet timestamp values from codec to stream timebase */
		av_packet_rescale_ts(pkt, ost->enc->time_base, ost->st->time_base);
		pkt->stream_index = ost->st->index;

		/* Write the compressed frame to the media file. */
		//log_packet(ost->oc, pkt);

		ret = av_interleaved_write_frame(ost->oc, pkt);
		/*
		 * pkt is now blank (av_interleaved_write_frame() takes ownership of
		 * its contents and resets pkt), so that no unreferencing is necessary.
		 * This would be different if one used av_write_frame().
		 */
		if (ret < 0) {
			fprintf(stderr, "Error while writing output packet: %s\n", av_err2str(ret));
			exit(1);
		}
	}

	return ret == AVERROR_EOF ? 1 : 0;
}

/*
 * encode one video frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
static double parse_double(const char *str, double defaultval) {
	if (str) {
		char *endptr = NULL;
		double dval = strtod(str, &endptr);

		if (!endptr || !*endptr)
			if (dval >= 1.0)
				return dval;
	}

	return defaultval;
}

static bool use_timeout;
static struct timespec base_ts;

static int rtsp_source_interrupt_cb(void *dummy __attribute__((unused))) {
	struct timespec new_ts;

	clock_gettime(CLOCK_MONOTONIC, &new_ts);

	int64_t diff = (new_ts.tv_sec - base_ts.tv_sec) * 1000000000 + (new_ts.tv_nsec - base_ts.tv_nsec);

	/* At least 2 fps is expected from the rtsp source */
	if (use_timeout && (diff > 500000000)) {
		printf("%s: timeout\n", __func__);
		return 1;
	}

	return 0;
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
	const enum AVPixelFormat *p;

	for (p = pix_fmts; *p != -1; p++) {
		if (*p == hwpixelformat)
			return *p;
	}

	printf("failed to get hardware surface format.\n");
	return AV_PIX_FMT_NONE;
}

static void *thrfunc(void *arg) {
	OutputStream *st = arg;

	while (!st->enc)
		usleep(100000);

	if (delay)
		sleep(delay);

	printf("\nReader thread started\n");

	int ret;
	AVCodecContext *dec = NULL;

	bool use_hwaccel = try_hwaccel;

	do {
		AVFormatContext *ic = avformat_alloc_context();

		/* Add interrupt callback */
		AVIOInterruptCB icb = { rtsp_source_interrupt_cb, NULL };
		ic->interrupt_callback = icb;
		use_timeout = false;

		AVDictionary *opts = NULL;
		av_dict_set(&opts, "rtsp_transport", "tcp", 0);
		av_dict_set(&opts, "buffer_size", "200000", 0);

		ret = quit_program ? AVERROR_EOF : avformat_open_input(&ic, src_url, NULL, &opts);
		if (ret >= 0) {
			ret = quit_program ? AVERROR_EOF : avformat_find_stream_info(ic, NULL);
			if (ret >= 0) {
				const AVCodec *decoder = NULL;

				const struct AVInputFormat *fmt = ic->iformat;
				bool file_based = !(fmt->flags & AVFMT_NOFILE);

				ret = quit_program ? AVERROR_EOF : av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
				if (ret >= 0) {
					int video_stream = ret;

					for (int i = 0; use_hwaccel; i++) {
						const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);

						if (config) {
							if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
								config->device_type == hwtype) {
								hwpixelformat = config->pix_fmt;
								break;
							}
						} else {
							printf("decoder %s does not support device type %s.\n", decoder->name, av_hwdevice_get_type_name(hwtype));
							use_hwaccel = false;
						}
					}

					dec = avcodec_alloc_context3(decoder);
					if (!dec) {
						printf("out of memory\n");
						exit(1);
					}

					AVStream *video = ic->streams[video_stream];

					useconds_t frame_delay;
					if (file_based) {
						double fps = (double)video->r_frame_rate.num / (double)video->r_frame_rate.den;

						frame_delay = (useconds_t)((double)1000000.0 / fps);
					}

					ret = quit_program ? -1 : avcodec_parameters_to_context(dec, video->codecpar);
					if (ret >= 0) {
						AVBufferRef *hw_dec_ctx = NULL;

						if (use_hwaccel) {
							void *old_get_format = dec->get_format;

							dec->get_format  = get_hw_format;

							printf("Attempting to use hardware acceleration for decoding\n");

							ret = av_hwdevice_ctx_create(&hw_dec_ctx, hwtype, NULL, NULL, 0);
							if (ret >= 0) {
								dec->hw_device_ctx = av_buffer_ref(hw_dec_ctx);
							} else {
								printf("hardware decoder initialization failed\n");
								dec->get_format = old_get_format;
								use_hwaccel = false;
							}
						}

						ret = quit_program ? AVERROR_EOF : avcodec_open2(dec, decoder, &opts);
						if (ret >= 0) {
							printf("Reader thread opened %s successfully.\n\n", src_url);

							av_dump_format(ic, 0, src_url, 0);

							AVPacket *dec_pkt = av_packet_alloc();

							struct SwsContext *sws_ctx =
								sws_getContext(dec->width, dec->height, dec->pix_fmt,
												dst_width, dst_height, pixelformat,
												SWS_BILINEAR, NULL, NULL, NULL);
							assert(sws_ctx);

							av_read_play(ic);

							AVFrame *hw_frame = use_hwaccel ? av_frame_alloc() : NULL;
							AVFrame *dec_frame = NULL;
							bool use_zerocopy = false;
							bool valid_zerocopy = false;

							while (!quit_program && ret >= 0) {
								use_timeout = true;
								clock_gettime(CLOCK_MONOTONIC, &base_ts);
								ret = quit_program ? AVERROR_EOF : av_read_frame(ic, dec_pkt);
								if (ret >= 0) {
									/*
									 * check if the packet belongs to a stream we are interested in,
									 * otherwise skip it
									 */
									if (dec_pkt->stream_index == video_stream) {
										/* submit the packet to the decoder */
										ret = quit_program ? AVERROR_EOF : avcodec_send_packet(dec, dec_pkt);
										if (ret >= 0) {
											if (!valid_zerocopy) {
												valid_zerocopy = true;

												use_zerocopy =
													dst_width == dec->width &&
													dst_height == dec->height && (
														(use_hwaccel && pixelformat == dec->sw_pix_fmt) ||
														(!use_hwaccel && pixelformat == dec->pix_fmt));

												if (use_zerocopy) {
													printf("rendering directly into destination frames\n");
												} else {
													dec_frame = av_frame_alloc();

													dec_frame->width = dec->width;
													dec_frame->height = dec->height;
													dec_frame->format = use_hwaccel ? dec->sw_pix_fmt : dec->pix_fmt;
													av_frame_get_buffer(dec_frame, 0);
												}
											}

											/* get all the available frames from the decoder */
											while (!quit_program && ret >= 0) {
												if (use_zerocopy)
													dec_frame = frames[1 - frame_idx];

												int ret = quit_program ? AVERROR_EOF : avcodec_receive_frame(dec, use_hwaccel ? hw_frame : dec_frame);
												if (ret >= 0) {
													AVFrame *tmp_frame = NULL;

													if (use_hwaccel) {
														if (hw_frame->format == hwpixelformat) {
															/* retrieve data from GPU to CPU */
															ret = av_hwframe_transfer_data(dec_frame, hw_frame, 0);
															if (ret >= 0) {
																tmp_frame = dec_frame;
															} else
																printf("error transferring the data to system memory\n");
														} else {
															tmp_frame = hw_frame;
														}
													} else {
														tmp_frame = dec_frame;
													}

													/* convert to destination format */
													if (!use_zerocopy) {
														AVFrame *dst_frame = frames[1 - frame_idx];

														ret = sws_scale(sws_ctx,
																	(const uint8_t * const *)tmp_frame->data,
																	tmp_frame->linesize, 0, tmp_frame->height,
																	dst_frame->data, dst_frame->linesize);
													}

													if (ret >= 0) {
														frame_idx = 1 - frame_idx;

														if (file_based)
															usleep(frame_delay);
													} else {
														printf("scaling failed: %s\n", av_err2str(ret));
													}
												} else if (!quit_program) {
													/*
													 * These two return values are special and mean there is no output
													 * frame available, but there were no errors during decoding
													 */
													if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
														ret = 0;
														break;
													} else if (!quit_program) {
														printf("error during decoding (%s)\n", av_err2str(ret));
													}
												}
											}
										} else if (!quit_program) {
											printf("error submitting a packet for decoding (%s)\n", av_err2str(ret));
										}
									}
									av_packet_unref(dec_pkt);
								}
							}

							if (!use_zerocopy)
								av_frame_free(&dec_frame);
							if (use_hwaccel)
								av_frame_free(&hw_frame);

							av_packet_free(&dec_pkt);
							sws_freeContext(sws_ctx);
							avcodec_free_context(&dec);
						} else if (!quit_program) {
							printf("Failed to open codec for decoding. Error code: %s\n", av_err2str(ret));
						}

						av_buffer_unref(&hw_dec_ctx);
					} else if (!quit_program) {
						printf("avcodec_parameters_to_context error. Error code: %s\n", av_err2str(ret));
					}

					avcodec_free_context(&dec);
				} else if (!quit_program) {
					fprintf(stderr, "Cannot find a video stream in the input file. Error code: %s\n", av_err2str(ret));
				}
			} else if (!quit_program) {
				printf("Cannot find stream information in %s\n", src_url);
			}
			avformat_close_input(&ic);
		}

		av_dict_free(&opts);

		if (!quit_program)
			sleep(1);
	} while (!quit_program && ret < 0);

	printf("reader thread exits\n");

	return NULL;
}

void parse_ini_file(const char *ininame, const char *section) {
	dictionary *dict = iniparser_load(ininame);
	int sections, i;

	if (!dict)
		return;

	sections = iniparser_getnsec(dict);

	for (i = 0; i < sections; i++) {
		const char *sect = iniparser_getsecname(dict, i);

		if (strcmp(sect, section) == 0) {
			int sectlen = strlen(sect) + 1;
			int nkeys = iniparser_getsecnkeys(dict, sect);

			const char **keys0 = malloc(nkeys * sizeof(char *));
			const char **keys;
			int j;

			keys = iniparser_getseckeys(dict, sect, keys0);

			for (j = 0; j < nkeys; j++) {
				const char *onlykey = keys0[j] + sectlen;
				const char *val = iniparser_getstring(dict, keys0[j], "");
				if (strcasecmp(onlykey, "SourceURL") == 0 && val && *val) {
					free(src_url);
					src_url = strdup(val);
				} else if (strcasecmp(onlykey, "SourceDelay") == 0 && val && *val) {
					free(src_delay);
					src_delay = strdup(val);
				} else if (strcasecmp(onlykey, "DecodeHWAccel") == 0 && val && *val) {
					free(src_accel);
					src_accel = strdup(val);
				} else if (strcasecmp(onlykey, "DestURL") == 0 && val && *val) {
					free(dst_url);
					dst_url = strdup(val);
				} else if (strcasecmp(onlykey, "DestResolution") == 0 && val && *val) {
					free(res);
					res = strdup(val);
				} else if (strcasecmp(onlykey, "DestFPS") == 0 && val && *val) {
					free(fps);
					fps = strdup(val);
				}
			}

			free(keys);
		}
	}

	iniparser_freedict(dict);
}

int main(int argc, char **argv) {
	static struct option opts[] = {
		{ "help",	no_argument,		NULL,	'h' },
		{ "ini",	required_argument,	NULL,	'i' },
		{ "src",	required_argument,	NULL,	's' },
		{ "dst",	required_argument,	NULL,	'd' },
		{ "src-accel",	required_argument,	NULL,	'a' },
		{ "res",	required_argument,	NULL,	'r' },
		{ "fps",	required_argument,	NULL,	'f' },
		{ "src-delay",	required_argument,	NULL,	'w' },
		{ NULL,		0,					NULL,	0   },
	};
	char *inisection = NULL;
	OutputStream st;
	pthread_t thr;

	memset(&st, 0, sizeof(OutputStream));

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	while (1) {
		int c = getopt_long(argc, argv, "a:hi:s:d:r:f:w:", opts, NULL);

		if (c == -1)
			break;

		switch (c) {
		case 'h':
			printf("RTSP proxy using FFmpeg libraries\n\nOptions:\n");
			printf("-h, --help\n\tThis help text.\n");
			printf("-i <section>, --ini <section>\n\tRead options from [section] in /etc/rtsp-proxy.ini\n");
			printf("-s url, --src url\n\tSource video file or URL\n");
			printf("-d url, --dst url\n\tDestination video file or URL\n");
			printf("-a <accel>, --src-accel <accel>\n\tHardware acceleration method for decoding.\n");
			printf("\tAvailable device types:");
			{
				enum AVHWDeviceType hwtype = AV_HWDEVICE_TYPE_NONE;
				while((hwtype = av_hwdevice_iterate_types(hwtype)) != AV_HWDEVICE_TYPE_NONE)
					printf(" %s", av_hwdevice_get_type_name(hwtype));
				printf("\n");
			}
			printf("-r WxH, --res WxH\n\tDestination video resolution\n");
			printf("-f N, --fps N\n\tDestination video frame rate\n");
			printf("-w N, --src-delay\n\tWait N seconds before opening the source\n");
			return 0;
		case 'i':
			free(inisection);
			inisection = optarg ? strdup(optarg) : NULL;
			break;
		case 's':
			free(src_url);
			src_url = optarg ? strdup(optarg) : NULL;
			break;
		case 'd':
			free(dst_url);
			dst_url = optarg ? strdup(optarg) : NULL;
			break;
		case 'a':
			free(src_accel);
			src_accel = optarg ? strdup(optarg) : NULL;
			break;
		case 'r':
			free(res);
			res = optarg ? strdup(optarg) : NULL;
			break;
		case 'f':
			free(fps);
			fps = optarg ? strdup(optarg) : NULL;
			break;
		case 'w':
			free(src_delay);
			src_delay = optarg ? strdup(optarg) : NULL;
			break;
		}
	}

	if (inisection)
		parse_ini_file("/etc/rtsp-proxy.ini", inisection);

	if (!res || av_parse_video_size(&dst_width, &dst_height, res) < 0) {
		dst_width = 800;
		dst_height = 600;
	}

	fpsval = parse_double(fps, 15.0);
	delay = (long)parse_double(src_delay, 0.0);

	if (!src_url || !dst_url) {
		printf("source or destination not specified\n");
		return 1;
	}

	printf("Source stream URL: %s\n", src_url ? src_url : "unset, error");
	printf("Destination stream URL: %s\n", dst_url ? dst_url : "unset, error");
	printf("Destination stream parameters: width x height: %dx%d @ %.2lf fps\n", dst_width, dst_height, fpsval);

	avformat_network_init();

	hwtype = src_accel ? av_hwdevice_find_type_by_name(src_accel) : AV_HWDEVICE_TYPE_NONE;
	if (src_accel && hwtype == AV_HWDEVICE_TYPE_NONE) {
		printf("Device type %s is not supported.\n", src_accel ? src_accel : "<unspecified>");
		printf("Available device types:");
		while((hwtype = av_hwdevice_iterate_types(hwtype)) != AV_HWDEVICE_TYPE_NONE)
			printf(" %s", av_hwdevice_get_type_name(hwtype));
		printf("\n");
	} else if (src_accel) {
		printf("Attempting to use %s hardware acceleration for decoding\n", src_accel);
		try_hwaccel = true;
	}

	printf("\n");

	avformat_alloc_output_context2(&st.oc, NULL, "rtsp", dst_url);
	if (!st.oc) {
		printf("Could not create rtsp output context.\n");
		return 1;
	}

	const AVOutputFormat *fmt = st.oc->oformat;
#if 1
	enum AVCodecID codec_id = fmt->video_codec;
#else
	enum AVCodecID codec_id = AV_CODEC_ID_H264; /* too much CPU usage */
#endif

	const AVCodec *video_codec;
	int ret;
	do {
		add_stream(&st, st.oc, &video_codec, codec_id);

		AVDictionary *opts = NULL;

		/* open the codec */
		ret = avcodec_open2(st.enc, video_codec, &opts);
		if (ret >= 0) {
			/* copy the stream parameters to the muxer */
			ret = avcodec_parameters_from_context(st.st->codecpar, st.enc);
			if (ret >= 0) {
				/* open the output file, if needed */
				if (!(fmt->flags & AVFMT_NOFILE)) {
					ret = avio_open(&st.oc->pb, dst_url, AVIO_FLAG_WRITE);
					if (ret < 0) {
						printf("could not open '%s': %s\n", dst_url, av_err2str(ret));
						avcodec_free_context(&st.enc);
						return 1;
					}
				}

				ret = avformat_write_header(st.oc, NULL);
				if (ret < 0) {
					printf("Failed to write header: %s\n", av_err2str(ret));
					avcodec_free_context(&st.enc);
					sleep(1);
				}
			} else {
				printf("Could not copy the stream parameters\n");
				avcodec_free_context(&st.enc);
				sleep(1);
			}
		} else {
			printf("Could not open video codec: %s\n", av_err2str(ret));
			sleep(1);
		}

		av_dict_free(&opts);
	} while (!quit_program && ret < 0);

	if (quit_program)
		return 0;

	av_dump_format(st.oc, 0, dst_url, 1);

	/* YUV420P is likely the format the source (camera?) supplies. */
	for (int i = 0; i < NFRAMES; i++) {
		frames[i] = av_frame_alloc();
		frames[i]->width = dst_width;
		frames[i]->height = dst_height;
		frames[i]->format = pixelformat;

		av_frame_get_buffer(frames[i], 0);

		/* Weird green frame */
		memset(frames[i]->data[0], 0x80, frames[i]->linesize[0] * frames[i]->height);		/* Y = 0x80 */
		memset(frames[i]->data[1], 0x00, frames[i]->linesize[1] * frames[i]->height / 2);	/* U = 0x00 */
		memset(frames[i]->data[2], 0x80, frames[i]->linesize[2] * frames[i]->height / 2);	/* V = 0x80 */
	}

	int64_t pts = 0;
	int64_t ptsinc = (int64_t)(1000.0 / fpsval);

	pthread_create(&thr, NULL, thrfunc, &st);

	AVPacket *enc_pkt = av_packet_alloc();

	while (!quit_program) {
		frames[frame_idx]->pts = pts;

		write_frame(&st, frames[frame_idx], enc_pkt);

		usleep(1000 * ptsinc);

		pts++;
	}

	printf("exiting...\n");

	av_write_trailer(st.oc);

	avcodec_free_context(&st.enc);
	av_packet_free(&enc_pkt);

	void *thread_retval __attribute__((unused)) = NULL;
	pthread_join(thr, &thread_retval);

	avformat_free_context(st.oc);

	for (int i = 0; i < NFRAMES; i++)
		av_frame_free(&frames[i]);

	avformat_network_init();

	free(inisection);
	free(src_url);
	free(dst_url);
	free(src_accel);
	free(src_delay);
	free(res);
	free(fps);

	return 0;
}
