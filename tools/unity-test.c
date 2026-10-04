/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Host harness for the Unity driver: boots an APK's Unity player on a Mac.
 *
 *   unity-test <apk> [seconds]
 *
 * Environment: TL_JNI_TRACE=1|2 (log JNI lookups / every call), TL_SKIP=a.so,...
 */
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "husk-tl-jni.h"
#include "husk-tl-ld.h"
#include "husk-tl-unity.h"
#include "husk-tl-xmem.h"

void tl_log_line(const char *fmt, ...)
{
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
    pthread_mutex_unlock(&m);
}

static void describe(const char *label, const void *addr)
{
    const char *lib = NULL; const void *sa = NULL;
    const char *sym = tl_ld_symbol_at(addr, &lib, &sa);
    if (lib) fprintf(stderr, "  %-6s %p  %s  %s+%#lx\n", label, addr, lib, sym ? sym : "?", sa ? (unsigned long)((const char *)addr - (const char *)sa) : 0ul);
    else fprintf(stderr, "  %-6s %p\n", label, addr);
}

static void on_dump(int sig, siginfo_t *info, void *uctx)
{
    (void)sig; (void)info;
    ucontext_t *uc = uctx;
    _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    fprintf(stderr, "\n--- UnityMain, interrupted ---\n");
    describe("pc", (void *)ss->__pc);
    describe("lr", (void *)ss->__lr);
    fprintf(stderr, "  x19=%#llx x20=%#llx x21=%#llx x22=%#llx x23=%#llx x24=%#llx x25=%#llx x26=%#llx\n",
            ss->__x[19], ss->__x[20], ss->__x[21], ss->__x[22], ss->__x[23], ss->__x[24], ss->__x[25], ss->__x[26]);
    if (getenv("TL_DUMP_MEM")) {
        uint64_t *m = (uint64_t *)ss->__x[21];
        fprintf(stderr, "  [x21 free list head]   = %#llx  (+8: %#llx)\n", (unsigned long long)m[0], (unsigned long long)m[1]);
        m = (uint64_t *)ss->__x[23];
        fprintf(stderr, "  [x23 counter]          = %#llx\n", (unsigned long long)m[0]);
        fprintf(stderr, "  [x24] byte = %#x   [x25] word = %#x\n", *(uint8_t *)ss->__x[24], *(uint32_t *)ss->__x[25]);
        {
            uint64_t *q = (uint64_t *)ss->__x[21];
            for (int i = 0; i < 20; i++) fprintf(stderr, "  list[+%#x] = %#llx\n", i * 8, (unsigned long long)q[i]);
            uint64_t head = q[8] & ~1ull, tail = q[16];
            fprintf(stderr, "  head node %#llx (locked bit %llu), tail %#llx\n", (unsigned long long)head, (unsigned long long)(q[8] & 1), (unsigned long long)tail);
            if (head > 0x100000000ull) for (int i = 0; i < 4; i++) fprintf(stderr, "  head[+%d] = %#llx\n", i * 8, (unsigned long long)((uint64_t *)head)[i]);
            if (tail > 0x100000000ull) for (int i = 0; i < 4; i++) fprintf(stderr, "  tail[+%d] = %#llx\n", i * 8, (unsigned long long)((uint64_t *)tail)[i]);
        }
        {
            /* the chunk the head node lives in: 16 KiB aligned, block size in its first word */
            uint64_t head = ((uint64_t *)ss->__x[21])[8] & ~1ull, tail = ((uint64_t *)ss->__x[21])[16];
            uint64_t chunk = head & ~0x3fffull;
            int bs = *(int *)chunk;
            fprintf(stderr, "  chunk %#llx: block size %d, header words %#x %#x\n", (unsigned long long)chunk, bs, ((int *)chunk)[1], ((int *)chunk)[2]);
            uint64_t first = (chunk + 0x13 + 15) & ~15ull;      /* where the carve loop starts */
            for (uint64_t n = first; bs > 0 && n + bs <= chunk + 0x4000; n += (uint64_t)((bs + 15) & ~15)) {
                uint64_t nx = *(uint64_t *)n;
                fprintf(stderr, "    node %#llx -> %#llx%s%s\n", (unsigned long long)n, (unsigned long long)nx,
                        n == head ? "   <== HEAD" : "", n == tail ? "   <== TAIL" : "");
                if (n > first + 40ull * (uint64_t)((bs + 15) & ~15)) break;
            }
        }
        {
            uint64_t n = ((uint64_t *)ss->__x[21])[8] & ~1ull;
            fprintf(stderr, "  chain from head:");
            for (int i = 0; i < 40 && n > 0x100000000ull; i++) { fprintf(stderr, " %#llx", (unsigned long long)n); n = *(uint64_t *)n; }
            fprintf(stderr, " -> end %#llx\n", (unsigned long long)n);
        }
        uint64_t *base = (uint64_t *)ss->__x[19];
        for (int i = 0; i < 12; i++) fprintf(stderr, "  allocator[+%#x] = %#llx\n", i * 8, (unsigned long long)base[i]);
    }
    /* Frame pointers are not reliable in this code, so scan the stack for return addresses. */
    uint64_t *sp = (uint64_t *)ss->__sp;
    int shown = 0;
    for (int i = 0; i < 4096 && shown < 40; i++) {
        uint64_t v = sp[i];
        if (v > 0x7000000000ull && v < 0x7100000000ull && tl_ld_lib_of((void *)v) && tl_xmem_is_rx((void *)v) && (v & 3) == 0) { describe("ret", (void *)v); shown++; }
    }
    fflush(stderr);
}

static void on_crash(int sig, siginfo_t *info, void *uctx)
{
    ucontext_t *uc = uctx;
    _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    fprintf(stderr, "\n=== CRASH: signal %d, fault address %p ===\n", sig, info->si_addr);
    describe("pc", (void *)ss->__pc);
    describe("lr", (void *)ss->__lr);
    if (info->si_addr) describe("fault", info->si_addr);
    uint64_t *fp = (uint64_t *)ss->__fp;
    for (int i = 0; i < 14 && fp && ((uintptr_t)fp & 7) == 0 && (uintptr_t)fp > 0x100000000ull; i++) {
        describe("frame", (void *)fp[1]);
        fp = (uint64_t *)fp[0];
    }
    fprintf(stderr, "  x0=%#llx x1=%#llx x2=%#llx x3=%#llx x8=%#llx x9=%#llx x19=%#llx x20=%#llx\n",
            ss->__x[0], ss->__x[1], ss->__x[2], ss->__x[3], ss->__x[8], ss->__x[9], ss->__x[19], ss->__x[20]);
    fflush(stderr);
    _exit(139);
}

static int print_lib(uintptr_t bias, const char *name, const void *phdr, unsigned phnum, void *user)
{
    (void)phdr; (void)phnum; (void)user;
    fprintf(stderr, "LIB %s %#lx\n", name, (unsigned long)bias);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <apk> [seconds]\n", argv[0]); return 2; }
    static uint8_t altstack[1 << 16];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof(altstack) };
    sigaltstack(&ss, NULL);
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_crash; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL); sigaction(SIGABRT, &sa, NULL);

    { struct sigaction sd; memset(&sd, 0, sizeof(sd)); sd.sa_sigaction = on_dump; sd.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sd.sa_mask); sigaction(SIGUSR2, &sd, NULL); }
    tl_ld_set_verbosity(getenv("TL_VERBOSE") ? atoi(getenv("TL_VERBOSE")) : 1);
    tl_jni_set_trace(getenv("TL_JNI_TRACE") ? atoi(getenv("TL_JNI_TRACE")) : 1);
    char tmp[] = "/tmp/husk-unity-XXXXXX";
    mkdtemp(tmp);
    const char *cef = "/Users/davi/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/Frameworks/Chromium Embedded Framework.framework/Versions/A/Libraries";
    char egl[600], gles[600]; snprintf(egl, sizeof(egl), "%s/libEGL.dylib", cef); snprintf(gles, sizeof(gles), "%s/libGLESv2.dylib", cef);
    char frames[] = "/tmp/husk-frames-XXXXXX"; mkdtemp(frames);
    fprintf(stderr, "frames: %s\n", frames);
    tl_unity_config cfg = { .apk_path = argv[1], .data_dir = tmp, .package_name = "com.kiloo.subwaysurf", .width = 540, .height = 1200,
                            .angle_egl = getenv("TL_ANGLE_EGL") ? getenv("TL_ANGLE_EGL") : egl, .angle_gles = getenv("TL_ANGLE_GLES") ? getenv("TL_ANGLE_GLES") : gles,
                            .frame_dir = frames, .frame_every = 30 };
    if (!tl_unity_start(&cfg)) { fprintf(stderr, "unity: start failed\n"); return 1; }
    if (getenv("TL_STOP_EARLY")) { tl_ld_iterate(print_lib, NULL); raise(SIGSTOP); }
    if (!tl_unity_run()) { fprintf(stderr, "unity: run failed\n"); return 1; }
    tl_ld_iterate(print_lib, NULL);
    int secs = argc > 2 ? atoi(argv[2]) : 5;
    if (getenv("TL_STOP_AT")) { sleep((unsigned)atoi(getenv("TL_STOP_AT"))); raise(SIGSTOP); }
    if (getenv("TL_POKE_AT")) { sleep((unsigned)atoi(getenv("TL_POKE_AT"))); tl_unity_poke(SIGUSR2); usleep(300000); secs -= atoi(getenv("TL_POKE_AT")); }
    if (secs > 0) sleep((unsigned)secs);
    fprintf(stderr, "unity: %lu frames in %d s\n", tl_unity_frames(), secs);
    tl_unity_stop();
    return 0;
}
