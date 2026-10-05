/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GLES 3.1 on a 3.0 driver (included by husk-tl-egl.c, which supplies a_glGetIntegerv / a_eglGetProcAddress).
 *
 * ANGLE over Metal stops at OpenGL ES 3.0, and Minecraft's Android materials hold only "#version 310 es" shaders, chosen by
 * reporting 3.1. For a game that asks for it (tl_egl_es31_shim) the driver is described as 3.1 and each such shader is written
 * down to 300 es: the version line changes, and `layout(binding = N)` -- which 3.0 does not have -- is dropped and applied after
 * the link as glUniform1i / glUniformBlockBinding. What 3.1 really adds (compute, images, buffers) is not there.
 */
static bool g_es31_shim;
void tl_egl_es31_shim(bool on) { g_es31_shim = on; }

typedef struct { char name[72]; int binding; bool block; } es31_bind;
#define ES31_SHADERS 1024
#define ES31_BINDS 12
#define ES31_ATTACHED 2048
static struct { unsigned shader; int n; es31_bind b[ES31_BINDS]; } g_es31_sh[ES31_SHADERS];
static struct { unsigned prog, shader; } g_es31_att[ES31_ATTACHED];
static int g_es31_natt;
static pthread_mutex_t g_es31_mu = PTHREAD_MUTEX_INITIALIZER;

/* "OpenGL ES 3.0 ..." -> "OpenGL ES 3.1 ...", "OpenGL ES GLSL ES 3.00 ..." -> "... 3.10 ..." (a copy: the driver's string is not ours to edit) */
static const char *es31_fix(const char *s, bool glsl)
{
    static char ring[8][300]; static atomic_uint k;
    char *out = ring[atomic_fetch_add(&k, 1) % 8];
    snprintf(out, 300, "%s", s);
    if (glsl) { char *p = strstr(out, "GLSL ES 3.00"); if (p) { p[10] = '1'; p[11] = '0'; } }
    else { char *p = strstr(out, "OpenGL ES 3.0"); if (p) p[12] = '1'; }
    return out;
}

static bool es31_ident(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

/* The 300 es text of a 310 es shader (malloc'd), with the bindings it dropped in rec[0..*nrec). Anything else is returned as it came. */
static char *es31_downgrade(const char *src, size_t n, es31_bind *rec, int *nrec)
{
    char *out = malloc(n + 8); size_t o = 0;
    *nrec = 0;
    const char *ver = strstr(src, "#version 310 es");
    if (!ver) { memcpy(out, src, n); out[n] = 0; return out; }
    for (size_t i = 0; i < n;) {
        if (src + i == ver) { memcpy(out + o, "#version 300 es", 15); o += 15; i += 15; continue; }
        if (i + 7 <= n && !strncmp(src + i, "layout(", 7) && (i == 0 || !es31_ident(src[i - 1]))) {
            size_t j = i + 7; int depth = 1;
            while (j < n && depth) { if (src[j] == '(') depth++; else if (src[j] == ')') depth--; j++; }
            char q[160]; size_t ql = j - 1 - (i + 7); if (ql >= sizeof(q)) ql = sizeof(q) - 1;
            memcpy(q, src + i + 7, ql); q[ql] = 0;
            char *b = strstr(q, "binding");
            size_t d = j; while (d < n && src[d] != ';' && src[d] != '{') d++;
            char decl[200]; size_t dl = d - j; if (dl >= sizeof(decl)) dl = sizeof(decl) - 1;
            memcpy(decl, src + j, dl); decl[dl] = 0;
            bool opaque = strstr(decl, "image") || strstr(decl, "buffer") || strstr(decl, "atomic_uint");
            if (b && !opaque) {
                char *eq = strchr(b, '='); int val = eq ? atoi(eq + 1) : -1;
                bool block = d < n && src[d] == '{';
                if (*nrec < ES31_BINDS && val >= 0) {
                    size_t e = dl;
                    while (e && (decl[e - 1] == ' ' || decl[e - 1] == '\n' || decl[e - 1] == '\t' || decl[e - 1] == '\r')) e--;
                    while (e && decl[e - 1] == ']') { while (e && decl[e - 1] != '[') e--; if (e) e--; }
                    size_t st = e; while (st && es31_ident(decl[st - 1])) st--;
                    if (e > st && e - st < sizeof(rec[0].name)) {
                        memcpy(rec[*nrec].name, decl + st, e - st); rec[*nrec].name[e - st] = 0;
                        rec[*nrec].binding = val; rec[*nrec].block = block; (*nrec)++;
                    }
                }
                /* the layout again, without the binding qualifier */
                char keep[160]; size_t kl = 0; const char *tok = q;
                while (tok) {
                    const char *comma = strchr(tok, ',');
                    size_t tl = comma ? (size_t)(comma - tok) : strlen(tok);
                    const char *t = tok; size_t tn = tl; while (tn && *t == ' ') { t++; tn--; }
                    if (!(tn >= 7 && !strncmp(t, "binding", 7)) && tn) { if (kl) keep[kl++] = ','; memcpy(keep + kl, t, tn); kl += tn; }
                    tok = comma ? comma + 1 : NULL;
                }
                if (kl) { memcpy(out + o, "layout(", 7); o += 7; memcpy(out + o, keep, kl); o += kl; out[o++] = ')'; }
                i = j; continue;
            }
            memcpy(out + o, src + i, j - i); o += j - i; i = j; continue;
        }
        out[o++] = src[i++];
    }
    out[o] = 0;
    return out;
}

static void (*r_glShaderSource)(unsigned, int, const char *const *, const int *);
static void w_glShaderSource(unsigned sh, int count, const char *const *str, const int *len)
{
    if (!g_es31_shim || count <= 0 || !str) { r_glShaderSource(sh, count, str, len); return; }
    size_t total = 0;
    for (int i = 0; i < count; i++) total += str[i] ? (len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i])) : 0;
    char *cat = malloc(total + 1); size_t k = 0;
    for (int i = 0; i < count; i++) if (str[i]) { size_t l = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]); memcpy(cat + k, str[i], l); k += l; }
    cat[k] = 0;
    es31_bind rec[ES31_BINDS]; int nrec = 0;
    char *down = es31_downgrade(cat, k, rec, &nrec);
    pthread_mutex_lock(&g_es31_mu);
    int slot = (int)(sh % ES31_SHADERS);
    g_es31_sh[slot].shader = sh; g_es31_sh[slot].n = nrec; memcpy(g_es31_sh[slot].b, rec, sizeof(es31_bind) * (size_t)nrec);
    pthread_mutex_unlock(&g_es31_mu);
    const char *one = down; int one_len = (int)strlen(down);
    r_glShaderSource(sh, 1, &one, &one_len);
    free(cat); free(down);
}

static void (*r_glAttachShader)(unsigned, unsigned);
static void w_glAttachShader(unsigned prog, unsigned sh)
{
    r_glAttachShader(prog, sh);
    if (!g_es31_shim) return;
    pthread_mutex_lock(&g_es31_mu);
    g_es31_att[g_es31_natt % ES31_ATTACHED].prog = prog; g_es31_att[g_es31_natt % ES31_ATTACHED].shader = sh; g_es31_natt++;
    pthread_mutex_unlock(&g_es31_mu);
}

static void (*r_glLinkProgram)(unsigned);
static void w_glLinkProgram(unsigned prog)
{
    r_glLinkProgram(prog);
    if (!g_es31_shim || !a_eglGetProcAddress) return;
    static int (*getloc)(unsigned, const char *); static unsigned (*getblk)(unsigned, const char *);
    static void (*blkbind)(unsigned, unsigned, unsigned), (*uni1i)(int, int), (*useprog)(unsigned), (*getprogiv)(unsigned, unsigned, int *);
    if (!getloc) {
        getloc = a_eglGetProcAddress("glGetUniformLocation"); getblk = a_eglGetProcAddress("glGetUniformBlockIndex");
        blkbind = a_eglGetProcAddress("glUniformBlockBinding"); uni1i = a_eglGetProcAddress("glUniform1i");
        useprog = a_eglGetProcAddress("glUseProgram"); getprogiv = a_eglGetProcAddress("glGetProgramiv");
    }
    if (!getloc || !getblk || !blkbind || !uni1i || !useprog || !getprogiv) return;
    int ok = 0; getprogiv(prog, 0x8B82 /* GL_LINK_STATUS */, &ok);
    if (!ok) return;
    int cur = 0; a_glGetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &cur);
    bool used = false;
    pthread_mutex_lock(&g_es31_mu);
    int total = g_es31_natt < ES31_ATTACHED ? g_es31_natt : ES31_ATTACHED;
    for (int a = 0; a < total; a++) {
        if (g_es31_att[a].prog != prog) continue;
        unsigned sh = g_es31_att[a].shader;
        int slot = (int)(sh % ES31_SHADERS);
        if (g_es31_sh[slot].shader != sh) continue;
        for (int b = 0; b < g_es31_sh[slot].n; b++) {
            const es31_bind *e = &g_es31_sh[slot].b[b];
            if (e->block) { unsigned idx = getblk(prog, e->name); if (idx != 0xFFFFFFFFu) blkbind(prog, idx, (unsigned)e->binding); }
            else { int loc = getloc(prog, e->name); if (loc >= 0) { if (!used) { useprog(prog); used = true; } uni1i(loc, e->binding); } }
        }
    }
    pthread_mutex_unlock(&g_es31_mu);
    if (used) useprog((unsigned)cur);
}

static void (*r_glGetIntegerv)(unsigned, int *);
static void w_glGetIntegerv(unsigned pname, int *data)
{
    r_glGetIntegerv(pname, data);
    if (g_es31_shim && pname == 0x821C /* GL_MINOR_VERSION */ && data && *data == 0) *data = 1;
}
