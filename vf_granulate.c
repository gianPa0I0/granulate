/*
 * Copyright (c) 2026 Gian Paolo Gigante
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
 * Granulate past frames in current frame
 *
 */

#include "avfilter.h"
#include "libavutil/frame.h"
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/lfg.h"
#include "formats.h"
#include "video.h"
#include "error.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


typedef enum FilterMode {
    MODE_PIXELS,
    MODE_INTERLACED_H,
    MODE_INTERLACED_V,
    MODE_DITHER
} filter_mode;

typedef void (*copy_grain)(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int w, int h, filter_mode mode, int zoom, int var_size);

typedef struct GrainPos{
    int pos_x;
    int pos_y;
    int g_pos_x;
    int g_pos_y;
} GrainPos;

typedef struct GranulateContext {
    const AVClass *class;
    AVLFG prng;
    int mode;
    int buffer_size, buffer_index, buffer_full;
    AVFrame **fbuffer;
    int zoom_amount, zoom_set;
    int zoom_offset_w, zoom_offset_h;
    int zoom_offset_time;
    copy_grain copy_grain_fn;
    int grain_w, grain_h;
    int n_grains;
    int static_grains;
    int chroma_grain;
    int luma_grain;
    GrainPos *grain_pos;
    int grains_set;
    int grains_reset_time;
    int var_size;
    int64_t frame_count;
} GranulateContext;

#define OFFSET(x) offsetof(GranulateContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
#define R AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption granulate_options[] = {
    { "mode", "set mode", OFFSET(mode), AV_OPT_TYPE_UINT, {.i64=MODE_PIXELS}, 0, MODE_DITHER, FLAGS},
    { "zoom", "set zoom amount", OFFSET(zoom_amount), AV_OPT_TYPE_UINT, {.i64=1}, 1, 256, FLAGS | R},
    {"zoom_offset_time", "set number of frames befor zoom offset is reset", OFFSET(zoom_offset_time), AV_OPT_TYPE_UINT, {.i64=0}, 0, INT64_MAX, FLAGS | R},
    { "buffer", "set the size of the buffer", OFFSET(buffer_size), AV_OPT_TYPE_UINT, {.i64=0}, 0, 8192, FLAGS},
    {"grain_w", "set the width of each grain in px", OFFSET(grain_w), AV_OPT_TYPE_UINT, {.i64=16}, 1, 8192, FLAGS},
    {"grain_h", "set the height of each grain in px", OFFSET(grain_h), AV_OPT_TYPE_UINT, {.i64=16}, 1, 8192, FLAGS},
    {"var_size", "toggle random grain size (grain_size as max size)", OFFSET(var_size), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS},
    {"n_grains", "number of grains per frame", OFFSET(n_grains), AV_OPT_TYPE_UINT, {.i64=0}, 0, INT64_MAX, FLAGS},
    {"chroma_grain", "toggle ghosting, chroma", OFFSET(chroma_grain), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS},
    {"luma_grain", "toggle ghosting, luma", OFFSET(luma_grain), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS},
    {"static_grains", "toggle stable grain position", OFFSET(static_grains), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS | R},
    {"grains_reset_time","set number of frames before grain_pos reset", OFFSET(grains_reset_time), AV_OPT_TYPE_UINT, {.i64=0}, 0, INT64_MAX, FLAGS | R},
    { NULL }
};

AVFILTER_DEFINE_CLASS(granulate);

static av_cold int init(AVFilterContext *ctx)
{
    int i;
    GranulateContext *granulate_ctx = ctx->priv;

    if (granulate_ctx->static_grains) {
            granulate_ctx->grain_pos = av_calloc(granulate_ctx->n_grains, sizeof(GrainPos));

            if (!granulate_ctx->grain_pos)
                return AVERROR(ENOMEM);
    }


    if (granulate_ctx->buffer_size) {
        
        granulate_ctx->fbuffer = av_calloc(granulate_ctx->buffer_size, sizeof(AVFrame *));

        if (!granulate_ctx->fbuffer)
            return AVERROR(ENOMEM);

        for (i = 0; i < granulate_ctx->buffer_size; i++) {
            granulate_ctx->fbuffer[i] = av_frame_alloc();
        }
        granulate_ctx->buffer_index = 0;
    }

    granulate_ctx->zoom_offset_w = 0;
    granulate_ctx->zoom_offset_h = 0;
    granulate_ctx->frame_count = 0;
    granulate_ctx->zoom_set = 0;
    granulate_ctx->buffer_full = 0;

    return 0;
}

static int query_formats(const AVFilterContext *ctx, AVFilterFormatsConfig **cfg_in, AVFilterFormatsConfig **cfg_out)
{
    const GranulateContext *granulate_ctx = ctx->priv;
    static const enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV444P, AV_PIX_FMT_GRAY8, AV_PIX_FMT_NONE};

    return ff_set_pixel_formats_from_list2(ctx, cfg_in, cfg_out, pix_fmts);
}

static int granulate_process_command(AVFilterContext *ctx, const char *cmd, const char *arg, char *res, int res_len, int flags)
{
    return ff_filter_process_command(ctx, cmd, arg, res, res_len, flags);
}

static void copy_grain_YUV420(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int grain_w, int grain_h, filter_mode mode, int zoom, int var_size);

static void copy_grain_YUV422(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int grain_w, int grain_h, filter_mode mode, int zoom, int var_size);

static void copy_grain_YUV444(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int grain_w, int grain_h, filter_mode mode, int zoom, int var_size);

static void copy_grain_GRAY(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int grain_w, int grain_h, filter_mode mode, int zoom, int var_size);

static void copy_grain_Y(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int grain_w, int grain_h, filter_mode mode, int zoom, int var_size);

static void copy_grain_UV444(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int grain_w, int grain_h, filter_mode mode, int zoom, int var_size);

static void copy_grain_UV422(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int grain_w, int grain_h, filter_mode mode, int zoom, int var_size);

static void copy_grain_UV420(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int grain_w, int grain_h, filter_mode mode, int zoom, int var_size);

static int config_props(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    GranulateContext *granulate_ctx = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);

    if (!granulate_ctx->chroma_grain && !granulate_ctx->luma_grain) {
        switch (inlink->format) {
            case AV_PIX_FMT_YUV420P: granulate_ctx->copy_grain_fn = copy_grain_YUV420; break;
            case AV_PIX_FMT_YUV422P: granulate_ctx->copy_grain_fn = copy_grain_YUV422; break;
            case AV_PIX_FMT_YUV444P: granulate_ctx->copy_grain_fn = copy_grain_YUV444; break;
            case AV_PIX_FMT_GRAY8: granulate_ctx->copy_grain_fn = copy_grain_GRAY; break;
            default: return AVERROR(EINVAL);
        }
    
    } else if (granulate_ctx->chroma_grain) {
        switch (inlink->format) {
            case AV_PIX_FMT_YUV420P: granulate_ctx->copy_grain_fn = copy_grain_UV420; break;
            case AV_PIX_FMT_YUV422P: granulate_ctx->copy_grain_fn = copy_grain_UV422; break;
            case AV_PIX_FMT_YUV444P: granulate_ctx->copy_grain_fn = copy_grain_UV444; break;
            case AV_PIX_FMT_GRAY8: granulate_ctx->copy_grain_fn = copy_grain_GRAY; break;
            default: return AVERROR(EINVAL);
        }

    } else {
        switch (inlink->format) {
            case AV_PIX_FMT_YUV420P: granulate_ctx->copy_grain_fn = copy_grain_Y; break;
            case AV_PIX_FMT_YUV422P: granulate_ctx->copy_grain_fn = copy_grain_Y; break;
            case AV_PIX_FMT_YUV444P: granulate_ctx->copy_grain_fn = copy_grain_Y; break;
            case AV_PIX_FMT_GRAY8: granulate_ctx->copy_grain_fn = copy_grain_GRAY; break;
            default: return AVERROR(EINVAL);
        }
    }

    if (granulate_ctx->buffer_size) {
        for (int i = 0; i < granulate_ctx->buffer_size; i++) {
            AVFrame *f = granulate_ctx->fbuffer[i];

            av_frame_unref(f);
            f->format = inlink->format;
            f->width  = inlink->w;
            f->height = inlink->h;

            int ret = av_frame_get_buffer(f, 0);
            if (ret < 0)
                return ret;
        }
    }

    return 0;
}

static void copy_grain_YUV420(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int grain_w, int grain_h, filter_mode mode, int zoom, int var_size)
{
    if (var_size) {
        grain_h = rand() % (grain_h + 1);
        grain_w = rand() % (grain_w + 1);
    }
    if (mode == MODE_PIXELS) {
        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                dst->data[0][(dy+row)*dst->linesize[0]+(dx+col)] = src->data[0][(sy+row/zoom)*src->linesize[0]+(sx+col/zoom)];
            }
        }

        for (int row = 0; row < grain_h/2; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                dst->data[1][(dy/2+row)*dst->linesize[1]+(dx/2+col)] = src->data[1][(sy/2+row/zoom)*src->linesize[1]+(sx/2+col/zoom)];
            }
        }

        for (int row = 0; row < grain_h/2; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                dst->data[2][(dy/2+row)*dst->linesize[2]+(dx/2+col)] = src->data[2][(sy/2+row/zoom)*src->linesize[2]+(sx/2+col/zoom)];
            }
        }
    }

    if (mode == MODE_INTERLACED_H) {
        for (int row = 0; row < grain_h; row++) {
            if (!(row % 2)) {
                for (int col = 0; col < grain_w; col++) {
                    dst->data[0][(dy+row)*dst->linesize[0]+(dx+col)] = src->data[0][(sy+row/zoom)*src->linesize[0]+(sx+col/zoom)];
                }
            }
        }

        for (int row = 0; row < grain_h/2; row++) {
            if (!(row % 2)) {
                for (int col = 0; col < grain_w/2; col++) {
                    dst->data[1][(dy/2+row)*dst->linesize[1]+(dx/2+col)] = src->data[1][(sy/2+row/zoom)*src->linesize[1]+(sx/2+col/zoom)];
                }
            }
        }

        for (int row = 0; row < grain_h/2; row++) {
            if (!(row % 2)) {
                for (int col = 0; col < grain_w/2; col++) {
                    dst->data[2][(dy/2+row)*dst->linesize[2]+(dx/2+col)] = src->data[2][(sy/2+row/zoom)*src->linesize[2]+(sx/2+col/zoom)];
                }
            }
        }
    }

    if (mode == MODE_INTERLACED_V) {
        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                if (!(col % 2)) {
                    dst->data[0][(dy+row)*dst->linesize[0]+(dx+col)] = src->data[0][(sy+row/zoom)*src->linesize[0]+(sx+col/zoom)];
                }
            }
        }

        for (int row = 0; row < grain_h/2; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                if (!(col % 2)) {
                    dst->data[1][(dy/2+row)*dst->linesize[1]+(dx/2+col)] = src->data[1][(sy/2+row/zoom)*src->linesize[1]+(sx/2+col/zoom)];
                }
            }
        }

        for (int row = 0; row < grain_h/2; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                if (!(col % 2)) {
                    dst->data[2][(dy/2+row)*dst->linesize[2]+(dx/2+col)] = src->data[2][(sy/2+row/zoom)*src->linesize[2]+(sx/2+col/zoom)];
                }
            }
        }
    }

    if (mode == MODE_DITHER) {
        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                if (rand() % 2)
                    dst->data[0][(dy+row)*dst->linesize[0]+(dx+col)] = src->data[0][(sy+row/zoom)*src->linesize[0]+(sx+col/zoom)];
            }
        }

        for (int row = 0; row < grain_h/2; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                if (rand()% 2)
                    dst->data[1][(dy/2+row)*dst->linesize[1]+(dx/2+col)] = src->data[1][(sy/2+row/zoom)*src->linesize[1]+(sx/2+col/zoom)];
            }
        }

        for (int row = 0; row < grain_h/2; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                if (rand() % 2)
                    dst->data[2][(dy/2+row)*dst->linesize[2]+(dx/2+col)] = src->data[2][(sy/2+row/zoom)*src->linesize[2]+(sx/2+col/zoom)];
            }
        }
    }
}

static void copy_grain_YUV422(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int grain_w, int grain_h, filter_mode mode, int zoom, int var_size)
{
    if (var_size) {
        grain_h = rand() % (grain_h + 1);
        grain_w = rand() % (grain_w + 1);
    }
    if (mode == MODE_PIXELS) {
        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                dst->data[0][(dy+row)*dst->linesize[0]+(dx+col)] = src->data[0][(sy+row/zoom)*src->linesize[0]+(sx+col/zoom)];
            }
        }

        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                dst->data[1][(dy+row)*dst->linesize[1]+(dx/2+col)] = src->data[1][(sy+row/zoom)*src->linesize[1]+(sx/2+col/zoom)];
            }
        }

        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                dst->data[2][(dy+row)*dst->linesize[2]+(dx/2+col)] = src->data[2][(sy+row/zoom)*src->linesize[2]+(sx/2+col/zoom)];
            }
        }
    }

    if (mode == MODE_INTERLACED_H) {
        for (int row = 0; row < grain_h; row++) {
            if (!(row % 2)) {
                for (int col = 0; col < grain_w; col++) {
                    dst->data[0][(dy+row)*dst->linesize[0]+(dx+col)] = src->data[0][(sy+row/zoom)*src->linesize[0]+(sx+col/zoom)];
                }
            }
        }

        for (int row = 0; row < grain_h; row++) {
            if (!(row % 2)) {
                for (int col = 0; col < grain_w/2; col++) {
                    dst->data[1][(dy+row)*dst->linesize[1]+(dx/2+col)] = src->data[1][(sy+row/zoom)*src->linesize[1]+(sx/2+col/zoom)];
                }
            }
        }

        for (int row = 0; row < grain_h; row++) {
            if (!(row % 2)) {
                for (int col = 0; col < grain_w/2; col++) {
                    dst->data[2][(dy+row)*dst->linesize[2]+(dx/2+col)] = src->data[2][(sy+row/zoom)*src->linesize[2]+(sx/2+col/zoom)];
                }
            }
        }
    }

    if (mode == MODE_INTERLACED_V) {
        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                if (!(col % 2)) {
                    dst->data[0][(dy+row)*dst->linesize[0]+(dx+col)] = src->data[0][(sy+row/zoom)*src->linesize[0]+(sx+col/zoom)];
                }
            }
        }

        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                if (!(col % 2)) {
                    dst->data[1][(dy+row)*dst->linesize[1]+(dx/2+col)] = src->data[1][(sy+row/zoom)*src->linesize[1]+(sx/2+col/zoom)];
                }
            }
        }

        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                if (!(col % 2)) {
                    dst->data[2][(dy+row)*dst->linesize[2]+(dx/2+col)] = src->data[2][(sy+row/zoom)*src->linesize[2]+(sx/2+col/zoom)];
                }
            }
        }
    }

    if (mode == MODE_DITHER) {
        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                if (rand() % 2)
                    dst->data[0][(dy+row)*dst->linesize[0]+(dx+col)] = src->data[0][(sy+row/zoom)*src->linesize[0]+(sx+col/zoom)];
            }
        }

        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                if (rand()% 2)
                    dst->data[1][(dy+row)*dst->linesize[1]+(dx/2+col)] = src->data[1][(sy+row/zoom)*src->linesize[1]+(sx/2+col/zoom)];
            }
        }

        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                if (rand() % 2)
                    dst->data[2][(dy+row)*dst->linesize[2]+(dx/2+col)] = src->data[2][(sy+row/zoom)*src->linesize[2]+(sx/2+col/zoom)];
            }
        }
    }
}

static void copy_grain_YUV444(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int grain_w, int grain_h, filter_mode mode, int zoom, int var_size)
{
    if (var_size) {
        grain_h = rand() % (grain_h + 1);
        grain_w = rand() % (grain_w + 1);
    }
    if (mode == MODE_PIXELS) {
        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                dst->data[0][(dy+row)*dst->linesize[0]+(dx+col)] = src->data[0][(sy+row/zoom)*src->linesize[0]+(sx+col/zoom)];
            }
        }

        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                dst->data[1][(dy+row)*dst->linesize[1]+(dx+col)] = src->data[1][(sy+row/zoom)*src->linesize[1]+(sx+col/zoom)];
            }
        }

        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                dst->data[2][(dy+row)*dst->linesize[2]+(dx+col)] = src->data[2][(sy+row/zoom)*src->linesize[2]+(sx+col/zoom)];
            }
        }
    }

    if (mode == MODE_INTERLACED_H) {
        for (int row = 0; row < grain_h; row++) {
            if (!(row % 2)) {
                for (int col = 0; col < grain_w; col++) {
                    dst->data[0][(dy+row)*dst->linesize[0]+(dx+col)] = src->data[0][(sy+row/zoom)*src->linesize[0]+(sx+col/zoom)];
                }
            }
        }

        for (int row = 0; row < grain_h; row++) {
            if (!(row % 2)) {
                for (int col = 0; col < grain_w; col++) {
                    dst->data[1][(dy+row)*dst->linesize[1]+(dx+col)] = src->data[1][(sy+row/zoom)*src->linesize[1]+(sx+col/zoom)];
                }
            }
        }

        for (int row = 0; row < grain_h; row++) {
            if (!(row % 2)) {
                for (int col = 0; col < grain_w; col++) {
                    dst->data[2][(dy+row)*dst->linesize[2]+(dx+col)] = src->data[2][(sy+row/zoom)*src->linesize[2]+(sx+col/zoom)];
                }
            }
        }
    }

    if (mode == MODE_INTERLACED_V) {
        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                if (!(col % 2)) {
                    dst->data[0][(dy+row)*dst->linesize[0]+(dx+col)] = src->data[0][(sy+row/zoom)*src->linesize[0]+(sx+col/zoom)];
                }
            }
        }

        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                if (!(col % 2)) {
                    dst->data[1][(dy+row)*dst->linesize[1]+(dx+col)] = src->data[1][(sy+row/zoom)*src->linesize[1]+(sx+col/zoom)];
                }
            }
        }

        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                if (!(col % 2)) {
                    dst->data[2][(dy+row)*dst->linesize[2]+(dx+col)] = src->data[2][(sy+row/zoom)*src->linesize[2]+(sx+col/zoom)];
                }
            }
        }
    }

    if (mode == MODE_DITHER) {
        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                if (rand() % 2)
                    dst->data[0][(dy+row)*dst->linesize[0]+(dx+col)] = src->data[0][(sy+row/zoom)*src->linesize[0]+(sx+col/zoom)];
            }
        }

        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                if (rand()% 2)
                    dst->data[1][(dy+row)*dst->linesize[1]+(dx+col)] = src->data[1][(sy+row/zoom)*src->linesize[1]+(sx+col/zoom)];
            }
        }

        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                if (rand() % 2)
                    dst->data[2][(dy+row)*dst->linesize[2]+(dx+col)] = src->data[2][(sy+row/zoom)*src->linesize[2]+(sx+col/zoom)];
            }
        }
    }
}

static void copy_grain_GRAY(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int grain_w, int grain_h, filter_mode mode, int zoom, int var_size)
{

}

static void copy_grain_Y(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int grain_w, int grain_h, filter_mode mode, int zoom, int var_size)
{
    if (var_size) {
        grain_h = rand() % (grain_h + 1);
        grain_w = rand() % (grain_w + 1);
    }
    if (mode == MODE_PIXELS) {
        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                dst->data[0][(dy+row)*dst->linesize[0]+(dx+col)] = src->data[0][(sy+row/zoom)*src->linesize[0]+(sx+col/zoom)];
            }
        }
    }

    if (mode == MODE_INTERLACED_H) {
        for (int row = 0; row < grain_h; row++) {
            if (!(row % 2)) {
                for (int col = 0; col < grain_w; col++) {
                    dst->data[0][(dy+row)*dst->linesize[0]+(dx+col)] = src->data[0][(sy+row/zoom)*src->linesize[0]+(sx+col/zoom)];
                }
            }
        }
    }

    if (mode == MODE_INTERLACED_V) {
        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                if (!(col % 2)) {
                    dst->data[0][(dy+row)*dst->linesize[0]+(dx+col)] = src->data[0][(sy+row/zoom)*src->linesize[0]+(sx+col/zoom)];
                }
            }
        }
    }

    if (mode == MODE_DITHER) {
        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                if (rand() % 2)
                    dst->data[0][(dy+row)*dst->linesize[0]+(dx+col)] = src->data[0][(sy+row/zoom)*src->linesize[0]+(sx+col/zoom)];
            }
        }
    }
}

static void copy_grain_UV444(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int grain_w, int grain_h, filter_mode mode, int zoom, int var_size)
{
    if (var_size) {
        grain_h = rand() % (grain_h + 1);
        grain_w = rand() % (grain_w + 1);
    }
    if (mode == MODE_PIXELS) {
        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                dst->data[1][(dy+row)*dst->linesize[1]+(dx+col)] = src->data[1][(sy+row/zoom)*src->linesize[1]+(sx+col/zoom)];
            }
        }

        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                dst->data[2][(dy+row)*dst->linesize[2]+(dx+col)] = src->data[2][(sy+row/zoom)*src->linesize[2]+(sx+col/zoom)];
            }
        }
    }

    if (mode == MODE_INTERLACED_H) {
        for (int row = 0; row < grain_h; row++) {
            if (!(row % 2)) {
                for (int col = 0; col < grain_w; col++) {
                    dst->data[1][(dy+row)*dst->linesize[1]+(dx+col)] = src->data[1][(sy+row/zoom)*src->linesize[1]+(sx+col/zoom)];
                }
            }
        }

        for (int row = 0; row < grain_h; row++) {
            if (!(row % 2)) {
                for (int col = 0; col < grain_w; col++) {
                    dst->data[2][(dy+row)*dst->linesize[2]+(dx+col)] = src->data[2][(sy+row/zoom)*src->linesize[2]+(sx+col/zoom)];
                }
            }
        }
    }

    if (mode == MODE_INTERLACED_V) {
        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                if (!(col % 2)) {
                    dst->data[1][(dy+row)*dst->linesize[1]+(dx+col)] = src->data[1][(sy+row/zoom)*src->linesize[1]+(sx+col/zoom)];
                }
            }
        }

        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                if (!(col % 2)) {
                    dst->data[2][(dy+row)*dst->linesize[2]+(dx+col)] = src->data[2][(sy+row/zoom)*src->linesize[2]+(sx+col/zoom)];
                }
            }
        }
    }

    if (mode == MODE_DITHER) {
        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                if (rand()% 2)
                    dst->data[1][(dy+row)*dst->linesize[1]+(dx+col)] = src->data[1][(sy+row/zoom)*src->linesize[1]+(sx+col/zoom)];
            }
        }

        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                if (rand() % 2)
                    dst->data[2][(dy+row)*dst->linesize[2]+(dx+col)] = src->data[2][(sy+row/zoom)*src->linesize[2]+(sx+col/zoom)];
            }
        }
    }
}

static void copy_grain_UV422(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int grain_w, int grain_h, filter_mode mode, int zoom, int var_size)
{
    if (var_size) {
        grain_h = rand() % (grain_h + 1);
        grain_w = rand() % (grain_w + 1);
    }
    if (mode == MODE_PIXELS) {
        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                dst->data[1][(dy+row)*dst->linesize[1]+(dx/2+col)] = src->data[1][(sy+row/zoom)*src->linesize[1]+(sx/2+col/zoom)];
            }
        }

        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                dst->data[2][(dy+row)*dst->linesize[2]+(dx/2+col)] = src->data[2][(sy+row/zoom)*src->linesize[2]+(sx/2+col/zoom)];
            }
        }
    }

    if (mode == MODE_INTERLACED_H) {
        for (int row = 0; row < grain_h; row++) {
            if (!(row % 2)) {
                for (int col = 0; col < grain_w/2; col++) {
                    dst->data[1][(dy+row)*dst->linesize[1]+(dx/2+col)] = src->data[1][(sy+row/zoom)*src->linesize[1]+(sx/2+col/zoom)];
                }
            }
        }

        for (int row = 0; row < grain_h; row++) {
            if (!(row % 2)) {
                for (int col = 0; col < grain_w/2; col++) {
                    dst->data[2][(dy+row)*dst->linesize[2]+(dx/2+col)] = src->data[2][(sy+row/zoom)*src->linesize[2]+(sx/2+col/zoom)];
                }
            }
        }
    }

    if (mode == MODE_INTERLACED_V) {
        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                if (!(col % 2)) {
                    dst->data[1][(dy+row)*dst->linesize[1]+(dx/2+col)] = src->data[1][(sy+row/zoom)*src->linesize[1]+(sx/2+col/zoom)];
                }
            }
        }

        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                if (!(col % 2)) {
                    dst->data[2][(dy+row)*dst->linesize[2]+(dx/2+col)] = src->data[2][(sy+row/zoom)*src->linesize[2]+(sx/2+col/zoom)];
                }
            }
        }
    }

    if (mode == MODE_DITHER) {
        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                if (rand()% 2)
                    dst->data[1][(dy+row)*dst->linesize[1]+(dx/2+col)] = src->data[1][(sy+row/zoom)*src->linesize[1]+(sx/2+col/zoom)];
            }
        }

        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                if (rand() % 2)
                    dst->data[2][(dy+row)*dst->linesize[2]+(dx/2+col)] = src->data[2][(sy+row/zoom)*src->linesize[2]+(sx/2+col/zoom)];
            }
        }
    }
}

static void copy_grain_UV420(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int grain_w, int grain_h, filter_mode mode, int zoom, int var_size)
{
    if (var_size) {
        grain_h = rand() % (grain_h + 1);
        grain_w = rand() % (grain_w + 1);
    }
    if (mode == MODE_PIXELS) {
        for (int row = 0; row < grain_h/2; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                dst->data[1][(dy/2+row)*dst->linesize[1]+(dx/2+col)] = src->data[1][(sy/2+row/zoom)*src->linesize[1]+(sx/2+col/zoom)];
            }
        }

        for (int row = 0; row < grain_h/2; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                dst->data[2][(dy/2+row)*dst->linesize[2]+(dx/2+col)] = src->data[2][(sy/2+row/zoom)*src->linesize[2]+(sx/2+col/zoom)];
            }
        }
    }

    if (mode == MODE_INTERLACED_H) {
        for (int row = 0; row < grain_h/2; row++) {
            if (!(row % 2)) {
                for (int col = 0; col < grain_w/2; col++) {
                    dst->data[1][(dy/2+row)*dst->linesize[1]+(dx/2+col)] = src->data[1][(sy/2+row/zoom)*src->linesize[1]+(sx/2+col/zoom)];
                }
            }
        }

        for (int row = 0; row < grain_h/2; row++) {
            if (!(row % 2)) {
                for (int col = 0; col < grain_w/2; col++) {
                    dst->data[2][(dy/2+row)*dst->linesize[2]+(dx/2+col)] = src->data[2][(sy/2+row/zoom)*src->linesize[2]+(sx/2+col/zoom)];
                }
            }
        }
    }

    if (mode == MODE_INTERLACED_V) {
        for (int row = 0; row < grain_h/2; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                if (!(col % 2)) {
                    dst->data[1][(dy/2+row)*dst->linesize[1]+(dx/2+col)] = src->data[1][(sy/2+row/zoom)*src->linesize[1]+(sx/2+col/zoom)];
                }
            }
        }

        for (int row = 0; row < grain_h/2; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                if (!(col % 2)) {
                    dst->data[2][(dy/2+row)*dst->linesize[2]+(dx/2+col)] = src->data[2][(sy/2+row/zoom)*src->linesize[2]+(sx/2+col/zoom)];
                }
            }
        }
    }

    if (mode == MODE_DITHER) {
        for (int row = 0; row < grain_h/2; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                if (rand()% 2)
                    dst->data[1][(dy/2+row)*dst->linesize[1]+(dx/2+col)] = src->data[1][(sy/2+row/zoom)*src->linesize[1]+(sx/2+col/zoom)];
            }
        }

        for (int row = 0; row < grain_h/2; row++) {
            for (int col = 0; col < grain_w/2; col++) {
                if (rand() % 2)
                    dst->data[2][(dy/2+row)*dst->linesize[2]+(dx/2+col)] = src->data[2][(sy/2+row/zoom)*src->linesize[2]+(sx/2+col/zoom)];
            }
        }
    }
}


static void granulate_rand(const GranulateContext *ctx, AVFrame *dst, AVFrame **src, int width, int height) 
{
    AVFrame *src_f = NULL;

    int grain_w = ctx->grain_w;
    int grain_h = ctx->grain_h;
    int n_grains = ctx->n_grains;
    int offset_w = ctx->zoom_offset_w;
    int offset_h = ctx->zoom_offset_h;
    
    for (int grain_count = 0; grain_count < n_grains; grain_count++) {
        if (ctx->buffer_full) {
            int r_src = rand() % ctx->buffer_size;
            src_f = src[r_src];
        }
        else if (ctx->buffer_index > 0) {
            int r_src = rand() % ctx->buffer_index;
            src_f = src[r_src];
        }
        else {
            src_f = dst;
        }
        int sx = rand() % (width - grain_w + 1);
        int sy = rand() % (height - grain_h + 1);
        int dx = rand() % (width - grain_w + 1);
        int dy = rand() % (height - grain_h + 1);

        ctx->copy_grain_fn(dst, src_f, sx + offset_w, sy + offset_h, dx, dy, grain_w, grain_h, ctx->mode, ctx->zoom_amount, ctx->var_size);
    }
}

static void granulate_pos(const GranulateContext *ctx, AVFrame *dst, AVFrame **src, int width, int height)
{
    AVFrame *src_f = NULL;

    int grain_w = ctx->grain_w;
    int grain_h = ctx->grain_h;
    int n_grains = ctx->n_grains;
    int offset_w = ctx->zoom_offset_w;
    int offset_h = ctx->zoom_offset_h;
    GrainPos *grain_pos = ctx->grain_pos;

    for (int grain_count = 0; grain_count < n_grains; grain_count++) {
        if (ctx->buffer_full) {
            int r_src = rand() % ctx->buffer_size;
            src_f = src[r_src];
        }
        else if (ctx->buffer_index > 0) {
            int r_src = rand() % ctx->buffer_index + 1;
            src_f = src[r_src];
        }
        else {
            src_f = dst;
        }
        ctx->copy_grain_fn(dst, src_f, grain_pos[grain_count].g_pos_x + offset_w, grain_pos[grain_count].g_pos_y + offset_h, grain_pos[grain_count].pos_x, grain_pos[grain_count].pos_y, grain_w, grain_h, ctx->mode, ctx->zoom_amount, ctx->var_size);
    }
}

static void granulate_in_frame(const GranulateContext *ctx, AVFrame *dst, int width, int height) 
{

    int grain_w = ctx->grain_w;
    int grain_h = ctx->grain_h;
    int n_grains = ctx->n_grains;
    int offset_w = ctx->zoom_offset_w;
    int offset_h = ctx->zoom_offset_h;

    for (int grain_count = 0; grain_count < n_grains; grain_count++) {
        int sx = rand() % (width - grain_w + 1);
        int sy = rand() % (height - grain_h + 1);
        int dx = rand() % (width - grain_w + 1);
        int dy = rand() % (height - grain_h + 1);

        ctx->copy_grain_fn(dst, dst, sx + offset_w, sy + offset_h, dx, dy, grain_w, grain_h, ctx->mode, ctx->zoom_amount, ctx->var_size);
    }
}

static void init_granulate_pos(const GranulateContext *ctx, int width, int height)
{

    int grain_w = ctx->grain_w;
    int grain_h = ctx->grain_h;
    int n_grains = ctx->n_grains;
    GrainPos *grain_pos = ctx->grain_pos;


    for (int grain_count = 0; grain_count < n_grains; grain_count++) {
        grain_pos[grain_count].g_pos_x = rand() % (width - grain_w + 1);
        grain_pos[grain_count].g_pos_y = rand() % (height - grain_h + 1);
        grain_pos[grain_count].pos_x = rand() % (width - grain_w + 1);
        grain_pos[grain_count].pos_y = rand() % (height - grain_h + 1);
    }
}


static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    GranulateContext *granulate_ctx = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out = NULL;
    int ret;

    ret = ff_inlink_make_frame_writable(inlink, &in);
    if (ret < 0)
        return ret;

    out = in;

    int width = in->width;
    int height = in->height;

    if (granulate_ctx->grain_w > width)
        granulate_ctx->grain_w = width;
    if (granulate_ctx->grain_h > height)
        granulate_ctx->grain_h = height;

    if (granulate_ctx->zoom_amount == 1) {
        granulate_ctx->zoom_set = 0;
        granulate_ctx->zoom_offset_w = 0;
        granulate_ctx->zoom_offset_h = 0;
    }
    
    if (granulate_ctx->zoom_set && granulate_ctx->zoom_amount > 1)
        if (granulate_ctx->zoom_offset_w >= (granulate_ctx->grain_w - (granulate_ctx->grain_w / granulate_ctx->zoom_amount)) || granulate_ctx->zoom_offset_h >= (granulate_ctx->grain_h - (granulate_ctx->grain_h / granulate_ctx->zoom_amount)))
            goto set_offset;

    if (granulate_ctx->zoom_set && granulate_ctx->zoom_offset_time && granulate_ctx->zoom_amount > 1) {
        if (!(granulate_ctx->frame_count % granulate_ctx->zoom_offset_time))
            goto set_offset;        
    }

    if (!granulate_ctx->zoom_set && granulate_ctx->zoom_amount > 1) {
        granulate_ctx->zoom_set = 1;
        set_offset:
            granulate_ctx->zoom_offset_w = rand() % (granulate_ctx->grain_w - (granulate_ctx->grain_w / granulate_ctx->zoom_amount));
            granulate_ctx->zoom_offset_h = rand() % (granulate_ctx->grain_h - (granulate_ctx->grain_h / granulate_ctx->zoom_amount));
    }

    if (granulate_ctx->buffer_size) {
        AVFrame *buf = granulate_ctx->fbuffer[granulate_ctx->buffer_index];
        ret = av_frame_copy(buf, in);
        if (ret < 0)
            return ret;
        av_frame_copy_props(buf, in);
        
        if (granulate_ctx->static_grains) {
            if (!granulate_ctx->grains_set) {
                init_granulate_pos(granulate_ctx, width, height);
                granulate_ctx->grains_set = 1;
            }
            else if (granulate_ctx->grains_reset_time && !(granulate_ctx->frame_count % granulate_ctx->grains_reset_time)) {
                init_granulate_pos(granulate_ctx, width, height);
                granulate_pos(granulate_ctx, out, granulate_ctx->fbuffer, width, height);
            }
            else {
                granulate_pos(granulate_ctx, out, granulate_ctx->fbuffer, width, height);
            }
        }
        else {
            granulate_rand(granulate_ctx, out, granulate_ctx->fbuffer, width, height);
        }

        granulate_ctx->buffer_index = (granulate_ctx->buffer_index + 1) % granulate_ctx->buffer_size;
        
        if (!granulate_ctx->buffer_index)
            granulate_ctx->buffer_full = 1;
    }

    else {
        granulate_in_frame(granulate_ctx, out, width, height);
    }
    
    granulate_ctx->frame_count++;

    return ff_filter_frame(outlink, out);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    int i;
    GranulateContext *granulate = ctx->priv;

    if (granulate->buffer_size) {
        for (i = 0; i < granulate->buffer_size; i++)
            av_frame_free(&granulate->fbuffer[i]);
        }

    if (granulate->static_grains)
        av_freep(&granulate->grain_pos);

    av_freep(&granulate->fbuffer);
}

static const AVFilterPad granulate_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
        .filter_frame = filter_frame,
    },
};

const FFFilter ff_vf_granulate = {
    .p.name        = "granulate",
    .p.description = NULL_IF_CONFIG_SMALL("Granulate past frames in current frame"),
    .p.priv_class  = &granulate_class,
    .p.flags       = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    .priv_size     = sizeof(GranulateContext),
    .init          = init,
    .uninit        = uninit,
    .process_command = granulate_process_command,
    FILTER_INPUTS(granulate_inputs),
    FILTER_OUTPUTS(ff_video_default_filterpad),
    FILTER_QUERY_FUNC2(query_formats),
};
