/*
 * Copyright (c) 2026 Gian Paolo Gigante
 */

/**
 * @file
 * Granulate past frames in current frame
 *
 */

#include "avfilter.h"
#include "libavutil/frame.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/lfg.h"
#include "libavutil/pixdesc.h"
#include "libavutil/random_seed.h"
#include "formats.h"
#include "video.h"
#include "error.h"
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
    filter_mode mode;
    ghosting_mode ghosting;
    int buffer_size, buffer_index, buffer_full;
    AVFrame **fbuffer;
    int zoom_amount, zoom_set;
    int zoom_offset_w, zoom_offset_h;
    int zoom_offset_time;
    copy_grain copy_grain_fn;
    int grain_w, grain_h;
    int fullscreen;
    int n_grains;
    int static_grains;
    GrainPos *grain_pos;
    int grains_set;
    int grains_reset_time;
    int var_size;
    uint64_t frame_count;
    uint8_t log2_chroma_h, log2_chroma_w;
    int delay;
    int delay_set;
} GranulateContext;

#define OFFSET(x) offsetof(GranulateContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
#define R AV_OPT_FLAG_RUNTIME_PARAM

static const AVOption granulate_options[] = {
    { "mode", "set mode", OFFSET(mode), AV_OPT_TYPE_UINT, {.i64=MODE_PIXELS}, MODE_PIXELS, MODE_DITHER, FLAGS},
    { "zoom", "set zoom amount", OFFSET(zoom_amount), AV_OPT_TYPE_UINT, {.i64=1}, 1, 256, FLAGS | R},
    {"zoom_offset_time", "set number of frames befor zoom offset is reset", OFFSET(zoom_offset_time), AV_OPT_TYPE_UINT, {.i64=0}, 0, INT64_MAX, FLAGS | R},
    { "buffer", "set the size of the buffer", OFFSET(buffer_size), AV_OPT_TYPE_UINT, {.i64=1}, 1, 8192, FLAGS},
    {"grain_w", "set the width of each grain in px", OFFSET(grain_w), AV_OPT_TYPE_UINT, {.i64=0}, 0, 8192, FLAGS},
    {"grain_h", "set the height of each grain in px", OFFSET(grain_h), AV_OPT_TYPE_UINT, {.i64=0}, 0, 8192, FLAGS},
    {"fullscreen", "set grain size equal to frame size", OFFSET(fullscreen), AV_OPT_TYPE_BOOL, {.i64=1}, 0, 1, FLAGS},
    {"var_size", "toggle random grain size (grain_size as max size)", OFFSET(var_size), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS},
    {"n_grains", "number of grains per frame", OFFSET(n_grains), AV_OPT_TYPE_UINT, {.i64=0}, 0, INT64_MAX, FLAGS},
    {"ghosting", "select type of ghosting", OFFSET(ghosting), AV_OPT_TYPE_INT, {.i64=NO_GHOSTING}, NO_GHOSTING, CHROMA_GHOSTING, FLAGS | R},
    {"static_grains", "toggle stable grain position", OFFSET(static_grains), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS},
    {"grains_reset_time","set number of frames before grain_pos reset", OFFSET(grains_reset_time), AV_OPT_TYPE_UINT, {.i64=0}, 0, INT64_MAX, FLAGS | R},
    {"delay", "set number of frames before refresh of delay", OFFSET(delay), AV_OPT_TYPE_UINT, {.i64=0}, 0, 8192, FLAGS | R},
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
    
    uint32_t seed = av_get_random_seed();
    av_lfg_init(granulate_ctx->lfg, seed);

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
    }
    granulate_ctx->buffer_index = 0;

    granulate_ctx->zoom_offset_w = 0;
    granulate_ctx->zoom_offset_h = 0;
    granulate_ctx->frame_count = 0;
    granulate_ctx->zoom_set = 0;
    granulate_ctx->buffer_full = 0;
    granulate_ctx->delay_set = 0;

    return 0;
}

static int query_formats(const AVFilterContext *ctx, AVFilterFormatsConfig **cfg_in, AVFilterFormatsConfig **cfg_out)
{
    static const enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV444P, AV_PIX_FMT_GRAY8, AV_PIX_FMT_NONE};

    return ff_set_pixel_formats_from_list2(ctx, cfg_in, cfg_out, pix_fmts);
}

static int granulate_process_command(AVFilterContext *ctx, const char *cmd, const char *arg, char *res, int res_len, int flags)
{
    return ff_filter_process_command(ctx, cmd, arg, res, res_len, flags);
}

static void copy_grain_YUV(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int grain_w, int grain_h, filter_mode mode, ghosting_mode ghosting, int zoom, int var_size, int PxFmt, uint8_t log2_chroma_h, uint8_t log2_chroma_w, AVLFG *lfg);

static void copy_grain_GRAY(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int grain_w, int grain_h, filter_mode mode, ghosting_mode ghosting, int zoom, int var_size, int PxFmt, uint8_t log2_chroma_h, uint8_t log2_chroma_w, AVLFG *lfg);

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
            default: return AVERROR(EINVAL);
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

static av_always_inline void copy_px_Y(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int zoom, int col, int row) {
    dst->data[0][(dy + row) * dst->linesize[0] + (dx + col)] = src->data[0][(sy + row / zoom) * src->linesize[0] + (sx + col / zoom)];
}

static av_always_inline void copy_px_U(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int zoom, int col, int row) {
    dst->data[1][(dy + row) * dst->linesize[1] + (dx + col)] = src->data[1][(sy + row / zoom) * src->linesize[1] + (sx + col / zoom)];
}

static av_always_inline void copy_px_V(const AVFrame *dst, const AVFrame *src, int sx, int sy, int dx, int dy, int zoom, int col, int row) {
    dst->data[2][(dy + row) * dst->linesize[2] + (dx + col)] = src->data[2][(sy + row / zoom) * src->linesize[2] + (sx + col / zoom)];
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

    if (mode == MODE_PIXELS) {
        if (ghosting != 2) {
            for (int row = 0; row < grain_h; row++) {
                for (int col = 0; col < grain_w; col++) {
                    copy_px_Y(dst, src, sx, sy, dx, dy, zoom, col, row);
                }
            }
        }

        if (ghosting != 1) {
            for (int row = 0; row < grain_h_chroma; row++) {
                for (int col = 0; col < grain_w_chroma; col++) {
                    copy_px_U(dst, src, sx_chroma, sy_chroma, dx_chroma, dy_chroma, zoom, col, row);
                }
            }

            for (int row = 0; row < grain_h_chroma; row++) {
                for (int col = 0; col < grain_w_chroma; col++) {
                    copy_px_V(dst, src, sx_chroma, sy_chroma, dx_chroma, dy_chroma, zoom, col, row);
                }
            }
        }
    }

    if (mode == MODE_INTERLACED_H) {
        if (ghosting != 2) {
            for (int row = 0; row < grain_h; row++) {
                if (!(row % 2)) {
                    for (int col = 0; col < grain_w; col++) {
                        copy_px_Y(dst, src, sx, sy, dx, dy, zoom, col, row);
                    }
                }
            }
        }

        if (ghosting != 1) {
            for (int row = 0; row < grain_h_chroma; row++) {
                if (!(row % 2)) {
                    for (int col = 0; col < grain_w_chroma; col++) {
                        copy_px_U(dst, src, sx_chroma, sy_chroma, dx_chroma, dy_chroma, zoom, col, row);
                    }
                }
            }

            for (int row = 0; row < grain_h_chroma; row++) {
                if (!(row % 2)) {
                    for (int col = 0; col < grain_w_chroma; col++) {
                        copy_px_V(dst, src, sx_chroma, sy_chroma, dx_chroma, dy_chroma, zoom, col, row);
                    }
                }
            }
        }
    }

    if (mode == MODE_INTERLACED_V) {
        if (ghosting != 2) {
            for (int row = 0; row < grain_h; row++) {
                for (int col = 0; col < grain_w; col++) {
                    if (!(col % 2)) {
                        copy_px_Y(dst, src, sx, sy, dx, dy, zoom, col, row);
                    }
                }
            }
        }

        if (ghosting != 1) {
            for (int row = 0; row < grain_h_chroma; row++) {
                for (int col = 0; col < grain_w_chroma; col++) {
                    if (!(col % 2)) {
                        copy_px_U(dst, src, sx_chroma, sy_chroma, dx_chroma, dy_chroma, zoom, col, row);
                    }
                }
            }

            for (int row = 0; row < grain_h_chroma; row++) {
                for (int col = 0; col < grain_w_chroma; col++) {
                    if (!(col % 2)) {
                        copy_px_V(dst, src, sx_chroma, sy_chroma, dx_chroma, dy_chroma, zoom, col, row);
                    }
                }
            }
        }
    }

    if (mode == MODE_DITHER) {
        if (ghosting != 2) {
            for (int row = 0; row < grain_h; row++) {
                for (int col = 0; col < grain_w; col++) {
                    if (av_lfg_get(lfg) % 2)
                        copy_px_Y(dst, src, sx, sy, dx, dy, zoom, col, row);
                }
            }
        }

        if (ghosting != 1) {
            for (int row = 0; row < grain_h_chroma; row++) {
                for (int col = 0; col < grain_w_chroma; col++) {
                    if (av_lfg_get(lfg) % 2)
                        copy_px_U(dst, src, sx_chroma, sy_chroma, dx_chroma, dy_chroma, zoom, col, row);
                }
            }

            for (int row = 0; row < grain_h_chroma; row++) {
                for (int col = 0; col < grain_w_chroma; col++) {
                    if (av_lfg_get(lfg) % 2)
                        copy_px_V(dst, src, sx_chroma, sy_chroma, dx_chroma, dy_chroma, zoom, col, row);
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

    if (mode == MODE_PIXELS) {
        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                copy_px_Y(dst, src, sx, sy, dx, dy, zoom, col, row);
            }
        }
    }

    if (mode == MODE_INTERLACED_H) {
        for (int row = 0; row < grain_h; row++) {
            if (!(row % 2)) {
                for (int col = 0; col < grain_w; col++) {
                    copy_px_Y(dst, src, sx, sy, dx, dy, zoom, col, row);
                }
            }
        }
    }

    if (mode == MODE_INTERLACED_V) {
        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                if (!(col % 2)) {
                    copy_px_Y(dst, src, sx, sy, dx, dy, zoom, col, row);
                }
            }
        }
        
    }

    if (mode == MODE_DITHER) {
        for (int row = 0; row < grain_h; row++) {
            for (int col = 0; col < grain_w; col++) {
                if (av_lfg_get(lfg) % 2)
                    copy_px_Y(dst, src, sx, sy, dx, dy, zoom, col, row);
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
    
    for (int grain_count = 0; grain_count < n_grains; grain_count++) {
        if (ctx->delay_set) {
            g_src = (ctx->delay_set + ctx->frame_count) % ctx->buffer_size;
            src_f = src[g_src];
        }
        else {
            if (ctx->buffer_full) {
                g_src = av_lfg_get(ctx->lfg) % ctx->buffer_size;
                src_f = src[g_src];
            }
            else if (ctx->buffer_index > 0) {
                g_src = av_lfg_get(ctx->lfg) % (ctx->buffer_index + 1);
                src_f = src[g_src];
            }
            else {
                src_f = dst;
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

    for (int grain_count = 0; grain_count < n_grains; grain_count++) {
        if (ctx->delay_set) {
            g_src = (ctx->delay_set + ctx->frame_count) % ctx->buffer_size;
            src_f = src[g_src];
        } else {
            if (ctx->buffer_full) {
                g_src = av_lfg_get(ctx->lfg) % ctx->buffer_size;
                src_f = src[g_src];
            }
            else if (ctx->buffer_index > 0) {
                g_src = av_lfg_get(ctx->lfg) % (ctx->buffer_index + 1);
                src_f = src[g_src];
            }
            else {
                src_f = dst;
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

    if (granulate_ctx->delay > granulate_ctx->buffer_size)
        granulate_ctx->delay = granulate_ctx->buffer_size - 1;
    if (!granulate_ctx->delay)
        granulate_ctx->delay_set = 0;

    if (!granulate_ctx->fullscreen) {
        if (granulate_ctx->grain_w > width)
            granulate_ctx->grain_w = width;
        if (granulate_ctx->grain_h > height)
            granulate_ctx->grain_h = height;
    } else {
        granulate_ctx->grain_w = width;
        granulate_ctx->grain_h = height;
    }

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
            granulate_ctx->zoom_offset_w = av_lfg_get(granulate_ctx->lfg) % (granulate_ctx->grain_w - (granulate_ctx->grain_w / granulate_ctx->zoom_amount));
            granulate_ctx->zoom_offset_h = av_lfg_get(granulate_ctx->lfg) % (granulate_ctx->grain_h - (granulate_ctx->grain_h / granulate_ctx->zoom_amount));
    }

    if (granulate_ctx->buffer_size > 1) {
        if (granulate_ctx->delay && granulate_ctx->buffer_full) {
            if (!(granulate_ctx->frame_count % granulate_ctx->delay))
                granulate_ctx->delay_set = av_lfg_get(granulate_ctx->lfg) % granulate_ctx->buffer_size;
        }
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
        AVFrame *buf = granulate_ctx->fbuffer[0];
        ret = av_frame_copy(buf, in);
        if (ret < 0)
            return ret;
        av_frame_copy_props(buf, in);
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
