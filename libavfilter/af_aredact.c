/*
 * Copyright (c) 2011 Hans-Christoph Steiner
 * Copyright (c) 2011 Stefano Sabatini
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * audio redaction filter
 * based on af_volume.c and vf_redact.c code
 */

#include "libavutil/avstring.h"
#include "libavutil/audioconvert.h"
#include "libavutil/eval.h"
#include "avfilter.h"

enum { Y, U, V, A };

typedef enum {redact_mute, redact_noise}
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
    boxtrack->method = redact_mute;

    if (av_strncasecmp(method, "noise", 5) == 0)
        boxtrack->method = redact_noise;
    else
        av_log(ctx, AV_LOG_ERROR, "Unknown audio redaction method '%s', using 'mute' .\n",
               method);

    av_log(ctx, AV_LOG_WARNING, "ROAR!!!!! %s .\n", method);

    return boxtrack;
}


static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    RedactionContext *redaction = ctx->priv;
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
    AVFilterFormats *formats = NULL;
    enum AVSampleFormat sample_fmts[] = {
        AV_SAMPLE_FMT_U8,
        AV_SAMPLE_FMT_S16,
        AV_SAMPLE_FMT_S32,
        AV_SAMPLE_FMT_FLT,
        AV_SAMPLE_FMT_DBL,
        AV_SAMPLE_FMT_NONE
    };
    int packing_fmts[] = { AVFILTER_PACKED, -1 };

    formats = avfilter_make_all_channel_layouts();
    if (!formats)
        return AVERROR(ENOMEM);
    avfilter_set_common_channel_layouts(ctx, formats);

    formats = avfilter_make_format_list(sample_fmts);
    if (!formats)
        return AVERROR(ENOMEM);
    avfilter_set_common_sample_formats(ctx, formats);

    formats = avfilter_make_format_list(packing_fmts);
    if (!formats)
        return AVERROR(ENOMEM);
    avfilter_set_common_packing_formats(ctx, formats);

    return 0;
}

static void filter_samples(AVFilterLink *inlink, AVFilterBufferRef *insamples)
{
    RedactionContext *redaction = inlink->dst->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];
    const int nb_samples = insamples->audio->nb_samples *
        av_get_channel_layout_nb_channels(insamples->audio->channel_layout);
    int i;

    switch (insamples->format) {
    case AV_SAMPLE_FMT_U8:
    {
        uint8_t *p = (void *)insamples->data[0];
        for (i = 0; i < nb_samples; i++) {
            *p++ = 0;
        }
        break;
    }
    case AV_SAMPLE_FMT_S16:
    {
        int16_t *p = (void *)insamples->data[0];
        for (i = 0; i < nb_samples; i++) {
            *p++ = 0;
        }
        break;
    }
    case AV_SAMPLE_FMT_S32:
    {
        int32_t *p = (void *)insamples->data[0];
        for (i = 0; i < nb_samples; i++) {
            *p++ = 0;
        }
        break;
    }
    case AV_SAMPLE_FMT_FLT:
    {
        float *p = (void *)insamples->data[0];
        for (i = 0; i < nb_samples; i++) {
            *p++ = 0.0;
        }
        break;
    }
    case AV_SAMPLE_FMT_DBL:
    {
        double *p = (void *)insamples->data[0];
        for (i = 0; i < nb_samples; i++) {
            *p++ = 0.0;
        }
        break;
    }
    }
    avfilter_filter_samples(outlink, insamples);
}

AVFilter avfilter_af_aredact = {
    .name           = "aredact",
    .description    = NULL_IF_CONFIG_SMALL("Redact the input audio according to a track file."),
    .query_formats  = query_formats,
    .priv_size      = sizeof(RedactionContext),
    .init           = init,

    .inputs  = (const AVFilterPad[])  {{ .name     = "default",
                                   .type           = AVMEDIA_TYPE_AUDIO,
                                   .filter_samples = filter_samples,
                                   .min_perms      = AV_PERM_READ|AV_PERM_WRITE},
                                 { .name = NULL}},

    .outputs = (const AVFilterPad[])  {{ .name     = "default",
                                   .type           = AVMEDIA_TYPE_AUDIO, },
                                 { .name = NULL}},
};
