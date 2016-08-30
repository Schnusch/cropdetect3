/* Copyright (c) 2016, Schnusch
   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:

       1. Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.

       2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.

       3. Neither the name of the copyright holder nor the names of its
       contributors may be used to endorse or promote products derived from this
       software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
   AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
   ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
   LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
   CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
   SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
   INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
   CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
   ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
   POSSIBILITY OF SUCH DAMAGE. */

#define ENABLE_OUTPUT

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/lfg.h>
#include <libavutil/random_seed.h>

#define CROPDETECT_IGNORED_FRAMES  2

#define arraylen(x)  (sizeof(x) / sizeof((x)[0]))

#ifdef ENABLE_STATISTICS
struct crop_frame_stats {
	int x1;
	int y1;
	int x2;
	int y2;
};

struct crop_stats {
	struct {
		int x1;
		int y1;
		int x2;
		int y2;
	} max;
	unsigned int bad_frames;
	unsigned int pkts;
	unsigned int pkts_skipped;

	size_t iframe;
	size_t nframes;
	struct crop_frame_stats *frames;
};
#endif

struct crop_info {
	int x1;
	int y1;
	int x2;
	int y2;

	unsigned int bad;
	unsigned int good;
	unsigned int pkts;
	unsigned int pkts_skipped;
	unsigned int npos;
};

static const char     *log_get_item_name(void *ctx);
static AVClassCategory log_get_category(void *ctx);
static const AVClass base_log_ctx = {
		.version      = LIBAVUTIL_VERSION_INT,
		.item_name    = &log_get_item_name,
		.get_category = &log_get_category
};

static const struct {
	const AVClass  *ctx;
	const char     *name;
	AVClassCategory category;
} log_ctxs[] = {
		{&base_log_ctx, "main",         AV_CLASS_CATEGORY_INPUT},
		{&base_log_ctx, "filter_init",  AV_CLASS_CATEGORY_FILTER},
		{&base_log_ctx, "seek",         AV_CLASS_CATEGORY_INPUT},
		{&base_log_ctx, "filter_parse", AV_CLASS_CATEGORY_FILTER},
		{&base_log_ctx, "output",       AV_CLASS_CATEGORY_OUTPUT},
		{&base_log_ctx, "TODO",         AV_CLASS_CATEGORY_NA}
};
#define LOG_MAIN         0
#define LOG_FILTER_INIT  1
#define LOG_SEEK         2
#define LOG_FILTER_PARSE 3
#define LOG_OUTPUT       4
#define LOG_TODO         5

const char *log_get_item_name(void *ctx)
{
	for(size_t i = 0; i < arraylen(log_ctxs); i++)
		if(ctx == &log_ctxs[i].ctx)
			return log_ctxs[i].name;
	return NULL;
}
AVClassCategory log_get_category(void *ctx)
{
	for(size_t i = 0; i < arraylen(log_ctxs); i++)
		if(ctx == &log_ctxs[i].ctx)
			return log_ctxs[i].category;
	return AV_CLASS_CATEGORY_NA;
}
#define log(x, ...)  av_log((AVClass **)&log_ctxs[LOG_##x].ctx, __VA_ARGS__)

static AVFilterContext *cropdetect_ctx = NULL;
#ifdef ENABLE_OUTPUT
//static AVCodecContext *enc_ctx = NULL;
#endif

static void log_suppress_cropdetect(void *ptr, int level, const char *fmt, va_list vl)
{
//	fprintf(stderr, "%p vs %p\n", ptr, enc_ctx);
	if(cropdetect_ctx == NULL || ptr != cropdetect_ctx)
		av_log_default_callback(ptr, level, fmt, vl);
}


struct verbose_io_ctx {
	int fd;
	unsigned int nreads;
	unsigned int nseeks;
	unsigned int nbytes;
};

static int verbose_io_read(void *opaque, uint8_t *buf, int n)
{
	struct verbose_io_ctx *ctx = opaque;
	int m = read(ctx->fd, buf, n);
	if(m == -1)
		return AVERROR(errno);
	ctx->nreads++;
	ctx->nbytes += m;
	return m;
}

static int64_t verbose_io_seek(void *opaque, int64_t off, int whence)
{
	struct verbose_io_ctx *ctx = opaque;
	off_t pos = lseek(ctx->fd, off, whence);
	if(pos == -1)
		return AVERROR(errno);
	ctx->nseeks++;
	return pos;
}

/**
 * \param  in_ctx   should be closed on failure
 * \param  io_data  io_data.fd should be closed on failure
 */
int init_verbose_io(AVIOContext **io_ctx, AVFormatContext **in_ctx,
		struct verbose_io_ctx *io_data, const char *input)
{
	static const size_t io_bufsize = 4096;

	io_data->fd = open(input, O_RDONLY);
	if(io_data->fd == -1)
		return AVERROR(errno);

	uint8_t *io_buf;
	if((*in_ctx = avformat_alloc_context()) == NULL
			|| (io_buf = av_malloc(io_bufsize)) == NULL)
		return AVERROR(ENOMEM);
	if((*io_ctx = avio_alloc_context(io_buf, io_bufsize, 0, io_data,
			&verbose_io_read, NULL, &verbose_io_seek)) == NULL)
	{
		av_free(io_buf);
		return AVERROR(ENOMEM);
	}
	(*in_ctx)->pb = *io_ctx;
	return 0;
}


int select_stream(const AVCodec **dec, int *idx, AVFormatContext *fmt_ctx)
{
	const AVStream *st;
	if(*idx == -1)
	{
		// automatically select stream
		*idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
		if(*idx == AVERROR_STREAM_NOT_FOUND)
		{
			log(MAIN, AV_LOG_FATAL, "Failed to guess stream");
			return -1;
		}
		log(MAIN, AV_LOG_INFO, "Stream #%d guessed\n", *idx);
		st = fmt_ctx->streams[*idx];
	}
	else if((unsigned int)*idx < fmt_ctx->nb_streams)
	{
		// manually select stream
		st = fmt_ctx->streams[*idx];
		if(st->codec->codec_type != AVMEDIA_TYPE_VIDEO)
		{
			const char *type = av_get_media_type_string(st->codec->codec_type);
			log(MAIN, AV_LOG_FATAL, "Type of stream #%d is %s", *idx,
					type == NULL ? "unknown" : type);
			return -1;
		}
	}
	else
	{
		log(MAIN, AV_LOG_FATAL, "Stream #%d does not exist", *idx);
		return -1;
	}
	// get decoder
	if((*dec = avcodec_find_decoder(st->codec->codec_id)) == NULL)
	{
		log(MAIN, AV_LOG_FATAL, "No decoder for stream #%d was found", *idx);
		return -1;
	}
	return 0;
}


int buffersink_get_single_frame(AVFilterContext *bufsink_ctx, AVFrame *f, const  AVClass * const *cls)
{
	AVFrame *f2 = av_frame_alloc();
	if(f2 == NULL)
	{
		av_log((void *)cls, AV_LOG_PANIC, "Allocating AVFrame failed: %s\n", av_err2str(AVERROR(ENOMEM)));
		return -1;
	}
	int ret;
	if((ret = av_buffersink_get_frame(bufsink_ctx, f)) >= 0)
	{
		// first frame successfully read, ignore all following
		unsigned int nread = 0;
		while((ret = av_buffersink_get_frame(bufsink_ctx, f2)) >= 0)
		{
			av_frame_unref(f2);
			nread++;
		}
		if(ret == AVERROR(EAGAIN))
			ret = 0;
		if(nread > 0)
			av_log((void *)cls, AV_LOG_WARNING, "cropdetect produced %u unexpected frames\n", nread);
	}
	av_frame_free(&f2);
	if(ret < 0)
		av_log((void *)cls, AV_LOG_PANIC, "Failed to read frame from filter: %s\n", av_err2str(ret));
	return -(ret != 0);
}


int prepare_args(const char *file, int line, const char *func, char *buf, size_t n, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	errno = 0;
	int nwrit = vsnprintf(buf, n, fmt, ap);
	va_end(ap);
	if(nwrit < 0)
	{
		if(errno == 0)
			log(FILTER_INIT, AV_LOG_PANIC, "Failed to initialize filter args\n");
		else
			log(FILTER_INIT, AV_LOG_PANIC, "Failed to initialize filter args: %s\n", strerror(errno));
		return -1;
	}
	else if((size_t)nwrit >= n)
	{
		log(FILTER_INIT, AV_LOG_PANIC, "Failed to initialize filter args: "
				"buffer too small (%s:%d in %s)\n", file, line, func);
		return -1;
	}
	else
		return 0;
}
#define prepare_args(...)  prepare_args(__FILE__, __LINE__, __func__, __VA_ARGS__)

int feed_frames_to_ignore(AVFilterContext *bufsrc_ctx,
		AVFilterContext *bufsink_ctx, AVCodecContext *dec_ctx)
{
	int         lvl = AV_LOG_WARNING;
	int         err = 0;
	const char *msg = NULL;
	// feed cropdetect initial frames to ignore
	AVFrame *f = av_frame_alloc();
	if(f == NULL)
	{
		err = AVERROR(ENOMEM);
		msg = "allocate framebuffer";
	}
	else
	{
		uint8_t *data[4];
		int      linesize[4];
		if((err = av_image_alloc(data, linesize, dec_ctx->width,
				dec_ctx->height, dec_ctx->pix_fmt, 32)) < 0) // TODO 32
			msg = "fill framebuffer";
		else
		{
			for(unsigned int i = 0; i < CROPDETECT_IGNORED_FRAMES; i++)
			{
				// create frame
				f->format = dec_ctx->pix_fmt;
				f->width  = dec_ctx->width;
				f->height = dec_ctx->height;
				memcpy(f->data,     data,     sizeof(data));
				memcpy(f->linesize, linesize, sizeof(linesize));

				// feed frame
				if((err = av_buffersrc_add_frame(bufsrc_ctx, f)) != 0) // f is reference counted
				{
					msg = "feed frame to filter";
					break;
				}

				// read filtered frame
				if((err = buffersink_get_single_frame(bufsink_ctx, f, &log_ctxs[LOG_FILTER_INIT].ctx)) != 0)
					break;
			}
			av_freep(&data);
			if(err != 0)
				lvl = AV_LOG_PANIC;
		}
		av_frame_free(&f);
	}
	if(err != 0 && msg != NULL)
		log(FILTER_INIT, AV_LOG_WARNING, "Failed to %s: %s\n", msg, av_err2str(err));
	return -(lvl != AV_LOG_WARNING);
}

/**
 * \param  filt_graph  should be freed on failure
 */
int init_cropdetect(AVFilterGraph **filt_graph, AVFilterContext **bufsrc_ctx,
		AVFilterContext **bufsink_ctx, AVCodecContext *dec_ctx, unsigned short black)
{
	// init buffer args
	char bufsrc_args[128];
	if(prepare_args(bufsrc_args, sizeof(bufsrc_args),
			"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
			dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
			dec_ctx->time_base.num, dec_ctx->time_base.den,
			dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den) != 0)
		return -1;
	char cropdetect_args[10];
	if(prepare_args(cropdetect_args, sizeof(cropdetect_args), "%hu:2:1", black) != 0)
		return -1;

	// create filter graph
	if((*filt_graph = avfilter_graph_alloc()) == NULL)
	{
		log(FILTER_INIT, AV_LOG_PANIC, "Failed to initialize filtergraph: %s\n", av_err2str(AVERROR(ENOMEM)));
		return -1;
	}

	int ret;
	struct {
		AVFilterContext **ctx;
		const AVFilter   *filt;
		const char       *name;
		const char       *args;
	} filts[] = {
			{bufsrc_ctx,      NULL, "buffer",     bufsrc_args},
			{&cropdetect_ctx, NULL, "cropdetect", cropdetect_args},
			{bufsink_ctx,     NULL, "buffersink", NULL}
	};
	for(size_t i = 0; i < arraylen(filts); i++)
	{
		// select filter
		if((filts[i].filt = avfilter_get_by_name(filts[i].name)) == NULL)
		{
			log(FILTER_INIT, AV_LOG_FATAL, "Could not find filter %s\n", filts[i].name);
			return -1;
		}
		// instantiate filter
		if((ret = avfilter_graph_create_filter(filts[i].ctx, filts[i].filt,
				filts[i].name, filts[i].args, NULL, *filt_graph)) < 0)
		{
			log(FILTER_INIT, AV_LOG_PANIC, "Failed to initialize filter %s: %s\n",
					filts[i].name, av_err2str(ret));
			return -1;
		}
		// link filter
		if(i > 0 && (ret = avfilter_link(*filts[i - 1].ctx, 0, *filts[i].ctx, 0)) != 0)
		{
			log(FILTER_INIT, AV_LOG_PANIC, "Failed to connect filters %s and %s: %s\n",
					filts[i - 1].name, filts[i].name, av_err2str(ret));
			return -1;
		}
	}

	// configure fiter graph
	if((ret = avfilter_graph_config(*filt_graph, NULL)) < 0)
	{
		log(FILTER_INIT, AV_LOG_PANIC, "Failed to configure filtergraph: %s\n", av_err2str(ret));
		return -1;
	}

	return feed_frames_to_ignore(*bufsrc_ctx, *bufsink_ctx, dec_ctx);
}


#ifdef ENABLE_OUTPUT
/**
 * \param  out_ctx  out_ctx->pb should be closed and out_ctx freed
 * \param  enc_ctx  should be closed
 * \param  opts     input should be freed on failure
 */
int open_output(AVFormatContext **out_ctx, AVCodecContext **enc_ctx,
		const char *outname, const char *outcodec, AVDictionary **opts,
		const AVCodecContext *dec_ctx)
{
	int ret = 0;

	// open output file
	if((ret = avformat_alloc_output_context2(out_ctx, NULL, NULL, outname)) < 0
			|| (ret = avio_open(&(*out_ctx)->pb, outname, AVIO_FLAG_WRITE)) < 0)
	{
		log(OUTPUT, AV_LOG_FATAL, "Failed to open output \"%s\": %s\n", outname, av_err2str(ret));
		return ret;
	}

	// add new stream to output
	AVCodec *enc = avcodec_find_encoder_by_name(outcodec);
	if(enc == NULL)
	{
		log(OUTPUT, AV_LOG_PANIC, "Could not find encoder %s\n", outcodec);
		return -1;
	}
	AVStream *st = avformat_new_stream(*out_ctx, enc);
	if(st == NULL)
	{
		log(OUTPUT, AV_LOG_PANIC, "Failed to add a new stream to output\n");
		return -1;
	}

	// init stream
	st->time_base                   = (AVRational){1, 2}; // TODO variable fps
	*enc_ctx = st->codec;
	(*enc_ctx)->width               = dec_ctx->width;
	(*enc_ctx)->height              = dec_ctx->height;
	(*enc_ctx)->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
	(*enc_ctx)->pix_fmt             = dec_ctx->pix_fmt; // TODO check if supported
	(*enc_ctx)->time_base           = st->time_base;
	if(strcmp(outcodec, "libschroedinger") == 0)
		(*enc_ctx)->global_quality = 0;
	else if(strcmp(outcodec, "libvpx-vp9") == 0)
		ret = av_dict_set(opts, "lossless", "1", 0);

	// open encoder
	if(ret == 0 && (ret = avcodec_open2(*enc_ctx, enc, opts)) == 0)
	{
		AVDictionaryEntry *e = NULL;
		while((e = av_dict_get(*opts, "", e, AV_DICT_IGNORE_SUFFIX)) != NULL)
		{
			log(OUTPUT, AV_LOG_WARNING, "Encoder %s did not recognize option %s=%s\n",
					enc->name, e->key, e->value);
			ret = AVERROR(EINVAL);
		}
	}
	av_dict_free(opts);
	if(ret != 0)
	{
		log(OUTPUT, AV_LOG_PANIC, "Failed to open encoder %s: %s\n", enc->name, av_err2str(ret));
		return ret;
	}

	log(OUTPUT, AV_LOG_WARNING, "Be warned, output is probably shit\n"); // FIXME

	return 0;
}
#endif


int parse_cropped_frame_metadata(struct crop_info *crop, AVFrame *frame)
{
	AVDictionary *meta = av_frame_get_metadata(frame);
	if(meta == NULL)
	{
		log(FILTER_PARSE, AV_LOG_WARNING, "Cropdetect did not provide frame metadata\n");
		crop->bad++;
		return 0;
	}

	struct {
		const char *key;
		int         value;
	} entries[] = {
			{"lavfi.cropdetect.x1", 0},
			{"lavfi.cropdetect.y1", 0},
			{"lavfi.cropdetect.x2", 0},
			{"lavfi.cropdetect.y2", 0}
	};
	for(size_t i = 0; i < arraylen(entries); i++)
	{
		AVDictionaryEntry *e = av_dict_get(meta, entries[i].key, NULL, 0); // TODO? flags
		if(e == NULL)
		{
			log(FILTER_PARSE, AV_LOG_PANIC, "Cropdetect did not provide necessary frame metadata %s\n", entries[i].key);
			return -1;
		}

		// parse int
		char *end;
		long l = strtol(e->value, &end, 0);
		if(l < 0 || l > (long)INT_MAX || *end != '\0')
		{
			log(FILTER_PARSE, AV_LOG_PANIC, "Cropdetect metadata %s = \"%s\" invalid\n",
						entries[i].key, e->value);
			return -1;
		}
		entries[i].value = l;
	}
	int x1 = entries[0].value;
	int y1 = entries[1].value;
	int x2 = entries[2].value + 1;
	int y2 = entries[3].value + 1;
	log(FILTER_PARSE, AV_LOG_INFO, "x1: %d, y1: %d, x2: %d, y2: %d\n", x1, y1, x2, y2);
	if(x1 < crop->x1) crop->x1 = x1;
	if(y1 < crop->y1) crop->y1 = y1;
	if(x2 > crop->x2) crop->x2 = x2;
	if(y2 > crop->y2) crop->y2 = y2;
	crop->good++;

	return 0;
}


int filter_packet(struct crop_info *crop, AVFrame *f,
		AVPacket *pkt_in,  int *frame_read,    AVCodecContext *dec_ctx,
#ifdef ENABLE_OUTPUT
		AVPacket *pkt_out, int *frame_written, AVCodecContext *enc_ctx, AVFormatContext *out_ctx,
#endif
		AVFilterContext *bufsrc_ctx, AVFilterContext *bufsink_ctx)
{
	int ret = 0;
	uint8_t *in_data = pkt_in->data;
	int      in_size = pkt_in->size;

	int nprocd;
	while(pkt_in->size > 0 && (nprocd = avcodec_decode_video2(dec_ctx, f, frame_read, pkt_in)) > 0)
	{
		if(*frame_read)
		{
			*frame_read = 0;
			// feed filter
			if((ret = av_buffersrc_add_frame(bufsrc_ctx, f)) != 0) // on success takes ownership of f
			{
				log(TODO, AV_LOG_PANIC, "Failed to feed frame to filter: %s\n", av_err2str(ret));
				av_frame_unref(f);
			}
			// read and parse filtered frame
			if(ret == 0 && (ret = buffersink_get_single_frame(bufsink_ctx, f, &log_ctxs[LOG_TODO].ctx)) >= 0)
			{
				ret = parse_cropped_frame_metadata(crop, f);
#ifdef ENABLE_OUTPUT
				if(ret == 0 && enc_ctx != NULL)
				{
					// TODO completely shit

					// write frame to output
					if((ret = avcodec_encode_video2(enc_ctx, pkt_out, f, frame_written)) != 0)
						log(OUTPUT, AV_LOG_PANIC, "Failed to encode frame: %s\n", av_err2str(ret));
					else if(*frame_written)
					{
						// TODO fix timestamp
						if((ret = av_interleaved_write_frame(out_ctx, pkt_out)) != 0)
							log(OUTPUT, AV_LOG_PANIC, "Failed to write frame: %s\n", av_err2str(ret));
						*frame_written = 0;
					}
				}
#endif
				av_frame_unref(f);
				*frame_read = 1;
			}
		}
		// av_frame_unref unnecessary because *frame_read == 0
		pkt_in->data += nprocd;
		pkt_in->size -= nprocd;
		if(ret != 0)
			break;
	}
	if(nprocd < 0)
	{
		log(TODO, AV_LOG_PANIC, "Video decoding failed: %s\n", av_err2str(nprocd));
		ret = nprocd;
	}

	pkt_in->data = in_data;
	pkt_in->size = in_size;
	return -(ret != 0);
}

void round_crop(int *off, int *dim, int limit, int mod)
{
	int fix = *dim % mod;
	if(fix == 0)
		return;

	fix = mod - fix;
	*dim += fix;
	if(*dim >= limit)
	{
		*off = 0;
		*dim = limit;
	}
	else if(fix / 2 > *off)
		*off = 0;
	else
	{
		*off -= fix / 2;
		if(*off + *dim > limit)
			*off = limit - *dim;
	}
}


static int parse_uint(void *i, size_t n, const char *s, unsigned long max)
{
	if(*s < '0' || '9' < *s)
		return -1;
	char *end;
	unsigned long l = strtoul(s, &end, 0);
	if(l == ULONG_MAX || *end != '\0' || l > max)
		return -1;
#if BYTE_ORDER == LITTLE_ENDIAN
	memcpy(i, &l, n);
#elif BYTE_ORDER == BIG_ENDIAN
	memcpy(i, &l + sizeof(l) - n, n);
#else
	#error Endianess not supported
#endif
	return 0;
}

int main(int argc, char **argv)
{
	// parse opts
	unsigned short black  = 24;
	unsigned int   modulo = 16;
	unsigned int   npos   = 100;
	unsigned int   num    = 1;
	int idx       = -1;
	int verbose   = 0;
	int handbrake = 0;
#ifdef ENABLE_OUTPUT
	const char *outname  = NULL;
	const char *outcodec = "libvpx-vp9";
//	outcodec = "libschroedinger";
#endif
	int used_opts = 0;
	char used_opt_search[3];
	used_opt_search[1] = ':';
	used_opt_search[2] = '\0';
	static const char optstring[] = "Hb:dhm:n:o:p:qs:v";
	int opt;
	while((opt = getopt(argc, argv, optstring)) != -1)
	{
		if(opt != '?')
		{
			// check option duplicates
			used_opt_search[0] = opt;
			char *o = strstr(optstring, used_opt_search);
			if(o != NULL && (size_t)(o - optstring) < sizeof(int) * CHAR_BIT)
			{
				int opt_bit = 1 << (o - optstring);
				if(used_opts & opt_bit)
					fprintf(stderr, "%s: option '%c' already specified, overriding...\n", argv[0], opt);
				used_opts |= opt_bit;
			}
		}

		const char *err = NULL;
		switch(opt)
		{
		case 'h':
			printf("Usage: %s [OPTION]... INPUT\n"
"Detect video crop\n"
"\n"
"  -bBLACK   set black value threshold to BLACK (by default 24)\n"
#ifdef ENABLE_OUTPUT
"  -d        use lossless libschroedinger instead of vp9 as output encoder\n"
#endif
"  -H        use HandBrake's crop format\n"
"  -h        display this help\n"
"  -mMODULO  round dimensions to the next bigger multiple of MODULO\n"
"            (by default 16)\n"
"  -nNUM     cropdetect NUM frames at every position (by default 1)\n"
#ifdef ENABLE_OUTPUT
"  -oOUTPUT  write cropdetected frames to OUTPUT\n"
#endif
"  -pNPOS    use NPOS positions to detect crop (by default 100)\n"
"  -q        do not print libavfilter's cropdetect notifications\n"
"  -sSTREAM  use stream with number STREAM instead of trying to guess one\n"
"  -v        verbose output, multiple occurences increase verbosity\n", argv[0]);
			return 0;

		case 'd':
#ifdef ENABLE_OUTPUT
			outcodec = "libschroedinger";
			break;
#endif
		case 'o':
#ifdef ENABLE_OUTPUT
			outname = optarg;
			break;
#else
			fprintf(stderr, "%s: Output file was not enabled on build\n", argv[0]);
			return 2;
#endif

		case 'H':
			handbrake = 1;
			break;
		case 'q':
			av_log_set_callback(&log_suppress_cropdetect);
			break;
		case 'v':
			if(verbose == INT_MAX)
			{
				fprintf(stderr, "%s: Verbosity must be less than or equal to %d\n", argv[0], INT_MAX);
				return 2;
			}
			verbose++;
			break;

		case 'b':
			if(parse_uint(&black, sizeof(black), optarg, USHRT_MAX) == -1)
				err = "black threshold";
			break;
		case 'm':
			if(parse_uint(&modulo, sizeof(modulo), optarg, UINT_MAX) == -1)
				err = "modulo";
			break;
		case 'n':
			if(parse_uint(&num, sizeof(num), optarg, UINT_MAX) == -1)
				err = "number of frames";
			break;
		case 'p':
			if(parse_uint(&npos, sizeof(npos), optarg, UINT_MAX) == -1)
				err = "number of positions";
			break;
		case 's':
			if(parse_uint(&idx, sizeof(idx), optarg, INT_MAX) == -1)
				err = "stream index";
			break;

		default:
			return 2;
		}
		if(err != NULL)
		{
			fprintf(stderr, "%s: %s %s invalid\n", argv[0], err, optarg);
			return 2;
		}
	}
	const char *input = argv[optind++];
	if(input == NULL)
	{
		fprintf(stderr, "%s: No input specified\n", argv[0]);
		return 2;
	}

	int ret = 0;
	struct verbose_io_ctx io_data    = {-1, 0, 0, 0};
#ifdef ENABLE_STATISTICS
	struct crop_stats     stats      = NULL;
#endif
	AVIOContext          *io_ctx     = NULL;
	AVFormatContext      *in_ctx     = NULL;
	AVCodecContext       *dec_ctx    = NULL;
#ifdef ENABLE_OUTPUT
	AVFormatContext      *out_ctx    = NULL;
	AVCodecContext       *enc_ctx    = NULL;
#endif
	AVFilterGraph        *filt_graph = NULL;
	AVFrame              *frame      = NULL;

	av_register_all();
	avfilter_register_all();

	// verbose I/O
	if(verbose >= 2)
		ret = init_verbose_io(&io_ctx, &in_ctx, &io_data, input);

	// open input file
	if(ret != 0 || (ret = avformat_open_input(&in_ctx, input, NULL, NULL)) != 0)
	{
		log(MAIN, AV_LOG_FATAL, "Failed to open input \"%s\": %s\n", input, av_err2str(ret));
		goto end;
	}
	if((ret = avformat_find_stream_info(in_ctx, NULL)) < 0)
	{
		log(MAIN, AV_LOG_FATAL, "Could not read \"%s\"'s stream information: %s\n", input, av_err2str(ret));
		goto end;
	}

	if(verbose >= 1)
		av_dump_format(in_ctx, 0, input, 0);

	// select stream/codec
	const AVCodec *dec;
	if(select_stream(&dec, &idx, in_ctx) != 0)
		goto end;

	// open decoder
	dec_ctx = avcodec_alloc_context3(dec);
	if(dec_ctx == NULL)
		ret = AVERROR(ENOMEM);
	else
		ret = avcodec_copy_context(dec_ctx, in_ctx->streams[idx]->codec);
	if(ret == 0)
	{
		dec_ctx->refcounted_frames = 1; // not sure if a good idea, but makes later code less non-sensical
		ret = avcodec_open2(dec_ctx, dec, NULL);
	}
	if(ret != 0)
	{
		log(MAIN, AV_LOG_PANIC, "Failed to open decoder %s: %s\n", dec->name, av_err2str(ret));
		goto end;
	}

	// init filter
	AVFilterContext *bufsrc_ctx, *bufsink_ctx;
	if(init_cropdetect(&filt_graph, &bufsrc_ctx, &bufsink_ctx, dec_ctx, black) != 0)
		goto end;

#ifdef ENABLE_OUTPUT
	if(outname != NULL)
	{
		AVDictionary *codec_opts = NULL;
		if(open_output(&out_ctx, &enc_ctx, outname, outcodec, &codec_opts, dec_ctx) != 0)
			goto end;

		if(verbose >= 1)
			av_dump_format(out_ctx, 0, outname, 1);
	}
#endif

	if(verbose >= 2)
	{
		log(MAIN, AV_LOG_INFO, "reads:           %u\n", io_data.nreads);
		log(MAIN, AV_LOG_INFO, "seeks:           %u\n", io_data.nseeks);
		log(MAIN, AV_LOG_INFO, "bytes read:      %u\n", io_data.nbytes);
		io_data.nreads = 0;
		io_data.nseeks = 0;
		io_data.nbytes = 0;
	}

	// init buffers
	frame = av_frame_alloc();
	if(frame == NULL)
	{
		ret = AVERROR(ENOMEM);
		log(MAIN, AV_LOG_PANIC, "Failed to initialize framebuffer: %s\n", av_err2str(ret));
		goto end;
	}
	AVPacket pkt_in;
	av_init_packet(&pkt_in);
	pkt_in.data = NULL;
	pkt_in.size = 0;
#ifdef ENABLE_OUTPUT
	AVPacket pkt_out;
	av_init_packet(&pkt_out);
	pkt_out.data = NULL;
	pkt_out.size = 0;
#endif

	struct crop_info crop = { .x1 = dec_ctx->width, .y1 = dec_ctx->height };

#ifdef ENABLE_STATISTICS
	size_t nframes = 0;
	size_t nbytes  = 0;
	if(verbose >= 1)
	{
		if(av_size_mult(npos, num, &nframes) != 0
				|| av_size_mult(nframes, sizeof(struct crop_frame_stats), &nbytes) != 0
				|| nbytes > SIZE_MAX - sizeof(*stats))
		{
			log(MAIN, AV_LOG_PANIC, "Failed to allocate crop statistics buffer: %s\n", strerror(EOVERFLOW));
			goto end;
		}
	}
	stats.max.x1  = dec_ctx->width;
	stats.max.y1  = dec_ctx->height;
	stats.max.x2  = 0;
	stats.max.y2  = 0;
	stats.iframe  = 0;
	stats.nframes = nframes;
	stats.frames  = malloc(nbytes);
	if(stats.frames == NULL)
	{
		log(MAIN, AV_LOG_PANIC, "Failed to allocate crop statistics buffer: %s\n", strerror(errno));
		goto end;
	}
#endif

	// TODO use codec length no container length
	AVRational *tb = &in_ctx->streams[idx]->time_base;
	int64_t duration = in_ctx->duration * tb->den / (tb->num * AV_TIME_BASE); // TODO av_rescale_q
	int64_t block    = duration / npos;

	int frame_read;
#ifdef ENABLE_OUTPUT
	int frame_written;
	// init container
	if(outname != NULL && (ret = avformat_write_header(out_ctx, NULL)) != 0)
	{
		log(OUTPUT, AV_LOG_PANIC, "Failed to write format headers: %s\n", av_err2str(ret));
		goto end;
	}
#endif
	for(unsigned int i = 0; i < npos; i++)
	{
		AVLFG lfg;
		av_lfg_init(&lfg, av_get_random_seed());

		uint64_t random = av_lfg_get(&lfg);
		if(block > UINT_MAX)
			random = (random << (sizeof(int) * CHAR_BIT)) | av_lfg_get(&lfg);
		random %= block;

		int64_t pos = i * block + random;

		if((ret = av_seek_frame(in_ctx, idx, pos, 0)) < 0)
		{
			log(MAIN, AV_LOG_PANIC, "Failed to seek input: %s%s\n", av_err2str(ret),
					ret == AVERROR(EPERM) ? " (we probably tried to seek beyond EOF)" : "");
			goto end;
		}

		crop.npos++;

		if(verbose >= 1)
			log(SEEK, AV_LOG_INFO, "seeked to %"PRIu64" + %"PRIu64" = %"PRIu64" (timebase: %u/%u)\n", i * block, random, pos, tb->num, tb->den);

		// read stream
		unsigned int frames_read = 0;
		while(frames_read < num && (ret = av_read_frame(in_ctx, &pkt_in)) == 0)
		{
			frame_read = 0;
			crop.pkts++;
			if(pkt_in.stream_index == idx)
				ret = filter_packet(&crop, frame,
						&pkt_in,  &frame_read,    dec_ctx,
#ifdef ENABLE_OUTPUT
						&pkt_out, &frame_written, enc_ctx, out_ctx,
#endif
						bufsrc_ctx, bufsink_ctx);
			else
				crop.pkts_skipped++;
			av_packet_unref(&pkt_in);
			if(ret != 0)
				goto end;
			if(pkt_in.stream_index == idx && frame_read)
				frames_read++; // TODO should go in filter_packet
		}
		if(ret == AVERROR_EOF)
		{
			log(MAIN, AV_LOG_WARNING, "EOF reached\n");
			ret = 0;
		}
		else if(ret != 0)
		{
			log(MAIN, AV_LOG_PANIC, "Failed to read input: %s\n", av_err2str(ret));
			goto end;
		}
	}
#ifdef ENABLE_OUTPUT
	if(enc_ctx != NULL)
	{
		// write buffered frames
		frame_written = 1;
		while(frame_written)
		{
			if((ret = avcodec_encode_video2(enc_ctx, &pkt_out, NULL, &frame_written)) != 0)
			{
				log(OUTPUT, AV_LOG_PANIC, "Error encoding frame: %s\n", av_err2str(ret));
				goto end;
			}
			if(frame_written)
			{
				// TODO fix timestamp
				if((ret = av_interleaved_write_frame(out_ctx, &pkt_out)) != 0)
				{
					log(OUTPUT, AV_LOG_PANIC, "Failed to write frame: %s\n", av_err2str(ret));
					goto end;
				}
				frame_written = 0;
			}
		}

		if((ret = av_write_trailer(out_ctx)) != 0)
		{
			log(OUTPUT, AV_LOG_PANIC, "Error failed to write format trailer: %s\n", av_err2str(ret));
			goto end;
		}
	}
#endif

	if(verbose >= 1)
	{
		log(MAIN, AV_LOG_INFO, "positions:       %u\n",    crop.npos);
		log(MAIN, AV_LOG_INFO, "bad frames:      %u/%u\n", crop.bad, crop.good + crop.bad);
		log(MAIN, AV_LOG_INFO, "ignored packets: %u/%u\n", crop.pkts_skipped, crop.pkts);
		if(verbose >= 2)
		{
			log(MAIN, AV_LOG_INFO, "reads:           %u\n", io_data.nreads);
			log(MAIN, AV_LOG_INFO, "seeks:           %u\n", io_data.nseeks);
			log(MAIN, AV_LOG_INFO, "bytes read:      %u\n", io_data.nbytes);
		}
		log(MAIN, AV_LOG_INFO, "crop:            %d,%d - %d,%d\n", crop.x1, crop.y1, crop.x2, crop.y2);
	}

	// round dimensions
	int x = crop.x1;
	int y = crop.y1;
	int w = crop.x2 - crop.x1;
	int h = crop.y2 - crop.y1;
	if(w < 0 || h < 0)
	{
		log(MAIN, AV_LOG_WARNING, "TODO handle negative width/height\n"); // TODO
		goto end;
	}
	// FIXME 34,140 - 1920,940 -> 1888:800:-32:140
	round_crop(&x, &w, dec_ctx->width,  modulo);
	round_crop(&y, &h, dec_ctx->height, modulo);

	if(handbrake)
		printf("%d:%d:%d:%d\n", y, dec_ctx->height - h - y, x, dec_ctx->width - w - x);
	else
		printf("%d:%d:%d:%d\n", w, h, x, y);

end:
#ifdef ENABLE_STATISTICS
	free(stats.frames);
#endif

	av_frame_free(&frame);
	avfilter_graph_free(&filt_graph);

#ifdef ENABLE_OUTPUT
	avcodec_close(enc_ctx);
	if(out_ctx != NULL)
	{
		avio_close(out_ctx->pb);
		avformat_free_context(out_ctx);
	}
#endif

	avcodec_free_context(&dec_ctx);
	if(verbose >= 2)
	{
		if(io_ctx != NULL)
		{
			av_free(io_ctx->buffer);
			av_freep(&io_ctx);
		}
		if(io_data.fd != -1)
			close(io_data.fd);
	}
	avformat_close_input(&in_ctx);

	return ret != 0;
}
