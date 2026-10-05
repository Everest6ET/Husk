/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What Android's libstdc++.so provides (operator new and delete, the pure-virtual trap, thread-safe
 * guards for function-local statics) and the odd libc entry that only a few libraries reach: the
 * process-identity calls and terminal calls OpenSSL's seeding code makes, which have nothing to
 * act on inside an app.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-bionic.h"

#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------ operator new/delete */

static void *b_new(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { tl_log_line("cxx: operator new(%zu) failed", n); abort(); }
    return p;
}
static void *b_new_nothrow(size_t n, const void *nt) { (void)nt; return malloc(n ? n : 1); }
static void b_delete(void *p) { free(p); }
static void b_delete_sized(void *p, size_t n) { (void)n; free(p); }

static void b_cxa_pure_virtual(void)
{
    tl_log_line("cxx: pure virtual function called");
    abort();
}

/*
 * Guards for function-local statics: the Itanium ABI's 64-bit guard whose first byte is the
 * "initialised" flag that compiled code tests inline. The second byte marks an initialiser in
 * progress, and a thread that finds it set waits for the first to finish.
 */
static pthread_mutex_t g_guard_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_guard_cv = PTHREAD_COND_INITIALIZER;

static int b_cxa_guard_acquire(volatile uint8_t *g)
{
    if (__atomic_load_n(&g[0], __ATOMIC_ACQUIRE)) return 0;
    pthread_mutex_lock(&g_guard_mu);
    for (;;) {
        if (g[0]) { pthread_mutex_unlock(&g_guard_mu); return 0; }
        if (!g[1]) { g[1] = 1; pthread_mutex_unlock(&g_guard_mu); return 1; }
        pthread_cond_wait(&g_guard_cv, &g_guard_mu);
    }
}
static void b_cxa_guard_release(volatile uint8_t *g)
{
    pthread_mutex_lock(&g_guard_mu);
    g[1] = 0;
    __atomic_store_n(&g[0], 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_guard_cv);
    pthread_mutex_unlock(&g_guard_mu);
}
static void b_cxa_guard_abort(volatile uint8_t *g)
{
    pthread_mutex_lock(&g_guard_mu);
    g[1] = 0;
    pthread_cond_broadcast(&g_guard_cv);
    pthread_mutex_unlock(&g_guard_mu);
}

/* Old-style stack protector: the guard value is a symbol, not a TLS slot. */
static uint64_t g_stack_chk_guard = 0x5ab3c17e9d2f4a00ull;

/* ------------------------------------------------------------------ process calls */

static int b_setid(unsigned id) { (void)id; tl_set_guest_errno(1 /* EPERM */); return -1; }
static int b_initgroups(const char *user, int group) { (void)user; (void)group; tl_set_guest_errno(1); return -1; }
static int b_tcattr(int fd, void *t) { (void)fd; (void)t; tl_set_guest_errno(25 /* ENOTTY */); return -1; }
static int b_tcsetattr(int fd, int act, const void *t) { (void)fd; (void)act; (void)t; tl_set_guest_errno(25); return -1; }
static int b_execl(const char *path) { (void)path; tl_set_guest_errno(38 /* ENOSYS */); return -1; }
static unsigned b_getgid(void) { return (unsigned)getgid(); }
static unsigned b_umask(unsigned m) { return (unsigned)umask((mode_t)m); }
static unsigned b_alarm(unsigned s) { (void)s; return 0; }
static int b_mlock(const void *a, size_t n) { (void)a; (void)n; return 0; }

/* The guest's kill() on its own process would take the whole app down; that is the guest asking to quit. */
static int b_kill(int pid, int sig)
{
    if (pid == getpid() && sig != 0) {
        tl_log_line("cxx: the game raised signal %d on its own process", sig);
        if (tl_guest_exit_hook) tl_guest_exit_hook(128 + sig);
        return 0;
    }
    int ds = sig == 0 ? 0 : tl_signal_to_darwin(sig);
    if (sig != 0 && ds <= 0) { tl_set_guest_errno(22); return -1; }
    TL_ERRNO_BEGIN(); int r = kill(pid, ds); TL_ERRNO_END();
    return r;
}

/* The process-wide mask means nothing to a guest that cannot receive signals; report an empty mask. */
static int b_sigprocmask(int how, const uint64_t *set, uint64_t *old)
{
    (void)how; (void)set;
    if (old) *old = 0;
    return 0;
}

const tl_bionic_entry tl_tab_cxx[] = {
    TL_WRAP("_Znwm", b_new), TL_WRAP("_Znam", b_new), TL_WRAP("_ZnwmRKSt9nothrow_t", b_new_nothrow), TL_WRAP("_ZnamRKSt9nothrow_t", b_new_nothrow),
    TL_WRAP("_ZdlPv", b_delete), TL_WRAP("_ZdaPv", b_delete), TL_WRAP("_ZdlPvm", b_delete_sized), TL_WRAP("_ZdaPvm", b_delete_sized),
    TL_WRAP("_ZdlPvRKSt9nothrow_t", b_delete_sized), TL_WRAP("_ZdaPvRKSt9nothrow_t", b_delete_sized),
    TL_WRAP("__cxa_pure_virtual", b_cxa_pure_virtual),
    TL_WRAP("__cxa_guard_acquire", b_cxa_guard_acquire), TL_WRAP("__cxa_guard_release", b_cxa_guard_release), TL_WRAP("__cxa_guard_abort", b_cxa_guard_abort),
    TL_DATA("__stack_chk_guard", &g_stack_chk_guard),
    TL_WRAP("getgid", b_getgid), TL_WRAP("setuid", b_setid), TL_WRAP("setgid", b_setid), TL_WRAP("initgroups", b_initgroups),
    TL_WRAP("tcgetattr", b_tcattr), TL_WRAP("tcsetattr", b_tcsetattr), TL_WRAP("execl", b_execl),
    TL_WRAP("umask", b_umask), TL_WRAP("alarm", b_alarm), TL_WRAP("mlock", b_mlock), TL_WRAP("kill", b_kill),
    TL_WRAP("sigprocmask", b_sigprocmask),
    TL_DIRECT(sigsetjmp), TL_DIRECT(siglongjmp),
    TL_END
};
