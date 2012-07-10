/*
 * Copyright (c) 2011 Andrew Senior 
 *
 * This file is for use with ffmpeg
 * Version 0.04 - reverting to single output.
 * No memory leak on linux. Checked with ffmpeg 0.10.4
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
 * "blur" for face blurring.
 * or an ffmpeg color specifier for solid redaction. 
 * The file can contain comments, ie lines beginning with "#".
 * Based on vf_transpose.
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

#include "libavutil/avstring.h"
#include "libavutil/colorspace.h"
#include "libavutil/lfg.h"
#include "libavutil/pixdesc.h"
#include "libavutil/parseutils.h"
#include "avfilter.h"
#include <strings.h>

enum { Y, U, V, A };
static int logging = 0;
typedef enum {redact_solid, 
	      redact_pixellate, 
	      redact_inverse_pixellate, 
	      redact_blur}
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
  AVLFG random;
  AVFilterBufferRef *lastredacted;    ///< Previous frame
} RedactionContext;

static void log_box_track(BoxTrack *bt,
			  AVFilterContext *ctx) {
  av_log(ctx, AV_LOG_INFO, "Box track: %d: (%.1f-%.1fs) %d-%d x %d-%d\n", 
	 bt->method, bt->start, bt->end,
	 bt->l, bt->r, bt->t, bt->b);
}

// memory status stuff from
// stackoverflow.com/questions/1558402/memory-usage-of-current-process-in-c
typedef struct {
    unsigned long size,resident,share,text,lib,data,dt;
} statm_t;

static void read_off_memory_status(statm_t * result)
{
  const char* statm_path = "/proc/self/statm";

  FILE *f = fopen(statm_path,"r");
  if(!f){
    abort();
  }
  if(7 != fscanf(f,"%ld %ld %ld %ld %ld %ld %ld",
		 &result->size,
		 &result->resident,
		 &result->share,
		 &result->text,
		 &result->lib,
		 &result->data,
		 &result->dt))
  {
    abort();
  }
  fclose(f);
}

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
  if (av_strncasecmp(method, "pixel", 5) == 0)
    boxtrack->method = redact_pixellate;
  else if (av_strncasecmp(method, "inv", 3) == 0)
    boxtrack->method = redact_inverse_pixellate;
  else if (av_strncasecmp(method, "blur", 4) == 0)
    boxtrack->method = redact_blur;
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
    unsigned int seed=298379;
    redaction->boxtracks = NULL;
    redaction->lastredacted = NULL;
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
      if (strncmp(buf, "seed", 4) == 0) {
	int rv = sscanf(buf, "seed %ud", &seed);
	if (rv != 1)
	  av_log(ctx, AV_LOG_ERROR, "Didn't parse seed: %s.\n", buf);

	continue;
      }
      new_track = box_track_from_string(buf, ctx);
      if (new_track == NULL)
	continue;
      log_box_track(new_track, ctx);
      // Resize the array and add the new track.
      boxtracks = (BoxTrack **)av_malloc((redaction->numtracks + 1) *
					 sizeof(BoxTrack *));
      for (int i = 0; i < redaction->numtracks; ++i)
	boxtracks[i] = redaction->boxtracks[i];
      boxtracks[redaction->numtracks++] = new_track;
      av_free(redaction->boxtracks);
      redaction->boxtracks = boxtracks;
    }
    av_log(ctx, AV_LOG_INFO, "Seed is : '%ud'\n", seed);
    av_lfg_init(&redaction->random, seed);
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

    av_log(inlink->dst, AV_LOG_INFO, "Redaction with %d tracks %d %d\n",
	   redaction->numtracks, redaction->hsub, redaction->vsub);

    return 0;
}

// Decode the timestamp.
static void start_frame(AVFilterLink *inlink, AVFilterBufferRef *picref)
{
  AVFilterContext *ctx = inlink->dst;
  RedactionContext *redaction = inlink->dst->priv;
  AVFilterLink *outlink0 = inlink->dst->outputs[0];
  
  redaction->time_seconds = picref->pts * av_q2d(inlink->time_base);
  outlink0->out_buf = avfilter_get_video_buffer(outlink0, AV_PERM_WRITE,
						outlink0->w, outlink0->h);
  outlink0->out_buf->pts = picref->pts;
  avfilter_start_frame(outlink0,
		       avfilter_ref_buffer(outlink0->out_buf, ~0));
}

static int noise = 10;
static void convolve_ny(int t, int b, int maxy,
			unsigned char *row, int blur, unsigned char *blurbuf,
			int step,
			AVLFG *random) {
  int halfblur = blur/2;
  int blursum = 0;
  int i;
  for (i = 0; i < blur; ++i) {
    int pos = t + i - halfblur;
    blurbuf[i] = (pos <= 0) ? row[0] : row[pos * step];
    blursum += blurbuf[i];
  }
  for (int y = t; y < b; ++y, ++i) {
    int newval = blursum / blur;
    int newpos = y + (blur + 1)/2;
    i %= blur;
    if (noise > 0) {
      newval += (av_lfg_get(random) % (2 * noise + 1)) - noise;
      if (newval < 0 ) newval = 0;
      else if (newval > 255) newval = 255;
    }
    row[y*step] = newval;
    blursum -= blurbuf[i];
    blurbuf[i] = row[step * ((newpos < maxy) ? newpos : (maxy - 1))];
    blursum += blurbuf[i];
  }
}
// Keep a rolling buffer of n values and their sum.
// Store the average in the output vector and update the sum by dropping one
// value and inserting another.
static void convolve_nx(int l, int r, int maxx,
			unsigned char *row, int blur, unsigned char *blurbuf,
			AVLFG *random) {
  int halfblur = blur/2;
  int blursum = 0;
  int i;
  for (i = 0; i < blur; ++i) {
    int pos = l + i - halfblur;
    blurbuf[i] = (pos <= 0) ? row[0] : row[pos];
    blursum += blurbuf[i];
  }
  for (int x = l; x < r; ++x, ++i) {
    int newval = blursum / blur;
    int newpos = x + (blur + 1)/2;
    i %= blur;
    if (noise > 0) {
      newval += (av_lfg_get(random) % (2 * noise + 1)) - noise;
      if (newval < 0 ) newval = 0;
      else if (newval > 255) newval = 255;
    }
    row[x] = newval;
    blursum -= blurbuf[i];
    blurbuf[i] = row[(newpos < maxx) ? newpos : (maxx - 1)];
    blursum += blurbuf[i];
  }
}

static void blur_one_round(AVFilterBufferRef *picref, BoxTrack *boxtrack,
			   int y0, int h, int hsub, int vsub,
			   int blur, unsigned char *blurbuf,
			   AVLFG *random) {
  int xb = boxtrack->l, yb = boxtrack->t;
  int hb = boxtrack->b - boxtrack->t;
  int wb = boxtrack->r - boxtrack->l;
  int x, y;
  int xmax, ymax;

#define BLURX
#define BLURY
#ifdef BLURX  
  x = FFMAX(xb, 0);
  blur = wb/2;
  xmax = FFMIN( (xb + wb), picref->video->w);
  for (y = FFMAX(yb, y0); y < (y0 + h) && y < (yb + hb); ++y) {
    for (int plane = 0; plane < 3; plane++) {
      int ds = (plane == 0) ? 0 : hsub;
      unsigned char *row = picref->data[plane] +
	picref->linesize[plane] * (y >> ((plane == 0) ? 0 : vsub));
      convolve_nx(x >> ds, (xmax + ((1<<ds) -1))>> ds, picref->video->w >> ds,
		  row,
		  (blur  + ((1<<ds) -1))>> ds, blurbuf, random);
    }
  }
#endif
#ifdef BLURY
  y  = FFMAX(yb, y0);
  ymax = FFMIN( (yb + hb), (y0 + h));
  blur = hb/2;
  for (x = FFMAX(xb, 0); x < (xb + wb) && x < picref->video->w; x++) {
    for (int plane = 0; plane < 3; plane++) {
      int ds = (plane == 0) ? 0 : vsub;
      unsigned char *col = picref->data[plane] + (x >>  ((plane == 0) ? 0 : hsub));
      convolve_ny(y >> ds, (ymax + ((1<<ds) -1))  >> ds,
		  (y0 + h) >> ds,
		  col, (blur  + ((1<<ds) -1))>> ds, blurbuf,
		  picref->linesize[plane], random);
    }
  }
#endif
}

static void copybox_mixold_alpha(AVFilterBufferRef *source,
				 AVFilterBufferRef *picref,
				 AVFilterBufferRef *lastref,
				 BoxTrack *boxtrack,
				 int hsub, int vsub,
				 AVLFG *random) {
  int xb = boxtrack->l;
  int yb = boxtrack->t;
  int hb = boxtrack->b - boxtrack->t;
  int wb = boxtrack->r - boxtrack->l;

  float blur_boundary = 0.2;
  for (int y = FFMAX(yb, 0); y < picref->video->h && y < boxtrack->b; y++) {
    float ynormsq = (y * 2.0 - (boxtrack->b + boxtrack->t)) / hb;
    ynormsq *= ynormsq;
    for (int plane = 0; plane < 3; plane++) {
      int ysub = (y >> ((plane == 0) ? 0 : vsub));
      unsigned char *row = picref->data[plane] +
	picref->linesize[plane] * ysub;
      unsigned char *srcrow = source->data[plane] +
	picref->linesize[plane] * ysub;
      unsigned char *lastrow = lastref->data[plane] +
	picref->linesize[plane] * ysub;
      
      int thishsub = (plane == 0) ? 0 : hsub;
      int xmin = FFMAX(xb, 0) >> thishsub;
      int xmax = FFMIN(boxtrack->r, picref->video->w)
	>> thishsub;
      for (int x = xmin; x < xmax; x++) {
	// TODO: do the alphablending in int.
	// TODO: allow a flag for alpha blending or not.
	float xnorm = ((x << thishsub) * 2.0 - (boxtrack->l + boxtrack->r))
	  / wb;
	float mixlast = ((av_lfg_get(random) % 20) + 10) / 40.0;
	float alphax = (1 - sqrt(xnorm * xnorm + ynormsq));
	if (alphax < 0) {
	  row[x] = srcrow[x];
	  continue;
	}
	if (alphax > blur_boundary)
	  alphax = 1;
	else
	  alphax /= blur_boundary;
	row[x] = (1 - alphax) * srcrow[x] +
	  alphax * ((1 - mixlast) * row[x] + mixlast * lastrow[x]);
      }
    }
  }
}
			    
// In a picture carry out the obscuration of boxtrack.
static void obscure_one_box(AVFilterBufferRef *source,
			    AVFilterBufferRef *picref,
			    AVFilterBufferRef *lastref,
			    BoxTrack *boxtrack,
			    int y0, int h, int hsub, int vsub,
			    AVLFG *random) {
  unsigned char *row[4];
  int xb = boxtrack->l, yb = boxtrack->t;
  int hb = boxtrack->b - boxtrack->t;
  int wb = boxtrack->r - boxtrack->l;
  int megapixel_size = 64;  // todo: get from file
  int x, y;
  if (boxtrack->method == redact_blur) {
    const int blur = FFMAX(hb, wb);
    unsigned char *blurbuf = (unsigned char *)av_malloc(blur);
    blur_one_round(picref, boxtrack, y0, h, hsub, vsub, blur, blurbuf, random);
    copybox_mixold_alpha(source, picref,
			 ((lastref==NULL)?source:lastref),
			 boxtrack, hsub, vsub, random);
    av_free(blurbuf);
    return;
  }

  for (y = FFMAX(yb, y0); y < (y0 + h) && y < (yb + hb); y++) {
    row[0] = picref->data[0] + y * picref->linesize[0];

    for (int plane = 1; plane < 3; plane++)
      row[plane] = picref->data[plane] +
	picref->linesize[plane] * (y >> vsub);

    for (x = FFMAX(xb, 0); x < (xb + wb) && x < picref->video->w; x++) {
      double alpha = (double)boxtrack->yuv_color[A] / 255;
      if (boxtrack->method == redact_solid) {
	row[0][x] = (1 - alpha) * row[0][x] +
	  alpha * boxtrack->yuv_color[Y];
	// todo: if hsub is non-zero this will do the same pixel mutliple times.
	// which is wasteful. Wrong if alpha is != 1
	row[1][x >> hsub] = (1 - alpha) * row[1][x >> hsub] +
	  alpha * boxtrack->yuv_color[U];
	row[2][x >> hsub] = (1 - alpha) * row[2][x >> hsub] +
	  alpha * boxtrack->yuv_color[V];
      } else if (boxtrack->method == redact_pixellate) {
	int x_quant = (x / megapixel_size) * megapixel_size;
	int y_quant = (y / megapixel_size) * megapixel_size;
	row[0][x] = (picref->data[0] + y_quant *
		     picref->linesize[0])[x_quant];
	row[1][x >> hsub] = (picref->data[1] + picref->linesize[1] *
	   (y_quant >> vsub))[x_quant >> hsub];
	row[2][x >> hsub] = (picref->data[2] + picref->linesize[2] *
	   (y_quant >> vsub))[x_quant >> hsub];
      }
    }
  }
}

// Set all elements of planes 0,1,2 to val: 128=grey 0=green
static void erase_output2(AVFilterBufferRef *outpic,
			  int y0, int h, int hsub, int vsub,
			  unsigned char val) {
  unsigned char v[] = { 16, 128, 128};
  for (int y = y0; y < (y0 + h); y++) {
    for (int plane = 0; plane < 3; plane++) {
      uint8_t *outrow = outpic->data[plane] +
	outpic->linesize[plane] * (y >> ((plane == 0)?0:vsub));
      const int xmax = outpic->video->w >> ((plane == 0)?0:hsub);
      memset(outrow, v[plane], xmax);
    }  // plane
  }  // y
}

// Copy all the image data from picref to outpic.
static void copy_all(AVFilterBufferRef *picref,
		     AVFilterBufferRef *outpic,
		     int hsub, int vsub) {
  for (int y = 0; y < picref->video->h; y++) {
    for (int plane = 0; plane < 3; plane++) {
      uint8_t *row = picref->data[plane] +
	picref->linesize[plane] * (y >> ((plane == 0)?0:vsub));
      uint8_t *outrow = outpic->data[plane] +
	outpic->linesize[plane] * (y >> ((plane == 0)?0:vsub));
      int xwid = picref->video->w >> ((plane == 0)?0:hsub);
      memcpy(outrow, row, xwid);
    }  // plane
  }  // y
}

// Copy one box from the input to the output.
static void copy_one_box(AVFilterBufferRef *picref,
			 AVFilterBufferRef *outpic,
			 BoxTrack *boxtrack,
			 int y0, int h, int hsub, int vsub) {
  int xb = boxtrack->l;
  int yb = boxtrack->t;
  int hb = boxtrack->b - boxtrack->t;
  int wb = boxtrack->r - boxtrack->l;

  for (int y = FFMAX(yb, y0); y < (y0 + h) && y < (yb + hb); y++) {
    for (int plane = 0; plane < 3; plane++) {
      uint8_t *row = picref->data[plane] +
	picref->linesize[plane] * (y >> ((plane == 0)?0:vsub));
      uint8_t *outrow = outpic->data[plane] +
	outpic->linesize[plane] * (y >> ((plane == 0)?0:vsub));
      int xmin = FFMAX(xb, 0)  >> ((plane == 0)?0:hsub);
      int xmax = FFMIN((xb + wb), picref->video->w)
	>> ((plane == 0)?0:hsub);
      memcpy(outrow + xmin, row + xmin, xmax - xmin);
    }  // plane
  }  // y
}

static void end_frame(AVFilterLink *inlink)
{
  AVFilterContext *ctx = inlink->dst;
  statm_t status;
  RedactionContext *redaction = inlink->dst->priv;
  AVFilterBufferRef *inpic  = inlink->cur_buf;
  AVFilterBufferRef *outpic0 = inlink->dst->outputs[0]->out_buf;
  AVFilterLink *outlink0 = inlink->dst->outputs[0];
  int box = 0;

  copy_all(inpic, outpic0, redaction->hsub, redaction->vsub);
  for (box = redaction->numtracks -1; box >= 0; --box) {
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
      // For output of redaction reversal.
      /* copy_one_box(picref, outpic1, boxtrack, 0, inlink->h, */
      /* 		   redaction->hsub, redaction->vsub); */
    }
  }
  // Now store the redacted video into outpic0.
  for (box = redaction->numtracks -1; box >= 0; --box) {
    BoxTrack *boxtrack = redaction->boxtracks[box];

    if (boxtrack->start > redaction->time_seconds)
      break;
    obscure_one_box(inpic, outpic0, redaction->lastredacted,
		    boxtrack, 0, inlink->h, 
		    redaction->hsub, redaction->vsub,
		    &redaction->random);
  }

  if (logging) {
    read_off_memory_status(&status);
    av_log(ctx, AV_LOG_INFO, "Redaction memory RSS %lu data %lu\n",
	   status.resident, status.data);
  }

  if (redaction->lastredacted != NULL)
    avfilter_unref_buffer(redaction->lastredacted);
  redaction->lastredacted = avfilter_ref_buffer(outlink0->out_buf, ~0);

  avfilter_unref_buffer(inpic);
  avfilter_draw_slice(outlink0, 0, outpic0->video->h, 1);
  avfilter_end_frame(outlink0);
  avfilter_unref_buffer(outpic0);
}

static av_cold void uninit(AVFilterContext *ctx)
{
  RedactionContext *redaction= ctx->priv;
  if (redaction->lastredacted != NULL)
    avfilter_unref_buffer(redaction->lastredacted);
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
  .inputs    = (const AVFilterPad[]) {
    { .name             = "default",
      .type             = AVMEDIA_TYPE_VIDEO,
      .config_props     = config_input,
      .get_video_buffer =
      avfilter_null_get_video_buffer,
      .start_frame      = start_frame,
      .draw_slice       = avfilter_null_draw_slice,
      .end_frame        = end_frame,
      .min_perms        = AV_PERM_WRITE | AV_PERM_READ,
      // .rej_perms        = AV_PERM_PRESERVE
    },
    { .name = NULL}},
  .outputs   = (const AVFilterPad[]) {
    { .name             = "output1",
      .type             = AVMEDIA_TYPE_VIDEO, },
    { .name = NULL}},
};
