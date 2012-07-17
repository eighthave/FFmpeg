/*
 * Copyright (c) 2011 Andrew Senior 
 *
 * This file is for use with ffmpeg
 *
 */

/**
 * @file
 * Redaction filter. Read a file describing redactions. Wipe boxes in frames
 * accordingly.
 */

/* The redactions filename is passed as the sole parameter to the filter.
 * The file consists of lines describing "boxtracks" which are defined
 * by 7 comma-separated values: "%lf,%lf,%d,%d,%d,%d,%s"
 * e.g. "0.5,1.5,50,100,0,1000,green"
 * First: start and end times (floating point, in seconds, referenced to the
 * presentation time stamp given by ffmpeg) and left,right, top, bottom
 * spatial coordinates of the redaction region. (origin is top left, 
 * coordinates increase down and to right.)
 * finally a redaction method string is given which is either "pixel" for
 * pixellation, "inverse" for inverse pixellation (not yet implemented)
 * or an ffmpeg color specifier for solid redaction. 
 * The file can contain comments, ie lines beginning with "#".
 */

/* Use:
 * put this file in the libavfilter directory
 * add the line
       REGISTER_FILTER (REDACT,      redact,      vf);
 * to avfilter_register_all in libavfilter/allfilters.c,
 * add
       OBJS-$(CONFIG_REDACT_FILTER)                 += vf_redact.o
 * to libavfilter/Makefile, and
       CONFIG_REDACT_FILTER=yes
 * to config.mak
 */


/* todo:
 * add noise to pixellation to make super-resolution attacks harder.
 * a more stable-across-time pixellation appearance
 * inverse pixellation
 * allow the megapixel size to be specified.
 * alternative ways to import the specification
 *    (e.g. another input port, or directly in the config string)
 * doing face detection & tracking on the fly in the filter
 * preserving redacted information in a separate stream
 * allow motion specification for tracks (velocity, spline...)
 * reference megapixels to the top left, rather than absolute grid?
 */

#include "libavutil/colorspace.h"
#include "libavutil/pixdesc.h"
#include "libavutil/parseutils.h"
#include "avfilter.h"

enum { Y, U, V, A };

typedef enum {redact_solid, redact_pixellate, redact_inverse_pixellate}
  redaction_method;
typedef struct {
  int l, r, t, b;
  double start, end;
  redaction_method method;
  unsigned char yuv_color[4];  // Used when method is redact_solid
} BoxTrack;

typedef struct {
  int vsub, hsub;   //< chroma subsampling
  int numtracks;
  double time_seconds;
  BoxTrack **boxtracks;
} RedactionContext;

static BoxTrack *box_track_from_string(const char *track_def,
				       AVFilterContext *ctx) {
  BoxTrack *boxtrack = NULL;
  int rv = 0;
  int l, r, t, b;
#define BUFLEN 1000
  char method[BUFLEN];
  double start, end;

  // Allow comments, empty lines.
  if (track_def[0] == '#' || track_def[0] == '\0')
    return NULL;

  rv = sscanf(track_def, "%lf,%lf,%d,%d,%d,%d,%s", &start,&end, 
	      &l, &r, &t, &b, method);
  if (rv != 7) {
    av_log(ctx, AV_LOG_ERROR, "Failed to parse boxtrack '%s' .\n", track_def);
    return NULL;
  }
  boxtrack = (BoxTrack *)av_malloc(sizeof(BoxTrack));
  boxtrack->l = l;
  boxtrack->r = r;
  boxtrack->t = t;
  boxtrack->b = b;
  boxtrack->start = start;
  boxtrack->end = end;
  boxtrack->method = redact_pixellate;

  // todo: allow the granularity of the pixellation to be specified.
  if (strncasecmp(method, "pixel", 5) == 0)
    boxtrack->method = redact_pixellate;
  else if (strncasecmp(method, "inv", 3) == 0)
    boxtrack->method = redact_inverse_pixellate;
  else {
    uint8_t rgba_color[4];

    boxtrack->method = redact_solid;

    if (av_parse_color(rgba_color, method, -1, ctx) < 0)
      av_log(ctx, AV_LOG_ERROR, "Couldn't parse colour '%s' .\n", method);

    boxtrack->yuv_color[Y] =
      RGB_TO_Y_CCIR(rgba_color[0], rgba_color[1], rgba_color[2]);
    boxtrack->yuv_color[U] =
      RGB_TO_U_CCIR(rgba_color[0], rgba_color[1], rgba_color[2], 0);
    boxtrack->yuv_color[V] =
      RGB_TO_V_CCIR(rgba_color[0], rgba_color[1], rgba_color[2], 0);
    boxtrack->yuv_color[A] = rgba_color[3];
  }
  return boxtrack;
}

static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    RedactionContext *redaction= ctx->priv;
    FILE *file = NULL;
    char buf[BUFLEN];

    redaction->boxtracks = NULL;
    redaction->time_seconds = NAN;
    if (!args) {
      av_log(ctx, AV_LOG_ERROR, "No arguments given to redact.\n");
      return 1;
    }
    file = fopen(args, "r");
    if (!file)  {
      av_log(ctx, AV_LOG_ERROR, "Can't open redaction file: '%s'\n", args);
      return 2;
    }
    redaction->numtracks = 0;
    // Parse the config file.
    while (!feof(file)) {
      BoxTrack **boxtracks = NULL;
      BoxTrack *new_track = NULL;

      if (fgets(buf, BUFLEN, file) == NULL) break; // EOF
      new_track = box_track_from_string(buf, ctx);
      if (new_track == NULL)
	continue;
      // Resize the array and add the new track.
      boxtracks = (BoxTrack **)av_malloc((redaction->numtracks + 1) *
					 sizeof(BoxTrack *));
      for (int i = 0; i < redaction->numtracks; ++i)
	boxtracks[i] = redaction->boxtracks[i];
      boxtracks[redaction->numtracks++] = new_track;
      av_free(redaction->boxtracks);
      redaction->boxtracks = boxtracks;
    }
    fclose(file);
    // Sort the tracks so the earliest-starting are at the end of the array.
    for (int j = 0; j < redaction->numtracks -1; ++j)
      for (int k = j + 1; k < redaction->numtracks; ++k)
	if (redaction->boxtracks[j]->start < redaction->boxtracks[k]->start) {
	  BoxTrack *temp = redaction->boxtracks[j];
	  redaction->boxtracks[j] = redaction->boxtracks[k];
	  redaction->boxtracks[k] = temp;
	}

    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    enum PixelFormat pix_fmts[] = {
        PIX_FMT_YUV444P,  PIX_FMT_YUV422P,  PIX_FMT_YUV420P,
        PIX_FMT_YUV411P,  PIX_FMT_YUV410P,
        PIX_FMT_YUVJ444P, PIX_FMT_YUVJ422P, PIX_FMT_YUVJ420P,
        PIX_FMT_YUV440P,  PIX_FMT_YUVJ440P,
        PIX_FMT_NONE
    };

    avfilter_set_common_pixel_formats(ctx, avfilter_make_format_list(pix_fmts));
    return 0;
}

static int config_input(AVFilterLink *inlink)
{
    RedactionContext *redaction = inlink->dst->priv;

    redaction->hsub = av_pix_fmt_descriptors[inlink->format].log2_chroma_w;
    redaction->vsub = av_pix_fmt_descriptors[inlink->format].log2_chroma_h;

    av_log(inlink->dst, AV_LOG_INFO, "Redaction with %d tracks\n",
	   redaction->numtracks);

    return 0;
}

// Decode the timestamp.
static void start_frame(AVFilterLink *inlink, AVFilterBufferRef *picref)
{
  RedactionContext *redaction = inlink->dst->priv;
  redaction->time_seconds = picref->pts * av_q2d(inlink->time_base);
  avfilter_start_frame(inlink->dst->outputs[0],
		       avfilter_ref_buffer(picref, ~0));
}

static void draw_slice(AVFilterLink *inlink, int y0, int h, int slice_dir)
{
  RedactionContext *redaction = inlink->dst->priv;
  int x, y;
  unsigned char *row[4];
  AVFilterBufferRef *picref = inlink->cur_buf;

  for (int box = redaction->numtracks -1; box >= 0; --box) {
    BoxTrack *boxtrack = redaction->boxtracks[box];

    // Tracks are sorted by start time, so if this one starts in the future
    // all remaining ones will.
    if (boxtrack->start > redaction->time_seconds)
      break;
    if (boxtrack->end < redaction->time_seconds) {
      // Delete any tracks we've passed.
      av_free(redaction->boxtracks[box]);
      // Shuffle down any still-active tracks higher in the array.
      // (We've already processed them this frame.)
      for (int t = box + 1; t < redaction->numtracks; ++t)
	redaction->boxtracks[t - 1] = redaction->boxtracks[t];
      // Reduce the count.
      redaction->boxtracks[--redaction->numtracks] = NULL;
    } else {
      int xb = boxtrack->l, yb = boxtrack->t;
      int hb = boxtrack->b - boxtrack->t;
      int wb = boxtrack->r - boxtrack->l;
      int megapixel_size = 64;  // todo: get from file
      for (y = FFMAX(yb, y0); y < (y0 + h) && y < (yb + hb); y++) {
	row[0] = picref->data[0] + y * picref->linesize[0];

	for (int plane = 1; plane < 3; plane++)
	  row[plane] = picref->data[plane] +
	    picref->linesize[plane] * (y >> redaction->vsub);

	for (x = FFMAX(xb, 0); x < (xb + wb) && x < picref->video->w; x++) {
	  double alpha = (double)boxtrack->yuv_color[A] / 255;

	  if (boxtrack->method == redact_solid) {
	    row[0][x                 ] =
	      (1 - alpha) * row[0][x                 ] +
	      alpha * boxtrack->yuv_color[Y];
	    row[1][x >> redaction->hsub] =
	      (1 - alpha) * row[1][x >> redaction->hsub] +
	      alpha * boxtrack->yuv_color[U];
	    row[2][x >> redaction->hsub] =
	      (1 - alpha) * row[2][x >> redaction->hsub] +
	      alpha * boxtrack->yuv_color[V];
	  } else if (boxtrack->method == redact_pixellate) {
	    int x_quant = (x / megapixel_size) * megapixel_size;
	    int y_quant = (y / megapixel_size) * megapixel_size;
	    row[0][x] = (picref->data[0] + y_quant *
			 picref->linesize[0])[x_quant];
	    row[1][x >> redaction->hsub] =
	      (picref->data[1] + picref->linesize[1] *
	       (y_quant >> redaction->vsub))[x_quant >> redaction->hsub];
	    row[2][x >> redaction->hsub] =
	      (picref->data[2] + picref->linesize[2] *
	       (y_quant >> redaction->vsub))[x_quant >> redaction->hsub];
	  }
	}
      }
    }
  }
  avfilter_draw_slice(inlink->dst->outputs[0], y0, h, 1);
}

static av_cold void uninit(AVFilterContext *ctx)
{
  RedactionContext *redaction= ctx->priv;
  for (int i = 0; i < redaction->numtracks; ++i) {
    av_free(redaction->boxtracks[i]);
  }
  av_free(redaction->boxtracks);
}

AVFilter avfilter_vf_redact = {
  .name      = "redact",
  .description =
  NULL_IF_CONFIG_SMALL("Redact the input video according to a track file."),
  .priv_size = sizeof(RedactionContext),
  .init      = init,
  .uninit      = uninit,

  .query_formats   = query_formats,
  .inputs    = (AVFilterPad[]) {{ .name             = "default",
				  .type             = AVMEDIA_TYPE_VIDEO,
				  .config_props     = config_input,
				  .get_video_buffer =
				  avfilter_null_get_video_buffer,
				  .start_frame      = start_frame,
				  .draw_slice       = draw_slice,
				  .end_frame        = avfilter_null_end_frame,
				  .min_perms        = AV_PERM_WRITE |
				  AV_PERM_READ,
				  .rej_perms        = AV_PERM_PRESERVE },
				{ .name = NULL}},
  .outputs   = (AVFilterPad[]) {{ .name             = "default",
				  .type             = AVMEDIA_TYPE_VIDEO, },
				{ .name = NULL}},
};
