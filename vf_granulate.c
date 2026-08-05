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
 * Time-Domain Video Granulator: Granulate past frames in current frame
 *
 */

#include "avfilter.h"
#include "libavutil/attributes.h"
#include "libavutil/frame.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/lfg.h"
#include "libavutil/pixdesc.h"
#include "libavutil/random_seed.h"
#include "formats.h"
#include "video.h"
#include "error.h"
#include <limits.h>
#include <stdint.h>
#include <string.h>


typedef enum FilterMode {
    MODE_PIXELS,
    MODE_INTERLACED_H,
    MODE_INTERLACED_V,
    MODE_DITHER
} filter_mode;

typedef enum GhostingMode {
    NO_GHOSTING,
    LUMA_GHOSTING,
    CHROMA_GHOSTING
} ghosting_mode;

typedef void (*copy_grain)(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, 
                            int w, int h, filter_mode mode, ghosting_mode ghosting, int zoom, int var_size, 
                            int PxFmt, uint8_t log2_chroma_h, uint8_t log2_chroma_w, AVLFG *lfg);

typedef struct GrainPos{
    int pos_x;
    int pos_y;
    int g_pos_x;
    int g_pos_y;
} GrainPos;

typedef struct GranulateContext {
    const AVClass *class;

    int PixFmt;
    AVLFG *lfg;
    uint32_t seed;
    filter_mode mode;
    ghosting_mode ghosting;
    unsigned int buffer_size, buffer_index, buffer_full;
    AVFrame **fbuffer;
    unsigned int zoom_amount, zoom_set;
    unsigned int zoom_offset_w, zoom_offset_h;
    unsigned int offset_time;
    copy_grain copy_grain_fn;
    unsigned int grain_w, grain_h;
    int fullscreen;
    unsigned int n_grains;
    int static_grains;
    GrainPos *grain_pos;
    int grains_set;
    unsigned int reset_time;
    int var_size;
    uint64_t frame_count;
    uint8_t log2_chroma_h, log2_chroma_w;
    unsigned int delay;
    unsigned int delay_set;
} GranulateContext;

#define OFFSET(x) offsetof(GranulateContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
#define R AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption granulate_options[] = {
    {"mode", "set mode", OFFSET(mode), AV_OPT_TYPE_INT, {.i64=MODE_PIXELS}, MODE_PIXELS, MODE_DITHER, FLAGS | R},
    {"zoom", "set zoom amount", OFFSET(zoom_amount), AV_OPT_TYPE_UINT, {.i64=1}, 1, 256, FLAGS | R},
    {"offset_time", "set number of frames befor zoom offset is reset", OFFSET(offset_time), AV_OPT_TYPE_UINT, {.i64=0}, 0, UINT_MAX, FLAGS | R},
    {"n_grains", "number of grains per frame", OFFSET(n_grains), AV_OPT_TYPE_UINT, {.i64=0}, 0, UINT_MAX, FLAGS},
    {"buffer", "set the size of the buffer", OFFSET(buffer_size), AV_OPT_TYPE_UINT, {.i64=1}, 1, 8192, FLAGS},
    {"grain_w", "set the width of each grain in px", OFFSET(grain_w), AV_OPT_TYPE_UINT, {.i64=0}, 0, 8192, FLAGS},
    {"grain_h", "set the height of each grain in px", OFFSET(grain_h), AV_OPT_TYPE_UINT, {.i64=0}, 0, 8192, FLAGS},
    {"var_size", "toggle random grain size (grain_size as max size)", OFFSET(var_size), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS},
    {"ghosting", "select type of ghosting", OFFSET(ghosting), AV_OPT_TYPE_INT, {.i64=NO_GHOSTING}, NO_GHOSTING, CHROMA_GHOSTING, FLAGS | R},
    {"static_grains", "toggle stable grain position", OFFSET(static_grains), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS},
    {"reset_time","set number of frames before grain_pos reset", OFFSET(reset_time), AV_OPT_TYPE_UINT, {.i64=0}, 0, UINT_MAX, FLAGS},
    {"delay", "set number of frames before refresh of delay", OFFSET(delay), AV_OPT_TYPE_UINT, {.i64=0}, 0, UINT_MAX, FLAGS | R},
    {"seed", "set seed for AVlfg", OFFSET(seed), AV_OPT_TYPE_UINT, {.i64=0}, 0, UINT32_MAX, FLAGS},
    { NULL }
};

AVFILTER_DEFINE_CLASS(granulate);

static av_cold int init(AVFilterContext *ctx)
{
    int i;
    GranulateContext *granulate_ctx = ctx->priv;

    granulate_ctx->lfg = av_calloc(1, sizeof(AVLFG));
    if (!granulate_ctx->lfg)
        return AVERROR(ENOMEM);
    
    if (!granulate_ctx->seed)
        granulate_ctx->seed = av_get_random_seed();
    av_lfg_init(granulate_ctx->lfg, granulate_ctx->seed);

    if (granulate_ctx->static_grains) {
        granulate_ctx->grain_pos = av_calloc(granulate_ctx->n_grains, sizeof(GrainPos));

        if (!granulate_ctx->grain_pos)
            return AVERROR(ENOMEM);
    }

    granulate_ctx->fbuffer = av_calloc(granulate_ctx->buffer_size, sizeof(AVFrame *));

    if (!granulate_ctx->fbuffer)
        return AVERROR(ENOMEM);

    for (i = 0; i < granulate_ctx->buffer_size; i++) {
        granulate_ctx->fbuffer[i] = av_frame_alloc();

        if (!granulate_ctx->fbuffer[i])
            return AVERROR(ENOMEM);
    }
    granulate_ctx->buffer_index = 0;
    granulate_ctx->zoom_offset_w = 0;
    granulate_ctx->zoom_offset_h = 0;
    granulate_ctx->frame_count = 0;
    granulate_ctx->zoom_set = 0;
    granulate_ctx->buffer_full = 0;
    granulate_ctx->delay_set = 0;
    granulate_ctx->fullscreen = 1;

    return 0;
}

static int query_formats(const AVFilterContext *ctx, AVFilterFormatsConfig **cfg_in, AVFilterFormatsConfig **cfg_out)
{
    static const enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV444P, 
                                                AV_PIX_FMT_GRAY8, AV_PIX_FMT_RGB24, AV_PIX_FMT_BGR24, AV_PIX_FMT_NONE};

    return ff_set_pixel_formats_from_list2(ctx, cfg_in, cfg_out, pix_fmts);
}

static int granulate_process_command(AVFilterContext *ctx, const char *cmd, const char *arg, char *res, int res_len, int flags)
{
    av_log(ctx, AV_LOG_INFO, "Received command: %s=%s\n", cmd, arg);
    return ff_filter_process_command(ctx, cmd, arg, res, res_len, flags);
}

static void copy_grain_YUV(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int grain_w, int grain_h, filter_mode mode, ghosting_mode ghosting, int zoom, int var_size, int PxFmt, uint8_t log2_chroma_h, uint8_t log2_chroma_w, AVLFG *lfg);

static void copy_grain_GRAY(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int grain_w, int grain_h, filter_mode mode, ghosting_mode ghosting, int zoom, int var_size, int PxFmt, uint8_t log2_chroma_h, uint8_t log2_chroma_w, AVLFG *lfg);

static void copy_grain_RGB(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int grain_w, int grain_h, filter_mode mode, ghosting_mode ghosting, int zoom, int var_size, int PxFmt, uint8_t log2_chroma_h, uint8_t log2_chroma_w, AVLFG *lfg);

static int config_props(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    GranulateContext *granulate_ctx = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);

    granulate_ctx->log2_chroma_h = desc->log2_chroma_h;
    granulate_ctx->log2_chroma_w = desc->log2_chroma_w;

    switch (inlink->format) {
        case (AV_PIX_FMT_YUV420P):
            granulate_ctx->PixFmt = AV_PIX_FMT_YUV420P; granulate_ctx->copy_grain_fn = copy_grain_YUV; break;
        case (AV_PIX_FMT_YUV422P):
            granulate_ctx->PixFmt = AV_PIX_FMT_YUV422P; granulate_ctx->copy_grain_fn = copy_grain_YUV; break;
        case (AV_PIX_FMT_YUV444P):
            granulate_ctx->PixFmt = AV_PIX_FMT_YUV444P; granulate_ctx->copy_grain_fn = copy_grain_YUV; break;
        case (AV_PIX_FMT_GRAY8):
            granulate_ctx->PixFmt = AV_PIX_FMT_GRAY8; granulate_ctx->copy_grain_fn = copy_grain_GRAY; break;
        case (AV_PIX_FMT_RGB24):
            granulate_ctx->PixFmt = AV_PIX_FMT_RGB24; granulate_ctx->copy_grain_fn = copy_grain_RGB; break;
        case (AV_PIX_FMT_BGR24):
            granulate_ctx->PixFmt = AV_PIX_FMT_BGR24; granulate_ctx->copy_grain_fn = copy_grain_RGB; break;
        default: 
            return AVERROR(EINVAL);
    }

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

    return 0;
}

static av_always_inline void copy_px_data0(const AVFrame *dst, const AVFrame *src, int d_row_offset, int s_row_offset, int d_col_offset,int s_col_offset) {
    dst->data[0][d_row_offset * dst->linesize[0] + d_col_offset] = src->data[0][s_row_offset * src->linesize[0] + s_col_offset];
}

static av_always_inline void copy_px_data1(const AVFrame *dst, const AVFrame *src, int d_row_offset, int s_row_offset, int d_col_offset,int s_col_offset) {
    dst->data[1][d_row_offset * dst->linesize[1] + d_col_offset] = src->data[1][s_row_offset * src->linesize[1] + s_col_offset];
}

static av_always_inline void copy_px_data2(const AVFrame *dst, const AVFrame *src, int d_row_offset, int s_row_offset, int d_col_offset,int s_col_offset) {
    dst->data[2][d_row_offset * dst->linesize[2] + d_col_offset] = src->data[2][s_row_offset * src->linesize[2] + s_col_offset];
}


static void copy_grain_YUV(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy,
                            int grain_w, int grain_h, filter_mode mode, ghosting_mode ghosting, 
                            int zoom, int var_size, int PxFmt, uint8_t log2_chroma_h, uint8_t log2_chroma_w, AVLFG *lfg)
{
    if (var_size) {
        grain_h = av_lfg_get(lfg) % (grain_h + 1);
        grain_w = av_lfg_get(lfg) % (grain_w + 1);
    }
    int grain_w_chroma;
    int grain_h_chroma;
    int sx_chroma, sy_chroma;
    int dx_chroma, dy_chroma;

    if (PxFmt == AV_PIX_FMT_YUV420P) {
        grain_w_chroma = AV_CEIL_RSHIFT(grain_w, log2_chroma_w);
        grain_h_chroma = AV_CEIL_RSHIFT(grain_h, log2_chroma_h);
        sx_chroma = sx >> 1;
        sy_chroma = sy >> 1;
        dx_chroma = dx >> 1;
        dy_chroma = dy >> 1;
    }
    

    if (PxFmt == AV_PIX_FMT_YUV422P) {
        grain_w_chroma = AV_CEIL_RSHIFT(grain_w, log2_chroma_w);
        grain_h_chroma = grain_h;
        sx_chroma = sx >> 1;
        sy_chroma = sy;
        dx_chroma = dx >> 1;
        dy_chroma = dy;
    }

    if (PxFmt == AV_PIX_FMT_YUV444P) {
        grain_w_chroma = grain_w;
        grain_h_chroma = grain_h;
        sx_chroma = sx;
        sy_chroma = sy;
        dx_chroma = dx;
        dy_chroma = dy;
    }

    int row_step = 1;
    int col_step = 1;

    if (mode == MODE_INTERLACED_H)
        row_step = 2;

    if (mode == MODE_INTERLACED_V)
        col_step = 2;
    
    int d_row_offset, d_col_offset;
    int s_row_offset, s_col_offset;

    if (ghosting != 2) {
        for (int row = 0; row < grain_h; row += row_step) {
            d_row_offset = dy + row;
            s_row_offset = sy + row / zoom;
            for (int col = 0; col < grain_w; col += col_step) {
                d_col_offset = dx + col;
                s_col_offset = sx + col / zoom;
                if (mode != MODE_DITHER) {
                    copy_px_data0(dst, src, d_row_offset, s_row_offset, d_col_offset, s_col_offset);
                }
                else {
                    if (av_lfg_get(lfg) & 1)
                        copy_px_data0(dst, src, d_row_offset, s_row_offset, d_col_offset, s_col_offset);
                }
            }
        }
    }
    if (ghosting != 1) {
        for (int row = 0; row < grain_h_chroma; row += row_step) {
            d_row_offset = dy_chroma + row;
            s_row_offset = sy_chroma + row / zoom;
            for (int col = 0; col < grain_w_chroma; col += col_step) {
                d_col_offset = dx_chroma + col;
                s_col_offset = sx_chroma + col / zoom;
                if (mode != MODE_DITHER) {
                    copy_px_data1(dst, src, d_row_offset, s_row_offset, d_col_offset, s_col_offset);
                    copy_px_data2(dst, src, d_row_offset, s_row_offset, d_col_offset, s_col_offset);
                }
                else {
                    if (av_lfg_get(lfg) & 1) {
                        copy_px_data1(dst, src, d_row_offset, s_row_offset, d_col_offset, s_col_offset);
                        copy_px_data2(dst, src, d_row_offset, s_row_offset, d_col_offset, s_col_offset);
                    }
                }
            }
        }
    }
}

static void copy_grain_GRAY(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, 
                            int grain_w, int grain_h, filter_mode mode, ghosting_mode ghosting, 
                            int zoom, int var_size, int PxFmt, uint8_t log2_chroma_h, uint8_t log2_chroma_w, AVLFG *lfg)
{
    if (var_size) {
        grain_h = av_lfg_get(lfg) % (grain_h + 1);
        grain_w = av_lfg_get(lfg) % (grain_w + 1);
    }

    int row_step = 1;
    int col_step = 1;

    if (mode == MODE_INTERLACED_H)
        row_step = 2;

    if (mode == MODE_INTERLACED_V)
        col_step = 2;
    
    int d_row_offset, d_col_offset;
    int s_row_offset, s_col_offset;

    for (int row = 0; row < grain_h; row += row_step) {
        d_row_offset = dy + row;
        s_row_offset = sy + row / zoom;
        for (int col = 0; col < grain_w; col += col_step) {
            d_col_offset = dx + col;
            s_col_offset = sx + col / zoom;
            if (mode != MODE_DITHER) {
                copy_px_data0(dst, src, d_row_offset, s_row_offset, d_col_offset, s_col_offset);
            }
            else {
                if (av_lfg_get(lfg) & 1)
                    copy_px_data0(dst, src, d_row_offset, s_row_offset, d_col_offset, s_col_offset);
            }
        }
    }

}

static void copy_grain_RGB(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy,
                            int grain_w, int grain_h, filter_mode mode, ghosting_mode ghosting, 
                            int zoom, int var_size, int PxFmt, uint8_t log2_chroma_h, uint8_t log2_chroma_w, AVLFG *lfg)
{
    if (var_size) {
        grain_h = av_lfg_get(lfg) % (grain_h + 1);
        grain_w = av_lfg_get(lfg) % (grain_w + 1);
    }

    int row_step = 1;
    int col_step = 1;

    if (mode == MODE_INTERLACED_H)
        row_step = 2;

    if (mode == MODE_INTERLACED_V)
        col_step = 2;
    
    int d_row_offset, s_row_offset;
    int d1_col_offset, d2_col_offset, d3_col_offset;
    int s1_col_offset, s2_col_offset, s3_col_offset;

    for (int row = 0; row < grain_h; row += row_step) {
        d_row_offset = dy + row;
        s_row_offset = sy + row / zoom;
        for (int col = 0; col < grain_w; col += col_step) {
            d1_col_offset = dx + col * 3;
            s1_col_offset = sx + col / zoom * 3;
            d2_col_offset = dx + col * 3 + 1;
            s2_col_offset = sx + col / zoom * 3 + 1;
            d3_col_offset = dx + col * 3 + 2;
            s3_col_offset = sx + col / zoom * 3 + 2;
            if (mode != MODE_DITHER) {
                if (!ghosting) {
                    copy_px_data0(dst, src, d_row_offset, s_row_offset, d1_col_offset, s1_col_offset);
                    copy_px_data0(dst, src, d_row_offset, s_row_offset, d2_col_offset, s2_col_offset);
                    copy_px_data0(dst, src, d_row_offset, s_row_offset, d3_col_offset, s3_col_offset);
                }
                else {
                    uint32_t r = av_lfg_get(lfg);
                    if (r & 2)
                        copy_px_data0(dst, src, d_row_offset, s_row_offset, d1_col_offset, s1_col_offset);
                    if (r & 4)
                        copy_px_data0(dst, src, d_row_offset, s_row_offset, d2_col_offset, s2_col_offset);
                    if (r & 8)
                        copy_px_data0(dst, src, d_row_offset, s_row_offset, d3_col_offset, s3_col_offset);
                }
            }
            else {
                uint32_t r = av_lfg_get(lfg);
                if (r & 1) {
                    if (!ghosting) {
                        copy_px_data0(dst, src, d_row_offset, s_row_offset, d1_col_offset, s1_col_offset);
                        copy_px_data0(dst, src, d_row_offset, s_row_offset, d2_col_offset, s2_col_offset);
                        copy_px_data0(dst, src, d_row_offset, s_row_offset, d3_col_offset, s3_col_offset);
                        }
                    else {
                        if (r & 2)
                            copy_px_data0(dst, src, d_row_offset, s_row_offset, d1_col_offset, s1_col_offset);
                        if (r & 4)
                            copy_px_data0(dst, src, d_row_offset, s_row_offset, d2_col_offset, s2_col_offset);
                        if (r & 8)
                            copy_px_data0(dst, src, d_row_offset, s_row_offset, d3_col_offset, s3_col_offset);
                    }
                }
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
    int g_src = 0;

    if (ctx->delay_set) {
        g_src = (ctx->delay_set + ctx->frame_count) % ctx->buffer_size;
        src_f = src[g_src];
    }
    
    for (int grain_count = 0; grain_count < n_grains; grain_count++) {
        if (!ctx->delay_set) {
            if (ctx->buffer_full) {
                g_src = av_lfg_get(ctx->lfg) % ctx->buffer_size;
                src_f = src[g_src];
            }
            else {
                g_src = av_lfg_get(ctx->lfg) % (ctx->buffer_index + 1);
                src_f = src[g_src];
            }
        }
        int sx = av_lfg_get(ctx->lfg) % (width - grain_w + 1);
        int sy = av_lfg_get(ctx->lfg) % (height - grain_h + 1);
        int dx = av_lfg_get(ctx->lfg) % (width - grain_w + 1);
        int dy = av_lfg_get(ctx->lfg) % (height - grain_h + 1);

        ctx->copy_grain_fn(dst, src_f, sx + offset_w, sy + offset_h, dx, dy, grain_w, grain_h, ctx->mode, ctx->ghosting, ctx->zoom_amount, ctx->var_size, ctx->PixFmt, ctx->log2_chroma_h, ctx->log2_chroma_w, ctx->lfg);
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
    int g_src = 0;

    if (ctx->delay_set) {
        g_src = (ctx->delay_set + ctx->frame_count) % ctx->buffer_size;
        src_f = src[g_src];
    }

    for (int grain_count = 0; grain_count < n_grains; grain_count++) {
        if (!ctx->delay_set) {
            if (ctx->buffer_full) {
                g_src = av_lfg_get(ctx->lfg) % ctx->buffer_size;
                src_f = src[g_src];
            }
            else {
                g_src = av_lfg_get(ctx->lfg) % (ctx->buffer_index + 1);
                src_f = src[g_src];
            }
        }
        ctx->copy_grain_fn(dst, src_f, grain_pos[grain_count].g_pos_x + offset_w, grain_pos[grain_count].g_pos_y + offset_h, grain_pos[grain_count].pos_x, grain_pos[grain_count].pos_y, grain_w, grain_h, ctx->mode, ctx->ghosting, ctx->zoom_amount, ctx->var_size, ctx->PixFmt, ctx->log2_chroma_h, ctx->log2_chroma_w, ctx->lfg);
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
        int sx = av_lfg_get(ctx->lfg) % (width - grain_w + 1);
        int sy = av_lfg_get(ctx->lfg) % (height - grain_h + 1);
        int dx = av_lfg_get(ctx->lfg) % (width - grain_w + 1);
        int dy = av_lfg_get(ctx->lfg) % (height - grain_h + 1);

        ctx->copy_grain_fn(dst, ctx->fbuffer[0], sx + offset_w, sy + offset_h, dx, dy, grain_w, grain_h, ctx->mode, ctx->ghosting, ctx->zoom_amount, ctx->var_size, ctx->PixFmt, ctx->log2_chroma_h, ctx->log2_chroma_w, ctx->lfg);
    }
}

static void init_granulate_pos(const GranulateContext *ctx, int width, int height)
{

    int grain_w = ctx->grain_w;
    int grain_h = ctx->grain_h;
    int n_grains = ctx->n_grains;
    GrainPos *grain_pos = ctx->grain_pos;

    for (int grain_count = 0; grain_count < n_grains; grain_count++) {
        grain_pos[grain_count].g_pos_x = av_lfg_get(ctx->lfg) % (width - grain_w + 1);
        grain_pos[grain_count].g_pos_y = av_lfg_get(ctx->lfg) % (height - grain_h + 1);
        grain_pos[grain_count].pos_x = av_lfg_get(ctx->lfg) % (width - grain_w + 1);
        grain_pos[grain_count].pos_y = av_lfg_get(ctx->lfg) % (height - grain_h + 1);
    }
}

static void set_offset(GranulateContext *ctx) {
    ctx->zoom_offset_w = av_lfg_get(ctx->lfg) % (ctx->grain_w - (ctx->grain_w / ctx->zoom_amount));
    ctx->zoom_offset_h = av_lfg_get(ctx->lfg) % (ctx->grain_h - (ctx->grain_h / ctx->zoom_amount));
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

    if (!granulate_ctx->n_grains)
        goto filter_end;

    int width = in->width;
    int height = in->height;

    if (granulate_ctx->grain_h && granulate_ctx->grain_w)
        granulate_ctx->fullscreen = 0;

    if (!granulate_ctx->fullscreen) {
        if (granulate_ctx->grain_w > width)
            granulate_ctx->grain_w = width;
        if (granulate_ctx->grain_h > height)
            granulate_ctx->grain_h = height;
    } else {
        granulate_ctx->grain_w = width;
        granulate_ctx->grain_h = height;
    }

    if (!granulate_ctx->delay)
        granulate_ctx->delay_set = 0;

    if (granulate_ctx->zoom_amount == 1) {
        granulate_ctx->zoom_set = 0;
        granulate_ctx->zoom_offset_w = 0;
        granulate_ctx->zoom_offset_h = 0;
    }
    
    if (granulate_ctx->zoom_set && granulate_ctx->zoom_amount > 1) {
        if (granulate_ctx->zoom_offset_w >= (granulate_ctx->grain_w - (granulate_ctx->grain_w / granulate_ctx->zoom_amount)) || granulate_ctx->zoom_offset_h >= (granulate_ctx->grain_h - (granulate_ctx->grain_h / granulate_ctx->zoom_amount)))
            set_offset(granulate_ctx);
        if (granulate_ctx->offset_time) {
            if (!(granulate_ctx->frame_count % granulate_ctx->offset_time))
                set_offset(granulate_ctx);
        }
    }

    if (!granulate_ctx->zoom_set && granulate_ctx->zoom_amount > 1) {
        granulate_ctx->zoom_set = 1;
        set_offset(granulate_ctx);
    }

    if (granulate_ctx->buffer_size > 1) {

        AVFrame *buf = granulate_ctx->fbuffer[granulate_ctx->buffer_index];
        ret = av_frame_copy(buf, in);
        if (ret < 0)
            return ret;

        if (granulate_ctx->delay && granulate_ctx->buffer_full) {
            if (!(granulate_ctx->frame_count % granulate_ctx->delay))
                granulate_ctx->delay_set = 1 + (av_lfg_get(granulate_ctx->lfg) % (granulate_ctx->buffer_size - 1));
        }
        
        if (granulate_ctx->static_grains) {
            if (!granulate_ctx->grains_set) {
                init_granulate_pos(granulate_ctx, width, height);
                granulate_ctx->grains_set = 1;
            }
            else if (granulate_ctx->reset_time && !(granulate_ctx->frame_count % granulate_ctx->reset_time)) {
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
        AVFrame *buf = granulate_ctx->fbuffer[0];
        ret = av_frame_copy(buf, in);
        if (ret < 0)
            return ret;
        granulate_in_frame(granulate_ctx, out, width, height);
    }
filter_end:
    granulate_ctx->frame_count++;


    return ff_filter_frame(outlink, out);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    int i;
    GranulateContext *granulate_ctx = ctx->priv;

    for (i = 0; i < granulate_ctx->buffer_size; i++)
        av_frame_free(&granulate_ctx->fbuffer[i]);

    if (granulate_ctx->static_grains)
        av_freep(&granulate_ctx->grain_pos);

    av_freep(&granulate_ctx->fbuffer);
    av_freep(&granulate_ctx->lfg);
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
