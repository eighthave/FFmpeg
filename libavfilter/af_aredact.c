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

/* for lazy char* allocation */
#define BUFLEN 1000

typedef enum {redact_none, redact_mute, redact_noise}
    redaction_method;

typedef struct {
    double start, end;
    redaction_method method;
} BoxTrack;

typedef struct {
    int numtracks;
    double time_seconds;
    BoxTrack **boxtracks;
} RedactionContext;

static BoxTrack *box_track_from_string(const char *track_def,
                                       AVFilterContext *ctx) {
    BoxTrack *boxtrack = NULL;
    int rv = 0;
    char methodbuf[BUFLEN];
    double start, end;

    // Allow comments, empty lines.
    if (track_def[0] == '#' || track_def[0] == '\0')
        return NULL;

    rv = sscanf(track_def, "%lf,%lf,%s", &start,&end,methodbuf);
    if (rv != 3) {
        av_log(ctx, AV_LOG_ERROR, "Failed to parse boxtrack '%s' .\n", track_def);
        return NULL;
    }
    boxtrack = (BoxTrack *)av_malloc(sizeof(BoxTrack));
    boxtrack->start = start;
    boxtrack->end = end;

    if (av_strncasecmp(methodbuf, "mute", 4) == 0)
        boxtrack->method = redact_mute;
    else if (av_strncasecmp(methodbuf, "noise", 5) == 0)
        boxtrack->method = redact_noise;
    else if (av_strncasecmp(methodbuf, "none", 4) == 0)
        boxtrack->method = redact_none;
    else {
        boxtrack->method = redact_mute;
        av_log(ctx, AV_LOG_ERROR, "Unknown audio redaction method '%s', using 'mute' .\n",
               methodbuf);
    }

    return boxtrack;
}


static av_cold int init(AVFilterContext *ctx, const char *args, void *opaque)
{
    RedactionContext *redaction = ctx->priv;
    FILE *file = NULL;
    char buf[BUFLEN];

    redaction->boxtracks = NULL;
    redaction->time_seconds = 0.0;
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
    int i, box;
    BoxTrack *boxtrack;
    redaction_method method = redact_none;

    redaction->time_seconds = redaction->time_seconds + 
        (((double) nb_samples) / inlink->sample_rate);

/* first, figure out what action we are taking. "" means no action, that's
 * the default.  "mute" means zero out the sample, and that overrides any
 * other filter mode.  Otherwise, use the last specified filter type.
 */
    for (box = redaction->numtracks -1; box >= 0; --box) {
        boxtrack = redaction->boxtracks[box];
        // Tracks are sorted by start time, so if this one starts in the future
        // all remaining ones will.
        if (boxtrack->start > redaction->time_seconds)
            goto finished;
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
            method = boxtrack->method;
            if (method == redact_mute)
                break;
        }
    }

    av_log(inlink->dst, AV_LOG_WARNING, "time %f redact %i\n",
           redaction->time_seconds, method);
    if (method != redact_none) {
        switch (insamples->format) {
        case AV_SAMPLE_FMT_U8:
        {
            uint8_t *p = (void *)insamples->data[0];
            if (method == redact_mute)
                for (i = 0; i < nb_samples; i++)
                    *p++ = 0;
            break;
        }
        case AV_SAMPLE_FMT_S16:
        {
            int16_t *p = (void *)insamples->data[0];
            if (method == redact_mute)
                for (i = 0; i < nb_samples; i++)
                    *p++ = 0;
            break;
        }
        case AV_SAMPLE_FMT_S32:
        {
            int32_t *p = (void *)insamples->data[0];
            if (method == redact_mute)
                for (i = 0; i < nb_samples; i++)
                    *p++ = 0;
            break;
        }
        case AV_SAMPLE_FMT_FLT:
        {
            float *p = (void *)insamples->data[0];
            if (method == redact_mute)
                for (i = 0; i < nb_samples; i++)
                    *p++ = 0.0;
            break;
        }
        case AV_SAMPLE_FMT_DBL:
        {
            double *p = (void *)insamples->data[0];
            if (method == redact_mute)
                for (i = 0; i < nb_samples; i++)
                    *p++ = 0.0;
            break;
        }
        }
    }
finished:
    avfilter_filter_samples(outlink, insamples);
}

    static av_cold void uninit(AVFilterContext *ctx)
    {
        RedactionContext *redaction= ctx->priv;
        for (int i = 0; i < redaction->numtracks; ++i) {
            av_free(redaction->boxtracks[i]);
        }
        av_free(redaction->boxtracks);
    }

AVFilter avfilter_af_aredact = {
    .name           = "aredact",
    .description    = NULL_IF_CONFIG_SMALL("Redact the input audio according to a track file."),
    .query_formats  = query_formats,
    .priv_size      = sizeof(RedactionContext),
    .init           = init,
    .uninit         = uninit,

    .inputs  = (const AVFilterPad[])  {{ .name     = "default",
                                   .type           = AVMEDIA_TYPE_AUDIO,
                                   .filter_samples = filter_samples,
                                   .min_perms      = AV_PERM_READ|AV_PERM_WRITE},
                                 { .name = NULL}},

    .outputs = (const AVFilterPad[])  {{ .name     = "default",
                                   .type           = AVMEDIA_TYPE_AUDIO, },
                                 { .name = NULL}},
};
