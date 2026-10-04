/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-tl-ld.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "husk-tl-internal.h"
#include "husk-tl-xmem.h"

/* The project's log sink, and the bionic shim's surface. */
void  tl_log_line(const char *fmt, ...);
void *tl_bionic_find(const char *name);
bool  tl_bionic_is_system_lib(const char *soname);

/* ------------------------------------------------------------------ ELF */

typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
                 uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
                 uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } elf_ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align; } elf_phdr;
typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx; uint64_t st_value, st_size; } elf_sym;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } elf_rela;
typedef struct { int64_t d_tag; uint64_t d_val; } elf_dyn;

enum {
    PT_LOAD_ = 1, PT_DYNAMIC_ = 2, PT_TLS_ = 7, PT_GNU_RELRO_ = 0x6474e552,
    DT_NULL_ = 0, DT_NEEDED_ = 1, DT_PLTRELSZ_ = 2, DT_HASH_ = 4, DT_STRTAB_ = 5, DT_SYMTAB_ = 6,
    DT_RELA_ = 7, DT_RELASZ_ = 8, DT_STRSZ_ = 10, DT_INIT_ = 12, DT_SONAME_ = 14, DT_JMPREL_ = 23,
    DT_INIT_ARRAY_ = 25, DT_INIT_ARRAYSZ_ = 27, DT_FLAGS_ = 30, DT_RELRSZ_ = 35, DT_RELR_ = 36,
    DT_GNU_HASH_ = 0x6ffffef5, DT_ANDROID_RELA_ = 0x60000011, DT_ANDROID_RELASZ_ = 0x60000012,
    DT_ANDROID_RELR_ = 0x6fffe000, DT_ANDROID_RELRSZ_ = 0x6fffe001,
    R_NONE = 0, R_ABS64 = 257, R_GLOB_DAT = 1025, R_JUMP_SLOT = 1026, R_RELATIVE = 1027,
    R_TLS_DTPMOD = 1028, R_TLS_TPREL = 1030, R_TLSDESC = 1031, R_IRELATIVE = 1032,
    STB_WEAK_ = 2, STT_TLS_ = 6, STT_GNU_IFUNC_ = 10, SHN_UNDEF_ = 0,
    PF_X_ = 1, PF_W_ = 2, PF_R_ = 4, EM_AARCH64_ = 183,
};

#define PAGE TL_XMEM_PAGE

/* ----------------------------------------------------------------- libs */

#define MAX_LIBS 96
#define MAX_DEPS 48

struct tl_lib {
    char name[96];                 /* as asked for */
    char soname[96];
    uint8_t *rx, *rw;              /* image: address of base_vaddr, in each view */
    uint64_t base_vaddr;
    size_t npages;
    uint8_t *pflags;               /* TL_PAGE_* per page */
    elf_phdr *phdr;                /* malloc'd copy, file addresses */
    unsigned phnum;

    /* dynamic section, as vaddrs */
    uint64_t strtab, strsz, symtab, gnu_hash, sysv_hash;
    uint64_t rela, relasz, jmprel, pltrelsz, arela, arelasz, relr, relrsz;
    uint64_t init, init_array, init_arraysz;
    uint32_t nsyms;
    uint64_t *symcache;            /* resolved import per symbol index; 0 = not yet */

    uint64_t needed[MAX_DEPS];
    int nneeded;
    struct tl_lib *deps[MAX_DEPS]; /* the closure, breadth-first, excluding self */
    int ndeps;
    bool deps_ready;

    uint8_t *stub_rx, *stub_rw;    /* one page after the image: stubs for rewritten `svc` sites */
    size_t stub_used;

    int state;                     /* 0 mapped, 1 relocating, 2 relocated, 3 initialising, 4 initialised */
    uint32_t n_unresolved;
};

static struct {
    pthread_mutex_t lock;
    tl_lib *libs[MAX_LIBS];
    int nlibs;
    tl_zip apks[4];
    int napks;
    int verbosity;
    size_t unresolved;
    uint8_t *tcb_rx, *tcb_rw;      /* the fake thread block every `mrs tpidr_el0` reads */
    char **argv, **envp;
    bool recursive_init;
} G = { .lock = PTHREAD_MUTEX_INITIALIZER, .verbosity = 1 };

static pthread_mutex_t g_big = PTHREAD_MUTEX_INITIALIZER;   /* serialises load/init */

void tl_ld_set_verbosity(int v) { G.verbosity = v; }
void tl_ld_set_environment(char **argv, char **envp) { G.argv = argv; G.envp = envp; }
size_t tl_ld_unresolved_count(void) { return G.unresolved; }

static inline const void *at(const tl_lib *L, uint64_t vaddr) { return L->rw + (vaddr - L->base_vaddr); }

/* ------------------------------------------------------------- the APKs */

bool tl_ld_add_apk(const char *path)
{
    if (G.napks >= 4) return false;
    char err[160];
    if (!tl_zip_open(&G.apks[G.napks], path, err, sizeof(err))) {
        tl_log_line("ld: cannot open %s: %s", path, err);
        return false;
    }
    G.napks++;
    return true;
}

const tl_zip *tl_ld_apk_at(int i) { return (i >= 0 && i < G.napks) ? &G.apks[i] : NULL; }

static bool fetch_from_apks(const char *name, uint8_t **out, size_t *len)
{
    char path[160];
    snprintf(path, sizeof(path), "lib/arm64-v8a/%s", name);
    for (int i = 0; i < G.napks; i++) {
        const tl_zip_entry *e = tl_zip_find(&G.apks[i], path);
        if (!e) continue;
        const uint8_t *data; size_t n; bool owned; char err[160];
        if (!tl_zip_data(&G.apks[i], e, (size_t)1 << 30, &data, &n, &owned, err, sizeof(err))) {
            tl_log_line("ld: %s: %s", name, err);
            return false;
        }
        if (!owned) {
            uint8_t *copy = malloc(n);
            if (!copy) return false;
            memcpy(copy, data, n);
            data = copy;
        }
        *out = (uint8_t *)data;
        *len = n;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------- hashing */

static uint32_t gnu_hash(const char *s)
{
    uint32_t h = 5381;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) h = h * 33 + *p;
    return h;
}

static uint32_t sysv_hash(const char *s)
{
    uint32_t h = 0, g;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h = (h << 4) + *p;
        if ((g = h & 0xf0000000u)) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static const char *sym_name(const tl_lib *L, const elf_sym *s)
{
    return (const char *)at(L, L->strtab) + s->st_name;
}

static const elf_sym *sym_at(const tl_lib *L, uint32_t i)
{
    return (const elf_sym *)at(L, L->symtab) + i;
}

/* The address a defined symbol is known by: the writable view for data. */
static void *sym_value(const tl_lib *L, const elf_sym *s)
{
    uint64_t off = s->st_value - L->base_vaddr;
    size_t page = (size_t)(off / PAGE);
    bool is_func = (s->st_info & 0xf) == 2;
    if (!is_func && page < L->npages && (L->pflags[page] & TL_PAGE_W)) return L->rw + off;
    return L->rx + off;
}

static const elf_sym *lib_find(const tl_lib *L, const char *name)
{
    if (!L->symtab || !L->strtab) return NULL;
    if (L->gnu_hash) {
        const uint8_t *g = at(L, L->gnu_hash);
        uint32_t nb, symoff, bloom_n, bloom_shift;
        memcpy(&nb, g, 4); memcpy(&symoff, g + 4, 4); memcpy(&bloom_n, g + 8, 4); memcpy(&bloom_shift, g + 12, 4);
        if (!nb) return NULL;
        const uint64_t *bloom = (const uint64_t *)(g + 16);
        const uint32_t *buckets = (const uint32_t *)(g + 16 + (size_t)bloom_n * 8);
        const uint32_t *chains = buckets + nb;
        uint32_t h = gnu_hash(name);
        if (bloom_n) {
            uint64_t w = bloom[(h / 64) % bloom_n];
            if (!((w >> (h % 64)) & 1) || !((w >> ((h >> bloom_shift) % 64)) & 1)) return NULL;
        }
        uint32_t b = buckets[h % nb];
        if (b < symoff) return NULL;
        for (uint32_t i = b;; i++) {
            uint32_t c = chains[i - symoff];
            if ((h | 1) == (c | 1)) {
                const elf_sym *s = sym_at(L, i);
                if (s->st_shndx != SHN_UNDEF_ && !strcmp(sym_name(L, s), name)) return s;
            }
            if (c & 1) break;
        }
        return NULL;
    }
    if (L->sysv_hash) {
        const uint32_t *t = at(L, L->sysv_hash);
        uint32_t nb = t[0], nc = t[1];
        if (!nb) return NULL;
        for (uint32_t i = t[2 + sysv_hash(name) % nb]; i && i < nc; i = t[2 + nb + i]) {
            const elf_sym *s = sym_at(L, i);
            if (s->st_shndx != SHN_UNDEF_ && !strcmp(sym_name(L, s), name)) return s;
        }
    }
    return NULL;
}

static uint32_t count_dynsyms(const tl_lib *L)
{
    if (L->sysv_hash) return ((const uint32_t *)at(L, L->sysv_hash))[1];
    if (!L->gnu_hash) return 0;
    const uint8_t *g = at(L, L->gnu_hash);
    uint32_t nb, symoff, bloom_n;
    memcpy(&nb, g, 4); memcpy(&symoff, g + 4, 4); memcpy(&bloom_n, g + 8, 4);
    const uint32_t *buckets = (const uint32_t *)(g + 16 + (size_t)bloom_n * 8);
    const uint32_t *chains = buckets + nb;
    uint32_t last = 0;
    for (uint32_t i = 0; i < nb; i++) if (buckets[i] > last) last = buckets[i];
    if (last < symoff) return symoff;
    while (!(chains[last - symoff] & 1)) last++;
    return last + 1;
}

/* ----------------------------------------------------- lookup by scope */

static tl_lib *find_loaded(const char *name)
{
    for (int i = 0; i < G.nlibs; i++) {
        if (!strcmp(G.libs[i]->name, name) || !strcmp(G.libs[i]->soname, name)) return G.libs[i];
    }
    return NULL;
}

tl_lib *tl_ld_find_lib(const char *name) { return find_loaded(name); }

static void build_scope(tl_lib *L)
{
    if (L->deps_ready) return;
    L->ndeps = 0;
    tl_lib *queue[MAX_DEPS + 1];
    int qh = 0, qt = 0;
    queue[qt++] = L;
    while (qh < qt && L->ndeps < MAX_DEPS) {
        tl_lib *cur = queue[qh++];
        for (int i = 0; i < cur->nneeded; i++) {
            const char *n = (const char *)at(cur, cur->strtab) + cur->needed[i];
            tl_lib *d = find_loaded(n);
            if (!d || d == L) continue;
            bool seen = false;
            for (int k = 0; k < L->ndeps; k++) if (L->deps[k] == d) seen = true;
            if (seen) continue;
            L->deps[L->ndeps++] = d;
            if (qt < MAX_DEPS + 1) queue[qt++] = d;
        }
    }
    L->deps_ready = true;
}

/* Symbol lookup the way a library sees it: its own scope, then the system. */
static void *lookup_for(tl_lib *L, const char *name, bool *weak_hit)
{
    (void)weak_hit;
    build_scope(L);
    const elf_sym *s = lib_find(L, name);
    if (s && (s->st_info & 0xf) != STT_GNU_IFUNC_) return sym_value(L, s);
    for (int i = 0; i < L->ndeps; i++) {
        s = lib_find(L->deps[i], name);
        if (s && (s->st_info & 0xf) != STT_GNU_IFUNC_) return sym_value(L->deps[i], s);
    }
    return tl_bionic_find(name);
}

void *tl_ld_sym(tl_lib *lib, const char *name)
{
    if (lib) {
        const elf_sym *s = lib_find(lib, name);
        return s ? sym_value(lib, s) : NULL;
    }
    for (int i = 0; i < G.nlibs; i++) {
        const elf_sym *s = lib_find(G.libs[i], name);
        if (s) return sym_value(G.libs[i], s);
    }
    return NULL;
}

tl_lib *tl_ld_lib_of(const void *addr)
{
    const uint8_t *a = addr;
    for (int i = 0; i < G.nlibs; i++) {
        tl_lib *L = G.libs[i];
        size_t span = L->npages * PAGE;
        if ((a >= L->rx && a < L->rx + span) || (a >= L->rw && a < L->rw + span)) return L;
    }
    return NULL;
}

const char *tl_ld_lib_name(const tl_lib *lib) { return lib ? lib->name : NULL; }

const char *tl_ld_symbol_at(const void *addr, const char **lib_name, const void **sym_addr)
{
    tl_lib *L = tl_ld_lib_of(addr);
    if (!L) return NULL;
    if (lib_name) *lib_name = L->name;
    uint64_t off = (const uint8_t *)addr >= L->rx && (const uint8_t *)addr < L->rx + L->npages * PAGE
                 ? (uint64_t)((const uint8_t *)addr - L->rx) : (uint64_t)((const uint8_t *)addr - L->rw);
    const elf_sym *best = NULL;
    uint32_t n = L->nsyms;
    for (uint32_t i = 1; i < n; i++) {
        const elf_sym *s = sym_at(L, i);
        if (s->st_shndx == SHN_UNDEF_ || !s->st_value) continue;
        uint64_t so = s->st_value - L->base_vaddr;
        if (so <= off && (!best || so > best->st_value - L->base_vaddr)) best = s;
    }
    if (!best) return NULL;
    if (sym_addr) *sym_addr = L->rx + (best->st_value - L->base_vaddr);
    return sym_name(L, best);
}

int tl_ld_iterate(tl_ld_phdr_cb cb, void *user)
{
    int r = 0;
    for (int i = 0; i < G.nlibs && !r; i++) {
        tl_lib *L = G.libs[i];
        if (L->state < 2) continue;
        r = cb((uintptr_t)(L->rx - L->base_vaddr), L->name, L->phdr, L->phnum, user);
    }
    return r;
}

/* ------------------------------------------------- unresolved-import stubs */

/*
 * An import nothing provides is bound to a 32-byte stub in the executable region
 * that loads its own name and jumps to the logger, so the first call announces
 * exactly what was missing. Failing the whole load would hide every import after
 * the first one; most of these are never called.
 */
static void tl_unresolved_called(const char *name, void *lr)
{
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    static const char *seen[512];
    static int nseen;
    pthread_mutex_lock(&m);
    bool again = false;
    for (int i = 0; i < nseen; i++) if (seen[i] == name) again = true;
    if (!again && nseen < 512) seen[nseen++] = name;
    pthread_mutex_unlock(&m);
    if (!again) {
        const char *ln = NULL; const void *sa = NULL;
        const char *caller = tl_ld_symbol_at(lr, &ln, &sa);
        tl_log_line("ld: CALLED an unresolved import: %s (from %s %s+%#lx)", name, ln ? ln : "?",
                    caller ? caller : "?", sa ? (unsigned long)((const char *)lr - (const char *)sa) : 0ul);
    }
}

__attribute__((naked, used)) static void tl_unresolved_entry(void)
{
#if defined(__aarch64__)
    __asm__ volatile(
        "mov x0, x17\n"
        "mov x1, x30\n"
        "stp x29, x30, [sp, #-16]!\n"
        "bl _tl_unresolved_called_c\n"
        "ldp x29, x30, [sp], #16\n"
        "mov x0, #0\n"
        "ret\n");
#endif
}
void tl_unresolved_called_c(const char *name, void *lr);
void tl_unresolved_called_c(const char *name, void *lr) { tl_unresolved_called(name, lr); }

static uint8_t *g_stub_rx, *g_stub_rw;
static size_t g_stub_left;

static void *make_stub(const char *name)
{
    if (g_stub_left < 32) {
        if (!tl_xmem_alloc(PAGE, &g_stub_rx, &g_stub_rw)) return NULL;
        g_stub_left = PAGE;
    }
    uint8_t *rw = g_stub_rw, *rx = g_stub_rx;
    uint32_t code[4] = {
        0x58000090u,        /* ldr x16, #16  (the entry) */
        0x580000B1u,        /* ldr x17, #20  (the name)  */
        0xD61F0200u,        /* br  x16                   */
        0xD503201Fu,        /* nop                       */
    };
    memcpy(rw, code, 16);
    uint64_t entry = (uint64_t)(uintptr_t)tl_unresolved_entry, nm = (uint64_t)(uintptr_t)name;
    memcpy(rw + 16, &entry, 8);
    memcpy(rw + 24, &nm, 8);
    tl_xmem_flush(rx, 32);
    g_stub_rx += 32; g_stub_rw += 32; g_stub_left -= 32;
    return rx;
}

/* ------------------------------------------------------- raw system calls */

/*
 * Some libraries make Linux system calls themselves: `mov x8, #nr; svc #0`. On Darwin
 * that traps into a different kernel's table. Each such site is rewritten to branch to
 * a small stub in the library's own stub page, which saves the two scratch registers
 * the host might clobber, calls tl_svc_common, and branches back to the instruction
 * after the site. tl_svc_common saves everything else the C handler may disturb --
 * the kernel preserves all registers but x0 across a system call, so code around a
 * raw `svc` relies on that -- and calls tl_linux_syscall with the arguments and number.
 */
long tl_linux_syscall(long a0, long a1, long a2, long a3, long a4, long a5, long nr);

__attribute__((naked, used)) void tl_svc_common(void)
{
#if defined(__aarch64__)
    __asm__ volatile(
        "stp x29, x30, [sp, #-16]!\n"
        "mov x29, sp\n"
        "sub sp, sp, #512\n"
        "stp x1, x2, [sp, #0]\n"
        "stp x3, x4, [sp, #16]\n"
        "stp x5, x6, [sp, #32]\n"
        "stp x7, x8, [sp, #48]\n"
        "stp x9, x10, [sp, #64]\n"
        "stp x11, x12, [sp, #80]\n"
        "stp x13, x14, [sp, #96]\n"
        "str x15, [sp, #112]\n"
        "stp q0, q1, [sp, #128]\n"
        "stp q2, q3, [sp, #160]\n"
        "stp q4, q5, [sp, #192]\n"
        "stp q6, q7, [sp, #224]\n"
        "stp q16, q17, [sp, #256]\n"
        "stp q18, q19, [sp, #288]\n"
        "stp q20, q21, [sp, #320]\n"
        "stp q22, q23, [sp, #352]\n"
        "stp q24, q25, [sp, #384]\n"
        "stp q26, q27, [sp, #416]\n"
        "stp q28, q29, [sp, #448]\n"
        "stp q30, q31, [sp, #480]\n"
        "mov x6, x8\n"
        "bl _tl_linux_syscall\n"
        "ldp x1, x2, [sp, #0]\n"
        "ldp x3, x4, [sp, #16]\n"
        "ldp x5, x6, [sp, #32]\n"
        "ldp x7, x8, [sp, #48]\n"
        "ldp x9, x10, [sp, #64]\n"
        "ldp x11, x12, [sp, #80]\n"
        "ldp x13, x14, [sp, #96]\n"
        "ldr x15, [sp, #112]\n"
        "ldp q0, q1, [sp, #128]\n"
        "ldp q2, q3, [sp, #160]\n"
        "ldp q4, q5, [sp, #192]\n"
        "ldp q6, q7, [sp, #224]\n"
        "ldp q16, q17, [sp, #256]\n"
        "ldp q18, q19, [sp, #288]\n"
        "ldp q20, q21, [sp, #320]\n"
        "ldp q22, q23, [sp, #352]\n"
        "ldp q24, q25, [sp, #384]\n"
        "ldp q26, q27, [sp, #416]\n"
        "ldp q28, q29, [sp, #448]\n"
        "ldp q30, q31, [sp, #480]\n"
        "mov sp, x29\n"
        "ldp x29, x30, [sp], #16\n"
        "ret\n");
#endif
}

/* A 32-byte stub for the `svc` at site_rx; returns its executable address. */
static uint8_t *svc_stub(tl_lib *L, const uint8_t *site_rx)
{
    if (L->stub_used + 32 > PAGE) return NULL;
    uint8_t *rx = L->stub_rx + L->stub_used, *rw = L->stub_rw + L->stub_used;
    L->stub_used += 32;
    uint32_t imm19 = (uint32_t)(((int64_t)L->stub_rx - (int64_t)(rx + 8)) / 4) & 0x7FFFFu;
    int64_t back = ((int64_t)(site_rx + 4) - (int64_t)(rx + 24)) / 4;
    uint32_t code[8] = {
        0xA9BF7BFDu,                    /* stp x29, x30, [sp, #-16]! */
        0xA9BF47F0u,                    /* stp x16, x17, [sp, #-16]! */
        0x58000010u | (imm19 << 5),     /* ldr x16, <the tl_svc_common literal at the page start> */
        0xD63F0200u,                    /* blr x16 */
        0xA8C147F0u,                    /* ldp x16, x17, [sp], #16 */
        0xA8C17BFDu,                    /* ldp x29, x30, [sp], #16 */
        0x14000000u | ((uint32_t)back & 0x3FFFFFFu),   /* b site+4 */
        0xD503201Fu,                    /* nop */
    };
    memcpy(rw, code, 32);
    return rx;
}

/* --------------------------------------------------------------- patching */

#if defined(__aarch64__)
static uint32_t encode_adrp(uint32_t rt, const void *pc, const void *target)
{
    int64_t delta = ((int64_t)((uintptr_t)target & ~(uintptr_t)0xFFF)
                   - (int64_t)((uintptr_t)pc & ~(uintptr_t)0xFFF)) >> 12;
    uint32_t imm = (uint32_t)delta & 0x1FFFFF;
    return 0x90000000u | ((imm & 3u) << 29) | ((imm >> 2) << 5) | (rt & 0x1Fu);
}

/*
 * Two rewrites in executable pages, both on the writable view:
 *
 *  - `mrs Xt, tpidr_el0` becomes `adrp Xt, <fake thread block>`. Android code
 *    reads its stack-protector cookie from [tpidr_el0 + 0x28]; Darwin keeps its
 *    own thread pointer elsewhere and leaves this register for nothing in
 *    particular. Every thread sharing one cookie is harmless: the cookie only has
 *    to be the same at a function's entry and exit.
 *  - an `adrp` that points into this image's writable pages is retargeted at the
 *    writable view, because code reaches its globals pc-relatively and the page
 *    the executable view shows is not writable.
 */
static void patch_image(tl_lib *L, size_t *n_tpidr, size_t *n_adrp, size_t *n_adr, size_t *n_svc)
{
    ptrdiff_t delta = L->rw - L->rx;
    *n_tpidr = *n_adrp = *n_adr = *n_svc = 0;
    for (size_t pg = 0; pg < L->npages; pg++) {
        if (!(L->pflags[pg] & TL_PAGE_X)) continue;
        uint32_t *w = (uint32_t *)(L->rw + pg * PAGE);
        const uint8_t *x = L->rx + pg * PAGE;
        for (size_t i = 0; i < PAGE / 4; i++) {
            uint32_t insn = w[i];
            const uint8_t *pc = x + i * 4;
            if ((insn & 0xFFFFFFE0u) == 0xD53BD040u) {            /* mrs Xt, tpidr_el0 */
                w[i] = encode_adrp(insn & 0x1Fu, pc, G.tcb_rw);
                (*n_tpidr)++;
            } else if ((insn & 0x9F000000u) == 0x90000000u) {     /* adrp */
                int64_t imm = (int64_t)((((insn >> 5) & 0x7FFFFu) << 2) | ((insn >> 29) & 3u));
                if (imm & 0x100000) imm -= 0x200000;
                uintptr_t tp = ((uintptr_t)pc & ~(uintptr_t)0xFFF) + (uintptr_t)(imm << 12);
                if (tp >= (uintptr_t)L->rx && tp < (uintptr_t)L->rx + L->npages * PAGE) {
                    size_t tpg = (tp - (uintptr_t)L->rx) / PAGE;
                    if (L->pflags[tpg] & TL_PAGE_W) {
                        w[i] = encode_adrp(insn & 0x1Fu, pc, (const void *)(tp + (uintptr_t)delta));
                        (*n_adrp)++;
                    }
                }
            } else if (insn == 0xD4000001u) {                       /* svc #0 */
                uint8_t *stub = svc_stub(L, pc);
                int64_t off = stub ? ((int64_t)stub - (int64_t)pc) / 4 : 0;
                if (stub && off > -(1 << 25) && off < (1 << 25)) {
                    w[i] = 0x14000000u | ((uint32_t)off & 0x3FFFFFFu);
                    (*n_svc)++;
                } else {
                    tl_log_line("ld: %s: could not rewrite an svc site", L->name);
                }
            } else if ((insn & 0x9F000000u) == 0x10000000u) {     /* adr */
                int64_t imm = (int64_t)((((insn >> 5) & 0x7FFFFu) << 2) | ((insn >> 29) & 3u));
                if (imm & 0x100000) imm -= 0x200000;
                uintptr_t tp = (uintptr_t)pc + (uintptr_t)imm;
                if (tp >= (uintptr_t)L->rx && tp < (uintptr_t)L->rx + L->npages * PAGE
                    && (L->pflags[(tp - (uintptr_t)L->rx) / PAGE] & TL_PAGE_W)) {
                    (*n_adr)++;
                }
            }
        }
    }
}
#else
static void patch_image(tl_lib *L, size_t *a, size_t *b, size_t *c, size_t *d) { (void)L; *a = *b = *c = *d = 0; }
#endif

/* -------------------------------------------------------------- relocation */

static inline uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* An address inside the image as pointer data records it: writable view for data. */
static uint64_t image_addr(const tl_lib *L, uint64_t vaddr)
{
    uint64_t off = vaddr - L->base_vaddr;
    size_t page = (size_t)(off / PAGE);
    if (page < L->npages && (L->pflags[page] & TL_PAGE_W)) return (uint64_t)(uintptr_t)(L->rw + off);
    return (uint64_t)(uintptr_t)(L->rx + off);
}

static uint64_t bind_symbol(tl_lib *L, uint32_t symidx, bool *failed)
{
    if (L->symcache && L->symcache[symidx]) return L->symcache[symidx];
    const elf_sym *s = sym_at(L, symidx);
    const char *name = sym_name(L, s);
    uint64_t val = 0;
    if (s->st_shndx != SHN_UNDEF_ && (s->st_info >> 4) != STB_WEAK_) {
        /* Defined here. Search the scope anyway so an earlier library's definition
         * wins, as it does under ELF interposition... except that a library's own
         * definition is what Android's linker uses first for its own symbols. */
        if ((s->st_info & 0xf) == STT_GNU_IFUNC_) { *failed = true; return 0; }
        val = (uint64_t)(uintptr_t)sym_value(L, s);
    } else {
        void *a = lookup_for(L, name, NULL);
        if (a) {
            val = (uint64_t)(uintptr_t)a;
        } else if ((s->st_info >> 4) == STB_WEAK_) {
            val = 0;
        } else {
            void *stub = make_stub(name);
            if (!stub) { *failed = true; return 0; }
            val = (uint64_t)(uintptr_t)stub;
            L->n_unresolved++;
            G.unresolved++;
            if (G.verbosity >= 2) tl_log_line("ld: %s: unresolved import %s", L->name, name);
        }
    }
    if (L->symcache) L->symcache[symidx] = val ? val : 1;   /* 1 marks a resolved NULL */
    return val;
}

static bool reloc_one(tl_lib *L, uint64_t r_offset, uint32_t type, uint32_t symidx, int64_t addend)
{
    uint64_t off = r_offset - L->base_vaddr;
    if (off + 8 > L->npages * PAGE) return false;
    uint64_t *place = (uint64_t *)(L->rw + off);
    bool failed = false;
    switch (type) {
    case R_NONE:
        return true;
    case R_RELATIVE:
        *place = image_addr(L, (uint64_t)addend);
        return true;
    case R_ABS64: case R_GLOB_DAT: case R_JUMP_SLOT: {
        if (symidx == 0) { *place = image_addr(L, (uint64_t)addend); return true; }
        uint64_t v = bind_symbol(L, symidx, &failed);
        if (failed) return false;
        if (v == 1 && L->symcache) v = 0;
        *place = v + (type == R_ABS64 ? (uint64_t)addend : 0);
        return true;
    }
    case R_IRELATIVE: {
        /* The resolver is guest code: run it, store what it returns. */
        uint64_t (*resolver)(void) = (uint64_t (*)(void))(uintptr_t)(L->rx + ((uint64_t)addend - L->base_vaddr));
        *place = resolver();
        return true;
    }
    case R_TLS_DTPMOD: case R_TLS_TPREL: case R_TLSDESC:
        tl_log_line("ld: %s: TLS relocation (type %u) -- thread-local storage is not implemented", L->name, type);
        return false;
    default:
        tl_log_line("ld: %s: unsupported relocation type %u", L->name, type);
        return false;
    }
}

static bool do_relas(tl_lib *L, uint64_t addr, uint64_t size, size_t *count)
{
    if (!addr || !size) return true;
    const elf_rela *r = at(L, addr);
    for (size_t i = 0; i < size / sizeof(elf_rela); i++) {
        if (!reloc_one(L, r[i].r_offset, (uint32_t)(r[i].r_info & 0xffffffffu),
                       (uint32_t)(r[i].r_info >> 32), r[i].r_addend)) return false;
        (*count)++;
    }
    return true;
}

typedef struct { const uint8_t *p, *end; bool bad; } sleb;
static int64_t rd_sleb(sleb *s)
{
    uint64_t v = 0; unsigned shift = 0; uint8_t b;
    do {
        if (s->p >= s->end || shift >= 64) { s->bad = true; return 0; }
        b = *s->p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) v |= ~0ull << shift;
    return (int64_t)v;
}

static bool do_packed(tl_lib *L, size_t *count)
{
    /*
     * Android's APS2 encoding, read the way bionic's linker reads it. Offsets and
     * addends are running totals, not absolute values: each entry adds a signed
     * delta to the previous one. Within a group the order is: offset delta (if the
     * group shares one), info (if shared), shared-addend delta, then per entry its
     * own offset delta, info and addend delta as the flags leave them unshared.
     */
    if (!L->arela || !L->arelasz) return true;
    const uint8_t *d = at(L, L->arela);
    if (memcmp(d, "APS2", 4) != 0) { tl_log_line("ld: %s: packed relocations lack APS2 magic", L->name); return false; }
    sleb s = { d + 4, d + L->arelasz, false };
    int64_t total = rd_sleb(&s);
    uint64_t r_offset = (uint64_t)rd_sleb(&s), r_info = 0;
    int64_t r_addend = 0;
    if (s.bad || total < 0) return false;
    enum { BY_INFO = 1, BY_DELTA = 2, BY_ADDEND = 4, HAS_ADDEND = 8 };
    for (int64_t idx = 0; idx < total;) {
        int64_t group = rd_sleb(&s), flags = rd_sleb(&s);
        if (s.bad || group <= 0 || idx + group > total) return false;
        uint64_t group_delta = 0;
        if (flags & BY_DELTA) group_delta = (uint64_t)rd_sleb(&s);
        if (flags & BY_INFO) r_info = (uint64_t)rd_sleb(&s);
        int addend_mode = (int)(flags & (HAS_ADDEND | BY_ADDEND));
        if (addend_mode == (HAS_ADDEND | BY_ADDEND)) r_addend += rd_sleb(&s);
        else if (addend_mode != HAS_ADDEND) r_addend = 0;
        for (int64_t i = 0; i < group; i++) {
            r_offset += (flags & BY_DELTA) ? group_delta : (uint64_t)rd_sleb(&s);
            if (!(flags & BY_INFO)) r_info = (uint64_t)rd_sleb(&s);
            if (addend_mode == HAS_ADDEND) r_addend += rd_sleb(&s);
            if (s.bad) return false;
            if (!reloc_one(L, r_offset, (uint32_t)(r_info & 0xffffffffu), (uint32_t)(r_info >> 32), r_addend)) return false;
            (*count)++;
        }
        idx += group;
    }
    return true;
}

static bool do_relr(tl_lib *L, size_t *count)
{
    if (!L->relr || !L->relrsz) return true;
    const uint8_t *w = at(L, L->relr);
    uint64_t where = 0;
    for (size_t i = 0; i < L->relrsz / 8; i++) {
        uint64_t word = rd64(w + i * 8);
        if (!(word & 1)) {
            if (!reloc_one(L, word, R_RELATIVE, 0, (int64_t)rd64(L->rw + (word - L->base_vaddr)))) return false;
            (*count)++;
            where = word + 8;
        } else {
            for (unsigned b = 1; b < 64; b++) {
                if (word & (1ull << b)) {
                    uint64_t a = where + (uint64_t)(b - 1) * 8;
                    if (!reloc_one(L, a, R_RELATIVE, 0, (int64_t)rd64(L->rw + (a - L->base_vaddr)))) return false;
                    (*count)++;
                }
            }
            where += 63 * 8;
        }
    }
    return true;
}

/* ----------------------------------------------------------------- mapping */

static void parse_dynamic(tl_lib *L, uint64_t dyn_vaddr, uint64_t dyn_size)
{
    const elf_dyn *d = at(L, dyn_vaddr);
    for (size_t i = 0; i < dyn_size / sizeof(elf_dyn); i++) {
        switch (d[i].d_tag) {
        case DT_NULL_: return;
        case DT_NEEDED_: if (L->nneeded < MAX_DEPS) L->needed[L->nneeded++] = d[i].d_val; break;
        case DT_STRTAB_: L->strtab = d[i].d_val; break;
        case DT_STRSZ_: L->strsz = d[i].d_val; break;
        case DT_SYMTAB_: L->symtab = d[i].d_val; break;
        case DT_GNU_HASH_: L->gnu_hash = d[i].d_val; break;
        case DT_HASH_: L->sysv_hash = d[i].d_val; break;
        case DT_RELA_: L->rela = d[i].d_val; break;
        case DT_RELASZ_: L->relasz = d[i].d_val; break;
        case DT_JMPREL_: L->jmprel = d[i].d_val; break;
        case DT_PLTRELSZ_: L->pltrelsz = d[i].d_val; break;
        case DT_ANDROID_RELA_: L->arela = d[i].d_val; break;
        case DT_ANDROID_RELASZ_: L->arelasz = d[i].d_val; break;
        case DT_RELR_: case DT_ANDROID_RELR_: L->relr = d[i].d_val; break;
        case DT_RELRSZ_: case DT_ANDROID_RELRSZ_: L->relrsz = d[i].d_val; break;
        case DT_INIT_: L->init = d[i].d_val; break;
        case DT_INIT_ARRAY_: L->init_array = d[i].d_val; break;
        case DT_INIT_ARRAYSZ_: L->init_arraysz = d[i].d_val; break;
        case DT_SONAME_: break;     /* read once the string table is known */
        default: break;
        }
    }
}

static bool ensure_tcb(void)
{
    if (G.tcb_rw) return true;
    if (!tl_xmem_alloc(PAGE, &G.tcb_rx, &G.tcb_rw)) return false;
    uint64_t *t = (uint64_t *)G.tcb_rw;
    t[0] = (uint64_t)(uintptr_t)G.tcb_rw;      /* self */
    t[1] = 1000; t[2] = 1000;
    t[5] = 0xdeadbeefcafebabeull;             /* [tpidr_el0 + 0x28]: the stack cookie */
    return true;
}

static tl_lib *map_library(const char *name, uint8_t *file, size_t flen)
{
    if (G.nlibs >= MAX_LIBS) { tl_log_line("ld: too many libraries"); return NULL; }
    const elf_ehdr *eh = (const elf_ehdr *)file;
    if (flen < sizeof(*eh) || memcmp(file, "\x7f""ELF", 4) != 0 || eh->e_ident[4] != 2 || eh->e_ident[5] != 1
        || eh->e_machine != EM_AARCH64_) {
        tl_log_line("ld: %s is not a 64-bit little-endian arm64 ELF image", name);
        return NULL;
    }
    if (eh->e_phentsize < sizeof(elf_phdr) || eh->e_phoff + (uint64_t)eh->e_phnum * eh->e_phentsize > flen) {
        tl_log_line("ld: %s: program headers run off the file", name);
        return NULL;
    }
    tl_segment loads[16]; int nloads = 0; tl_segment relro = {0}; bool has_relro = false;
    uint64_t dyn_v = 0, dyn_n = 0;
    elf_phdr *phs = malloc((size_t)eh->e_phnum * sizeof(elf_phdr));
    if (!phs) return NULL;
    for (unsigned i = 0; i < eh->e_phnum; i++) {
        memcpy(&phs[i], file + eh->e_phoff + (size_t)i * eh->e_phentsize, sizeof(elf_phdr));
        const elf_phdr *p = &phs[i];
        if (p->p_type == PT_LOAD_ && nloads < 16) {
            loads[nloads].vaddr = p->p_vaddr; loads[nloads].memsz = p->p_memsz; loads[nloads].flags = p->p_flags;
            nloads++;
        } else if (p->p_type == PT_GNU_RELRO_) {
            relro.vaddr = p->p_vaddr; relro.memsz = p->p_memsz; has_relro = true;
        } else if (p->p_type == PT_DYNAMIC_) {
            dyn_v = p->p_vaddr; dyn_n = p->p_filesz;
        } else if (p->p_type == PT_TLS_) {
            tl_log_line("ld: %s has a PT_TLS segment -- thread-local storage is not implemented", name);
            free(phs);
            return NULL;
        }
    }
    if (!nloads || !dyn_n) { tl_log_line("ld: %s has no loadable or dynamic segments", name); free(phs); return NULL; }

    uint64_t base_vaddr = 0;
    size_t npages = tl_page_plan(loads, (size_t)nloads, has_relro ? &relro : NULL, PAGE, NULL, 0, &base_vaddr);
    if (!npages) { tl_log_line("ld: %s: unusable page layout", name); free(phs); return NULL; }
    uint8_t *flags = malloc(npages);
    tl_page_plan(loads, (size_t)nloads, has_relro ? &relro : NULL, PAGE, flags, npages, &base_vaddr);

    uint8_t *rx, *rw;
    if (!tl_xmem_alloc((npages + 1) * PAGE, &rx, &rw)) {
        tl_log_line("ld: %s needs %zu MiB of executable memory and the region has %zu MiB left", name,
                    npages * PAGE >> 20, (tl_xmem_size() - tl_xmem_used()) >> 20);
        free(flags); free(phs);
        return NULL;
    }
    /* The region's pages are not guaranteed zero (StikDebug writes a byte into each),
     * and .bss has to be. */
    memset(rw, 0, (npages + 1) * PAGE);
    for (unsigned i = 0; i < eh->e_phnum; i++) {
        const elf_phdr *p = &phs[i];
        if (p->p_type != PT_LOAD_ || !p->p_filesz) continue;
        if (p->p_offset + p->p_filesz > flen || p->p_vaddr < base_vaddr
            || p->p_vaddr - base_vaddr + p->p_filesz > npages * PAGE) {
            tl_log_line("ld: %s: segment %u is outside the file or the image", name, i);
            free(flags); free(phs);
            return NULL;
        }
        memcpy(rw + (p->p_vaddr - base_vaddr), file + p->p_offset, p->p_filesz);
    }

    tl_lib *L = calloc(1, sizeof(*L));
    snprintf(L->name, sizeof(L->name), "%s", name);
    L->rx = rx; L->rw = rw; L->base_vaddr = base_vaddr; L->npages = npages; L->pflags = flags;
    L->phdr = phs; L->phnum = eh->e_phnum;
    L->stub_rx = rx + npages * PAGE; L->stub_rw = rw + npages * PAGE; L->stub_used = 16;
    { uint64_t h = (uint64_t)(uintptr_t)tl_svc_common; memcpy(L->stub_rw, &h, 8); }
    parse_dynamic(L, dyn_v, dyn_n);
    if (L->strtab) {
        const elf_dyn *d = at(L, dyn_v);
        for (size_t i = 0; i < dyn_n / sizeof(elf_dyn) && d[i].d_tag != DT_NULL_; i++) {
            if (d[i].d_tag == DT_SONAME_) snprintf(L->soname, sizeof(L->soname), "%s", (const char *)at(L, L->strtab) + d[i].d_val);
        }
    }
    if (!L->soname[0]) snprintf(L->soname, sizeof(L->soname), "%s", name);
    L->nsyms = count_dynsyms(L);
    if (L->nsyms) L->symcache = calloc(L->nsyms, sizeof(uint64_t));
    G.libs[G.nlibs++] = L;
    return L;
}

/* ------------------------------------------------------------------ loading */

static bool relocate(tl_lib *L)
{
    size_t count = 0;
    L->state = 1;
    bool ok = do_relas(L, L->rela, L->relasz, &count)
           && do_relas(L, L->jmprel, L->pltrelsz, &count)
           && do_packed(L, &count)
           && do_relr(L, &count);
    free(L->symcache);
    L->symcache = NULL;
    if (!ok) { tl_log_line("ld: %s: relocation failed", L->name); return false; }
    size_t t = 0, a = 0, ad = 0, sv = 0;
    patch_image(L, &t, &a, &ad, &sv);
    tl_xmem_flush(L->rx, (L->npages + 1) * PAGE);
    L->state = 2;
    if (G.verbosity >= 1) {
        tl_log_line("ld: %-36s %5.1f MiB  %7zu relocs, %4zu tpidr + %5zu adrp patched%s%s", L->name,
                    (double)(L->npages * PAGE) / 1048576.0, count, t, a,
                    L->n_unresolved ? ", unresolved imports: " : "", "");
        if (L->n_unresolved) tl_log_line("ld:   %s: %u imports bound to logging stubs", L->name, L->n_unresolved);
        if (ad) tl_log_line("ld:   %s: %zu 'adr' instructions reach writable data through the read-only view", L->name, ad);
        if (sv) tl_log_line("ld:   %s: %zu raw system-call sites rewritten", L->name, sv);
    }
    return true;
}

static tl_lib *load_locked(const char *name, int depth)
{
    tl_lib *L = find_loaded(name);
    if (L) return L;
    if (tl_bionic_is_system_lib(name)) return NULL;
    if (depth > 32) { tl_log_line("ld: dependency chain too deep at %s", name); return NULL; }

    uint8_t *file; size_t flen;
    if (!fetch_from_apks(name, &file, &flen)) {
        tl_log_line("ld: %s is not in the APK and is not a system library", name);
        return NULL;
    }
    { char e[160] = ""; if (!tl_xmem_open(768u << 20, e, sizeof(e))) { tl_log_line("ld: %s", e); free(file); return NULL; } }
    {   /* adrp, which the loader uses to retarget code at the writable view, reaches +-4 GiB. */
        ptrdiff_t d = tl_xmem_delta();
        if (d > ((ptrdiff_t)3 << 30) || d < -((ptrdiff_t)3 << 30)) { tl_log_line("ld: the writable and executable views are %td MiB apart: too far for adrp", d >> 20); free(file); return NULL; }
    }
    if (!ensure_tcb()) { free(file); return NULL; }
    L = map_library(name, file, flen);
    free(file);
    if (!L) return NULL;

    /* Dependencies first, so everything this library binds against exists. */
    for (int i = 0; i < L->nneeded; i++) {
        const char *dn = (const char *)at(L, L->strtab) + L->needed[i];
        if (find_loaded(dn) || tl_bionic_is_system_lib(dn)) continue;
        if (!load_locked(dn, depth + 1)) tl_log_line("ld: %s: needed library %s could not be loaded", name, dn);
    }
    build_scope(L);
    if (!relocate(L)) return NULL;
    return L;
}

tl_lib *tl_ld_load(const char *name)
{
    pthread_mutex_lock(&g_big);
    tl_lib *L = load_locked(name, 0);
    pthread_mutex_unlock(&g_big);
    return L;
}

static bool init_locked(tl_lib *L)
{
    if (!L || L->state >= 3) return true;
    if (L->state < 2) return false;
    L->state = 3;
    build_scope(L);
    /* Needed libraries initialise first, in the order the linker would run them: the
     * deepest dependency first. The BFS list is shallowest-first, so walk it backwards. */
    for (int i = L->ndeps - 1; i >= 0; i--) init_locked(L->deps[i]);

    char *fallback_argv[] = { (char *)"app_process64", NULL };
    char *fallback_envp[] = { NULL };
    char **argv = G.argv ? G.argv : fallback_argv, **envp = G.envp ? G.envp : fallback_envp;
    int argc = 1;
    typedef void (*init_fn)(int, char **, char **);
    if (L->init) ((init_fn)(L->rx + (L->init - L->base_vaddr)))(argc, argv, envp);
    if (L->init_array) {
        const uint64_t *fns = at(L, L->init_array);
        size_t n = L->init_arraysz / 8;
        for (size_t i = 0; i < n; i++) {
            uint64_t f = fns[i];
            if (f && f != ~0ull) ((init_fn)(uintptr_t)f)(argc, argv, envp);
        }
    }
    L->state = 4;
    if (G.verbosity >= 1) tl_log_line("ld: %-36s initialised (%s%zu constructors)", L->name, L->init ? "DT_INIT + " : "", (size_t)(L->init_arraysz / 8));
    return true;
}

bool tl_ld_init(tl_lib *lib)
{
    pthread_mutex_lock(&g_big);
    bool ok = init_locked(lib);
    pthread_mutex_unlock(&g_big);
    return ok;
}
