/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#include "husk-tl-egl.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "husk-tl-bionic.h"

void *tl_nwindow_native(void *window);
int tl_nwindow_width(void *window);
int tl_nwindow_height(void *window);

typedef void *EGLDisplay, *EGLSurface, *EGLContext, *EGLConfig, *EGLNativeWindowType;
typedef int32_t EGLint;
typedef unsigned EGLBoolean, EGLenum;
#define EGL_NONE 0x3038
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#define EGL_FALSE 0
#define EGL_TRUE 1

static struct {
    void *egl, *gles;
    bool ready;
    char frame_dir[512];
    int frame_every;
    bool latest_only;           /* overwrite latest.bmp instead of keeping every frame */
    atomic_ulong presented;
    EGLDisplay display;
    pthread_mutex_t lock;
} E = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* ANGLE's entry points */
static void *(*a_eglGetProcAddress)(const char *);
static EGLDisplay (*a_eglGetPlatformDisplayEXT)(EGLenum, void *, const EGLint *);
static EGLBoolean (*a_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
static EGLBoolean (*a_eglTerminate)(EGLDisplay);
static const char *(*a_eglQueryString)(EGLDisplay, EGLint);
static EGLBoolean (*a_eglGetConfigs)(EGLDisplay, EGLConfig *, EGLint, EGLint *);
static EGLBoolean (*a_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
static EGLBoolean (*a_eglGetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
static EGLContext (*a_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
static EGLBoolean (*a_eglDestroyContext)(EGLDisplay, EGLContext);
static EGLSurface (*a_eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
static EGLSurface (*a_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
static EGLBoolean (*a_eglDestroySurface)(EGLDisplay, EGLSurface);
static EGLBoolean (*a_eglQuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
static EGLBoolean (*a_eglSurfaceAttrib)(EGLDisplay, EGLSurface, EGLint, EGLint);
static EGLBoolean (*a_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLBoolean (*a_eglSwapBuffers)(EGLDisplay, EGLSurface);
static EGLBoolean (*a_eglSwapInterval)(EGLDisplay, EGLint);
static EGLContext (*a_eglGetCurrentContext)(void);
static EGLSurface (*a_eglGetCurrentSurface)(EGLint);
static EGLDisplay (*a_eglGetCurrentDisplay)(void);
static EGLBoolean (*a_eglBindAPI)(EGLenum);
static EGLenum (*a_eglQueryAPI)(void);
static EGLint (*a_eglGetError)(void);
static EGLBoolean (*a_eglWaitGL)(void);
static EGLBoolean (*a_eglWaitNative)(EGLint);
static EGLBoolean (*a_eglReleaseThread)(void);
static void (*a_glReadPixels)(int, int, int, int, unsigned, unsigned, void *);
static void (*a_glGetIntegerv)(unsigned, int *);
static void (*a_glBindFramebuffer)(unsigned, unsigned);
static void (*a_glPixelStorei)(unsigned, int);

#define LOAD(lib, name) do { a_##name = dlsym(lib, #name); if (!a_##name) tl_log_line("egl: ANGLE does not export " #name); } while (0)

bool tl_egl_init(const char *egl_path, const char *gles_path, const char *frame_dir, int frame_every)
{
    pthread_mutex_lock(&E.lock);
    if (E.ready) { pthread_mutex_unlock(&E.lock); return true; }
    E.egl = dlopen(egl_path, RTLD_NOW | RTLD_LOCAL);
    if (!E.egl) { tl_log_line("egl: cannot load %s: %s", egl_path, dlerror()); pthread_mutex_unlock(&E.lock); return false; }
    E.gles = gles_path ? dlopen(gles_path, RTLD_NOW | RTLD_LOCAL) : E.egl;
    if (!E.gles) { tl_log_line("egl: cannot load %s: %s", gles_path, dlerror()); pthread_mutex_unlock(&E.lock); return false; }
    void *g = E.egl;
    LOAD(g, eglGetProcAddress); LOAD(g, eglInitialize); LOAD(g, eglTerminate); LOAD(g, eglQueryString); LOAD(g, eglGetConfigs);
    LOAD(g, eglChooseConfig); LOAD(g, eglGetConfigAttrib); LOAD(g, eglCreateContext); LOAD(g, eglDestroyContext);
    LOAD(g, eglCreateWindowSurface); LOAD(g, eglCreatePbufferSurface); LOAD(g, eglDestroySurface); LOAD(g, eglQuerySurface);
    LOAD(g, eglSurfaceAttrib); LOAD(g, eglMakeCurrent); LOAD(g, eglSwapBuffers); LOAD(g, eglSwapInterval);
    LOAD(g, eglGetCurrentContext); LOAD(g, eglGetCurrentSurface); LOAD(g, eglGetCurrentDisplay); LOAD(g, eglBindAPI);
    LOAD(g, eglQueryAPI); LOAD(g, eglGetError); LOAD(g, eglWaitGL); LOAD(g, eglWaitNative); LOAD(g, eglReleaseThread);
    a_eglGetPlatformDisplayEXT = a_eglGetProcAddress ? a_eglGetProcAddress("eglGetPlatformDisplayEXT") : NULL;
    if (!a_eglGetPlatformDisplayEXT) { tl_log_line("egl: ANGLE has no eglGetPlatformDisplayEXT"); pthread_mutex_unlock(&E.lock); return false; }
    /* GLES functions this layer itself calls (frame capture) */
    a_glReadPixels = a_eglGetProcAddress("glReadPixels");
    a_glGetIntegerv = a_eglGetProcAddress("glGetIntegerv");
    a_glBindFramebuffer = a_eglGetProcAddress("glBindFramebuffer");
    a_glPixelStorei = a_eglGetProcAddress("glPixelStorei");
    if (frame_dir) { snprintf(E.frame_dir, sizeof(E.frame_dir), "%s", frame_dir); E.latest_only = frame_every < 0; E.frame_every = frame_every != 0 ? (frame_every < 0 ? -frame_every : frame_every) : 60; }
    E.ready = true;
    pthread_mutex_unlock(&E.lock);
    tl_log_line("egl: ANGLE loaded%s%s", frame_dir ? ", off-screen, frames to " : "", frame_dir ? frame_dir : "");
    return true;
}

unsigned long tl_egl_frames_presented(void) { return atomic_load(&E.presented); }

/* ------------------------------------------------------------ EGL wrappers */

static EGLDisplay w_eglGetDisplay(void *native)
{
    (void)native;
    pthread_mutex_lock(&E.lock);
    if (!E.display && E.ready) {
        const EGLint attribs[] = { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE };
        E.display = a_eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, NULL, attribs);
        tl_log_line("egl: display %p (ANGLE over Metal)", E.display);
    }
    EGLDisplay d = E.display;
    pthread_mutex_unlock(&E.lock);
    return d;
}

#define PASS_BOOL(name, params, args) static EGLBoolean w_##name params { return a_##name args; }
PASS_BOOL(eglInitialize, (EGLDisplay d, EGLint *a, EGLint *b), (d, a, b))
PASS_BOOL(eglTerminate, (EGLDisplay d), (d))
PASS_BOOL(eglGetConfigs, (EGLDisplay d, EGLConfig *c, EGLint n, EGLint *r), (d, c, n, r))
PASS_BOOL(eglChooseConfig, (EGLDisplay d, const EGLint *at, EGLConfig *c, EGLint n, EGLint *r), (d, at, c, n, r))
PASS_BOOL(eglGetConfigAttrib, (EGLDisplay d, EGLConfig c, EGLint at, EGLint *v), (d, c, at, v))
PASS_BOOL(eglDestroyContext, (EGLDisplay d, EGLContext c), (d, c))
PASS_BOOL(eglDestroySurface, (EGLDisplay d, EGLSurface s), (d, s))
PASS_BOOL(eglQuerySurface, (EGLDisplay d, EGLSurface s, EGLint at, EGLint *v), (d, s, at, v))
PASS_BOOL(eglSurfaceAttrib, (EGLDisplay d, EGLSurface s, EGLint at, EGLint v), (d, s, at, v))
PASS_BOOL(eglMakeCurrent, (EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c), (d, dr, rd, c))
PASS_BOOL(eglSwapInterval, (EGLDisplay d, EGLint i), (d, i))
PASS_BOOL(eglBindAPI, (EGLenum api), (api))
PASS_BOOL(eglWaitGL, (void), ())
PASS_BOOL(eglWaitNative, (EGLint e), (e))
PASS_BOOL(eglReleaseThread, (void), ())

static const char *w_eglQueryString(EGLDisplay d, EGLint name) { return a_eglQueryString(d, name); }
static EGLContext w_eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *at) { return a_eglCreateContext(d, c, share, at); }
static EGLContext w_eglGetCurrentContext(void) { return a_eglGetCurrentContext(); }
static EGLSurface w_eglGetCurrentSurface(EGLint w) { return a_eglGetCurrentSurface(w); }
static EGLDisplay w_eglGetCurrentDisplay(void) { return a_eglGetCurrentDisplay(); }
static EGLenum w_eglQueryAPI(void) { return a_eglQueryAPI(); }
static EGLint w_eglGetError(void) { return E.ready ? a_eglGetError() : 0x3001; }

/* The guest's ANativeWindow: a Metal layer on the phone, an off-screen buffer for tests. */
static EGLSurface w_eglCreateWindowSurface(EGLDisplay d, EGLConfig cfg, void *win, const EGLint *at)
{
    if (E.frame_dir[0]) {
        EGLint pb[] = { EGL_WIDTH, tl_nwindow_width(win), EGL_HEIGHT, tl_nwindow_height(win), EGL_NONE };
        EGLSurface s = a_eglCreatePbufferSurface(d, cfg, pb);
        tl_log_line("egl: window %dx%d -> off-screen surface %p", pb[1], pb[3], s);
        return s;
    }
    return a_eglCreateWindowSurface(d, cfg, tl_nwindow_native(win), at);
}
static EGLSurface w_eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint *at) { return a_eglCreatePbufferSurface(d, c, at); }

/* A frame as a BMP: bottom-up BGR from GL's bottom-up RGBA, so no flip is needed. */
static void save_frame(EGLDisplay d, EGLSurface s, unsigned long n)
{
    EGLint w = 0, h = 0;
    a_eglQuerySurface(d, s, EGL_WIDTH, &w);
    a_eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0 || !a_glReadPixels) return;
    uint8_t *rgba = malloc((size_t)w * h * 4);
    int old_fb = 0, old_pack = 0;
    a_glGetIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &old_fb);
    a_glGetIntegerv(0x0D05 /* GL_PACK_ALIGNMENT */, &old_pack);
    a_glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);
    a_glPixelStorei(0x0D05, 1);
    a_glReadPixels(0, 0, w, h, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, rgba);
    a_glPixelStorei(0x0D05, old_pack);
    a_glBindFramebuffer(0x8CA8, (unsigned)old_fb);
    char path[700], final_path[700] = "";
    if (E.latest_only) {
        snprintf(final_path, sizeof(final_path), "%s/latest.bmp", E.frame_dir);
        snprintf(path, sizeof(path), "%s/latest.tmp", E.frame_dir);
    } else {
        snprintf(path, sizeof(path), "%s/frame-%05lu.bmp", E.frame_dir, n);
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        uint32_t rowbytes = ((uint32_t)w * 3 + 3) & ~3u, size = 54 + rowbytes * (uint32_t)h;
        uint8_t hdr[54] = { 'B', 'M' };
        memcpy(hdr + 2, &size, 4); uint32_t off = 54; memcpy(hdr + 10, &off, 4);
        uint32_t dib = 40; memcpy(hdr + 14, &dib, 4); memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
        hdr[26] = 1; hdr[28] = 24; uint32_t img = rowbytes * (uint32_t)h; memcpy(hdr + 34, &img, 4);
        fwrite(hdr, 1, 54, f);
        uint8_t *row = calloc(1, rowbytes);
        for (int y = 0; y < h; y++) {
            const uint8_t *src = rgba + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) { row[x * 3] = src[x * 4 + 2]; row[x * 3 + 1] = src[x * 4 + 1]; row[x * 3 + 2] = src[x * 4]; }
            fwrite(row, 1, rowbytes, f);
        }
        free(row); fclose(f);
        if (final_path[0]) rename(path, final_path);
    }
    free(rgba);
}

static EGLBoolean w_eglSwapBuffers(EGLDisplay d, EGLSurface s)
{
    unsigned long n = atomic_fetch_add(&E.presented, 1) + 1;
    if (E.frame_dir[0] && (n % (unsigned long)E.frame_every == 0 || n <= 3)) save_frame(d, s, n);
    return E.frame_dir[0] ? EGL_TRUE : a_eglSwapBuffers(d, s);
}

/* Android extensions this ANGLE does not have: Swappy and Unity probe for them. */
static EGLBoolean w_eglPresentationTimeANDROID(EGLDisplay d, EGLSurface s, int64_t t) { (void)d; (void)s; (void)t; return EGL_TRUE; }
static EGLBoolean w_eglGetNextFrameIdANDROID(EGLDisplay d, EGLSurface s, uint64_t *id) { (void)d; (void)s; if (id) *id = 0; return EGL_FALSE; }
static EGLBoolean w_eglGetFrameTimestampsANDROID(EGLDisplay d, EGLSurface s, uint64_t id, EGLint n, const EGLint *t, int64_t *v) { (void)d; (void)s; (void)id; (void)n; (void)t; (void)v; return EGL_FALSE; }
static EGLBoolean w_eglGetCompositorTimingANDROID(EGLDisplay d, EGLSurface s, EGLint n, const EGLint *t, int64_t *v) { (void)d; (void)s; (void)n; (void)t; (void)v; return EGL_FALSE; }

/* ------------------------------------------------- GLES: stack-argument fixes */

/*
 * AAPCS64 gives each stack argument its own 8-byte slot; Apple's ABI packs small ones. A call
 * with two 4-byte stack arguments therefore lays them out differently, and the callee reads
 * the second from the wrong place. Each adapter takes its stack arguments as 8-byte values,
 * which is how the guest wrote them, and passes them on properly typed.
 */
static void (*r_glBlitFramebuffer)(int, int, int, int, int, int, int, int, unsigned, unsigned);
static void w_glBlitFramebuffer(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7, uint64_t mask, uint64_t filter)
{ r_glBlitFramebuffer(a0, a1, a2, a3, a4, a5, a6, a7, (unsigned)mask, (unsigned)filter); }

static void (*r_glTexSubImage3D)(unsigned, int, int, int, int, int, int, int, unsigned, unsigned, const void *);
static void w_glTexSubImage3D(unsigned t, int l, int x, int y, int z, int w, int h, int d, uint64_t fmt, uint64_t type, const void *px)
{ r_glTexSubImage3D(t, l, x, y, z, w, h, d, (unsigned)fmt, (unsigned)type, px); }

static void (*r_glCompressedTexSubImage3D)(unsigned, int, int, int, int, int, int, int, unsigned, int, const void *);
static void w_glCompressedTexSubImage3D(unsigned t, int l, int x, int y, int z, int w, int h, int d, uint64_t fmt, uint64_t size, const void *data)
{ r_glCompressedTexSubImage3D(t, l, x, y, z, w, h, d, (unsigned)fmt, (int)size, data); }

static void (*r_glCopyImageSubData)(unsigned, unsigned, int, int, int, int, unsigned, unsigned, int, int, int, int, int, int, int);
static void w_glCopyImageSubData(unsigned sn, unsigned st, int sl, int sx, int sy, int sz, unsigned dn, unsigned dt,
                                 uint64_t dl, uint64_t dx, uint64_t dy, uint64_t dz, uint64_t w, uint64_t h, uint64_t d)
{ r_glCopyImageSubData(sn, st, sl, sx, sy, sz, dn, dt, (int)dl, (int)dx, (int)dy, (int)dz, (int)w, (int)h, (int)d); }

/* GLboolean arguments: AAPCS64 callers do not extend them, Apple's callee may assume they are. */
static void (*r_glColorMask)(unsigned, unsigned, unsigned, unsigned);
static void w_glColorMask(unsigned r, unsigned g, unsigned b, unsigned a) { r_glColorMask(r & 0xff, g & 0xff, b & 0xff, a & 0xff); }
static void (*r_glDepthMask)(unsigned);
static void w_glDepthMask(unsigned f) { r_glDepthMask(f & 0xff); }
static void (*r_glVertexAttribPointer)(unsigned, int, unsigned, unsigned, int, const void *);
static void w_glVertexAttribPointer(unsigned i, int s, unsigned t, unsigned n, int st, const void *p) { r_glVertexAttribPointer(i, s, t, n & 0xff, st, p); }
static void (*r_glUniformMatrix2fv)(int, int, unsigned, const float *);
static void w_glUniformMatrix2fv(int l, int c, unsigned t, const float *v) { r_glUniformMatrix2fv(l, c, t & 0xff, v); }
static void (*r_glUniformMatrix3fv)(int, int, unsigned, const float *);
static void w_glUniformMatrix3fv(int l, int c, unsigned t, const float *v) { r_glUniformMatrix3fv(l, c, t & 0xff, v); }
static void (*r_glUniformMatrix4fv)(int, int, unsigned, const float *);
static void w_glUniformMatrix4fv(int l, int c, unsigned t, const float *v) { r_glUniformMatrix4fv(l, c, t & 0xff, v); }
static void (*r_glSampleCoverage)(float, unsigned);
static void w_glSampleCoverage(float v, unsigned i) { r_glSampleCoverage(v, i & 0xff); }

#define ADAPT(name) { #name, (void *)w_##name, (void **)&r_##name }
static const struct { const char *name; void *wrap; void **real; } k_adapt[] = {
    ADAPT(glBlitFramebuffer), ADAPT(glTexSubImage3D), ADAPT(glCompressedTexSubImage3D), ADAPT(glCopyImageSubData),
    ADAPT(glColorMask), ADAPT(glDepthMask), ADAPT(glVertexAttribPointer), ADAPT(glUniformMatrix2fv),
    ADAPT(glUniformMatrix3fv), ADAPT(glUniformMatrix4fv), ADAPT(glSampleCoverage),
};

static const struct { const char *name; void *fn; } k_egl[] = {
    { "eglGetDisplay", w_eglGetDisplay }, { "eglInitialize", w_eglInitialize }, { "eglTerminate", w_eglTerminate },
    { "eglQueryString", w_eglQueryString }, { "eglGetConfigs", w_eglGetConfigs }, { "eglChooseConfig", w_eglChooseConfig },
    { "eglGetConfigAttrib", w_eglGetConfigAttrib }, { "eglCreateContext", w_eglCreateContext },
    { "eglDestroyContext", w_eglDestroyContext }, { "eglCreateWindowSurface", w_eglCreateWindowSurface },
    { "eglCreatePbufferSurface", w_eglCreatePbufferSurface }, { "eglDestroySurface", w_eglDestroySurface },
    { "eglQuerySurface", w_eglQuerySurface }, { "eglSurfaceAttrib", w_eglSurfaceAttrib }, { "eglMakeCurrent", w_eglMakeCurrent },
    { "eglSwapBuffers", w_eglSwapBuffers }, { "eglSwapInterval", w_eglSwapInterval },
    { "eglGetCurrentContext", w_eglGetCurrentContext }, { "eglGetCurrentSurface", w_eglGetCurrentSurface },
    { "eglGetCurrentDisplay", w_eglGetCurrentDisplay }, { "eglBindAPI", w_eglBindAPI }, { "eglQueryAPI", w_eglQueryAPI },
    { "eglGetError", w_eglGetError }, { "eglWaitGL", w_eglWaitGL }, { "eglWaitNative", w_eglWaitNative },
    { "eglReleaseThread", w_eglReleaseThread },
    { "eglPresentationTimeANDROID", w_eglPresentationTimeANDROID }, { "eglGetNextFrameIdANDROID", w_eglGetNextFrameIdANDROID },
    { "eglGetFrameTimestampsANDROID", w_eglGetFrameTimestampsANDROID }, { "eglGetCompositorTimingANDROID", w_eglGetCompositorTimingANDROID },
};

void *tl_egl_resolve(const char *name)
{
    for (size_t i = 0; i < sizeof(k_egl) / sizeof(k_egl[0]); i++) if (!strcmp(k_egl[i].name, name)) return k_egl[i].fn;
    if (!E.ready || !a_eglGetProcAddress) return NULL;
    void *real = a_eglGetProcAddress(name);
    if (!real && E.gles) real = dlsym(E.gles, name);
    if (!real) return NULL;
    for (size_t i = 0; i < sizeof(k_adapt) / sizeof(k_adapt[0]); i++) {
        if (!strcmp(k_adapt[i].name, name)) { *k_adapt[i].real = real; return k_adapt[i].wrap; }
    }
    return real;
}

/* eglGetProcAddress for the guest */
static void *w_eglGetProcAddress(const char *name) { return tl_egl_resolve(name); }
static const unsigned char *w_glGetString(unsigned name)
{
    const unsigned char *(*f)(unsigned) = tl_egl_resolve("glGetString");
    return f ? f(name) : NULL;
}

const tl_bionic_entry tl_tab_egl[] = {
    TL_WRAP("eglGetProcAddress", w_eglGetProcAddress), TL_WRAP("glGetString", w_glGetString),
    { "eglGetDisplay", w_eglGetDisplay }, { "eglInitialize", w_eglInitialize }, { "eglTerminate", w_eglTerminate },
    { "eglQueryString", w_eglQueryString }, { "eglGetConfigs", w_eglGetConfigs }, { "eglChooseConfig", w_eglChooseConfig },
    { "eglGetConfigAttrib", w_eglGetConfigAttrib }, { "eglCreateContext", w_eglCreateContext },
    { "eglDestroyContext", w_eglDestroyContext }, { "eglCreateWindowSurface", w_eglCreateWindowSurface },
    { "eglCreatePbufferSurface", w_eglCreatePbufferSurface }, { "eglDestroySurface", w_eglDestroySurface },
    { "eglQuerySurface", w_eglQuerySurface }, { "eglSurfaceAttrib", w_eglSurfaceAttrib }, { "eglMakeCurrent", w_eglMakeCurrent },
    { "eglSwapBuffers", w_eglSwapBuffers }, { "eglSwapInterval", w_eglSwapInterval },
    { "eglGetCurrentContext", w_eglGetCurrentContext }, { "eglGetCurrentSurface", w_eglGetCurrentSurface },
    { "eglGetCurrentDisplay", w_eglGetCurrentDisplay }, { "eglBindAPI", w_eglBindAPI }, { "eglQueryAPI", w_eglQueryAPI },
    { "eglGetError", w_eglGetError }, { "eglWaitGL", w_eglWaitGL }, { "eglWaitNative", w_eglWaitNative },
    { "eglReleaseThread", w_eglReleaseThread },
    TL_END
};
