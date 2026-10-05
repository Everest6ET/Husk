/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The NDK's own libraries: libandroid (looper, assets, windows, sensors), libmediandk,
 * and zlib. EGL and GLES are in husk-tl-egl.c.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-bionic.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "husk-tl-internal.h"
#include "husk-tl-ld.h"

/* ---------------------------------------------------------------- looper */

/*
 * Android's ALooper is a per-thread event loop that other threads wake with a write
 * to a pipe. Unity's main thread polls it between frames. This one has the wake pipe
 * and nothing else registered on it: no file descriptors, no callbacks.
 */
typedef struct tl_looper { int wake[2]; atomic_int refs; pthread_t thread; struct tl_looper *next; } tl_looper;
static __thread tl_looper *t_looper;

enum { ALOOPER_POLL_WAKE = -1, ALOOPER_POLL_CALLBACK = -2, ALOOPER_POLL_TIMEOUT = -3, ALOOPER_POLL_ERROR = -4 };

static tl_looper *looper_make(void)
{
    tl_looper *l = calloc(1, sizeof(*l));
    if (pipe(l->wake) == 0) {
        fcntl(l->wake[0], F_SETFL, O_NONBLOCK);
        fcntl(l->wake[1], F_SETFL, O_NONBLOCK);
    }
    atomic_init(&l->refs, 1);
    l->thread = pthread_self();
    return l;
}
static void *b_ALooper_prepare(int opts) { (void)opts; if (!t_looper) t_looper = looper_make(); return t_looper; }
static void *b_ALooper_forThread(void) { return t_looper; }
static void b_ALooper_acquire(tl_looper *l) { if (l) atomic_fetch_add(&l->refs, 1); }
static void b_ALooper_release(tl_looper *l) { (void)l; }
static void b_ALooper_wake(tl_looper *l)
{
    if (!l) return;
    char c = 1;
    (void)!write(l->wake[1], &c, 1);
}
static int b_ALooper_pollOnce(int timeout_ms, int *out_fd, int *out_events, void **out_data)
{
    tl_looper *l = t_looper;
    if (out_fd) *out_fd = -1;
    if (out_events) *out_events = 0;
    if (out_data) *out_data = NULL;
    if (!l) return ALOOPER_POLL_ERROR;
    struct pollfd p = { l->wake[0], POLLIN, 0 };
    int r = poll(&p, 1, timeout_ms);
    if (r < 0) return ALOOPER_POLL_ERROR;
    if (r == 0) return ALOOPER_POLL_TIMEOUT;
    char buf[64];
    while (read(l->wake[0], buf, sizeof(buf)) > 0) {}
    return ALOOPER_POLL_WAKE;
}

/* ---------------------------------------------------------------- assets */

typedef struct { const uint8_t *data; size_t len; size_t pos; uint8_t *owned; } tl_asset;

static void *b_AAssetManager_fromJava(void *env, void *obj) { (void)env; (void)obj; static int mgr; return &mgr; }

static void *b_AAssetManager_open(void *mgr, const char *name, int mode)
{
    (void)mgr; (void)mode;
    char path[1024];
    snprintf(path, sizeof(path), "assets/%s", name);
    for (int i = 0;; i++) {
        const tl_zip *z = tl_ld_apk_at(i);
        if (!z) break;
        const tl_zip_entry *e = tl_zip_find(z, path);
        if (!e) continue;
        const uint8_t *data; size_t n; bool owned; char err[160];
        if (!tl_zip_data(z, e, (size_t)2 << 30, &data, &n, &owned, err, sizeof(err))) {
            tl_log_line("assets: %s: %s", name, err);
            return NULL;
        }
        tl_asset *a = calloc(1, sizeof(*a));
        a->data = data; a->len = n;
        if (owned) a->owned = (uint8_t *)data;
        return a;
    }
    return NULL;
}
static void b_AAsset_close(tl_asset *a) { if (a) { free(a->owned); free(a); } }
static int b_AAsset_read(tl_asset *a, void *buf, size_t n)
{
    size_t left = a->len - a->pos;
    if (n > left) n = left;
    memcpy(buf, a->data + a->pos, n);
    a->pos += n;
    return (int)n;
}
static long b_AAsset_getLength(tl_asset *a) { return (long)a->len; }
static long b_AAsset_getRemainingLength(tl_asset *a) { return (long)(a->len - a->pos); }
static const void *b_AAsset_getBuffer(tl_asset *a) { return a->data; }
static long b_AAsset_seek(tl_asset *a, long off, int whence)
{
    long base = whence == 0 ? 0 : whence == 1 ? (long)a->pos : (long)a->len;
    long np = base + off;
    if (np < 0 || (size_t)np > a->len) return -1;
    a->pos = (size_t)np;
    return np;
}
static int b_AAsset_isAllocated(tl_asset *a) { return a->owned != NULL; }

/* ---------------------------------------------------------------- windows */

typedef struct tl_nwindow { atomic_int refs; int width, height, format; void *layer; } tl_nwindow;
static tl_nwindow g_window = { 1, 1080, 2400, 1, NULL };

void tl_nwindow_configure(int w, int h, void *layer) { g_window.width = w; g_window.height = h; g_window.layer = layer; }
void *tl_nwindow_get(void) { atomic_fetch_add(&g_window.refs, 1); return &g_window; }
void *tl_nwindow_native(void *window) { return window ? ((tl_nwindow *)window)->layer : NULL; }
int tl_nwindow_width(void *window) { return window ? ((tl_nwindow *)window)->width : 0; }
int tl_nwindow_height(void *window) { return window ? ((tl_nwindow *)window)->height : 0; }

static void *b_ANativeWindow_fromSurface(void *env, void *surface) { (void)env; (void)surface; atomic_fetch_add(&g_window.refs, 1); return &g_window; }
static void b_ANativeWindow_acquire(tl_nwindow *w) { if (w) atomic_fetch_add(&w->refs, 1); }
static void b_ANativeWindow_release(tl_nwindow *w) { if (w) atomic_fetch_sub(&w->refs, 1); }
static int b_ANativeWindow_getWidth(tl_nwindow *w) { return w ? w->width : 0; }
static int b_ANativeWindow_getHeight(tl_nwindow *w) { return w ? w->height : 0; }
static int b_ANativeWindow_getFormat(tl_nwindow *w) { return w ? w->format : 0; }
static int b_ANativeWindow_setBuffersGeometry(tl_nwindow *w, int width, int height, int format)
{
    (void)width; (void)height; (void)format; (void)w;
    return 0;
}
static void *b_ANativeWindow_toSurface(void *env, void *w) { (void)env; (void)w; return NULL; }

/* ---------------------------------------------------------------- sensors */

static void *b_ASensorManager_getInstance(void) { static int m; return &m; }
static void *b_ASensorManager_getDefaultSensor(void *m, int type) { (void)m; (void)type; return NULL; }
static int b_ASensorManager_getSensorList(void *m, const void ***list) { (void)m; if (list) *list = NULL; return 0; }
static void *b_ASensorManager_createEventQueue(void *m, void *looper, int ident, void *cb, void *data)
{
    (void)m; (void)looper; (void)ident; (void)cb; (void)data;
    static int q; return &q;
}
static int b_ASensorManager_destroyEventQueue(void *m, void *q) { (void)m; (void)q; return 0; }
static int b_ASensorEventQueue_enableSensor(void *q, void *s) { (void)q; (void)s; return -1; }
static int b_ASensorEventQueue_disableSensor(void *q, void *s) { (void)q; (void)s; return -1; }
static int b_ASensorEventQueue_setEventRate(void *q, void *s, int us) { (void)q; (void)s; (void)us; return -1; }
static int b_ASensorEventQueue_hasEvents(void *q) { (void)q; return 0; }
static long b_ASensorEventQueue_getEvents(void *q, void *ev, size_t n) { (void)q; (void)ev; (void)n; return 0; }
static const char *b_ASensor_getName(void *s) { (void)s; return ""; }
static const char *b_ASensor_getVendor(void *s) { (void)s; return ""; }
static int b_ASensor_getType(void *s) { (void)s; return 0; }
static float b_ASensor_getResolution(void *s) { (void)s; return 0.f; }
static int b_ASensor_getMinDelay(void *s) { (void)s; return 0; }

/* ------------------------------------------------------------ media (none) */

#define MEDIA_ERR (-10000)
static void *b_media_null(void) { tl_note_once("libmediandk: media decoding is not provided (returning NULL)"); return NULL; }
static int b_media_err(void) { return MEDIA_ERR; }
static int b_media_zero(void) { return 0; }

#define KEY(sym, text) static const char *g_##sym = text;
KEY(AMEDIAFORMAT_KEY_CHANNEL_COUNT, "channel-count") KEY(AMEDIAFORMAT_KEY_COLOR_FORMAT, "color-format")
KEY(AMEDIAFORMAT_KEY_COLOR_RANGE, "color-range") KEY(AMEDIAFORMAT_KEY_COLOR_STANDARD, "color-standard")
KEY(AMEDIAFORMAT_KEY_DURATION, "durationUs") KEY(AMEDIAFORMAT_KEY_ENCODER_DELAY, "encoder-delay")
KEY(AMEDIAFORMAT_KEY_FRAME_RATE, "frame-rate") KEY(AMEDIAFORMAT_KEY_HEIGHT, "height")
KEY(AMEDIAFORMAT_KEY_LANGUAGE, "language") KEY(AMEDIAFORMAT_KEY_MIME, "mime")
KEY(AMEDIAFORMAT_KEY_ROTATION, "rotation-degrees") KEY(AMEDIAFORMAT_KEY_SAMPLE_RATE, "sample-rate")
KEY(AMEDIAFORMAT_KEY_SLICE_HEIGHT, "slice-height") KEY(AMEDIAFORMAT_KEY_STRIDE, "stride")
KEY(AMEDIAFORMAT_KEY_WIDTH, "width")

#define MEDIA_NULL(n)  TL_WRAP(n, b_media_null)
#define MEDIA_ERRF(n)  TL_WRAP(n, b_media_err)
#define MEDIA_DATA(n)  TL_DATA(#n, &g_##n)

const tl_bionic_entry tl_tab_ndk[] = {
    /* zlib */
    TL_DIRECT(deflate), TL_DIRECT(deflateEnd), TL_DIRECT(deflateInit2_), TL_DIRECT(deflateInit_),
    TL_DIRECT(inflate), TL_DIRECT(inflateEnd), TL_DIRECT(inflateInit2_), TL_DIRECT(inflateInit_), TL_DIRECT(zError),
    /* looper */
    TL_WRAP("ALooper_prepare", b_ALooper_prepare), TL_WRAP("ALooper_forThread", b_ALooper_forThread),
    TL_WRAP("ALooper_acquire", b_ALooper_acquire), TL_WRAP("ALooper_release", b_ALooper_release),
    TL_WRAP("ALooper_wake", b_ALooper_wake), TL_WRAP("ALooper_pollOnce", b_ALooper_pollOnce),
    /* assets */
    TL_WRAP("AAssetManager_fromJava", b_AAssetManager_fromJava), TL_WRAP("AAssetManager_open", b_AAssetManager_open),
    TL_WRAP("AAsset_close", b_AAsset_close), TL_WRAP("AAsset_read", b_AAsset_read), TL_WRAP("AAsset_getLength", b_AAsset_getLength),
    TL_WRAP("AAsset_getLength64", b_AAsset_getLength), TL_WRAP("AAsset_getRemainingLength", b_AAsset_getRemainingLength),
    TL_WRAP("AAsset_getRemainingLength64", b_AAsset_getRemainingLength), TL_WRAP("AAsset_getBuffer", b_AAsset_getBuffer),
    TL_WRAP("AAsset_seek", b_AAsset_seek), TL_WRAP("AAsset_seek64", b_AAsset_seek), TL_WRAP("AAsset_isAllocated", b_AAsset_isAllocated),
    /* windows */
    TL_WRAP("ANativeWindow_fromSurface", b_ANativeWindow_fromSurface), TL_WRAP("ANativeWindow_acquire", b_ANativeWindow_acquire),
    TL_WRAP("ANativeWindow_release", b_ANativeWindow_release), TL_WRAP("ANativeWindow_getWidth", b_ANativeWindow_getWidth),
    TL_WRAP("ANativeWindow_getHeight", b_ANativeWindow_getHeight), TL_WRAP("ANativeWindow_getFormat", b_ANativeWindow_getFormat),
    TL_WRAP("ANativeWindow_setBuffersGeometry", b_ANativeWindow_setBuffersGeometry),
    TL_WRAP("ANativeWindow_toSurface", b_ANativeWindow_toSurface),
    /* sensors */
    TL_WRAP("ASensorManager_getInstance", b_ASensorManager_getInstance), TL_WRAP("ASensorManager_getDefaultSensor", b_ASensorManager_getDefaultSensor),
    TL_WRAP("ASensorManager_getSensorList", b_ASensorManager_getSensorList), TL_WRAP("ASensorManager_createEventQueue", b_ASensorManager_createEventQueue),
    TL_WRAP("ASensorManager_destroyEventQueue", b_ASensorManager_destroyEventQueue), TL_WRAP("ASensorEventQueue_enableSensor", b_ASensorEventQueue_enableSensor),
    TL_WRAP("ASensorEventQueue_disableSensor", b_ASensorEventQueue_disableSensor), TL_WRAP("ASensorEventQueue_setEventRate", b_ASensorEventQueue_setEventRate),
    TL_WRAP("ASensorEventQueue_hasEvents", b_ASensorEventQueue_hasEvents), TL_WRAP("ASensorEventQueue_getEvents", b_ASensorEventQueue_getEvents),
    TL_WRAP("ASensor_getName", b_ASensor_getName), TL_WRAP("ASensor_getVendor", b_ASensor_getVendor), TL_WRAP("ASensor_getType", b_ASensor_getType),
    TL_WRAP("ASensor_getResolution", b_ASensor_getResolution), TL_WRAP("ASensor_getMinDelay", b_ASensor_getMinDelay),
    /* media */
    MEDIA_NULL("AMediaCodec_createDecoderByType"), MEDIA_NULL("AMediaExtractor_new"), MEDIA_NULL("AMediaCodec_getOutputFormat"),
    MEDIA_NULL("AMediaExtractor_getTrackFormat"), MEDIA_NULL("AMediaCodec_getInputBuffer"), MEDIA_NULL("AMediaCodec_getOutputBuffer"),
    MEDIA_ERRF("AMediaCodec_configure"), MEDIA_ERRF("AMediaCodec_start"), MEDIA_ERRF("AMediaCodec_stop"), MEDIA_ERRF("AMediaCodec_flush"),
    MEDIA_ERRF("AMediaCodec_delete"), MEDIA_ERRF("AMediaCodec_dequeueInputBuffer"), MEDIA_ERRF("AMediaCodec_dequeueOutputBuffer"),
    MEDIA_ERRF("AMediaCodec_queueInputBuffer"), MEDIA_ERRF("AMediaCodec_releaseOutputBuffer"),
    MEDIA_ERRF("AMediaExtractor_setDataSource"), MEDIA_ERRF("AMediaExtractor_setDataSourceFd"), MEDIA_ERRF("AMediaExtractor_selectTrack"),
    MEDIA_ERRF("AMediaExtractor_seekTo"), MEDIA_ERRF("AMediaExtractor_delete"), MEDIA_ERRF("AMediaExtractor_advance"),
    MEDIA_ERRF("AMediaExtractor_readSampleData"), MEDIA_ERRF("AMediaExtractor_getSampleTime"), MEDIA_ERRF("AMediaExtractor_getSampleTrackIndex"),
    TL_WRAP("AMediaExtractor_getTrackCount", b_media_zero), MEDIA_ERRF("AMediaFormat_delete"), MEDIA_ERRF("AMediaFormat_getFloat"),
    MEDIA_ERRF("AMediaFormat_getInt32"), MEDIA_ERRF("AMediaFormat_getInt64"), MEDIA_ERRF("AMediaFormat_getString"),
    MEDIA_ERRF("AMediaFormat_setInt32"),
    MEDIA_DATA(AMEDIAFORMAT_KEY_CHANNEL_COUNT), MEDIA_DATA(AMEDIAFORMAT_KEY_COLOR_FORMAT), MEDIA_DATA(AMEDIAFORMAT_KEY_COLOR_RANGE),
    MEDIA_DATA(AMEDIAFORMAT_KEY_COLOR_STANDARD), MEDIA_DATA(AMEDIAFORMAT_KEY_DURATION), MEDIA_DATA(AMEDIAFORMAT_KEY_ENCODER_DELAY),
    MEDIA_DATA(AMEDIAFORMAT_KEY_FRAME_RATE), MEDIA_DATA(AMEDIAFORMAT_KEY_HEIGHT), MEDIA_DATA(AMEDIAFORMAT_KEY_LANGUAGE),
    MEDIA_DATA(AMEDIAFORMAT_KEY_MIME), MEDIA_DATA(AMEDIAFORMAT_KEY_ROTATION), MEDIA_DATA(AMEDIAFORMAT_KEY_SAMPLE_RATE),
    MEDIA_DATA(AMEDIAFORMAT_KEY_SLICE_HEIGHT), MEDIA_DATA(AMEDIAFORMAT_KEY_STRIDE), MEDIA_DATA(AMEDIAFORMAT_KEY_WIDTH),
    TL_END
};
