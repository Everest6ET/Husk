/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#include "husk-tl-gameactivity.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "husk-tl-bionic.h"
#include "husk-tl-dexindex.h"
#include "husk-tl-egl.h"
#include "husk-tl-jni.h"
#include "husk-tl-ld.h"

void tl_jni_hle_install(void);
void tl_hle_configure(const char *pkg, const char *apk, const char *data, int w, int h);
void tl_hle_set_activity(jobj *a);
jobj *tl_hle_assets(void);
jobj *tl_hle_config(void);
void tl_set_data_dir(const char *dir);
void tl_nwindow_configure(int w, int h, void *layer);
void tl_mc_hle_install(const char *pkg, const char *apk, const char *data, int w, int h);
void tl_mc_set_activity(jobj *activity);
void tl_security_install(void);
void tl_fmod_install(void);

#define CLS_GA "com/google/androidgamesdk/GameActivity"
#define CLS_MAIN "com/mojang/minecraftpe/MainActivity"

static struct {
    tl_ga_config cfg;
    char apk[1024], data[512], pkg[128], frame_dir[512], angle_egl[600], angle_gles[600];
    jobj *activity, *surface;
    int64_t handle;
    atomic_bool paused;
} G;

static void load_library(const char *name)
{
    jvalue a; a.j = 0; a.l = tl_jni_new_string(name);
    tl_jni_call(tl_jni_class_object("java/lang/System"), "loadLibrary", "(Ljava/lang/String;)V", &a);
}

bool tl_ga_start(const tl_ga_config *cfg)
{
    G.cfg = *cfg;
    snprintf(G.apk, sizeof(G.apk), "%s", cfg->apk_path);
    snprintf(G.data, sizeof(G.data), "%s", cfg->data_dir);
    snprintf(G.pkg, sizeof(G.pkg), "%s", cfg->package_name);
    G.cfg.apk_path = G.apk; G.cfg.data_dir = G.data; G.cfg.package_name = G.pkg;
    if (cfg->frame_dir) { snprintf(G.frame_dir, sizeof(G.frame_dir), "%s", cfg->frame_dir); G.cfg.frame_dir = G.frame_dir; }
    if (cfg->angle_egl) { snprintf(G.angle_egl, sizeof(G.angle_egl), "%s", cfg->angle_egl); G.cfg.angle_egl = G.angle_egl; }
    if (cfg->angle_gles) { snprintf(G.angle_gles, sizeof(G.angle_gles), "%s", cfg->angle_gles); G.cfg.angle_gles = G.angle_gles; }

    tl_set_data_dir(cfg->data_dir);
    tl_nwindow_configure(cfg->width, cfg->height, cfg->metal_layer);
    int n = tl_dexidx_open(cfg->apk_path);
    tl_log_line("minecraft: %d classes in the APK's DEX", n);
    if (!tl_ld_add_apk(cfg->apk_path)) return false;
    if (cfg->angle_egl && !tl_egl_init(cfg->angle_egl, cfg->angle_gles, cfg->frame_dir, cfg->frame_every)) return false;
    tl_jni_init();
    tl_hle_configure(cfg->package_name, cfg->apk_path, cfg->data_dir, cfg->width, cfg->height);
    tl_jni_hle_install();
    tl_mc_hle_install(cfg->package_name, cfg->apk_path, cfg->data_dir, cfg->width, cfg->height);
    tl_security_install();
    tl_fmod_install();

    G.activity = tl_jni_new_object(tl_jni_class(CLS_MAIN));
    tl_hle_set_activity(G.activity);
    tl_mc_set_activity(G.activity);

    /* MainActivity.<clinit>: the libraries it loads, each running its JNI_OnLoad. */
    static const char *const libs[] = { "fmod", "MediaDecoders_Android", "minecraftpe", NULL };
    for (int i = 0; libs[i]; i++) {
        load_library(libs[i]);
        if (tl_jni_pending()) { tl_log_line("minecraft: loading lib%s.so failed", libs[i]); return false; }
    }
    /*
     * The game embeds V8 (its UI and scripting run on it). V8 normally generates machine code at run time, which needs
     * memory that is writable and executable and which an iPhone does not give an app. Its interpreter and built-in
     * code need none of that, so it is set to run without a JIT before anything starts it.
     */
    {
        tl_lib *mc = tl_ld_find_lib("libminecraftpe.so");
        void (*set_flags)(const char *) = mc ? (void (*)(const char *))tl_ld_sym(mc, "_ZN2v82V818SetFlagsFromStringEPKc") : NULL;
        if (set_flags) { set_flags("--jitless"); tl_log_line("minecraft: V8 set to run without a JIT"); }
        else tl_log_line("minecraft: V8's flag setter is not exported; the game's JavaScript may need executable memory");
    }
    tl_log_line("minecraft: libraries loaded");
    return true;
}

/* The native a Java method would have bound: registered by the library, or found by its JNI name. */
static void *native_of(const char *cls, const char *name, const char *sig, const char *mangled)
{
    void *fn = tl_jni_native(cls, name, sig);
    if (!fn) {
        tl_lib *lib = tl_ld_find_lib("libminecraftpe.so");
        if (lib) fn = tl_ld_sym(lib, mangled);
    }
    if (!fn) tl_log_line("minecraft: native %s.%s%s is not provided by the library", cls, name, sig);
    return fn;
}

/* GameActivity's natives are registered by the library when it initialises; each takes the handle first. */
#define GA_NATIVE(name, sig) native_of(CLS_GA, name, sig, "Java_com_google_androidgamesdk_GameActivity_" name)
#define MAIN_NATIVE(name, sig) native_of(CLS_MAIN, name, sig, "Java_com_mojang_minecraftpe_MainActivity_" name)

/* ------------------------------------------------------------- the UI thread */

/*
 * Android runs an activity's callbacks on one thread with a message loop, and the game posts work to it
 * (runOnUiThread). This is that: a queue of jobs, served in order by the thread that also ran the lifecycle.
 */
typedef struct ui_job { void (*fn)(void *); void *arg; struct ui_job *next; } ui_job;
static struct { pthread_mutex_t mu; pthread_cond_t cv; ui_job *head, *tail; pthread_t thread; bool started; void *looper; } UI = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

void tl_ga_post(void (*fn)(void *), void *arg)
{
    ui_job *j = calloc(1, sizeof(*j));
    j->fn = fn; j->arg = arg;
    pthread_mutex_lock(&UI.mu);
    if (UI.tail) UI.tail->next = j; else UI.head = j;
    UI.tail = j;
    pthread_cond_signal(&UI.cv);
    void *looper = UI.looper;
    pthread_mutex_unlock(&UI.mu);
    if (looper) ((void (*)(void *))tl_bionic_find("ALooper_wake"))(looper);
}

static void *ui_main(void *arg)
{
    (void)arg;
    pthread_setname_np("UiThread");
    /* Android's main thread already has a looper; the game asks for it (ALooper_forThread) while it initialises. */
    UI.looper = ((void *(*)(int))tl_bionic_find("ALooper_prepare"))(0);
    void *env = tl_jni_env();
    jobj *activity = G.activity;
    jobj *surface = tl_jni_new_object(tl_jni_class("android/view/Surface"));
    G.surface = surface;

    /* GameActivity.onCreate: the library's own initialiser, with the directories and the asset manager. */
    typedef int64_t (*init_fn)(void *env, void *self, void *internal, void *obb, void *ext, void *assets, void *saved, void *config);
    init_fn init = (init_fn)native_of(CLS_GA, "initializeNativeCode",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Landroid/content/res/AssetManager;[BLandroid/content/res/Configuration;)J",
        "Java_com_google_androidgamesdk_GameActivity_initializeNativeCode");
    if (!init) return NULL;
    char files[600], obb[600], ext[700];
    snprintf(files, sizeof(files), "%s/files", G.data);
    snprintf(obb, sizeof(obb), "%s/obb", G.data);
    snprintf(ext, sizeof(ext), "%s/sdcard/Android/data/%s/files", G.data, G.pkg);
    tl_log_line("minecraft: initializeNativeCode");
    G.handle = init(env, activity, tl_jni_new_string(files), tl_jni_new_string(obb), tl_jni_new_string(ext), tl_hle_assets(), NULL, tl_hle_config());
    tl_log_line("minecraft: initializeNativeCode -> %#llx", (unsigned long long)G.handle);
    if (!G.handle || tl_jni_pending()) { tl_log_line("minecraft: the game's native code did not initialise"); return NULL; }

    typedef void (*set_input_fn)(void *env, void *self, int64_t h, void *conn);
    set_input_fn set_input = (set_input_fn)GA_NATIVE("setInputConnectionNative", "(JLcom/google/androidgamesdk/gametextinput/InputConnection;)V");
    if (set_input) set_input(env, activity, G.handle, tl_jni_new_object(tl_jni_class("com/google/androidgamesdk/gametextinput/InputConnection")));

    /* MainActivity.onCreate, after super.onCreate: the Java crash manager is set up by the game's own thread,
     * and the UI thread waits for it to say so. */
    typedef void (*wait_fn)(void *env, void *cls);
    wait_fn wait = (wait_fn)MAIN_NATIVE("nativeWaitCrashManagementSetupComplete", "()V");
    if (wait) { tl_log_line("minecraft: waiting for the crash manager setup"); wait(env, tl_jni_class_object(CLS_MAIN)); tl_log_line("minecraft: crash manager setup complete"); }

    typedef void (*life_fn)(void *env, void *self, int64_t h);
    life_fn on_start = (life_fn)GA_NATIVE("onStartNative", "(J)V");
    life_fn on_resume = (life_fn)GA_NATIVE("onResumeNative", "(J)V");
    if (on_start) on_start(env, activity, G.handle);
    tl_log_line("minecraft: onStart done");
    if (on_resume) on_resume(env, activity, G.handle);
    tl_log_line("minecraft: onResume done");

    typedef void (*surf_fn)(void *env, void *self, int64_t h, void *surface);
    typedef void (*surf_changed_fn)(void *env, void *self, int64_t h, void *surface, int format, int w, int h2);
    typedef void (*focus_fn)(void *env, void *self, int64_t h, uint8_t focused);
    surf_fn created = (surf_fn)GA_NATIVE("onSurfaceCreatedNative", "(JLandroid/view/Surface;)V");
    surf_changed_fn changed = (surf_changed_fn)GA_NATIVE("onSurfaceChangedNative", "(JLandroid/view/Surface;III)V");
    focus_fn focus = (focus_fn)GA_NATIVE("onWindowFocusChangedNative", "(JZ)V");
    if (created) created(env, activity, G.handle, surface);
    tl_log_line("minecraft: surface created");
    if (changed) changed(env, activity, G.handle, surface, 1 /* PixelFormat.RGBA_8888 */, G.cfg.width, G.cfg.height);
    tl_log_line("minecraft: surface changed %dx%d", G.cfg.width, G.cfg.height);
    if (focus) focus(env, activity, G.handle, 1);
    tl_log_line("minecraft: focus gained");

    /* The message loop: the looper serves what the game's native glue registered on it (its main-work pipe), and
     * the jobs posted with tl_ga_post run in between. */
    int (*poll_once)(int, int *, int *, void **) = tl_bionic_find("ALooper_pollOnce");
    for (;;) {
        poll_once(50, NULL, NULL, NULL);
        for (;;) {
            pthread_mutex_lock(&UI.mu);
            ui_job *j = UI.head;
            if (j) { UI.head = j->next; if (!UI.head) UI.tail = NULL; }
            pthread_mutex_unlock(&UI.mu);
            if (!j) break;
            j->fn(j->arg);
            free(j);
            if (tl_jni_pending()) tl_jni_clear();
        }
    }
    return NULL;
}

bool tl_ga_is_ui_thread(void) { return UI.started && pthread_equal(pthread_self(), UI.thread); }

bool tl_ga_run(void)
{
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8u << 20);
    if (pthread_create(&UI.thread, &a, ui_main, NULL) != 0) return false;
    UI.started = true;
    return true;
}

void tl_ga_touch(int phase, int id, float x, float y) { (void)phase; (void)id; (void)x; (void)y; }
void tl_ga_set_paused(bool paused) { atomic_store(&G.paused, paused); }
unsigned long tl_ga_frames(void) { return tl_egl_frames_presented(); }
