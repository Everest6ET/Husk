/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#include "husk-tl-unity.h"

#include <pthread.h>
#include <stdarg.h>
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
jobj *tl_hle_activity(void);
void tl_set_data_dir(const char *dir);
void tl_nwindow_configure(int w, int h, void *layer);

static struct {
    tl_unity_config cfg;
    jobj *activity;          /* the Context handed to the engine */
    jobj *player;            /* the UnityPlayer object natives are called on */
    atomic_ulong frames;
    atomic_bool stop;
    pthread_t thread;
    bool thread_started;
} U;

typedef int32_t (*onload_fn)(void *vm, void *reserved);

static void *native_of(const char *cls, const char *name, const char *sig)
{
    void *fn = tl_jni_native(cls, name, sig);
    if (!fn) tl_log_line("unity: native %s.%s%s was not registered", cls, name, sig);
    return fn;
}

bool tl_unity_start(const tl_unity_config *cfg)
{
    U.cfg = *cfg;
    tl_set_data_dir(cfg->data_dir);
    tl_nwindow_configure(cfg->width, cfg->height, cfg->metal_layer);
    int n = tl_dexidx_open(cfg->apk_path);
    tl_log_line("unity: %d classes in the APK's DEX", n);
    if (!tl_ld_add_apk(cfg->apk_path)) return false;
    if (cfg->angle_egl && !tl_egl_init(cfg->angle_egl, cfg->angle_gles, cfg->frame_dir, cfg->frame_every)) return false;
    tl_jni_init();
    tl_hle_configure(cfg->package_name, cfg->apk_path, cfg->data_dir, cfg->width, cfg->height);
    tl_jni_hle_install();

    /* new UnityPlayer(activity): the Java side's own constructor calls loadNative(dir),
     * which is System.load(dir + "/libmain.so") followed by NativeLoader.load(dir). */
    U.activity = tl_hle_activity();
    U.player = tl_jni_new_object(tl_jni_class("com/unity3d/player/UnityPlayer"));

    tl_lib *main_lib = tl_ld_load("libmain.so");
    if (!main_lib) return false;
    tl_ld_init(main_lib);
    onload_fn onload = (onload_fn)tl_ld_sym(main_lib, "JNI_OnLoad");
    if (!onload) { tl_log_line("unity: libmain.so has no JNI_OnLoad"); return false; }
    int32_t ver = onload(tl_jni_vm(), NULL);
    tl_log_line("unity: libmain JNI_OnLoad -> %#x", ver);
    if (tl_jni_pending()) { tl_log_line("unity: an exception is pending after libmain's JNI_OnLoad"); return false; }

    typedef uint8_t (*load_fn)(void *env, void *cls, void *dir);
    load_fn load = (load_fn)native_of("com/unity3d/player/NativeLoader", "load", "(Ljava/lang/String;)Z");
    if (!load) return false;
    jobj *dir = tl_jni_new_string("/data/app/lib/arm64");
    uint8_t ok = load(tl_jni_env(), tl_jni_class_object("com/unity3d/player/NativeLoader"), dir);
    tl_log_line("unity: NativeLoader.load -> %d", ok);
    return ok != 0;
}

/*
 * Call a registered UnityPlayer native: (env, player, a, b). Fixed arity on purpose: a
 * variadic pointer type would put the arguments on the stack, where guest code (compiled
 * for AAPCS64) does not look for them.
 */
typedef void (*native_call_fn)(void *env, void *self, uintptr_t a, uintptr_t b);
static void call_native_on(const char *cls, jobj *self, const char *name, const char *sig, uintptr_t a, uintptr_t b)
{
    void *fn = native_of(cls, name, sig);
    if (fn) ((native_call_fn)fn)(tl_jni_env(), self, a, b);
}
static void call_native(const char *name, const char *sig, uintptr_t a, uintptr_t b)
{
    call_native_on("com/unity3d/player/UnityPlayer", U.player, name, sig, a, b);
}
#define NATIVE_VOID(name, sig, a, b) call_native(name, sig, (uintptr_t)(a), (uintptr_t)(b))

static void *unity_main(void *arg)
{
    (void)arg;
    pthread_setname_np("UnityMain");
    tl_log_line("unity: UnityMain thread started");

    /* The jobs UnityPlayer queues for this thread, in the order it queues them. */
    jobj *surface = tl_jni_new_object(tl_jni_class("android/view/Surface"));
    NATIVE_VOID("nativeRecreateGfxState", "(ILandroid/view/Surface;)V", 0, surface);
    tl_log_line("unity: nativeRecreateGfxState done");
    NATIVE_VOID("nativeSendSurfaceChangedEvent", "()V", 0, 0);
    NATIVE_VOID("nativeResume", "()V", 0, 0);
    tl_log_line("unity: nativeResume done");
    NATIVE_VOID("nativeFocusChanged", "(Z)V", 1, 0);

    void *render = native_of("com/unity3d/player/UnityPlayer", "nativeRender", "()Z");
    while (render && !atomic_load(&U.stop)) {
        uint8_t keep = ((uint8_t (*)(void *, void *))render)(tl_jni_env(), U.player);
        atomic_fetch_add(&U.frames, 1);
        if (!keep) { tl_log_line("unity: nativeRender returned false: the engine asked to quit"); break; }
        usleep(2000);
    }
    return NULL;
}

bool tl_unity_run(void)
{
    /* The UnityPlayer constructor's last native step before it starts the thread. */
    NATIVE_VOID("initJni", "(Landroid/content/Context;)V", U.activity, 0);
    tl_log_line("unity: initJni done");
    /* The rest of the UnityPlayer constructor: the helpers it builds, whose constructors each call a
     * native that gives the engine its reference to the helper's Java class. Without these the engine
     * holds NULL where it expects a class and fails later, far from the cause. */
    jobj *cam = tl_jni_new_object(tl_jni_class("com/unity3d/player/Camera2Wrapper"));
    call_native_on("com/unity3d/player/Camera2Wrapper", cam, "initCamera2Jni", "()V", 0, 0);
    jobj *hfp = tl_jni_new_object(tl_jni_class("com/unity3d/player/HFPStatus"));
    call_native_on("com/unity3d/player/HFPStatus", hfp, "initHFPStatusJni", "()V", 0, 0);
    jobj *olock = tl_jni_new_object(tl_jni_class("com/unity3d/player/OrientationLockListener"));
    call_native_on("com/unity3d/player/OrientationLockListener", olock, "nativeUpdateOrientationLockState", "(I)V", 1, 0);
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16u << 20);
    if (pthread_create(&U.thread, &a, unity_main, NULL) != 0) return false;
    U.thread_started = true;
    return true;
}

unsigned long tl_unity_frames(void) { return atomic_load(&U.frames); }
void tl_unity_poke(int signo) { if (U.thread_started) pthread_kill(U.thread, signo); }

void tl_unity_stop(void)
{
    atomic_store(&U.stop, true);
    if (U.thread_started) pthread_join(U.thread, NULL);
}
