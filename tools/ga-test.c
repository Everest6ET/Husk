/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host harness for the GameActivity driver: runs an APK's GameActivity game (Minecraft) on a Mac, off-screen.
 *
 *   ga-test <apk> [seconds] [width height]
 *
 * Landscape by default (the phone's aspect). Environment: TL_JNI_TRACE=1|2, TL_VERBOSE=0..2,
 * TL_CTL=<fifo> (lines "tap X Y", "hold X Y MS", "swipe X1 Y1 X2 Y2 MS", "wait MS", "shot PNG", "quit").
 */
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/arm/thread_status.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include "husk-tl-gameactivity.h"
#include "husk-tl-jni.h"
#include "husk-tl-ld.h"

void tl_log_line(const char *fmt, ...)
{
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    static struct timespec t0;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    pthread_mutex_lock(&m);
    if (!t0.tv_sec) t0 = t;
    char line[4096];
    int n = 0;
    if (getenv("TL_LOG_TIME")) n = snprintf(line, sizeof(line), "[%7.3f] ", (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9);
    va_list ap; va_start(ap, fmt);
    int w = vsnprintf(line + n, sizeof(line) - (size_t)n - 2, fmt, ap);
    va_end(ap);
    if (w > (int)(sizeof(line) - (size_t)n - 3)) w = (int)(sizeof(line) - (size_t)n - 3);
    n += w;
    line[n++] = '\n';
    fwrite(line, 1, (size_t)n, stderr);
    pthread_mutex_unlock(&m);
}

void tl_egl_recent_calls(char *out, size_t n, int count);

static void describe(const char *label, const void *addr)
{
    const char *lib = NULL; const void *sa = NULL;
    const char *sym = tl_ld_symbol_at(addr, &lib, &sa);
    if (lib) fprintf(stderr, "  %-6s %p  %s  %s+%#lx\n", label, addr, lib, sym ? sym : "?", sa ? (unsigned long)((const char *)addr - (const char *)sa) : 0ul);
    else {
        Dl_info di;
        if (dladdr(addr, &di) && di.dli_fname) fprintf(stderr, "  %-6s %p  [host] %s  %s+%#lx\n", label, addr, strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname, di.dli_sname ? di.dli_sname : "?", di.dli_saddr ? (unsigned long)((const char *)addr - (const char *)di.dli_saddr) : 0ul);
        else fprintf(stderr, "  %-6s %p\n", label, addr);
    }
}

static int safe_read(uintptr_t addr, void *out, size_t n)
{
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), (vm_address_t)addr, n, (vm_address_t)out, &got) == KERN_SUCCESS && got == n;
}

static void on_crash(int sig, siginfo_t *info, void *uctx)
{
    ucontext_t *uc = uctx;
    _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    char tn[32] = ""; pthread_getname_np(pthread_self(), tn, sizeof(tn));
    fprintf(stderr, "\n=== CRASH: signal %d, fault address %p, thread '%s' ===\n", sig, info->si_addr, tn);
    describe("pc", (void *)ss->__pc);
    describe("lr", (void *)ss->__lr);
    if (info->si_addr) describe("fault", info->si_addr);
    uintptr_t fp = ss->__fp;
    for (int i = 0; i < 16 && fp && (fp & 7) == 0; i++) {
        uint64_t fr[2];
        if (!safe_read(fp, fr, sizeof(fr))) break;
        describe("frame", (void *)fr[1]);
        fp = fr[0];
    }
    uint64_t *sp = (uint64_t *)ss->__sp; int shown = 0;
    for (int i = 0; i < 8192 && shown < 24; i++) {
        uint64_t v;
        if (!safe_read((uintptr_t)(sp + i), &v, 8)) break;
        if (v > 0x7000000000ull && v < 0x7100000000ull && tl_ld_lib_of((void *)v) && (v & 3) == 0) { describe("stk", (void *)v); shown++; }
    }
    for (int i = 0; i < 29; i += 4)
        fprintf(stderr, "  x%d=%#llx x%d=%#llx x%d=%#llx x%d=%#llx\n", i, ss->__x[i], i + 1, ss->__x[i + 1], i + 2, i + 2 < 29 ? ss->__x[i + 2] : 0, i + 3, i + 3 < 29 ? ss->__x[i + 3] : 0);
    fprintf(stderr, "  sp=%#llx fp=%#llx\n", ss->__sp, ss->__fp);
    { char gl[2048]; tl_egl_recent_calls(gl, sizeof(gl), 40); if (gl[0]) fprintf(stderr, "  last GL calls: %s\n", gl); }
    fflush(stderr);
    _exit(139);
}

static const char *g_frame_dir;
static void sleep_ms(long ms) { usleep((useconds_t)ms * 1000); }
static void do_swipe(float x1, float y1, float x2, float y2, long ms)
{
    int steps = (int)(ms / 16); if (steps < 2) steps = 2;
    tl_ga_touch(0, 0, x1, y1);
    for (int i = 1; i <= steps; i++) { sleep_ms(ms / steps); tl_ga_touch(1, 0, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps); }
    tl_ga_touch(2, 0, x2, y2);
}
static void *control_thread(void *arg)
{
    const char *path = arg;
    for (;;) {
        FILE *f = fopen(path, "r");
        if (!f) { sleep_ms(200); continue; }
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            float a, b, c, d; long ms; char p[400];
            if (sscanf(line, "tap %f %f", &a, &b) == 2) { tl_ga_touch(0, 0, a, b); sleep_ms(80); tl_ga_touch(2, 0, a, b); }
            else if (sscanf(line, "hold %f %f %ld", &a, &b, &ms) == 3) { tl_ga_touch(0, 0, a, b); sleep_ms(ms); tl_ga_touch(2, 0, a, b); }
            else if (sscanf(line, "swipe %f %f %f %f %ld", &a, &b, &c, &d, &ms) == 5) do_swipe(a, b, c, d, ms);
            else if (sscanf(line, "wait %ld", &ms) == 1) sleep_ms(ms);
            else if (sscanf(line, "shot %399s", p) == 1) {
                char cmd[900]; snprintf(cmd, sizeof(cmd), "sips -s format png '%s/latest.bmp' --out '%s' >/dev/null 2>&1", g_frame_dir, p);
                if (system(cmd)) fprintf(stderr, "ctl: shot failed\n");
                else fprintf(stderr, "ctl: shot %s (frame %lu)\n", p, tl_ga_frames());
            }
            else if (!strncmp(line, "pause", 5)) tl_ga_set_paused(true);
            else if (!strncmp(line, "resume", 6)) tl_ga_set_paused(false);
            else if (!strncmp(line, "quit", 4)) { fprintf(stderr, "ctl: quit\n"); fflush(stderr); _exit(0); }
        }
        fclose(f);
    }
    return NULL;
}

static int g_ran;
static void tl_ran(void) { g_ran++; }

/* Bedrock's assertion handler builds its message just before it decides to crash on purpose (store 0xdeadc0de to
 * address 0). A probe there prints the message, which the game itself only logs after the crash. */
static void assert_probe(uint64_t *r)
{
    const char *msg = (const char *)r[5];
    tl_log_line("ASSERTION FAILED: %.1500s", msg && (uintptr_t)msg > 0x100000 ? msg : "(no message)");
}

/* RenderDragon's bgfx callback: fatal(code in x1, message in x2) */
static void fatal_probe(uint64_t *r)
{
    const char *msg = (const char *)r[2];
    tl_log_line("BGFX FATAL %d: %.1500s", (int)r[1], msg && (uintptr_t)msg > 0x100000 ? msg : "(no message)");
}

/* TL_SAMPLE=<seconds>: every so often, say where every thread is (the guest library and offset of its pc and lr). For finding what a stuck game is waiting for. */
static void *sampler(void *arg)
{
    int period = (int)(intptr_t)arg;
    for (;;) {
        sleep((unsigned)period);
        thread_act_array_t th; mach_msg_type_number_t n;
        if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) continue;
        fprintf(stderr, "---- sample: %u threads ----\n", n);
        { char gl[1500]; tl_egl_recent_calls(gl, sizeof(gl), 24); if (gl[0]) fprintf(stderr, "  last GL calls: %s\n", gl); }
        for (mach_msg_type_number_t i = 0; i < n; i++) {
            arm_thread_state64_t st; mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
            pthread_t pt = pthread_from_mach_thread_np(th[i]);
            char name[40] = "";
            if (pt) pthread_getname_np(pt, name, sizeof(name));
            if (th[i] == mach_thread_self()) continue;
            thread_suspend(th[i]);
            if (thread_get_state(th[i], ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS) {
                const char *lib = NULL; const void *sa = NULL;
                const char *sym = tl_ld_symbol_at((void *)arm_thread_state64_get_pc(st), &lib, &sa);
                const char *lib2 = NULL; const void *sa2 = NULL;
                const char *sym2 = tl_ld_symbol_at((void *)arm_thread_state64_get_lr(st), &lib2, &sa2);
                Dl_info di; const char *host = "";
                if (!lib && dladdr((void *)arm_thread_state64_get_pc(st), &di) && di.dli_sname) host = di.dli_sname;
                fprintf(stderr, "  [%-24s] pc %s%s+%#lx  lr %s%s+%#lx\n", name, lib ? lib : "[host] ", lib ? sym ? sym : "?" : host,
                        lib && sa ? (unsigned long)(arm_thread_state64_get_pc(st) - (uintptr_t)sa) : 0ul,
                        lib2 ? lib2 : "", lib2 ? sym2 ? sym2 : "?" : "",
                        lib2 && sa2 ? (unsigned long)(arm_thread_state64_get_lr(st) - (uintptr_t)sa2) : 0ul);
            }
            if (getenv("TL_SAMPLE_BT") && strstr(name, getenv("TL_SAMPLE_BT"))) {
                uintptr_t fp = arm_thread_state64_get_fp(st);
                for (int d = 0; d < 14 && fp && (fp & 7) == 0; d++) {
                    uint64_t fr[2];
                    if (!safe_read(fp, fr, sizeof(fr))) break;
                    const char *l3 = NULL; const void *s3 = NULL;
                    const char *sy3 = tl_ld_symbol_at((void *)fr[1], &l3, &s3);
                    fprintf(stderr, "      frame %s %s+%#lx\n", l3 ? l3 : "?", l3 && sy3 ? sy3 : "?", l3 && s3 ? (unsigned long)(fr[1] - (uintptr_t)s3) : 0ul);
                    fp = fr[0];
                }
            }
            thread_resume(th[i]);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <apk> [seconds] [width height]\n", argv[0]); return 2; }
    static uint8_t altstack[1 << 16];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof(altstack) };
    sigaltstack(&ss, NULL);
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_crash; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL); sigaction(SIGABRT, &sa, NULL);

    tl_ld_set_verbosity(getenv("TL_VERBOSE") ? atoi(getenv("TL_VERBOSE")) : 1);
    tl_jni_set_trace(getenv("TL_JNI_TRACE") ? atoi(getenv("TL_JNI_TRACE")) : 1);
    char tmp[] = "/tmp/husk-mc-XXXXXX";
    mkdtemp(tmp);
    const char *cef = "/Users/davi/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/Frameworks/Chromium Embedded Framework.framework/Versions/A/Libraries";
    char egl[600], gles[600]; snprintf(egl, sizeof(egl), "%s/libEGL.dylib", cef); snprintf(gles, sizeof(gles), "%s/libGLESv2.dylib", cef);
    char frames[] = "/tmp/husk-mframes-XXXXXX"; mkdtemp(frames);
    fprintf(stderr, "frames: %s\ndata: %s\n", frames, tmp);
    int w = argc > 4 ? atoi(argv[3]) : 1200, h = argc > 4 ? atoi(argv[4]) : 552;
    tl_ga_config cfg = { .apk_path = argv[1], .data_dir = tmp, .package_name = "com.mojang.minecraftpe", .width = w, .height = h,
                         .angle_egl = getenv("TL_ANGLE_EGL") ? getenv("TL_ANGLE_EGL") : egl, .angle_gles = getenv("TL_ANGLE_GLES") ? getenv("TL_ANGLE_GLES") : gles,
                         .frame_dir = frames, .frame_every = getenv("TL_FRAMES") ? atoi(getenv("TL_FRAMES")) : -6 };
    g_frame_dir = frames;
    if (!tl_ga_start(&cfg)) { fprintf(stderr, "minecraft: start failed\n"); return 1; }
    if (getenv("TL_MC_RAND_TEST")) {
        tl_lib *L = tl_ld_find_lib("libminecraftpe.so");
        int (*rb)(void *, int) = L ? tl_ld_sym(L, "RAND_bytes") : NULL;
        int (*status)(void) = L ? tl_ld_sym(L, "RAND_status") : NULL;
        unsigned long (*geterr)(void) = L ? tl_ld_sym(L, "ERR_get_error") : NULL;
        char *(*errstr)(unsigned long, char *) = L ? tl_ld_sym(L, "ERR_error_string") : NULL;
        unsigned char buf[16] = { 0 };
        int (*poll)(void) = L ? tl_ld_sym(L, "RAND_poll") : NULL;
        void *(*master)(void) = L ? tl_ld_sym(L, "RAND_DRBG_get0_master") : NULL;
        int (*inst)(void *, const unsigned char *, size_t) = L ? tl_ld_sym(L, "RAND_DRBG_instantiate") : NULL;
        { int (*ic)(uint64_t, void *) = L ? tl_ld_sym(L, "OPENSSL_init_crypto") : NULL;
          void *(*ln)(void) = L ? tl_ld_sym(L, "CRYPTO_THREAD_lock_new") : NULL;
          { int (*il)(void *, void *) = L ? tl_ld_sym(L, "CRYPTO_THREAD_init_local") : NULL; uint64_t key[2] = { 0xdead, 0xbeef };
            fprintf(stderr, "CRYPTO_THREAD_init_local=%d key=%llx next=%llx\n", il ? il(key, NULL) : -99, (unsigned long long)key[0], (unsigned long long)key[1]);
            key[0] = 0xdead;
            fprintf(stderr, "CRYPTO_THREAD_init_local(dtor)=%d key=%llx\n", il ? il(key, tl_ld_sym(L, "OPENSSL_cleanup")) : -99, (unsigned long long)key[0]); }
          { int (*ro)(void *, void (*)(void)) = L ? tl_ld_sym(L, "CRYPTO_THREAD_run_once") : NULL; static uint64_t once[2];
            fprintf(stderr, "CRYPTO_THREAD_run_once=%d ran=%d\n", ro ? ro(once, tl_ran) : -99, g_ran); }
          { uint8_t *bl = L ? tl_ld_sym(L, "bio_type_lock") : NULL;      /* bio_type_lock is at vaddr 0x15b92528 */
            if (bl) { uint8_t *b = bl + (0x15b94fb0 - 0x15b92528);
              fprintf(stderr, "openssl state @0x15b94fb0:"); for (int i = 0; i < 0x40; i++) { if (i % 16 == 0) fprintf(stderr, "\n  +%02x:", i); fprintf(stderr, " %02x", b[i]); } fprintf(stderr, "\n"); } }
          fprintf(stderr, "OPENSSL_init_crypto(0)=%d\n", ic ? ic(0, NULL) : -99);
          { uint8_t *bl2 = L ? tl_ld_sym(L, "bio_type_lock") : NULL;
            if (bl2) { uint8_t *b = bl2 + (0x15b94fb0 - 0x15b92528);
              fprintf(stderr, "openssl state after:"); for (int i = 0; i < 0x40; i++) { if (i % 16 == 0) fprintf(stderr, "\n  +%02x:", i); fprintf(stderr, " %02x", b[i]); } fprintf(stderr, "\n"); } }
          void *lk = ln ? ln() : NULL; fprintf(stderr, "CRYPTO_THREAD_lock_new=%p\n", lk);
          int (*rl)(void *) = L ? tl_ld_sym(L, "CRYPTO_THREAD_read_lock") : NULL;
          fprintf(stderr, "read_lock=%d\n", rl && lk ? rl(lk) : -99);
          void *(*zalloc)(size_t, const char *, int) = L ? tl_ld_sym(L, "CRYPTO_zalloc") : NULL;
          fprintf(stderr, "CRYPTO_zalloc(64)=%p\n", zalloc ? zalloc(64, "t", 1) : NULL); }
        fprintf(stderr, "RAND_status=%d\n", status ? status() : -99);
        fprintf(stderr, "RAND_poll=%d\n", poll ? poll() : -99);
        void *m = master ? master() : NULL;
        fprintf(stderr, "master DRBG=%p\n", m);
        if (m && inst) fprintf(stderr, "master instantiate=%d\n", inst(m, (const unsigned char *)"x", 1));
        { unsigned long e0; while (geterr && (e0 = geterr()) != 0) { char eb[256]; fprintf(stderr, "ERR %#lx %s\n", e0, errstr ? errstr(e0, eb) : "?"); } }
        fprintf(stderr, "RAND_status=%d\n", status ? status() : -99);
        int r = rb ? rb(buf, 16) : -99;
        fprintf(stderr, "RAND_bytes=%d bytes=", r);
        for (int i = 0; i < 16; i++) fprintf(stderr, "%02x", buf[i]);
        fprintf(stderr, "\n");
        unsigned long e;
        while (geterr && (e = geterr()) != 0) { char eb[256]; fprintf(stderr, "ERR %#lx %s\n", e, errstr ? errstr(e, eb) : "?"); }
    }
    if (getenv("TL_MC_FATAL_PROBE")) {
        tl_lib *L = tl_ld_find_lib("libminecraftpe.so");
        if (L && !tl_ld_probe(L, strtoull(getenv("TL_MC_FATAL_PROBE"), NULL, 16), fatal_probe)) fprintf(stderr, "fatal probe failed\n");
    }
    if (getenv("TL_MC_ASSERT_PROBE")) {
        tl_lib *L = tl_ld_find_lib("libminecraftpe.so");
        if (L && !tl_ld_probe(L, strtoull(getenv("TL_MC_ASSERT_PROBE"), NULL, 16), assert_probe)) fprintf(stderr, "probe failed\n");
    }
    if (!tl_ga_run()) { fprintf(stderr, "minecraft: run failed\n"); return 1; }
    if (getenv("TL_CTL")) { static pthread_t ct; pthread_create(&ct, NULL, control_thread, getenv("TL_CTL")); }
    if (getenv("TL_SAMPLE")) { pthread_t st; pthread_create(&st, NULL, sampler, (void *)(intptr_t)atoi(getenv("TL_SAMPLE"))); }
    int secs = argc > 2 ? atoi(argv[2]) : 5;
    for (int i = 0; i < secs; i++) sleep(1);
    fprintf(stderr, "minecraft: %lu frames in %d s\n", tl_ga_frames(), secs);
    return 0;
}
