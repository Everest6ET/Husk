/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-tl-framework.h"
#include "husk-tl-dex.h"
#include "husk-tl-internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif

/* ----------------------------------------------------- Data Structures */

typedef struct {
    int width;
    int height;
    uint32_t *pixels;       /* ARGB8888 */
#if defined(__APPLE__)
    CGImageRef cg_image;
#endif
} tl_framework_bitmap;

typedef struct {
    float m[9];             /* 3x3 matrix in row-major order: [sx kx tx, ky sy ty, 0 0 1] */
} tl_framework_matrix;

typedef struct {
    uint32_t color;         /* ARGB */
    int alpha;
    bool filter;
    bool antialias;
} tl_framework_paint;

typedef struct {
    float left, top, right, bottom;
} tl_framework_rect;

typedef struct {
    int capacity;
    int size;
    tl_dex_val *items;
} tl_framework_list;

typedef struct {
    tl_framework_list *list;
    int cursor;
} tl_framework_iterator;

typedef struct {
    int high_score;
} tl_framework_prefs;

typedef struct {
    CGContextRef cg_ctx;
    tl_dex_object *canvas_obj;
    tl_dex_object *activity_obj;
    tl_dex_object *resources_obj;
    tl_dex_object *choreographer_obj;
    tl_dex_object *prefs_obj;
    tl_framework_prefs prefs;
} tl_framework_state;

static void matrix_identity(tl_framework_matrix *mat)
{
    mat->m[0] = 1.0f; mat->m[1] = 0.0f; mat->m[2] = 0.0f;
    mat->m[3] = 0.0f; mat->m[4] = 1.0f; mat->m[5] = 0.0f;
    mat->m[6] = 0.0f; mat->m[7] = 0.0f; mat->m[8] = 1.0f;
}

static void matrix_multiply(tl_framework_matrix *dst, const tl_framework_matrix *a, const tl_framework_matrix *b)
{
    tl_framework_matrix res;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            res.m[r * 3 + c] = a->m[r * 3 + 0] * b->m[0 * 3 + c] +
                               a->m[r * 3 + 1] * b->m[1 * 3 + c] +
                               a->m[r * 3 + 2] * b->m[2 * 3 + c];
        }
    }
    memcpy(dst, &res, sizeof(res));
}

/* ----------------------------------------------------- Asset & Bitmap Loading */

static tl_framework_bitmap *load_png_from_apk(const char *apk_path, const char *entry_name)
{
    tl_zip z;
    char zerr[128] = {0};
    if (!tl_zip_open(&z, apk_path, zerr, sizeof(zerr))) return NULL;

    const tl_zip_entry *entry = tl_zip_find(&z, entry_name);
    if (!entry) {
        tl_zip_close(&z);
        return NULL;
    }

    const uint8_t *data = NULL;
    size_t len = 0;
    bool owned = false;
    if (!tl_zip_data(&z, entry, 32 * 1024 * 1024, &data, &len, &owned, zerr, sizeof(zerr))) {
        tl_zip_close(&z);
        return NULL;
    }

    tl_framework_bitmap *bmp = NULL;
#if defined(__APPLE__)
    CFDataRef cf_data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, data, len, kCFAllocatorNull);
    CGImageSourceRef isrc = CGImageSourceCreateWithData(cf_data, NULL);
    CGImageRef img = isrc ? CGImageSourceCreateImageAtIndex(isrc, 0, NULL) : NULL;

    if (img) {
        bmp = calloc(1, sizeof(*bmp));
        bmp->width = (int)CGImageGetWidth(img);
        bmp->height = (int)CGImageGetHeight(img);
        bmp->pixels = calloc(bmp->width * bmp->height, sizeof(uint32_t));
        bmp->cg_image = img;

        /* Rasterize pixels in ARGB format */
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef c = CGBitmapContextCreate(bmp->pixels, bmp->width, bmp->height, 8,
                                               bmp->width * 4, cs,
                                               kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGContextDrawImage(c, CGRectMake(0, 0, bmp->width, bmp->height), img);
        CGContextRelease(c);
        CGColorSpaceRelease(cs);
    }
    if (isrc) CFRelease(isrc);
    if (cf_data) CFRelease(cf_data);
#endif

    if (owned) free((void *)data);
    tl_zip_close(&z);
    return bmp;
}

/* ----------------------------------------------------- Native Framework Methods */

/* java/lang/Object */
static bool obj_init(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)this_obj; (void)args; (void)nargs; (void)ret;
    return true;
}

/* android/graphics/Matrix */
static bool matrix_init(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)args; (void)nargs; (void)ret;
    tl_framework_matrix *m = calloc(1, sizeof(*m));
    matrix_identity(m);
    this_obj->native_ptr = m;
    return true;
}

static bool matrix_reset(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)args; (void)nargs; (void)ret;
    if (this_obj && this_obj->native_ptr) {
        matrix_identity((tl_framework_matrix *)this_obj->native_ptr);
    }
    return true;
}

static bool matrix_postTranslate(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)nargs;
    if (this_obj && this_obj->native_ptr) {
        tl_framework_matrix *m = this_obj->native_ptr;
        float dx = args[1].f;
        float dy = args[2].f;
        tl_framework_matrix t;
        matrix_identity(&t);
        t.m[2] = dx;
        t.m[5] = dy;
        matrix_multiply(m, &t, m);
    }
    if (ret) ret->i = 1;
    return true;
}

static bool matrix_postRotate(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx;
    if (this_obj && this_obj->native_ptr) {
        tl_framework_matrix *m = this_obj->native_ptr;
        float degrees = args[1].f;
        float rad = degrees * (float)(M_PI / 180.0);
        float c = cosf(rad);
        float s = sinf(rad);

        tl_framework_matrix r;
        matrix_identity(&r);
        r.m[0] = c;  r.m[1] = -s;
        r.m[3] = s;  r.m[4] = c;

        if (nargs >= 4) {
            float px = args[2].f;
            float py = args[3].f;
            tl_framework_matrix t1, t2;
            matrix_identity(&t1);
            t1.m[2] = -px; t1.m[5] = -py;
            matrix_identity(&t2);
            t2.m[2] = px;  t2.m[5] = py;

            tl_framework_matrix tmp;
            matrix_multiply(&tmp, &r, &t1);
            matrix_multiply(&tmp, &t2, &tmp);
            matrix_multiply(m, &tmp, m);
        } else {
            matrix_multiply(m, &r, m);
        }
    }
    if (ret) ret->i = 1;
    return true;
}

static bool matrix_postScale(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)nargs;
    if (this_obj && this_obj->native_ptr) {
        tl_framework_matrix *m = this_obj->native_ptr;
        float sx = args[1].f;
        float sy = args[2].f;
        tl_framework_matrix s;
        matrix_identity(&s);
        s.m[0] = sx;
        s.m[4] = sy;
        matrix_multiply(m, &s, m);
    }
    if (ret) ret->i = 1;
    return true;
}

/* android/graphics/Paint */
static bool paint_init(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)args; (void)nargs; (void)ret;
    tl_framework_paint *p = calloc(1, sizeof(*p));
    p->color = 0xffffffff;
    p->alpha = 255;
    this_obj->native_ptr = p;
    return true;
}

static bool paint_setFilterBitmap(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)nargs; (void)ret;
    if (this_obj && this_obj->native_ptr) {
        ((tl_framework_paint *)this_obj->native_ptr)->filter = (args[1].i != 0);
    }
    return true;
}

static bool paint_setAntiAlias(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)nargs; (void)ret;
    if (this_obj && this_obj->native_ptr) {
        ((tl_framework_paint *)this_obj->native_ptr)->antialias = (args[1].i != 0);
    }
    return true;
}

static bool paint_setARGB(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)nargs; (void)ret;
    if (this_obj && this_obj->native_ptr) {
        tl_framework_paint *p = this_obj->native_ptr;
        int a = args[1].i & 0xff;
        int r = args[2].i & 0xff;
        int g = args[3].i & 0xff;
        int b = args[4].i & 0xff;
        p->alpha = a;
        p->color = (uint32_t)((a << 24) | (r << 16) | (g << 8) | b);
    }
    return true;
}

static bool paint_setColor(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)nargs; (void)ret;
    if (this_obj && this_obj->native_ptr) {
        tl_framework_paint *p = this_obj->native_ptr;
        p->color = args[1].raw32;
        p->alpha = (int)((p->color >> 24) & 0xff);
    }
    return true;
}

static bool paint_setAlpha(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)nargs; (void)ret;
    if (this_obj && this_obj->native_ptr) {
        tl_framework_paint *p = this_obj->native_ptr;
        p->alpha = args[1].i & 0xff;
        p->color = (p->color & 0x00ffffff) | ((uint32_t)p->alpha << 24);
    }
    return true;
}

static bool paint_getAlpha(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)args; (void)nargs;
    int a = 255;
    if (this_obj && this_obj->native_ptr) {
        a = ((tl_framework_paint *)this_obj->native_ptr)->alpha;
    }
    if (ret) ret->i = a;
    return true;
}

static bool paint_setColorFilter(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)this_obj; (void)args; (void)nargs; (void)ret;
    return true;
}

/* android/graphics/Rect & RectF */
static bool rect_init_void(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)args; (void)nargs; (void)ret;
    tl_framework_rect *r = calloc(1, sizeof(*r));
    this_obj->native_ptr = r;
    return true;
}

static bool rect_init_int(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)ret;
    tl_framework_rect *r = calloc(1, sizeof(*r));
    if (nargs >= 5) {
        r->left = (float)args[1].i;
        r->top = (float)args[2].i;
        r->right = (float)args[3].i;
        r->bottom = (float)args[4].i;
    }
    this_obj->native_ptr = r;
    return true;
}

static bool rect_init_float(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)ret;
    tl_framework_rect *r = calloc(1, sizeof(*r));
    if (nargs >= 5) {
        r->left = args[1].f;
        r->top = args[2].f;
        r->right = args[3].f;
        r->bottom = args[4].f;
    }
    this_obj->native_ptr = r;
    return true;
}

static bool rect_set_int(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)nargs; (void)ret;
    if (this_obj && this_obj->native_ptr && nargs >= 5) {
        tl_framework_rect *r = this_obj->native_ptr;
        r->left   = (float)args[1].i;
        r->top    = (float)args[2].i;
        r->right  = (float)args[3].i;
        r->bottom = (float)args[4].i;
    }
    return true;
}

static bool rect_set_float(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)nargs; (void)ret;
    if (this_obj && this_obj->native_ptr && nargs >= 5) {
        tl_framework_rect *r = this_obj->native_ptr;
        r->left   = args[1].f;
        r->top    = args[2].f;
        r->right  = args[3].f;
        r->bottom = args[4].f;
    }
    return true;
}

static bool rect_contains(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)nargs;
    int inside = 0;
    if (this_obj && this_obj->native_ptr) {
        tl_framework_rect *r = this_obj->native_ptr;
        float x = (float)args[1].i;
        float y = (float)args[2].i;
        if (x >= r->left && x <= r->right && y >= r->top && y <= r->bottom) {
            inside = 1;
        }
    }
    if (ret) ret->i = inside;
    return true;
}

/* android/graphics/Bitmap */
static bool bitmap_getWidth(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)args; (void)nargs;
    int w = 0;
    if (this_obj && this_obj->native_ptr) {
        w = ((tl_framework_bitmap *)this_obj->native_ptr)->width;
    }
    if (ret) ret->i = w;
    return true;
}

static bool bitmap_getHeight(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)args; (void)nargs;
    int h = 0;
    if (this_obj && this_obj->native_ptr) {
        h = ((tl_framework_bitmap *)this_obj->native_ptr)->height;
    }
    if (ret) ret->i = h;
    return true;
}

static bool bitmap_recycle(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)this_obj; (void)args; (void)nargs; (void)ret;
    return true;
}

static bool bitmap_createBitmap(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)this_obj;
    tl_dex_class *b_class = tl_dex_find_class(ctx, "Landroid/graphics/Bitmap;");
    tl_dex_object *new_obj = tl_dex_alloc_object(b_class);

    if (nargs >= 5 && args[0].l && args[0].l->native_ptr) {
        /* createBitmap(Bitmap src, int x, int y, int width, int height) */
        tl_framework_bitmap *src = args[0].l->native_ptr;
        int sx = args[1].i;
        int sy = args[2].i;
        int sw = args[3].i;
        int sh = args[4].i;

        tl_framework_bitmap *dst = calloc(1, sizeof(*dst));
        dst->width = sw;
        dst->height = sh;
        dst->pixels = calloc(sw * sh, sizeof(uint32_t));

        for (int y = 0; y < sh; y++) {
            int src_y = sy + y;
            if (src_y >= 0 && src_y < src->height) {
                for (int x = 0; x < sw; x++) {
                    int src_x = sx + x;
                    if (src_x >= 0 && src_x < src->width) {
                        dst->pixels[y * sw + x] = src->pixels[src_y * src->width + src_x];
                    }
                }
            }
        }
#if defined(__APPLE__)
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef c = CGBitmapContextCreate(dst->pixels, sw, sh, 8, sw * 4, cs,
                                               kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        dst->cg_image = CGBitmapContextCreateImage(c);
        CGContextRelease(c);
        CGColorSpaceRelease(cs);
#endif
        new_obj->native_ptr = dst;
    } else if (nargs >= 2) {
        /* createBitmap(int width, int height, ...) */
        int w = args[0].i;
        int h = args[1].i;
        tl_framework_bitmap *dst = calloc(1, sizeof(*dst));
        dst->width = w;
        dst->height = h;
        dst->pixels = calloc(w * h, sizeof(uint32_t));
        new_obj->native_ptr = dst;
    }
    if (ret) ret->l = new_obj;
    return true;
}

static bool bitmap_createScaledBitmap(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)this_obj; (void)nargs;
    tl_dex_class *b_class = tl_dex_find_class(ctx, "Landroid/graphics/Bitmap;");
    tl_dex_object *new_obj = tl_dex_alloc_object(b_class);

    if (args[0].l && args[0].l->native_ptr) {
        tl_framework_bitmap *src = args[0].l->native_ptr;
        int dstW = args[1].i;
        int dstH = args[2].i;
        tl_framework_bitmap *dst = calloc(1, sizeof(*dst));
        dst->width = dstW;
        dst->height = dstH;
        dst->pixels = calloc(dstW * dstH, sizeof(uint32_t));

        /* Nearest-neighbor scale */
        for (int y = 0; y < dstH; y++) {
            int sy = (y * src->height) / dstH;
            for (int x = 0; x < dstW; x++) {
                int sx = (x * src->width) / dstW;
                dst->pixels[y * dstW + x] = src->pixels[sy * src->width + sx];
            }
        }
#if defined(__APPLE__)
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef c = CGBitmapContextCreate(dst->pixels, dstW, dstH, 8, dstW * 4, cs,
                                               kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        dst->cg_image = CGBitmapContextCreateImage(c);
        CGContextRelease(c);
        CGColorSpaceRelease(cs);
#endif
        new_obj->native_ptr = dst;
    }
    if (ret) ret->l = new_obj;
    return true;
}

/* android/graphics/BitmapFactory */
static bool bitmapFactory_decodeResource(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)this_obj; (void)nargs;
    uint32_t res_id = args[1].raw32;
    const char *entry = NULL;
    char num_buf[64] = {0};
    if (res_id >= 0x7f080200 && res_id <= 0x7f080209) {
        snprintf(num_buf, sizeof(num_buf), "res/drawable/number_%d.png", (int)(res_id - 0x7f080200));
        entry = num_buf;
    } else {
        switch (res_id) {
            case 0x7f080077: entry = "res/drawable/atlas.png"; break;
            case 0x7f0800ff: entry = "res/drawable/pipe-green.png"; break;
            case 0x7f08007b: entry = "res/drawable/bluebird-downflap.png"; break;
            case 0x7f08007c: entry = "res/drawable/bluebird-midflap.png"; break;
            case 0x7f08007d: entry = "res/drawable/bluebird-upflap.png"; break;
            case 0x7f080108: entry = "res/drawable/yellowbird-downflap.png"; break;
            case 0x7f080109: entry = "res/drawable/yellowbird-midflap.png"; break;
            case 0x7f08010a: entry = "res/drawable/yellowbird-upflap.png"; break;
            case 0x7f080100: entry = "res/drawable/redbird-downflap.png"; break;
            case 0x7f080101: entry = "res/drawable/redbird-midflap.png"; break;
            case 0x7f080102: entry = "res/drawable/redbird-upflap.png"; break;
            default: break;
        }
    }

    tl_framework_bitmap *bmp = NULL;
    if (entry && ctx->apk_path) {
        bmp = load_png_from_apk(ctx->apk_path, entry);
    }

    tl_dex_class *b_class = tl_dex_find_class(ctx, "Landroid/graphics/Bitmap;");
    tl_dex_object *b_obj = tl_dex_alloc_object(b_class);
    b_obj->native_ptr = bmp;

    if (ret) ret->l = b_obj;
    return true;
}

/* android/graphics/Canvas */
static bool canvas_save(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)this_obj; (void)args; (void)nargs;
    tl_framework_state *st = ctx->framework_data;
    int count = 1;
    if (st && st->cg_ctx) {
        CGContextSaveGState(st->cg_ctx);
    }
    if (ret) ret->i = count;
    return true;
}

static bool canvas_restore(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)this_obj; (void)args; (void)nargs; (void)ret;
    tl_framework_state *st = ctx->framework_data;
    if (st && st->cg_ctx) {
        CGContextRestoreGState(st->cg_ctx);
    }
    return true;
}

static bool canvas_scale(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)this_obj; (void)ret;
    tl_framework_state *st = ctx->framework_data;
    if (st && st->cg_ctx) {
        float sx = args[1].f;
        float sy = args[2].f;
        if (nargs >= 5) {
            float px = args[3].f;
            float py = args[4].f;
            CGContextTranslateCTM(st->cg_ctx, px, py);
            CGContextScaleCTM(st->cg_ctx, sx, sy);
            CGContextTranslateCTM(st->cg_ctx, -px, -py);
        } else {
            CGContextScaleCTM(st->cg_ctx, sx, sy);
        }
    }
    return true;
}

static bool canvas_drawBitmap_xy(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)this_obj; (void)nargs; (void)ret;
    tl_framework_state *st = ctx->framework_data;
    if (!st || !st->cg_ctx || !args[1].l || !args[1].l->native_ptr) return true;
    tl_framework_bitmap *bmp = args[1].l->native_ptr;
    if (!bmp->cg_image) return true;

    float x = args[2].f;
    float y = args[3].f;
    float w = bmp->width;
    float h = bmp->height;

    CGContextSaveGState(st->cg_ctx);
    CGContextTranslateCTM(st->cg_ctx, x, y + h);
    CGContextScaleCTM(st->cg_ctx, 1.0, -1.0);
    CGContextDrawImage(st->cg_ctx, CGRectMake(0, 0, w, h), bmp->cg_image);
    CGContextRestoreGState(st->cg_ctx);
    return true;
}

static bool canvas_drawBitmap_matrix(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)this_obj; (void)nargs; (void)ret;
    tl_framework_state *st = ctx->framework_data;
    if (!st || !st->cg_ctx || !args[1].l || !args[1].l->native_ptr) return true;
    tl_framework_bitmap *bmp = args[1].l->native_ptr;
    if (!bmp->cg_image || !args[2].l || !args[2].l->native_ptr) return true;

    tl_framework_matrix *mat = args[2].l->native_ptr;
    CGContextSaveGState(st->cg_ctx);
    CGAffineTransform t = CGAffineTransformMake(mat->m[0], mat->m[3],
                                                    mat->m[1], mat->m[4],
                                                    mat->m[2], mat->m[5]);
    CGContextConcatCTM(st->cg_ctx, t);
    CGContextTranslateCTM(st->cg_ctx, 0, bmp->height);
    CGContextScaleCTM(st->cg_ctx, 1.0, -1.0);
    CGContextDrawImage(st->cg_ctx, CGRectMake(0, 0, bmp->width, bmp->height), bmp->cg_image);
    CGContextRestoreGState(st->cg_ctx);
    return true;
}

static bool canvas_drawBitmap_rect(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)this_obj; (void)nargs; (void)ret;
    tl_framework_state *st = ctx->framework_data;
    if (!st || !st->cg_ctx || !args[1].l || !args[1].l->native_ptr) return true;
    tl_framework_bitmap *bmp = args[1].l->native_ptr;
    if (!bmp->cg_image) return true;

    tl_framework_rect *src = (args[2].l) ? args[2].l->native_ptr : NULL;
    tl_framework_rect *dst = (args[3].l) ? args[3].l->native_ptr : NULL;
    if (!dst) return true;

    float dst_x = dst->left;
    float dst_y = dst->top;
    float dst_w = dst->right - dst->left;
    float dst_h = dst->bottom - dst->top;
    if (dst_w <= 0 || dst_h <= 0) return true;

    CGImageRef img_to_draw = bmp->cg_image;
    bool need_release = false;
    if (src) {
        float sx = src->left;
        float sy = src->top;
        float sw = src->right - src->left;
        float sh = src->bottom - src->top;
        if (sw > 0 && sh > 0) {
            img_to_draw = CGImageCreateWithImageInRect(bmp->cg_image, CGRectMake(sx, sy, sw, sh));
            need_release = true;
        }
    }

    if (img_to_draw) {
        CGContextSaveGState(st->cg_ctx);
        CGContextTranslateCTM(st->cg_ctx, dst_x, dst_y + dst_h);
        CGContextScaleCTM(st->cg_ctx, 1.0, -1.0);
        CGContextDrawImage(st->cg_ctx, CGRectMake(0, 0, dst_w, dst_h), img_to_draw);
        CGContextRestoreGState(st->cg_ctx);
        if (need_release) CGImageRelease(img_to_draw);
    }
    return true;
}


static bool canvas_drawRect(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)this_obj; (void)ret;
    tl_framework_state *st = ctx->framework_data;
    if (!st || !st->cg_ctx) return true;

    float l = 0, t = 0, w = 0, h = 0;
    tl_framework_paint *paint = NULL;

    if (nargs >= 6) {
        /* drawRect(float left, float top, float right, float bottom, Paint paint) */
        l = args[1].f;
        t = args[2].f;
        w = args[3].f - args[1].f;
        h = args[4].f - args[2].f;
        if (args[5].l && args[5].l->native_ptr) {
            paint = args[5].l->native_ptr;
        }
    } else if (nargs >= 3 && args[1].l && args[1].l->native_ptr) {
        /* drawRect(Rect/RectF rect, Paint paint) */
        tl_framework_rect *r = args[1].l->native_ptr;
        l = r->left;
        t = r->top;
        w = r->right - r->left;
        h = r->bottom - r->top;
        if (args[2].l && args[2].l->native_ptr) {
            paint = args[2].l->native_ptr;
        }
    }

    if (paint) {
        float a = (((paint->color >> 24) & 0xff) / 255.0f) * (paint->alpha / 255.0f);
        float r_col = ((paint->color >> 16) & 0xff) / 255.0f;
        float g_col = ((paint->color >> 8) & 0xff) / 255.0f;
        float b_col = (paint->color & 0xff) / 255.0f;
        CGContextSetRGBFillColor(st->cg_ctx, r_col, g_col, b_col, a);
    }
    CGContextFillRect(st->cg_ctx, CGRectMake(l, t, w, h));
    return true;
}

/* android/view/View */
static bool view_invalidate(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)this_obj; (void)args; (void)nargs; (void)ret;
    return true;
}

static bool view_getContext(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)this_obj; (void)args; (void)nargs;
    tl_framework_state *st = ctx->framework_data;
    if (ret && st) ret->l = st->activity_obj;
    return true;
}

static bool view_getResources(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)this_obj; (void)args; (void)nargs;
    tl_framework_state *st = ctx->framework_data;
    if (ret && st) ret->l = st->resources_obj;
    return true;
}

/* android/app/Activity & Context */
static bool activity_getSharedPreferences(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)this_obj; (void)args; (void)nargs;
    tl_framework_state *st = ctx->framework_data;
    if (ret && st) ret->l = st->prefs_obj;
    return true;
}

static bool activity_getPackageName(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)this_obj; (void)args; (void)nargs;
    if (ret) ret->l = tl_dex_alloc_string(ctx, "com.flappybird.recreation");
    return true;
}

static bool sharedPrefs_getInt(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)this_obj; (void)nargs;
    tl_framework_state *st = ctx->framework_data;
    int val = args[2].i; /* defValue */
    if (st) val = st->prefs.high_score;
    if (ret) ret->i = val;
    return true;
}

static bool sharedPrefs_getBoolean(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)this_obj; (void)nargs;
    if (ret) ret->i = (nargs >= 3) ? args[2].i : 0;
    return true;
}

static bool sharedPrefs_getFloat(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)this_obj; (void)nargs;
    if (ret) ret->f = (nargs >= 3) ? args[2].f : 0.0f;
    return true;
}

static bool sharedPrefs_getString(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)this_obj; (void)nargs;
    if (ret) ret->l = (nargs >= 3) ? args[2].l : NULL;
    return true;
}

static bool sharedPrefs_getLong(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)this_obj; (void)nargs;
    if (ret) ret->j = (nargs >= 3) ? args[2].j : 0;
    return true;
}

/* android/content/res/Resources */
static bool resources_getIdentifier(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)this_obj; (void)nargs;
    const char *name = (args[1].l && args[1].l->str_utf8) ? args[1].l->str_utf8 : "";
    int id = 0;
    if (!strcmp(name, "atlas")) id = 0x7f080077;
    else if (!strcmp(name, "pipe_green") || !strcmp(name, "pipe-green")) id = 0x7f0800ff;
    else if (!strncmp(name, "number_", 7)) id = 0x7f080200 + atoi(name + 7);
    else if (!strncmp(name, "font_", 5)) id = 0x7f080300 + atoi(name + 5);
    if (ret) ret->i = id;
    return true;
}

/* android/view/Choreographer */
static bool choreographer_getInstance(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)this_obj; (void)args; (void)nargs;
    tl_framework_state *st = ctx->framework_data;
    if (ret && st) ret->l = st->choreographer_obj;
    return true;
}

static bool choreographer_postFrameCallback(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)this_obj; (void)nargs; (void)ret;
    if (args[1].l) {
        ctx->choreographer_cb = args[1].l;
    }
    return true;
}

/* java/util/Random */
static bool random_init(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)this_obj; (void)args; (void)nargs; (void)ret;
    return true;
}

static bool random_nextInt(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)this_obj;
    int bound = (nargs >= 2 && args[1].i > 0) ? args[1].i : 100;
    int val = rand() % bound;
    if (ret) ret->i = val;
    return true;
}

static bool random_nextFloat(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)this_obj; (void)args; (void)nargs;
    float val = (float)rand() / (float)RAND_MAX;
    if (ret) ret->f = val;
    return true;
}

static bool random_nextBoolean(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)this_obj; (void)args; (void)nargs;
    if (ret) ret->i = (rand() & 1);
    return true;
}

/* java/lang/Math */
static bool math_abs(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)this_obj; (void)nargs;
    if (ret) ret->f = fabsf(args[0].f);
    return true;
}

/* java/util/ArrayList */
static bool arrayList_init(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)args; (void)nargs; (void)ret;
    tl_framework_list *lst = calloc(1, sizeof(*lst));
    lst->capacity = 16;
    lst->items = calloc(lst->capacity, sizeof(tl_dex_val));
    this_obj->native_ptr = lst;
    return true;
}

static bool arrayList_add(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)nargs;
    if (this_obj && this_obj->native_ptr) {
        tl_framework_list *lst = this_obj->native_ptr;
        if (lst->size >= lst->capacity) {
            lst->capacity *= 2;
            lst->items = realloc(lst->items, lst->capacity * sizeof(tl_dex_val));
        }
        lst->items[lst->size++] = args[1];
    }
    if (ret) ret->i = 1;
    return true;
}

static bool arrayList_size(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)args; (void)nargs;
    int s = 0;
    if (this_obj && this_obj->native_ptr) {
        s = ((tl_framework_list *)this_obj->native_ptr)->size;
    }
    if (ret) ret->i = s;
    return true;
}

static bool arrayList_get(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)nargs;
    if (this_obj && this_obj->native_ptr && ret) {
        tl_framework_list *lst = this_obj->native_ptr;
        int idx = args[1].i;
        if (idx >= 0 && idx < lst->size) {
            *ret = lst->items[idx];
        } else {
            ret->l = NULL;
        }
    }
    return true;
}

static bool arrayList_clear(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)args; (void)nargs; (void)ret;
    if (this_obj && this_obj->native_ptr) {
        ((tl_framework_list *)this_obj->native_ptr)->size = 0;
    }
    return true;
}

static bool arrayList_iterator(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)args; (void)nargs;
    tl_dex_class *it_class = tl_dex_find_class(ctx, "Ljava/util/Iterator;");
    tl_dex_object *it_obj = tl_dex_alloc_object(it_class);
    tl_framework_iterator *it = calloc(1, sizeof(*it));
    it->list = this_obj ? this_obj->native_ptr : NULL;
    it->cursor = 0;
    it_obj->native_ptr = it;
    if (ret) ret->l = it_obj;
    return true;
}

static bool iterator_hasNext(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)args; (void)nargs;
    int has = 0;
    if (this_obj && this_obj->native_ptr) {
        tl_framework_iterator *it = this_obj->native_ptr;
        if (it->list && it->cursor < it->list->size) {
            has = 1;
        }
    }
    if (ret) ret->i = has;
    return true;
}

static bool iterator_next(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)args; (void)nargs;
    if (this_obj && this_obj->native_ptr && ret) {
        tl_framework_iterator *it = this_obj->native_ptr;
        if (it->list && it->cursor < it->list->size) {
            *ret = it->list->items[it->cursor++];
        } else {
            ret->l = NULL;
        }
    }
    return true;
}

/* java/lang/reflect/Array */
static bool array_newInstance(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)this_obj; (void)nargs;
    tl_dex_class *elem_class = args[0].l ? args[0].l->clazz : NULL;
    tl_dex_object *dim_obj = args[1].l;
    int len = 0;

    if (dim_obj && dim_obj->array.elements) {
        int32_t *dims = (int32_t *)dim_obj->array.elements;
        len = dims[0];
    } else {
        len = args[1].i;
    }

    tl_dex_object *arr = tl_dex_alloc_array(elem_class, len, sizeof(void *));
    if (ret) ret->l = arr;
    return true;
}

/* android/view/MotionEvent */
static bool motionEvent_getX(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)args; (void)nargs;
    float x = 0;
    if (this_obj && this_obj->fields) x = this_obj->fields[0].f;
    if (ret) ret->f = x;
    return true;
}

static bool motionEvent_getY(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)args; (void)nargs;
    float y = 0;
    if (this_obj && this_obj->fields) y = this_obj->fields[1].f;
    if (ret) ret->f = y;
    return true;
}

static bool motionEvent_getAction(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)args; (void)nargs;
    int act = 0;
    if (this_obj && this_obj->fields) act = this_obj->fields[2].i;
    if (ret) ret->i = act;
    return true;
}

/* java/util/concurrent/Executors */
static bool executors_newSingleThreadExecutor(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)this_obj; (void)args; (void)nargs;
    tl_dex_class *ex_class = tl_dex_find_class(ctx, "Ljava/util/concurrent/ExecutorService;");
    if (ret) ret->l = tl_dex_alloc_object(ex_class);
    return true;
}

/* android/util/Log */
static bool log_print(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)this_obj; (void)nargs;
    const char *tag = (args[0].l && args[0].l->str_utf8) ? args[0].l->str_utf8 : "";
    const char *msg = (args[1].l && args[1].l->str_utf8) ? args[1].l->str_utf8 : "";
    printf("[%s] %s\n", tag, msg);
    if (ret) ret->i = 0;
    return true;
}

/* java/lang/StringBuilder */
static bool stringBuilder_init_void(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)args; (void)nargs; (void)ret;
    if (this_obj) {
        if (this_obj->str_utf8) free(this_obj->str_utf8);
        this_obj->str_utf8 = strdup("");
    }
    return true;
}

static bool stringBuilder_init_str(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)ret;
    const char *s = (nargs >= 2 && args[1].l && args[1].l->str_utf8) ? args[1].l->str_utf8 : "";
    if (this_obj) {
        if (this_obj->str_utf8) free(this_obj->str_utf8);
        this_obj->str_utf8 = strdup(s);
    }
    return true;
}

static void stringBuilder_append_text(tl_dex_object *this_obj, const char *add)
{
    if (!this_obj || !add) return;
    size_t old_len = this_obj->str_utf8 ? strlen(this_obj->str_utf8) : 0;
    size_t add_len = strlen(add);
    char *nb = malloc(old_len + add_len + 1);
    if (!nb) return;
    if (this_obj->str_utf8) {
        memcpy(nb, this_obj->str_utf8, old_len);
        free(this_obj->str_utf8);
    }
    memcpy(nb + old_len, add, add_len);
    nb[old_len + add_len] = '\0';
    this_obj->str_utf8 = nb;
}

static bool stringBuilder_append_str(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx;
    const char *s = (nargs >= 2 && args[1].l && args[1].l->str_utf8) ? args[1].l->str_utf8 : "null";
    stringBuilder_append_text(this_obj, s);
    if (ret) ret->l = this_obj;
    return true;
}

static bool stringBuilder_append_int(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx;
    char buf[32];
    int32_t val = (nargs >= 2) ? args[1].i : 0;
    snprintf(buf, sizeof(buf), "%d", val);
    stringBuilder_append_text(this_obj, buf);
    if (ret) ret->l = this_obj;
    return true;
}

static bool stringBuilder_append_float(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx;
    char buf[32];
    float val = (nargs >= 2) ? args[1].f : 0.0f;
    snprintf(buf, sizeof(buf), "%f", val);
    stringBuilder_append_text(this_obj, buf);
    if (ret) ret->l = this_obj;
    return true;
}

static bool stringBuilder_append_char(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx;
    char buf[2];
    buf[0] = (nargs >= 2) ? (char)args[1].i : '\0';
    buf[1] = '\0';
    stringBuilder_append_text(this_obj, buf);
    if (ret) ret->l = this_obj;
    return true;
}

static bool stringBuilder_append_bool(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx;
    bool val = (nargs >= 2) ? (args[1].i != 0) : false;
    stringBuilder_append_text(this_obj, val ? "true" : "false");
    if (ret) ret->l = this_obj;
    return true;
}

static bool stringBuilder_toString(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)args; (void)nargs;
    const char *s = (this_obj && this_obj->str_utf8) ? this_obj->str_utf8 : "";
    if (ret) ret->l = tl_dex_alloc_string(ctx, s);
    return true;
}

/* No-op stub for unneeded framework calls */
static bool noop_stub(tl_dex_context *ctx, tl_dex_object *this_obj, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    (void)ctx; (void)this_obj; (void)args; (void)nargs;
    if (ret) ret->raw64 = 0;
    return true;
}

/* ----------------------------------------------------- Method Table */

typedef struct {
    const char *class_desc;
    const char *method_name;
    const char *shorty;
    tl_dex_native_func func;
} tl_native_entry;

static const tl_native_entry s_native_methods[] = {
    { "Ljava/lang/Object;", "<init>", NULL, obj_init },

    /* StringBuilder */
    { "Ljava/lang/StringBuilder;", "<init>", "V", stringBuilder_init_void },
    { "Ljava/lang/StringBuilder;", "<init>", "VL", stringBuilder_init_str },
    { "Ljava/lang/StringBuilder;", "<init>", "VI", stringBuilder_init_void },
    { "Ljava/lang/StringBuilder;", "<init>", NULL, stringBuilder_init_void },
    { "Ljava/lang/StringBuilder;", "append", "LL", stringBuilder_append_str },
    { "Ljava/lang/StringBuilder;", "append", "LI", stringBuilder_append_int },
    { "Ljava/lang/StringBuilder;", "append", "LF", stringBuilder_append_float },
    { "Ljava/lang/StringBuilder;", "append", "LC", stringBuilder_append_char },
    { "Ljava/lang/StringBuilder;", "append", "LZ", stringBuilder_append_bool },
    { "Ljava/lang/StringBuilder;", "append", NULL, stringBuilder_append_str },
    { "Ljava/lang/StringBuilder;", "toString", NULL, stringBuilder_toString },

    /* Matrix */
    { "Landroid/graphics/Matrix;", "<init>", NULL, matrix_init },
    { "Landroid/graphics/Matrix;", "reset", NULL, matrix_reset },
    { "Landroid/graphics/Matrix;", "postTranslate", NULL, matrix_postTranslate },
    { "Landroid/graphics/Matrix;", "postRotate", NULL, matrix_postRotate },
    { "Landroid/graphics/Matrix;", "postScale", NULL, matrix_postScale },

    /* Paint */
    { "Landroid/graphics/Paint;", "<init>", NULL, paint_init },
    { "Landroid/graphics/Paint;", "setFilterBitmap", NULL, paint_setFilterBitmap },
    { "Landroid/graphics/Paint;", "setAntiAlias", NULL, paint_setAntiAlias },
    { "Landroid/graphics/Paint;", "setARGB", NULL, paint_setARGB },
    { "Landroid/graphics/Paint;", "setColor", NULL, paint_setColor },
    { "Landroid/graphics/Paint;", "setAlpha", NULL, paint_setAlpha },
    { "Landroid/graphics/Paint;", "getAlpha", NULL, paint_getAlpha },
    { "Landroid/graphics/Paint;", "setColorFilter", NULL, paint_setColorFilter },

    /* Rect / RectF */
    { "Landroid/graphics/Rect;", "<init>", "V", rect_init_void },
    { "Landroid/graphics/Rect;", "<init>", "VIIII", rect_init_int },
    { "Landroid/graphics/Rect;", "<init>", NULL, rect_init_int },
    { "Landroid/graphics/Rect;", "set", "VIIII", rect_set_int },
    { "Landroid/graphics/Rect;", "set", NULL, rect_set_int },
    { "Landroid/graphics/Rect;", "contains", NULL, rect_contains },
    { "Landroid/graphics/RectF;", "<init>", "V", rect_init_void },
    { "Landroid/graphics/RectF;", "<init>", "VFFFF", rect_init_float },
    { "Landroid/graphics/RectF;", "<init>", NULL, rect_init_float },
    { "Landroid/graphics/RectF;", "set", "VFFFF", rect_set_float },
    { "Landroid/graphics/RectF;", "set", NULL, rect_set_float },

    /* Bitmap */
    { "Landroid/graphics/Bitmap;", "getWidth", NULL, bitmap_getWidth },
    { "Landroid/graphics/Bitmap;", "getHeight", NULL, bitmap_getHeight },
    { "Landroid/graphics/Bitmap;", "recycle", NULL, bitmap_recycle },
    { "Landroid/graphics/Bitmap;", "createBitmap", NULL, bitmap_createBitmap },
    { "Landroid/graphics/Bitmap;", "createScaledBitmap", NULL, bitmap_createScaledBitmap },

    /* BitmapFactory */
    { "Landroid/graphics/BitmapFactory;", "decodeResource", NULL, bitmapFactory_decodeResource },
    { "Landroid/graphics/BitmapFactory$Options;", "<init>", NULL, noop_stub },

    /* Canvas */
    { "Landroid/graphics/Canvas;", "save", NULL, canvas_save },
    { "Landroid/graphics/Canvas;", "restore", NULL, canvas_restore },
    { "Landroid/graphics/Canvas;", "scale", NULL, canvas_scale },
    { "Landroid/graphics/Canvas;", "drawBitmap", "VLFFL", canvas_drawBitmap_xy },
    { "Landroid/graphics/Canvas;", "drawBitmap", "VLFF", canvas_drawBitmap_xy },
    { "Landroid/graphics/Canvas;", "drawBitmap", "VLLL", canvas_drawBitmap_matrix },
    { "Landroid/graphics/Canvas;", "drawBitmap", "VLLLL", canvas_drawBitmap_rect },
    { "Landroid/graphics/Canvas;", "drawBitmap", NULL, canvas_drawBitmap_xy },
    { "Landroid/graphics/Canvas;", "drawRect", NULL, canvas_drawRect },

    /* View */
    { "Landroid/view/View;", "<init>", NULL, noop_stub },
    { "Landroid/view/View;", "onSizeChanged", NULL, noop_stub },
    { "Landroid/view/View;", "invalidate", NULL, view_invalidate },
    { "Landroid/view/View;", "getContext", NULL, view_getContext },
    { "Landroid/view/View;", "getResources", NULL, view_getResources },

    /* Activity & Context */
    { "Landroid/app/Activity;", "<init>", NULL, noop_stub },
    { "Landroid/app/Activity;", "onCreate", NULL, noop_stub },
    { "Landroid/app/Activity;", "getSharedPreferences", NULL, activity_getSharedPreferences },
    { "Landroid/content/Context;", "getSharedPreferences", NULL, activity_getSharedPreferences },
    { "Landroid/content/Context;", "getPackageName", NULL, activity_getPackageName },
    { "Landroid/content/Context;", "startActivity", NULL, noop_stub },
    { "Landroid/content/SharedPreferences;", "getInt", NULL, sharedPrefs_getInt },
    { "Landroid/content/SharedPreferences;", "getBoolean", NULL, sharedPrefs_getBoolean },
    { "Landroid/content/SharedPreferences;", "getFloat", NULL, sharedPrefs_getFloat },
    { "Landroid/content/SharedPreferences;", "getString", NULL, sharedPrefs_getString },
    { "Landroid/content/SharedPreferences;", "getLong", NULL, sharedPrefs_getLong },
    { "Landroid/content/res/Resources;", "getIdentifier", NULL, resources_getIdentifier },

    /* Choreographer */
    { "Landroid/view/Choreographer;", "getInstance", NULL, choreographer_getInstance },
    { "Landroid/view/Choreographer;", "postFrameCallback", NULL, choreographer_postFrameCallback },

    /* MotionEvent */
    { "Landroid/view/MotionEvent;", "getX", NULL, motionEvent_getX },
    { "Landroid/view/MotionEvent;", "getY", NULL, motionEvent_getY },
    { "Landroid/view/MotionEvent;", "getAction", NULL, motionEvent_getAction },

    /* Math & Collections */
    { "Ljava/lang/Math;", "abs", NULL, math_abs },
    { "Ljava/util/Random;", "<init>", NULL, random_init },
    { "Ljava/util/Random;", "nextInt", NULL, random_nextInt },
    { "Ljava/util/Random;", "nextFloat", NULL, random_nextFloat },
    { "Ljava/util/Random;", "nextBoolean", NULL, random_nextBoolean },

    { "Ljava/util/ArrayList;", "<init>", NULL, arrayList_init },
    { "Ljava/util/ArrayList;", "add", NULL, arrayList_add },
    { "Ljava/util/ArrayList;", "get", NULL, arrayList_get },
    { "Ljava/util/ArrayList;", "size", NULL, arrayList_size },
    { "Ljava/util/ArrayList;", "clear", NULL, arrayList_clear },
    { "Ljava/util/ArrayList;", "iterator", NULL, arrayList_iterator },
    { "Ljava/util/List;", "add", NULL, arrayList_add },
    { "Ljava/util/List;", "get", NULL, arrayList_get },
    { "Ljava/util/List;", "size", NULL, arrayList_size },
    { "Ljava/util/List;", "clear", NULL, arrayList_clear },
    { "Ljava/util/List;", "iterator", NULL, arrayList_iterator },
    { "Ljava/util/Iterator;", "hasNext", NULL, iterator_hasNext },
    { "Ljava/util/Iterator;", "next", NULL, iterator_next },

    { "Ljava/lang/reflect/Array;", "newInstance", NULL, array_newInstance },
    { "Ljava/util/concurrent/Executors;", "newSingleThreadExecutor", NULL, executors_newSingleThreadExecutor },
    { "Ljava/util/concurrent/ExecutorService;", "execute", NULL, noop_stub },
    { "Landroid/util/Log;", "w", NULL, log_print },
    { "Landroid/util/Log;", "e", NULL, log_print },
    { "Landroid/util/Log;", "d", NULL, log_print },
    { "Landroid/util/Log;", "i", NULL, log_print },

    { NULL, NULL, NULL, NULL }
};

tl_dex_native_func tl_framework_lookup(const char *class_desc, const char *method_name, const char *shorty)
{
    if (!class_desc || !method_name) return NULL;
    /* First pass: exact shorty match */
    if (shorty) {
        for (int i = 0; s_native_methods[i].class_desc; i++) {
            if (!strcmp(s_native_methods[i].class_desc, class_desc) &&
                !strcmp(s_native_methods[i].method_name, method_name) &&
                s_native_methods[i].shorty &&
                !strcmp(s_native_methods[i].shorty, shorty)) {
                return s_native_methods[i].func;
            }
        }
    }
    /* Second pass: wild-card shorty */
    for (int i = 0; s_native_methods[i].class_desc; i++) {
        if (!strcmp(s_native_methods[i].class_desc, class_desc) &&
            !strcmp(s_native_methods[i].method_name, method_name) &&
            !s_native_methods[i].shorty) {
            return s_native_methods[i].func;
        }
    }
    return NULL;
}


bool tl_framework_init(tl_dex_context *ctx)
{
    tl_framework_state *st = calloc(1, sizeof(*st));
    ctx->framework_data = st;

    if (ctx->framebuffer && ctx->fb_width > 0 && ctx->fb_height > 0) {
#if defined(__APPLE__)
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        st->cg_ctx = CGBitmapContextCreate(ctx->framebuffer, ctx->fb_width, ctx->fb_height, 8,
                                           ctx->fb_width * 4, cs,
                                           kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(cs);
        if (st->cg_ctx) {
            /* Flip CoreGraphics to match Android top-left origin */
            CGContextTranslateCTM(st->cg_ctx, 0, ctx->fb_height);
            CGContextScaleCTM(st->cg_ctx, 1.0f, -1.0f);
        }
#endif
    }

    tl_dex_class *c_canvas = tl_dex_find_class(ctx, "Landroid/graphics/Canvas;");
    st->canvas_obj = tl_dex_alloc_object(c_canvas);

    tl_dex_class *c_act = tl_dex_find_class(ctx, "Landroid/app/Activity;");
    st->activity_obj = tl_dex_alloc_object(c_act);

    tl_dex_class *c_res = tl_dex_find_class(ctx, "Landroid/content/res/Resources;");
    st->resources_obj = tl_dex_alloc_object(c_res);

    tl_dex_class *c_ch = tl_dex_find_class(ctx, "Landroid/view/Choreographer;");
    st->choreographer_obj = tl_dex_alloc_object(c_ch);

    tl_dex_class *c_pref = tl_dex_find_class(ctx, "Landroid/content/SharedPreferences;");
    st->prefs_obj = tl_dex_alloc_object(c_pref);

    return true;
}

void tl_framework_cleanup(tl_dex_context *ctx)
{
    if (!ctx || !ctx->framework_data) return;
    tl_framework_state *st = ctx->framework_data;
#if defined(__APPLE__)
    if (st->cg_ctx) CGContextRelease(st->cg_ctx);
#endif
    free(st);
    ctx->framework_data = NULL;
}

void tl_framework_render_view(tl_dex_context *ctx)
{
    if (!ctx || !ctx->current_view) return;
    tl_framework_state *st = ctx->framework_data;
    if (!st || !st->canvas_obj) return;

    tl_dex_method *onDraw = tl_dex_find_method(ctx->current_view->clazz, "onDraw", "VL");
    if (onDraw) {
        tl_dex_val args[2];
        args[0].l = ctx->current_view;
        args[1].l = st->canvas_obj;
        tl_dex_invoke(ctx, onDraw, args, 2, NULL);
    }
}
