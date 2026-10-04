/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Files, directories, memory mapping, time, signals, polling, sockets.
 *
 * Everything here crosses a boundary where Linux and Darwin disagree about a number or
 * a layout: open flags, the shape of struct stat and struct dirent, mmap flags, clock
 * ids, signal numbers, errno values. Each wrapper translates in and out and publishes
 * errno in the guest's own numbering.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-bionic.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <xlocale.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

/* ------------------------------------------------------------ path mapping */

/*
 * Android paths the guest expects, mapped to somewhere that exists. The data
 * directory is wherever the host set aside for this app; /proc files that apps read
 * to learn about the device are synthesised into unlinked temporary files.
 */
static char g_data_dir[512];
void tl_set_data_dir(const char *dir) { snprintf(g_data_dir, sizeof(g_data_dir), "%s", dir ? dir : ""); }
const char *tl_data_dir(void) { return g_data_dir[0] ? g_data_dir : "/tmp"; }

static int synth_file(const char *content)
{
    char tmpl[] = "/tmp/husk-synth-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    unlink(tmpl);
    size_t n = strlen(content);
    if (write(fd, content, n) != (ssize_t)n) { close(fd); return -1; }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

static const char *synth_content(const char *path, char *buf, size_t n)
{
    if (!strcmp(path, "/proc/cpuinfo")) {
        buf[0] = 0;
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        for (long i = 0; i < ncpu; i++) {
            size_t l = strlen(buf);
            snprintf(buf + l, n - l, "processor\t: %ld\nBogoMIPS\t: 48.00\nFeatures\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp\n"
                     "CPU implementer\t: 0x61\nCPU architecture: 8\nCPU variant\t: 0x0\nCPU part\t: 0x0\nCPU revision\t: 0\n\n", i);
        }
        return buf;
    }
    if (!strcmp(path, "/proc/meminfo")) {
        snprintf(buf, n, "MemTotal:       11722000 kB\nMemFree:         3000000 kB\nMemAvailable:    5000000 kB\nBuffers:           10000 kB\nCached:          2000000 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\n");
        return buf;
    }
    if (!strncmp(path, "/sys/devices/system/cpu/", 24) && (strstr(path, "/present") || strstr(path, "/possible") || strstr(path, "/online"))) {
        snprintf(buf, n, "0-%ld\n", sysconf(_SC_NPROCESSORS_ONLN) - 1);
        return buf;
    }
    if (!strcmp(path, "/proc/self/status")) {
        snprintf(buf, n, "Name:\tapp_process64\nState:\tR (running)\nTgid:\t%d\nPid:\t%d\nPPid:\t1\nUid:\t10001\t10001\t10001\t10001\nThreads:\t8\nVmRSS:\t  300000 kB\n", getpid(), getpid());
        return buf;
    }
    return NULL;
}

const char *tl_path_resolve(const char *path, char *buf, size_t n)
{
    if (!path) return path;
    const char *pkg = "/data/data/";
    if (!strncmp(path, pkg, strlen(pkg))) {
        const char *rest = strchr(path + strlen(pkg), '/');
        snprintf(buf, n, "%s%s", tl_data_dir(), rest ? rest : "");
        return buf;
    }
    if (!strncmp(path, "/data/user/0/", 13)) {
        const char *rest = strchr(path + 13, '/');
        snprintf(buf, n, "%s%s", tl_data_dir(), rest ? rest : "");
        return buf;
    }
    if (!strncmp(path, "/sdcard", 7) || !strncmp(path, "/storage/emulated/0", 19)) {
        const char *rest = !strncmp(path, "/sdcard", 7) ? path + 7 : path + 19;
        snprintf(buf, n, "%s/sdcard%s", tl_data_dir(), rest);
        return buf;
    }
    return path;
}

/* ----------------------------------------------------------- open & friends */

/* Linux arm64 open flags -> Darwin. */
static int oflags_to_darwin(int f)
{
    int d = f & 3;                                       /* O_RDONLY / O_WRONLY / O_RDWR agree */
    if (f & 0x40)      d |= O_CREAT;
    if (f & 0x80)      d |= O_EXCL;
    if (f & 0x100)     d |= O_NOCTTY;
    if (f & 0x200)     d |= O_TRUNC;
    if (f & 0x400)     d |= O_APPEND;
    if (f & 0x800)     d |= O_NONBLOCK;
    if (f & 0x101000)  d |= O_SYNC;
    if (f & 0x4000)    d |= O_DIRECTORY;
    if (f & 0x8000)    d |= O_NOFOLLOW;
    if (f & 0x80000)   d |= O_CLOEXEC;
    return d;
}
static int oflags_from_darwin(int d)
{
    int f = d & 3;
    if (d & O_APPEND) f |= 0x400;
    if (d & O_NONBLOCK) f |= 0x800;
    return f;
}

static int b_open(const char *path, int flags, unsigned mode)
{
    char buf[1024], content[8192];
    const char *real = tl_path_resolve(path, buf, sizeof(buf));
    const char *s = synth_content(path, content, sizeof(content));
    if (s) {
        int fd = synth_file(s);
        if (fd < 0) tl_set_guest_errno(2);
        return fd;
    }
    TL_ERRNO_BEGIN();
    int fd = open(real, oflags_to_darwin(flags), mode);
    TL_ERRNO_END();
    return fd;
}
static int b___open_2(const char *path, int flags) { return b_open(path, flags, 0); }
static int b_close(int fd) { TL_ERRNO_BEGIN(); int r = close(fd); TL_ERRNO_END(); return r; }
static long b_read(int fd, void *p, size_t n) { TL_ERRNO_BEGIN(); long r = read(fd, p, n); TL_ERRNO_END(); return r; }
static long b___read_chk(int fd, void *p, size_t n, size_t bufsz)
{
    if (n > bufsz) { tl_log_line("bionic: __read_chk overflow"); abort(); }
    return b_read(fd, p, n);
}
static long b_write(int fd, const void *p, size_t n) { TL_ERRNO_BEGIN(); long r = write(fd, p, n); TL_ERRNO_END(); return r; }
static long b_writev(int fd, const struct iovec *v, int n) { TL_ERRNO_BEGIN(); long r = writev(fd, v, n); TL_ERRNO_END(); return r; }
static long b_pread64(int fd, void *p, size_t n, long off) { TL_ERRNO_BEGIN(); long r = pread(fd, p, n, off); TL_ERRNO_END(); return r; }
static long b_lseek(int fd, long off, int whence) { TL_ERRNO_BEGIN(); long r = lseek(fd, off, whence); TL_ERRNO_END(); return r; }
static int b_dup(int fd) { TL_ERRNO_BEGIN(); int r = dup(fd); TL_ERRNO_END(); return r; }
static int b_dup2(int a, int b) { TL_ERRNO_BEGIN(); int r = dup2(a, b); TL_ERRNO_END(); return r; }
static int b_pipe(int fds[2]) { TL_ERRNO_BEGIN(); int r = pipe(fds); TL_ERRNO_END(); return r; }
static int b_fsync(int fd) { TL_ERRNO_BEGIN(); int r = fsync(fd); TL_ERRNO_END(); return r; }
static int b_ftruncate(int fd, long n) { TL_ERRNO_BEGIN(); int r = ftruncate(fd, n); TL_ERRNO_END(); return r; }
static int b_truncate(const char *p, long n) { char b[1024]; TL_ERRNO_BEGIN(); int r = truncate(tl_path_resolve(p, b, sizeof(b)), n); TL_ERRNO_END(); return r; }
static int b_isatty(int fd) { int r = isatty(fd); if (!r) tl_set_guest_errno(25); return r; }
static int b_flock(int fd, int op) { TL_ERRNO_BEGIN(); int r = flock(fd, op); TL_ERRNO_END(); return r; }

#define PATH1(rt, name, call) \
    static rt b_##name(const char *p) { char b[1024]; TL_ERRNO_BEGIN(); rt r = call(tl_path_resolve(p, b, sizeof(b))); TL_ERRNO_END(); return r; }
PATH1(int, unlink, unlink)
PATH1(int, rmdir, rmdir)
static int b_mkdir(const char *p, unsigned mode) { char b[1024]; TL_ERRNO_BEGIN(); int r = mkdir(tl_path_resolve(p, b, sizeof(b)), (mode_t)mode); TL_ERRNO_END(); return r; }
static int b_access(const char *p, int m) { char b[1024]; TL_ERRNO_BEGIN(); int r = access(tl_path_resolve(p, b, sizeof(b)), m); TL_ERRNO_END(); return r; }
static int b_chmod(const char *p, unsigned m) { char b[1024]; TL_ERRNO_BEGIN(); int r = chmod(tl_path_resolve(p, b, sizeof(b)), (mode_t)m); TL_ERRNO_END(); return r; }
static int b_fchmod(int fd, unsigned m) { TL_ERRNO_BEGIN(); int r = fchmod(fd, (mode_t)m); TL_ERRNO_END(); return r; }
static int b_link(const char *a, const char *b2) { char x[1024], y[1024]; TL_ERRNO_BEGIN(); int r = link(tl_path_resolve(a, x, sizeof(x)), tl_path_resolve(b2, y, sizeof(y))); TL_ERRNO_END(); return r; }
static int b_symlink(const char *a, const char *b2) { char y[1024]; TL_ERRNO_BEGIN(); int r = symlink(a, tl_path_resolve(b2, y, sizeof(y))); TL_ERRNO_END(); return r; }
static long b_readlink(const char *p, char *buf, size_t n) { char b[1024]; TL_ERRNO_BEGIN(); long r = readlink(tl_path_resolve(p, b, sizeof(b)), buf, n); TL_ERRNO_END(); return r; }
static char *b_realpath(const char *p, char *out) { char b[1024]; TL_ERRNO_BEGIN(); char *r = realpath(tl_path_resolve(p, b, sizeof(b)), out); TL_ERRNO_END(); return r; }
static char *b_getcwd(char *buf, size_t n) { TL_ERRNO_BEGIN(); char *r = getcwd(buf, n); TL_ERRNO_END(); return r; }
static int b_utimes(const char *p, const struct timeval tv[2]) { char b[1024]; TL_ERRNO_BEGIN(); int r = utimes(tl_path_resolve(p, b, sizeof(b)), tv); TL_ERRNO_END(); return r; }
static int b_utime(const char *p, const struct utimbuf *t) { char b[1024]; TL_ERRNO_BEGIN(); int r = utime(tl_path_resolve(p, b, sizeof(b)), t); TL_ERRNO_END(); return r; }
static int b_futimens(int fd, const struct timespec ts[2]) { TL_ERRNO_BEGIN(); int r = futimens(fd, ts); TL_ERRNO_END(); return r; }
static unsigned b___umask_chk(unsigned m) { return umask((mode_t)m); }

/* sendfile: Linux's (out, in, off*, count) emulated with read/write. */
static long b_sendfile(int out, int in, long *off, size_t count)
{
    char buf[16384]; long total = 0;
    while ((size_t)total < count) {
        size_t chunk = count - (size_t)total < sizeof(buf) ? count - (size_t)total : sizeof(buf);
        long n = off ? pread(in, buf, chunk, *off) : read(in, buf, chunk);
        if (n <= 0) break;
        long w = write(out, buf, (size_t)n);
        if (w <= 0) break;
        total += w;
        if (off) *off += w;
    }
    return total;
}

/* --- stat: bionic arm64's 128-byte layout --- */

typedef struct {
    uint64_t st_dev, st_ino; uint32_t st_mode, st_nlink, st_uid, st_gid; uint64_t st_rdev, pad1;
    int64_t st_size; int32_t st_blksize, pad2; int64_t st_blocks;
    int64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns; uint32_t unused4, unused5;
} guest_stat;

static void fill_stat(guest_stat *g, const struct stat *s)
{
    memset(g, 0, sizeof(*g));
    g->st_dev = (uint64_t)s->st_dev; g->st_ino = s->st_ino; g->st_mode = s->st_mode; g->st_nlink = s->st_nlink;
    g->st_uid = s->st_uid; g->st_gid = s->st_gid; g->st_rdev = (uint64_t)s->st_rdev; g->st_size = s->st_size;
    g->st_blksize = s->st_blksize; g->st_blocks = s->st_blocks;
    g->atime = s->st_atimespec.tv_sec; g->atime_ns = s->st_atimespec.tv_nsec;
    g->mtime = s->st_mtimespec.tv_sec; g->mtime_ns = s->st_mtimespec.tv_nsec;
    g->ctime = s->st_ctimespec.tv_sec; g->ctime_ns = s->st_ctimespec.tv_nsec;
}
static int b_stat(const char *p, guest_stat *g)
{
    char b[1024], c[8192]; struct stat s;
    if (synth_content(p, c, sizeof(c))) { memset(g, 0, sizeof(*g)); g->st_mode = S_IFREG | 0444; g->st_nlink = 1; return 0; }
    TL_ERRNO_BEGIN(); int r = stat(tl_path_resolve(p, b, sizeof(b)), &s); TL_ERRNO_END();
    if (r == 0) fill_stat(g, &s);
    return r;
}
static int b_lstat(const char *p, guest_stat *g)
{
    char b[1024]; struct stat s;
    TL_ERRNO_BEGIN(); int r = lstat(tl_path_resolve(p, b, sizeof(b)), &s); TL_ERRNO_END();
    if (r == 0) fill_stat(g, &s);
    return r;
}
static int b_fstat(int fd, guest_stat *g)
{
    struct stat s;
    TL_ERRNO_BEGIN(); int r = fstat(fd, &s); TL_ERRNO_END();
    if (r == 0) fill_stat(g, &s);
    return r;
}

typedef struct { int64_t f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree; int32_t fsid[2]; int64_t f_namelen, f_frsize, f_flags, spare[4]; } guest_statfs;
static int b_statfs(const char *p, guest_statfs *g)
{
    char b[1024]; struct statfs s;
    TL_ERRNO_BEGIN(); int r = statfs(tl_path_resolve(p, b, sizeof(b)), &s); TL_ERRNO_END();
    if (r == 0) {
        memset(g, 0, sizeof(*g));
        g->f_type = 0xEF53; g->f_bsize = s.f_bsize; g->f_blocks = s.f_blocks; g->f_bfree = s.f_bfree;
        g->f_bavail = s.f_bavail; g->f_files = s.f_files; g->f_ffree = s.f_ffree; g->f_namelen = 255; g->f_frsize = s.f_bsize;
    }
    return r;
}

/* fcntl / ioctl */
typedef struct { int16_t l_type, l_whence; int pad; int64_t l_start, l_len; int32_t l_pid; } guest_flock;

static int b_fcntl(int fd, int cmd, long arg)
{
    int r;
    TL_ERRNO_BEGIN();
    switch (cmd) {
    case 0:    r = fcntl(fd, F_DUPFD, (int)arg); break;
    case 1:    r = fcntl(fd, F_GETFD); break;
    case 2:    r = fcntl(fd, F_SETFD, (int)arg); break;
    case 3:    r = fcntl(fd, F_GETFL); if (r >= 0) r = oflags_from_darwin(r); break;
    case 4:    r = fcntl(fd, F_SETFL, oflags_to_darwin((int)arg) & ~3); break;
    case 1030: r = fcntl(fd, F_DUPFD_CLOEXEC, (int)arg); break;
    case 5: case 6: case 7: {                               /* F_GETLK, F_SETLK, F_SETLKW */
        guest_flock *g = (guest_flock *)arg;
        struct flock f = { .l_start = g->l_start, .l_len = g->l_len, .l_pid = g->l_pid,
                           .l_type = g->l_type == 0 ? F_RDLCK : g->l_type == 1 ? F_WRLCK : F_UNLCK, .l_whence = g->l_whence };
        r = fcntl(fd, cmd == 5 ? F_GETLK : cmd == 6 ? F_SETLK : F_SETLKW, &f);
        if (cmd == 5 && r == 0) { g->l_type = f.l_type == F_RDLCK ? 0 : f.l_type == F_WRLCK ? 1 : 2; g->l_pid = f.l_pid; }
        break;
    }
    default: errno = EINVAL; r = -1; break;
    }
    TL_ERRNO_END();
    return r;
}

static int b_ioctl(int fd, unsigned long req, long arg)
{
    if (req == 0x541B) { int n = 0; int r = ioctl(fd, FIONREAD, &n); if (r == 0) *(int *)arg = n; else TL_ERRNO_END(); return r; }  /* FIONREAD */
    if (req == 0x5421) { int on = *(int *)arg; int fl = fcntl(fd, F_GETFL); fcntl(fd, F_SETFL, on ? fl | O_NONBLOCK : fl & ~O_NONBLOCK); return 0; }   /* FIONBIO */
    tl_set_guest_errno(25);                                                                                                                    /* ENOTTY */
    return -1;
}

/* ------------------------------------------------------------ directories */

typedef struct { uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type; char d_name[256]; } guest_dirent;
typedef struct { DIR *dir; guest_dirent ent; } guest_dir;

static void *b_opendir(const char *p)
{
    char b[1024];
    TL_ERRNO_BEGIN(); DIR *d = opendir(tl_path_resolve(p, b, sizeof(b))); TL_ERRNO_END();
    if (!d) return NULL;
    guest_dir *g = calloc(1, sizeof(*g));
    g->dir = d;
    return g;
}
static void *b_readdir(void *dp)
{
    guest_dir *g = dp;
    TL_ERRNO_BEGIN(); struct dirent *e = readdir(g->dir); TL_ERRNO_END();
    if (!e) return NULL;
    g->ent.d_ino = e->d_ino; g->ent.d_off = 0; g->ent.d_reclen = sizeof(guest_dirent); g->ent.d_type = e->d_type;
    snprintf(g->ent.d_name, sizeof(g->ent.d_name), "%s", e->d_name);
    return &g->ent;
}
static int b_closedir(void *dp) { guest_dir *g = dp; int r = closedir(g->dir); free(g); return r; }

/* ----------------------------------------------------------------- mmap */

static int prot_filter(int prot, const char *what)
{
    if (prot & PROT_EXEC) {
        /* Executable memory comes from the StikDebug region only; a guest asking for
         * its own gets writable memory and finds out when it jumps there. */
        char note[96]; snprintf(note, sizeof(note), "%s asked for PROT_EXEC memory: refused", what);
        tl_note_once(note);
        prot &= ~PROT_EXEC;
    }
    return prot;
}

static void *b_mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
    int df = flags & 0x3;                                   /* MAP_SHARED / MAP_PRIVATE */
    if (flags & 0x10)   df |= MAP_FIXED;
    if (flags & 0x20)   df |= MAP_ANON;
    if (flags & 0x4000) df |= MAP_NOCACHE & 0;              /* MAP_NORESERVE: no equivalent needed */
    TL_ERRNO_BEGIN();
    void *r = mmap(addr, len, prot_filter(prot, "mmap"), df, (flags & 0x20) ? -1 : fd, off);
    TL_ERRNO_END();
    return r;
}
static int b_munmap(void *a, size_t l) { TL_ERRNO_BEGIN(); int r = munmap(a, l); TL_ERRNO_END(); return r; }
static int b_mprotect(void *a, size_t l, int prot) { TL_ERRNO_BEGIN(); int r = mprotect(a, l, prot_filter(prot, "mprotect")); TL_ERRNO_END(); return r; }
static int b_madvise(void *a, size_t l, int adv)
{
    /* MADV_DONTNEED (4) on Linux zeroes private pages; Darwin's keeps them. Zero them
     * explicitly where it is safe to: the pages must be mapped writable. */
    if (adv == 4) { return 0; }
    if (adv == 8) return madvise(a, l, MADV_FREE);          /* MADV_FREE */
    return 0;
}
static void *b_mremap(void *old, size_t olds, size_t news, int flags, void *newaddr)
{
    (void)flags; (void)newaddr;
    void *n = mmap(NULL, news, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (n == MAP_FAILED) { tl_set_guest_errno(12); return (void *)-1; }
    memcpy(n, old, olds < news ? olds : news);
    munmap(old, olds);
    return n;
}

/* ------------------------------------------------------------------- time */

static clockid_t clock_to_darwin(int id)
{
    switch (id) {
    case 0: return CLOCK_REALTIME;
    case 1: case 4: case 6: case 7: return CLOCK_MONOTONIC;
    case 2: return CLOCK_PROCESS_CPUTIME_ID;
    case 3: return CLOCK_THREAD_CPUTIME_ID;
    case 5: return CLOCK_REALTIME;
    default: return CLOCK_MONOTONIC;
    }
}
static int b_clock_gettime(int id, struct timespec *ts) { TL_ERRNO_BEGIN(); int r = clock_gettime(clock_to_darwin(id), ts); TL_ERRNO_END(); return r; }
static int b_clock_getres(int id, struct timespec *ts) { TL_ERRNO_BEGIN(); int r = clock_getres(clock_to_darwin(id), ts); TL_ERRNO_END(); return r; }
static int b_gettimeofday(int64_t *tv, void *tz)
{
    struct timeval t; (void)tz;
    gettimeofday(&t, NULL);
    if (tv) { tv[0] = t.tv_sec; tv[1] = t.tv_usec; }
    return 0;
}
static int b_nanosleep(const struct timespec *req, struct timespec *rem) { TL_ERRNO_BEGIN(); int r = nanosleep(req, rem); TL_ERRNO_END(); return r; }
static int b_usleep(unsigned us) { return usleep(us); }

/* ---------------------------------------------------------------- signals */

/* bionic's LP64 struct sigaction: flags, handler, 64-bit mask, restorer. */
typedef struct { int sa_flags; int pad; void *handler; uint64_t sa_mask; void *restorer; } guest_sigaction;
typedef void (*guest_handler)(int, void *, void *);

static struct { void *handler; int flags; } g_guest_sig[32];

static void host_signal_entry(int dsig, siginfo_t *info, void *uctx)
{
    int gsig = tl_signal_from_darwin(dsig);
    void *h = g_guest_sig[gsig < 32 ? gsig : 0].handler;
    if (!h || h == (void *)1) return;
    ((guest_handler)h)(gsig, info, uctx);
}

static int is_fault_signal(int g) { return g == 4 || g == 5 || g == 6 || g == 7 || g == 8 || g == 11; }

static int b_sigaction(int sig, const guest_sigaction *act, guest_sigaction *old)
{
    if (sig <= 0 || sig >= 32) { tl_set_guest_errno(22); return -1; }
    int d = tl_signal_to_darwin(sig);
    if (d <= 0) { tl_set_guest_errno(22); return -1; }
    if (old) {
        memset(old, 0, sizeof(*old));
        old->handler = g_guest_sig[sig].handler;
        old->sa_flags = g_guest_sig[sig].flags;
    }
    if (!act) return 0;
    g_guest_sig[sig].handler = act->handler;
    g_guest_sig[sig].flags = act->sa_flags;
    /* Synchronous fault signals stay with the host: the app's own crash handling and the
     * JIT guard depend on them, and a crash reporter replacing them would turn every
     * guest fault into silence. */
    if (is_fault_signal(sig)) {
        char note[96]; snprintf(note, sizeof(note), "guest installed a handler for fault signal %d: recorded, not installed", sig);
        tl_note_once(note);
        return 0;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    if (act->handler == (void *)0) sa.sa_handler = SIG_DFL;
    else if (act->handler == (void *)1) sa.sa_handler = SIG_IGN;
    else { sa.sa_sigaction = host_signal_entry; sa.sa_flags = SA_SIGINFO; }
    if (act->sa_flags & 0x10000000) sa.sa_flags |= SA_RESTART;
    sigemptyset(&sa.sa_mask);
    TL_ERRNO_BEGIN(); int r = sigaction(d, &sa, NULL); TL_ERRNO_END();
    return r;
}
static void *b_signal(int sig, void *handler)
{
    guest_sigaction a = { .handler = handler }, o;
    if (b_sigaction(sig, &a, &o)) return (void *)-1;
    return o.handler;
}
static int b_sigemptyset(uint64_t *s) { *s = 0; return 0; }
static int b_sigfillset(uint64_t *s) { *s = ~0ull; return 0; }
static int b_sigaddset(uint64_t *s, int sig) { if (sig < 1 || sig > 64) { tl_set_guest_errno(22); return -1; } *s |= 1ull << (sig - 1); return 0; }
static int b_sigdelset(uint64_t *s, int sig) { if (sig < 1 || sig > 64) { tl_set_guest_errno(22); return -1; } *s &= ~(1ull << (sig - 1)); return 0; }
static int b_sigsuspend(const uint64_t *mask)
{
    uint32_t m = 0;
    for (int s = 1; s < 32; s++) if (*mask & (1ull << (s - 1))) { int d = tl_signal_to_darwin(s); if (d > 0) m |= 1u << (d - 1); }
    sigset_t ds; memcpy(&ds, &m, sizeof(m));
    TL_ERRNO_BEGIN(); int r = sigsuspend(&ds); TL_ERRNO_END();
    return r;
}
typedef struct { void *ss_sp; int ss_flags; int pad; size_t ss_size; } guest_stack_t;
static int b_sigaltstack(const guest_stack_t *ss, guest_stack_t *old)
{
    stack_t d, od;
    if (ss) { d.ss_sp = ss->ss_sp; d.ss_size = ss->ss_size; d.ss_flags = ss->ss_flags & 2 ? SS_DISABLE : 0; }
    TL_ERRNO_BEGIN(); int r = sigaltstack(ss ? &d : NULL, old ? &od : NULL); TL_ERRNO_END();
    if (old && r == 0) { old->ss_sp = od.ss_sp; old->ss_size = od.ss_size; old->ss_flags = od.ss_flags & SS_DISABLE ? 2 : 0; }
    return r;
}
static int b_raise(int sig) { int d = tl_signal_to_darwin(sig); if (d < 0) { tl_set_guest_errno(22); return -1; } return raise(d); }


/* ------------------------------------------------- raw Linux system calls */

/*
 * What `svc #0` and syscall() mean here: the arm64 Linux numbers a guest actually uses,
 * answered with Darwin's equivalents. Returns the result, or -errno in Linux numbering.
 */
#define FUT_BUCKETS 64
static struct { pthread_mutex_t m; pthread_cond_t c; } g_fut[FUT_BUCKETS] = {
#define B { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER }
    B,B,B,B,B,B,B,B,B,B,B,B,B,B,B,B, B,B,B,B,B,B,B,B,B,B,B,B,B,B,B,B,
    B,B,B,B,B,B,B,B,B,B,B,B,B,B,B,B, B,B,B,B,B,B,B,B,B,B,B,B,B,B,B,B,
#undef B
};

static long futex_call(uint32_t *addr, int op, uint32_t val, const struct timespec *to)
{
    int cmd = op & 127;
    size_t h = ((uintptr_t)addr >> 2) % FUT_BUCKETS;
    if (cmd == 1 || cmd == 10) {                                  /* FUTEX_WAKE / WAKE_BITSET */
        pthread_mutex_lock(&g_fut[h].m);
        pthread_cond_broadcast(&g_fut[h].c);
        pthread_mutex_unlock(&g_fut[h].m);
        return val ? 1 : 0;
    }
    if (cmd == 0 || cmd == 9) {                                   /* FUTEX_WAIT / WAIT_BITSET */
        pthread_mutex_lock(&g_fut[h].m);
        if (__atomic_load_n(addr, __ATOMIC_SEQ_CST) != val) { pthread_mutex_unlock(&g_fut[h].m); return -11; }
        long r = 0;
        if (!to) {
            pthread_cond_wait(&g_fut[h].c, &g_fut[h].m);
        } else {
            struct timespec rel = *to;
            if (cmd == 9) {                                       /* absolute: against CLOCK_MONOTONIC unless the realtime flag is set */
                struct timespec now;
                clock_gettime((op & 256) ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
                rel.tv_sec = to->tv_sec - now.tv_sec; rel.tv_nsec = to->tv_nsec - now.tv_nsec;
                if (rel.tv_nsec < 0) { rel.tv_sec--; rel.tv_nsec += 1000000000L; }
                if (rel.tv_sec < 0) { rel.tv_sec = 0; rel.tv_nsec = 0; }
            }
            if (pthread_cond_timedwait_relative_np(&g_fut[h].c, &g_fut[h].m, &rel) == ETIMEDOUT) r = -110;
        }
        pthread_mutex_unlock(&g_fut[h].m);
        return r;
    }
    return -38;
}

long tl_linux_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long nr)
{
    (void)a4; (void)a5;
    switch (nr) {
    case 178: { uint64_t t = 0; pthread_threadid_np(NULL, &t); return (long)t; }       /* gettid */
    case 172: return getpid();
    case 173: return getppid();
    case 174: case 175: return getuid();
    case 176: case 177: return getgid();
    case 98:  return futex_call((uint32_t *)a0, (int)a1, (uint32_t)a2, (const struct timespec *)a3);
    case 129: case 130: case 131: {                                                     /* kill, tkill, tgkill */
        int sig = nr == 131 ? (int)a2 : (int)a1;
        if (sig == 0) return 0;
        int d = tl_signal_to_darwin(sig);
        if (d < 0) return -22;
        return raise(d) == 0 ? 0 : -3;
    }
    case 56: {                                                                          /* openat: only AT_FDCWD */
        if ((int)a0 != -100) return -38;
        int fd = b_open((const char *)a1, (int)a2, (unsigned)a3);
        return fd < 0 ? -*tl_guest_errno_ptr() : fd;
    }
    case 57: { int r = close((int)a0); return r < 0 ? -tl_errno_to_guest(errno) : r; }
    case 63: { long r = read((int)a0, (void *)a1, (size_t)a2); return r < 0 ? -tl_errno_to_guest(errno) : r; }
    case 64: { long r = write((int)a0, (const void *)a1, (size_t)a2); return r < 0 ? -tl_errno_to_guest(errno) : r; }
    case 113: { int r = clock_gettime(clock_to_darwin((int)a0), (struct timespec *)a1); return r < 0 ? -tl_errno_to_guest(errno) : 0; }
    case 169: return b_gettimeofday((int64_t *)a0, (void *)a1);
    case 93: case 94: tl_log_line("bionic: exit(%ld) by raw system call", a0); exit((int)a0);
    case 278: arc4random_buf((void *)a0, (size_t)a1); return a1;                        /* getrandom */
    case 283: return 0;                                                                 /* membarrier */
    case 134: case 135: return 0;                                                       /* rt_sigaction, rt_sigprocmask: accepted */
    case 233: return 0;                                                                 /* madvise */
    case 167: return 0;                                                                 /* prctl */
    case 160: return 0;                                                                 /* uname */
    default: break;
    }
    char what[96];
    snprintf(what, sizeof(what), "raw system call %ld is not provided (ENOSYS)", nr);
    tl_note_once(what);
    return -38;
}

/* ------------------------------------------------------- poll, select, etc. */

static int b_poll(struct pollfd *fds, unsigned long n, int timeout) { TL_ERRNO_BEGIN(); int r = poll(fds, (nfds_t)n, timeout); TL_ERRNO_END(); return r; }
static int b_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)
{
    TL_ERRNO_BEGIN(); int x = select(n, r, w, e, tv); TL_ERRNO_END(); return x;
}
static void b___FD_SET_chk(int fd, uint64_t *set, size_t size)
{
    if (fd < 0 || (size_t)fd >= size * 8) { tl_log_line("bionic: __FD_SET_chk: fd %d out of range", fd); abort(); }
    set[fd / 64] |= 1ull << (fd % 64);
}
static int b___FD_ISSET_chk(int fd, const uint64_t *set, size_t size)
{
    if (fd < 0 || (size_t)fd >= size * 8) { tl_log_line("bionic: __FD_ISSET_chk: fd %d out of range", fd); abort(); }
    return (set[fd / 64] >> (fd % 64)) & 1;
}
static void *b___cmsg_nxthdr(void *msg, void *cmsg) { (void)msg; (void)cmsg; return NULL; }

/* Linux-only facilities. Failing cleanly is the honest answer until something needs more. */
static int stub_enosys_i(const char *what)
{
    char note[96]; snprintf(note, sizeof(note), "%s is not provided (ENOSYS)", what);
    tl_note_once(note);
    tl_set_guest_errno(38);
    return -1;
}
static int b_epoll_create1(int flags) { (void)flags; return stub_enosys_i("epoll_create1"); }
static int b_epoll_ctl(int a, int b, int c, void *d) { (void)a; (void)b; (void)c; (void)d; return stub_enosys_i("epoll_ctl"); }
static int b_epoll_wait(int a, void *b, int c, int d) { (void)a; (void)b; (void)c; (void)d; return stub_enosys_i("epoll_wait"); }
static int b_eventfd(unsigned a, int b) { (void)a; (void)b; return stub_enosys_i("eventfd"); }
static int b_inotify_init(void) { return stub_enosys_i("inotify_init"); }
static int b_inotify_add_watch(int a, const char *b, unsigned c) { (void)a; (void)b; (void)c; return stub_enosys_i("inotify_add_watch"); }

/* Sockets: offline for now. A refused socket is a condition code already handles. */
static int sock_down(const char *what) { char n[96]; snprintf(n, sizeof(n), "%s: networking is not provided (ENETDOWN)", what); tl_note_once(n); tl_set_guest_errno(100); return -1; }
static int b_socket(int a, int b, int c) { (void)a; (void)b; (void)c; return sock_down("socket"); }
static int b_bind(int a, const void *b, unsigned c) { (void)a; (void)b; (void)c; return sock_down("bind"); }
static int b_connect(int a, const void *b, unsigned c) { (void)a; (void)b; (void)c; return sock_down("connect"); }
static int b_listen(int a, int b) { (void)a; (void)b; return sock_down("listen"); }
static int b_accept(int a, void *b, void *c) { (void)a; (void)b; (void)c; return sock_down("accept"); }
static long b_send(int a, const void *b, size_t c, int d) { (void)a; (void)b; (void)c; (void)d; return sock_down("send"); }
static long b_recv(int a, void *b, size_t c, int d) { (void)a; (void)b; (void)c; (void)d; return sock_down("recv"); }
static long b_sendto(int a, const void *b, size_t c, int d, const void *e, unsigned f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return sock_down("sendto"); }
static long b_recvfrom(int a, void *b, size_t c, int d, void *e, void *f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; return sock_down("recvfrom"); }
static long b_sendmsg(int a, const void *b, int c) { (void)a; (void)b; (void)c; return sock_down("sendmsg"); }
static long b_recvmsg(int a, void *b, int c) { (void)a; (void)b; (void)c; return sock_down("recvmsg"); }
static int b_shutdown(int a, int b) { (void)a; (void)b; return sock_down("shutdown"); }
static int b_getsockname(int a, void *b, void *c) { (void)a; (void)b; (void)c; return sock_down("getsockname"); }
static int b_getpeername(int a, void *b, void *c) { (void)a; (void)b; (void)c; return sock_down("getpeername"); }
static int b_setsockopt(int a, int b, int c, const void *d, unsigned e) { (void)a; (void)b; (void)c; (void)d; (void)e; return sock_down("setsockopt"); }
static int b_getsockopt(int a, int b, int c, void *d, void *e) { (void)a; (void)b; (void)c; (void)d; (void)e; return sock_down("getsockopt"); }
static int b_getaddrinfo(const char *a, const char *b, const void *c, void **d) { (void)a; (void)b; (void)c; (void)d; return 8; /* EAI_NONAME */ }
static void b_freeaddrinfo(void *a) { (void)a; }
static int b_getnameinfo(const void *a, unsigned b, char *c, unsigned d, char *e, unsigned f, int g) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; return 8; }
static void *b_gethostbyname(const char *n) { (void)n; return NULL; }
static void *b_gethostbyaddr(const void *a, unsigned b, int c) { (void)a; (void)b; (void)c; return NULL; }
static unsigned b_if_nametoindex(const char *n) { (void)n; return 0; }
static uint32_t b_inet_addr(const char *s) { return inet_addr(s); }
static int b_inet_pton(int af, const char *src, void *dst) { return inet_pton(af == 10 ? AF_INET6 : af, src, dst); }
static const char *b_inet_ntop(int af, const void *src, char *dst, unsigned size) { return inet_ntop(af == 10 ? AF_INET6 : af, src, dst, size); }

const tl_bionic_entry tl_tab_io[] = {
    TL_WRAP("open", b_open), TL_WRAP("__open_2", b___open_2), TL_WRAP("close", b_close), TL_WRAP("read", b_read),
    TL_WRAP("__read_chk", b___read_chk), TL_WRAP("write", b_write), TL_WRAP("writev", b_writev),
    TL_WRAP("pread64", b_pread64), TL_WRAP("lseek", b_lseek), TL_WRAP("lseek64", b_lseek),
    TL_WRAP("dup", b_dup), TL_WRAP("dup2", b_dup2), TL_WRAP("pipe", b_pipe), TL_WRAP("fsync", b_fsync),
    TL_WRAP("ftruncate", b_ftruncate), TL_WRAP("truncate", b_truncate), TL_WRAP("isatty", b_isatty), TL_WRAP("flock", b_flock),
    TL_WRAP("unlink", b_unlink), TL_WRAP("rmdir", b_rmdir), TL_WRAP("mkdir", b_mkdir), TL_WRAP("access", b_access),
    TL_WRAP("chmod", b_chmod), TL_WRAP("fchmod", b_fchmod), TL_WRAP("link", b_link), TL_WRAP("symlink", b_symlink),
    TL_WRAP("readlink", b_readlink), TL_WRAP("realpath", b_realpath), TL_WRAP("getcwd", b_getcwd),
    TL_WRAP("utimes", b_utimes), TL_WRAP("utime", b_utime), TL_WRAP("futimens", b_futimens),
    TL_WRAP("__umask_chk", b___umask_chk), TL_WRAP("sendfile", b_sendfile),
    TL_WRAP("stat", b_stat), TL_WRAP("lstat", b_lstat), TL_WRAP("fstat", b_fstat), TL_WRAP("statfs", b_statfs),
    TL_WRAP("fcntl", b_fcntl), TL_WRAP("ioctl", b_ioctl),
    TL_WRAP("opendir", b_opendir), TL_WRAP("readdir", b_readdir), TL_WRAP("closedir", b_closedir),
    TL_WRAP("mmap", b_mmap), TL_WRAP("munmap", b_munmap), TL_WRAP("mprotect", b_mprotect), TL_WRAP("madvise", b_madvise),
    TL_WRAP("mremap", b_mremap),
    TL_WRAP("clock_gettime", b_clock_gettime), TL_WRAP("clock_getres", b_clock_getres), TL_WRAP("gettimeofday", b_gettimeofday),
    TL_WRAP("nanosleep", b_nanosleep), TL_WRAP("usleep", b_usleep),
    TL_DIRECT(clock), TL_DIRECT(time), TL_DIRECT(difftime), TL_DIRECT(gmtime), TL_DIRECT(gmtime_r), TL_DIRECT(localtime),
    TL_DIRECT(localtime_r), TL_DIRECT(mktime), TL_DIRECT(strftime), TL_DIRECT(strftime_l), TL_DIRECT(tzset),
    TL_WRAP("sigaction", b_sigaction), TL_WRAP("signal", b_signal), TL_WRAP("sigemptyset", b_sigemptyset),
    TL_WRAP("sigfillset", b_sigfillset), TL_WRAP("sigaddset", b_sigaddset), TL_WRAP("sigdelset", b_sigdelset),
    TL_WRAP("sigsuspend", b_sigsuspend), TL_WRAP("sigaltstack", b_sigaltstack), TL_WRAP("raise", b_raise),
    TL_WRAP("poll", b_poll), TL_WRAP("select", b_select), TL_WRAP("__FD_SET_chk", b___FD_SET_chk),
    TL_WRAP("__FD_ISSET_chk", b___FD_ISSET_chk), TL_WRAP("__cmsg_nxthdr", b___cmsg_nxthdr),
    TL_WRAP("epoll_create1", b_epoll_create1), TL_WRAP("epoll_ctl", b_epoll_ctl), TL_WRAP("epoll_wait", b_epoll_wait),
    TL_WRAP("eventfd", b_eventfd), TL_WRAP("inotify_init", b_inotify_init), TL_WRAP("inotify_add_watch", b_inotify_add_watch),
    TL_WRAP("socket", b_socket), TL_WRAP("bind", b_bind), TL_WRAP("connect", b_connect), TL_WRAP("listen", b_listen),
    TL_WRAP("accept", b_accept), TL_WRAP("send", b_send), TL_WRAP("recv", b_recv), TL_WRAP("sendto", b_sendto),
    TL_WRAP("recvfrom", b_recvfrom), TL_WRAP("sendmsg", b_sendmsg), TL_WRAP("recvmsg", b_recvmsg),
    TL_WRAP("shutdown", b_shutdown), TL_WRAP("getsockname", b_getsockname), TL_WRAP("getpeername", b_getpeername),
    TL_WRAP("setsockopt", b_setsockopt), TL_WRAP("getsockopt", b_getsockopt), TL_WRAP("getaddrinfo", b_getaddrinfo),
    TL_WRAP("freeaddrinfo", b_freeaddrinfo), TL_WRAP("getnameinfo", b_getnameinfo), TL_WRAP("gethostbyname", b_gethostbyname),
    TL_WRAP("gethostbyaddr", b_gethostbyaddr), TL_WRAP("if_nametoindex", b_if_nametoindex),
    TL_WRAP("inet_addr", b_inet_addr), TL_WRAP("inet_pton", b_inet_pton), TL_WRAP("inet_ntop", b_inet_ntop),
    TL_END
};
