/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-tl-vulkan.h"

#import <QuartzCore/CAMetalLayer.h>
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <dispatch/dispatch.h>
#endif
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

/* Only the parts of the Vulkan structures this layer reads or writes; they are the same on the guest and the host. */
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; const void *pApplicationInfo; uint32_t layerCount; const char *const *layers; uint32_t extCount; const char *const *exts; } vk_instance_ci;
typedef struct { uint32_t sType; const void *pNext; const char *appName; uint32_t appVersion; const char *engineName; uint32_t engineVersion; uint32_t apiVersion; } vk_app_info;
typedef struct { char name[256]; uint32_t specVersion; } vk_ext_props;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; void *window; } vk_android_surface_ci;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; const void *pLayer; } vk_metal_surface_ci;

#define VK_SUCCESS 0
#define VK_INCOMPLETE 5
#define VK_ERROR_EXTENSION_NOT_PRESENT (-7)
#define VK_STYPE_METAL_SURFACE_CI 1000217000

typedef void *(*pfn_gipa)(void *, const char *);
typedef int (*pfn_enum_ext)(const char *, uint32_t *, vk_ext_props *);
typedef int (*pfn_create_instance)(const vk_instance_ci *, const void *, void **);
typedef int (*pfn_create_metal_surface)(void *, const vk_metal_surface_ci *, const void *, uint64_t *);

static struct {
    pthread_once_t once;
    char path[1024];
    bool have_path;
    void *lib;
    pfn_gipa gipa;
    int trace;
    char frame_dir[700];
    int frame_every;
    atomic_ulong frames;
} V = { .once = PTHREAD_ONCE_INIT };

#define LOG(...) do { if (V.trace) tl_log_line(__VA_ARGS__); } while (0)

void tl_vk_configure(const char *path, const char *frame_dir, int frame_every)
{
    if (path) { snprintf(V.path, sizeof(V.path), "%s", path); V.have_path = true; }
    if (frame_dir) { snprintf(V.frame_dir, sizeof(V.frame_dir), "%s", frame_dir); V.frame_every = frame_every > 0 ? frame_every : 60; }
}

static void vk_load(void)
{
    V.trace = getenv("TL_VK_TRACE") ? 1 : 0;
    V.lib = V.have_path ? dlopen(V.path, RTLD_NOW | RTLD_LOCAL) : RTLD_DEFAULT;
    V.gipa = V.lib ? (pfn_gipa)dlsym(V.lib, "vkGetInstanceProcAddr") : NULL;
    tl_log_line("vulkan: %s", V.gipa ? (V.have_path ? V.path : "MoltenVK linked into the process") : "MoltenVK not found");
}
static bool vk_ready(void) { pthread_once(&V.once, vk_load); return V.gipa != NULL; }
bool tl_vk_available(void) { return V.have_path && vk_ready(); }

/* MoltenVK's own list of instance extensions */
static vk_ext_props *mvk_instance_exts(uint32_t *n)
{
    *n = 0;
    pfn_enum_ext f = (pfn_enum_ext)V.gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (!f || f(NULL, n, NULL) != VK_SUCCESS) return NULL;
    vk_ext_props *p = calloc(*n + 1, sizeof(vk_ext_props));
    if (f(NULL, n, p) < 0) { free(p); *n = 0; return NULL; }
    return p;
}

/* The guest's list with Android's surface extension swapped for the Metal one and anything MoltenVK lacks dropped. */
static int w_vkCreateInstance(const vk_instance_ci *ci, const void *alloc, void **out)
{
    uint32_t nhave = 0;
    vk_ext_props *have = mvk_instance_exts(&nhave);
    const char **ext = calloc(ci->extCount + 2, sizeof(char *));
    uint32_t n = 0;
    bool metal = false;
    for (uint32_t i = 0; i < ci->extCount; i++) {
        const char *e = ci->exts[i];
        if (!strcmp(e, "VK_KHR_android_surface")) e = "VK_EXT_metal_surface";
        bool ok = false;
        for (uint32_t j = 0; j < nhave; j++) if (!strcmp(have[j].name, e)) { ok = true; break; }
        if (!ok) { tl_log_line("vulkan: instance extension %s is not offered by MoltenVK, left out", e); continue; }
        if (!strcmp(e, "VK_EXT_metal_surface")) { if (metal) continue; metal = true; }
        ext[n++] = e;
    }
    vk_instance_ci copy = *ci;
    /* MoltenVK hands out a core entry point only when the instance asked for the version that has it, where Android's loader does not mind:
     * an engine that creates a 1.0 instance and then looks up vkGetPhysicalDeviceMemoryProperties2 gets NULL from it. So ask for 1.2. */
    vk_app_info app = { 0 };
    if (ci->pApplicationInfo) app = *(const vk_app_info *)ci->pApplicationInfo;
    else app.sType = 0;
    if (app.apiVersion < ((1u << 22) | (2u << 12))) app.apiVersion = (1u << 22) | (2u << 12);
    copy.pApplicationInfo = &app;
    copy.extCount = n; copy.exts = ext;
    copy.layerCount = 0; copy.layers = NULL;
    pfn_create_instance create = (pfn_create_instance)V.gipa(NULL, "vkCreateInstance");
    int r = create ? create(&copy, alloc, out) : -3;
    tl_log_line("vulkan: vkCreateInstance(%u extensions) -> %d", n, r);
    free(ext); free(have);
    return r;
}

static int w_vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count, vk_ext_props *props)
{
    if (layer) { *count = 0; return VK_SUCCESS; }
    uint32_t nhave = 0;
    vk_ext_props *have = mvk_instance_exts(&nhave);
    uint32_t total = nhave;
    vk_ext_props *all = calloc(nhave + 1, sizeof(vk_ext_props));
    if (nhave) memcpy(all, have, nhave * sizeof(vk_ext_props));
    bool has_android = false;
    for (uint32_t i = 0; i < nhave; i++) if (!strcmp(all[i].name, "VK_KHR_android_surface")) has_android = true;
    if (!has_android) { snprintf(all[total].name, sizeof(all[total].name), "VK_KHR_android_surface"); all[total].specVersion = 6; total++; }
    int r = VK_SUCCESS;
    if (!props) *count = total;
    else {
        uint32_t k = *count < total ? *count : total;
        memcpy(props, all, k * sizeof(vk_ext_props));
        if (k < total) r = VK_INCOMPLETE;
        *count = k;
    }
    free(all); free(have);
    return r;
}

/* On the phone the window's layer is the app's own; for a test on a Mac there is no view, so make one the size of the window. */
static void *layer_for(void *window)
{
    void *layer = tl_nwindow_native(window);
    if (layer) return layer;
    static CAMetalLayer *offscreen;
    if (!offscreen) {
        offscreen = [CAMetalLayer layer];
        offscreen.frame = CGRectMake(0, 0, tl_nwindow_width(window), tl_nwindow_height(window));
        offscreen.contentsScale = 1.0;
        offscreen.drawableSize = CGSizeMake(tl_nwindow_width(window), tl_nwindow_height(window));
        offscreen.framebufferOnly = NO;
        CFRetain((__bridge CFTypeRef)offscreen);
    }
    return (__bridge void *)offscreen;
}

static int w_vkCreateAndroidSurfaceKHR(void *instance, const vk_android_surface_ci *ci, const void *alloc, uint64_t *surface)
{
    pfn_create_metal_surface create = (pfn_create_metal_surface)V.gipa(instance, "vkCreateMetalSurfaceEXT");
    if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
    void *layer = layer_for(ci->window);
    vk_metal_surface_ci m = { VK_STYPE_METAL_SURFACE_CI, NULL, 0, layer };
    int r = create(instance, &m, alloc, surface);
    tl_log_line("vulkan: surface on layer %p (%dx%d) -> %d", layer, tl_nwindow_width(ci->window), tl_nwindow_height(ci->window), r);
    return r;
}

#if TARGET_OS_OSX
#import <Metal/Metal.h>
/* A test on a Mac has no view to look at: the frame about to be presented is read back from its swapchain image's Metal texture and written out like the GL path's
 * frames (latest.bmp, 24-bit). The queue is waited idle first, so the texture holds the finished frame. */
static void capture_frame(void *queue, const void *info)
{
    if (!V.frame_dir[0] || !V.lib) return;
    typedef int (*pfn_wait)(void *);
    typedef int (*pfn_get_images)(void *, uint64_t, uint32_t *, uint64_t *);
    typedef int (*pfn_get_tex)(uint64_t, void *);
    pfn_wait wait_idle = (pfn_wait)dlsym(V.lib, "vkQueueWaitIdle");
    pfn_get_images get_images = (pfn_get_images)dlsym(V.lib, "vkGetSwapchainImagesKHR");
    pfn_get_tex get_tex = (pfn_get_tex)dlsym(V.lib, "vkGetMTLTextureMVK");
    if (!wait_idle || !get_images || !get_tex) { tl_log_line("vulkan: capture: missing entry points (%p %p %p)", (void *)wait_idle, (void *)get_images, (void *)get_tex); return; }
    const uint8_t *pi = info;
    uint32_t nsw; memcpy(&nsw, pi + 32, 4);
    const uint64_t *sws; memcpy(&sws, pi + 40, 8);
    const uint32_t *idx; memcpy(&idx, pi + 48, 8);
    if (nsw < 1 || !sws || !idx) return;
    wait_idle(queue);
    uint32_t cnt = 0; get_images(NULL, sws[0], &cnt, NULL);
    if (idx[0] >= cnt || cnt > 8) { tl_log_line("vulkan: capture: image %u of %u", idx[0], cnt); return; }
    uint64_t imgs[8]; get_images(NULL, sws[0], &cnt, imgs);
    void *raw = NULL;
    get_tex(imgs[idx[0]], &raw);                          /* returns void: the texture is what it wrote */
    if (!raw) return;
    id<MTLTexture> tex = (__bridge id<MTLTexture>)raw;
    int w = (int)tex.width, h = (int)tex.height;
    static int said; if (said++ < 2) tl_log_line("vulkan: capture %dx%d format %d storage %d", w, h, (int)tex.pixelFormat, (int)tex.storageMode);
    if (tex.storageMode == MTLStorageModePrivate || w <= 0 || h <= 0) return;
    size_t bpr = (size_t)w * 4; uint8_t *px = malloc(bpr * h);
    [tex getBytes:px bytesPerRow:bpr fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    char tmp[760], fin[760]; snprintf(tmp, sizeof(tmp), "%s/latest.tmp", V.frame_dir); snprintf(fin, sizeof(fin), "%s/latest.bmp", V.frame_dir);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        uint32_t rowbytes = ((uint32_t)w * 3 + 3) & ~3u, size = 54 + rowbytes * (uint32_t)h;
        uint8_t hdr[54] = { 'B', 'M' };
        memcpy(hdr + 2, &size, 4); uint32_t off = 54; memcpy(hdr + 10, &off, 4);
        uint32_t dib = 40; memcpy(hdr + 14, &dib, 4); memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
        hdr[26] = 1; hdr[28] = 24; uint32_t img = rowbytes * (uint32_t)h; memcpy(hdr + 34, &img, 4);
        fwrite(hdr, 1, 54, f);
        uint8_t *row = calloc(1, rowbytes);
        for (int y = h - 1; y >= 0; y--) {                         /* BMP is bottom-up; the drawable is BGRA */
            const uint8_t *src = px + (size_t)y * bpr;
            for (int x = 0; x < w; x++) { row[x * 3] = src[x * 4]; row[x * 3 + 1] = src[x * 4 + 1]; row[x * 3 + 2] = src[x * 4 + 2]; }
            fwrite(row, 1, rowbytes, f);
        }
        free(row); fclose(f); rename(tmp, fin);
    }
    free(px);
}
#endif

typedef int (*pfn_queue_present)(void *, const void *);
static int w_vkQueuePresentKHR(void *queue, const void *info)
{
    static pfn_queue_present real;
    if (!real && V.lib) real = (pfn_queue_present)dlsym(V.lib, "vkQueuePresentKHR");
    unsigned long n = atomic_fetch_add(&V.frames, 1) + 1;
#if TARGET_OS_OSX
    if (V.frame_dir[0] && n % (V.frame_every > 0 ? (unsigned)V.frame_every : 60u) == 0) capture_frame(queue, info);
#endif
    int r = real ? real(queue, info) : -3;
    { static int tr = -1; if (tr < 0) tr = getenv("TL_VK_TRACE") ? 1 : 0; if (tr && n <= 5) tl_log_line("vulkan: vkQueuePresentKHR #%lu -> %d", n, r); }
    return r;
}

static void *w_vkGetInstanceProcAddr(void *instance, const char *name);
static void *w_vkGetDeviceProcAddr(void *device, const char *name);

static const struct { const char *name; void *fn; } k_over[] = {
    { "vkGetInstanceProcAddr", w_vkGetInstanceProcAddr },
    { "vkGetDeviceProcAddr", w_vkGetDeviceProcAddr },
    { "vkCreateInstance", w_vkCreateInstance },
    { "vkEnumerateInstanceExtensionProperties", w_vkEnumerateInstanceExtensionProperties },
    { "vkCreateAndroidSurfaceKHR", w_vkCreateAndroidSurfaceKHR },
    { "vkQueuePresentKHR", w_vkQueuePresentKHR },
};

static void *overridden(const char *name)
{
    for (size_t i = 0; i < sizeof(k_over) / sizeof(k_over[0]); i++) if (!strcmp(k_over[i].name, name)) return k_over[i].fn;
    return NULL;
}

static void *w_vkGetInstanceProcAddr(void *instance, const char *name)
{
    if (!name || !vk_ready()) return NULL;
    void *fn = overridden(name);
    if (fn) return fn;
    fn = V.gipa(instance, name);
    LOG("vulkan: vkGetInstanceProcAddr(%s) -> %p", name, fn);
    return fn;
}

static void *w_vkGetDeviceProcAddr(void *device, const char *name)
{
    if (!name || !vk_ready()) return NULL;
    void *fn = overridden(name);
    if (fn) return fn;
    typedef void *(*pfn_gdpa)(void *, const char *);
    static pfn_gdpa mvk_gdpa;
    if (!mvk_gdpa) mvk_gdpa = V.lib ? (pfn_gdpa)dlsym(V.lib, "vkGetDeviceProcAddr") : NULL;
    return mvk_gdpa ? mvk_gdpa(device, name) : NULL;
}

void *tl_vk_resolve(const char *name)
{
    if (!vk_ready()) return NULL;
    void *fn = overridden(name);
    if (fn) return fn;
    return V.lib ? dlsym(V.lib, name) : NULL;
}

unsigned long tl_vk_frames_presented(void) { return atomic_load(&V.frames); }
