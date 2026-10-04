/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-tl-dex.h"
#include "husk-tl-framework.h"
#include "husk-tl-internal.h"
#include "husk-tl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <dlfcn.h>

/* DEX file header format */
typedef struct {
    uint8_t  magic[8];
    uint32_t checksum;
    uint8_t  signature[20];
    uint32_t file_size;
    uint32_t header_size;
    uint32_t endian_tag;
    uint32_t link_size;
    uint32_t link_off;
    uint32_t map_off;
    uint32_t string_ids_size;
    uint32_t string_ids_off;
    uint32_t type_ids_size;
    uint32_t type_ids_off;
    uint32_t proto_ids_size;
    uint32_t proto_ids_off;
    uint32_t field_ids_size;
    uint32_t field_ids_off;
    uint32_t method_ids_size;
    uint32_t method_ids_off;
    uint32_t class_defs_size;
    uint32_t class_defs_off;
    uint32_t data_size;
    uint32_t data_off;
} dex_header;

struct tl_dex_file {
    uint8_t *data;
    size_t size;
    const dex_header *hdr;
    const uint32_t *string_ids;
    const uint32_t *type_ids;
    const uint8_t  *proto_ids;
    const uint8_t  *field_ids;
    const uint8_t  *method_ids;
    const uint8_t  *class_defs;

    /* Resolution caches */
    tl_dex_field  **resolved_fields;
    tl_dex_method **resolved_methods;
    tl_dex_class  **resolved_types;
};

/* LEB128 decoders */
static uint32_t read_uleb128(const uint8_t **p)
{
    uint32_t val = 0;
    int shift = 0;
    while (1) {
        uint8_t b = *(*p)++;
        val |= (uint32_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) break;
    }
    return val;
}

__attribute__((unused)) static int32_t read_sleb128(const uint8_t **p)
{
    int32_t val = 0;
    int shift = 0;
    uint8_t b;
    do {
        b = *(*p)++;
        val |= (int32_t)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if (shift < 32 && (b & 0x40)) {
        val |= ~0 << shift;
    }
    return val;
}

static const char *dex_get_string(const tl_dex_file *dex, uint32_t idx)
{
    if (!dex || idx >= dex->hdr->string_ids_size) return "";
    uint32_t off = dex->string_ids[idx];
    if (off >= dex->size) return "";
    const uint8_t *p = dex->data + off;
    (void)read_uleb128(&p); /* skip utf16_size */
    return (const char *)p;
}

static const char *dex_get_type(const tl_dex_file *dex, uint32_t idx)
{
    if (!dex || idx >= dex->hdr->type_ids_size) return "";
    uint32_t str_idx = dex->type_ids[idx];
    return dex_get_string(dex, str_idx);
}

static void dex_get_field_info(const tl_dex_file *dex, uint32_t idx,
                               const char **class_desc, const char **name, const char **type_desc)
{
    if (!dex || idx >= dex->hdr->field_ids_size) return;
    const uint8_t *f = dex->field_ids + idx * 8;
    uint16_t c_idx = (uint16_t)f[0] | ((uint16_t)f[1] << 8);
    uint16_t t_idx = (uint16_t)f[2] | ((uint16_t)f[3] << 8);
    uint32_t n_idx = (uint32_t)f[4] | ((uint32_t)f[5] << 8) | ((uint32_t)f[6] << 16) | ((uint32_t)f[7] << 24);
    if (class_desc) *class_desc = dex_get_type(dex, c_idx);
    if (type_desc)  *type_desc  = dex_get_type(dex, t_idx);
    if (name)       *name       = dex_get_string(dex, n_idx);
}

static void dex_get_method_info(const tl_dex_file *dex, uint32_t idx,
                                const char **class_desc, const char **name, const char **shorty)
{
    if (!dex || idx >= dex->hdr->method_ids_size) return;
    const uint8_t *m = dex->method_ids + idx * 8;
    uint16_t c_idx = (uint16_t)m[0] | ((uint16_t)m[1] << 8);
    uint16_t p_idx = (uint16_t)m[2] | ((uint16_t)m[3] << 8);
    uint32_t n_idx = (uint32_t)m[4] | ((uint32_t)m[5] << 8) | ((uint32_t)m[6] << 16) | ((uint32_t)m[7] << 24);
    if (class_desc) *class_desc = dex_get_type(dex, c_idx);
    if (name)       *name       = dex_get_string(dex, n_idx);
    if (shorty) {
        const uint8_t *pr = dex->proto_ids + p_idx * 12;
        uint32_t s_idx = (uint32_t)pr[0] | ((uint32_t)pr[1] << 8) | ((uint32_t)pr[2] << 16) | ((uint32_t)pr[3] << 24);
        *shorty = dex_get_string(dex, s_idx);
    }
}

/* ------------------------------------------------------------- Context */

tl_dex_context *tl_dex_context_create(const char *apk_path, uint32_t *fb, int width, int height)
{
    tl_dex_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->apk_path = apk_path ? strdup(apk_path) : NULL;
    ctx->framebuffer = fb;
    ctx->fb_width = width;
    ctx->fb_height = height;
    ctx->class_capacity = 512;
    ctx->classes = calloc(ctx->class_capacity, sizeof(tl_dex_class *));
    tl_framework_init(ctx);
    return ctx;
}

void tl_dex_context_destroy(tl_dex_context *ctx)
{
    if (!ctx) return;
    tl_framework_cleanup(ctx);
    for (int i = 0; i < ctx->num_dex_files; i++) {
        if (ctx->dex_files[i]) {
            free(ctx->dex_files[i]->resolved_fields);
            free(ctx->dex_files[i]->resolved_methods);
            free(ctx->dex_files[i]->resolved_types);
            free(ctx->dex_files[i]->data);
            free(ctx->dex_files[i]);
        }
    }
    for (int i = 0; i < ctx->num_classes; i++) {
        tl_dex_class *c = ctx->classes[i];
        if (c) {
            free(c->fields);
            free(c->methods);
            free(c->static_values);
            free(c);
        }
    }
    free(ctx->classes);
    free((void *)ctx->apk_path);
    free(ctx);
}

static inline uint32_t class_hash_str(const char *s)
{
    uint32_t h = 5381;
    while (*s) h = ((h << 5) + h) + (uint8_t)(*s++);
    return h;
}

static void context_add_class(tl_dex_context *ctx, tl_dex_class *clazz)
{
    if (ctx->num_classes >= ctx->class_capacity) {
        ctx->class_capacity *= 2;
        ctx->classes = realloc(ctx->classes, ctx->class_capacity * sizeof(tl_dex_class *));
    }
    ctx->classes[ctx->num_classes++] = clazz;

    if (clazz->descriptor) {
        uint32_t h = class_hash_str(clazz->descriptor) % TL_DEX_CLASS_HASH_SIZE;
        clazz->next_hash = ctx->class_hash[h];
        ctx->class_hash[h] = clazz;
    }
}

tl_dex_class *tl_dex_find_class(tl_dex_context *ctx, const char *descriptor)
{
    if (!ctx || !descriptor) return NULL;
    uint32_t h = class_hash_str(descriptor) % TL_DEX_CLASS_HASH_SIZE;
    for (tl_dex_class *c = ctx->class_hash[h]; c; c = c->next_hash) {
        if (!strcmp(c->descriptor, descriptor)) {
            return c;
        }
    }
    return NULL;
}

tl_dex_method *tl_dex_find_method(tl_dex_class *clazz, const char *name, const char *shorty)
{
    if (!clazz || !name) return NULL;
    for (int i = 0; i < clazz->num_methods; i++) {
        if (!strcmp(clazz->methods[i].name, name)) {
            if (!shorty || !strcmp(clazz->methods[i].shorty, shorty)) {
                return &clazz->methods[i];
            }
        }
    }
    if (clazz->super_class) {
        return tl_dex_find_method(clazz->super_class, name, shorty);
    }
    return NULL;
}

tl_dex_field *tl_dex_find_field(tl_dex_class *clazz, const char *name, const char *type)
{
    if (!clazz || !name) return NULL;
    for (int i = 0; i < clazz->num_fields; i++) {
        if (!strcmp(clazz->fields[i].name, name)) {
            if (!type || !strcmp(clazz->fields[i].type, type)) {
                return &clazz->fields[i];
            }
        }
    }
    if (clazz->super_class) {
        return tl_dex_find_field(clazz->super_class, name, type);
    }
    return NULL;
}

/* ------------------------------------------------------------- Resolvers */

static tl_dex_field *dex_resolve_field(tl_dex_context *ctx, tl_dex_file *dex, uint32_t idx)
{
    if (!dex || idx >= dex->hdr->field_ids_size) return NULL;
    if (dex->resolved_fields[idx]) return dex->resolved_fields[idx];

    const char *class_desc = NULL, *fname = NULL, *ftype = NULL;
    dex_get_field_info(dex, idx, &class_desc, &fname, &ftype);
    tl_dex_class *c = tl_dex_find_class(ctx, class_desc);
    tl_dex_field *f = c ? tl_dex_find_field(c, fname, ftype) : NULL;
    if (!f) {
        f = calloc(1, sizeof(*f));
        f->clazz = c;
        f->name = strdup(fname ? fname : "");
        f->type = strdup(ftype ? ftype : "");
        f->slot = c ? (uint32_t)c->num_fields++ : 0;
    }
    dex->resolved_fields[idx] = f;
    return f;
}

static tl_dex_method *dex_resolve_method(tl_dex_context *ctx, tl_dex_file *dex, uint32_t idx)
{
    if (!dex || idx >= dex->hdr->method_ids_size) return NULL;
    if (dex->resolved_methods[idx]) return dex->resolved_methods[idx];

    const char *class_desc = NULL, *mname = NULL, *shorty = NULL;
    dex_get_method_info(dex, idx, &class_desc, &mname, &shorty);

    tl_dex_class *c = tl_dex_find_class(ctx, class_desc);
    tl_dex_method *m = c ? tl_dex_find_method(c, mname, shorty) : NULL;

    if (!m) {
        tl_dex_native_func nfunc = tl_framework_lookup(class_desc, mname, shorty);
        m = calloc(1, sizeof(*m));
        m->clazz = c;
        m->name = strdup(mname ? mname : "");
        m->shorty = strdup(shorty ? shorty : "V");
        m->native_func = (void *)nfunc;
    }
    dex->resolved_methods[idx] = m;
    return m;
}

/* ----------------------------------------------------------- DEX Loading */

static tl_dex_file *parse_dex_buffer(tl_dex_context *ctx, uint8_t *data, size_t size)
{
    if (size < sizeof(dex_header)) return NULL;
    if (memcmp(data, "dex\n", 4) != 0) return NULL;

    const dex_header *hdr = (const dex_header *)data;
    tl_dex_file *dex = calloc(1, sizeof(*dex));
    if (!dex) return NULL;
    dex->data = data;
    dex->size = size;
    dex->hdr = hdr;
    dex->string_ids = (const uint32_t *)(data + hdr->string_ids_off);
    dex->type_ids   = (const uint32_t *)(data + hdr->type_ids_off);
    dex->proto_ids  = data + hdr->proto_ids_off;
    dex->field_ids  = data + hdr->field_ids_off;
    dex->method_ids = data + hdr->method_ids_off;
    dex->class_defs = data + hdr->class_defs_off;

    dex->resolved_fields  = calloc(hdr->field_ids_size, sizeof(void *));
    dex->resolved_methods = calloc(hdr->method_ids_size, sizeof(void *));
    dex->resolved_types   = calloc(hdr->type_ids_size, sizeof(void *));

    for (uint32_t i = 0; i < hdr->class_defs_size; i++) {
        const uint8_t *cd = dex->class_defs + i * 32;
        uint32_t class_idx = (uint32_t)cd[0] | ((uint32_t)cd[1] << 8) | ((uint32_t)cd[2] << 16) | ((uint32_t)cd[3] << 24);
        uint32_t access_flags = (uint32_t)cd[4] | ((uint32_t)cd[5] << 8) | ((uint32_t)cd[6] << 16) | ((uint32_t)cd[7] << 24);
        uint32_t superclass_idx = (uint32_t)cd[8] | ((uint32_t)cd[9] << 8) | ((uint32_t)cd[10] << 16) | ((uint32_t)cd[11] << 24);
        uint32_t class_data_off = (uint32_t)cd[24] | ((uint32_t)cd[25] << 8) | ((uint32_t)cd[26] << 16) | ((uint32_t)cd[27] << 24);

        tl_dex_class *c = calloc(1, sizeof(*c));
        c->ctx = ctx;
        c->dex = dex;
        c->descriptor = dex_get_type(dex, class_idx);
        c->super_descriptor = superclass_idx != 0xFFFFFFFFu ? dex_get_type(dex, superclass_idx) : NULL;
        c->access_flags = access_flags;

        if (class_data_off != 0 && class_data_off < size) {
            const uint8_t *p = dex->data + class_data_off;
            uint32_t static_fields_count   = read_uleb128(&p);
            uint32_t instance_fields_count = read_uleb128(&p);
            uint32_t direct_methods_count   = read_uleb128(&p);
            uint32_t virtual_methods_count  = read_uleb128(&p);

            c->num_fields = static_fields_count + instance_fields_count;
            c->num_static_fields = static_fields_count;
            c->num_instance_fields = instance_fields_count;
            if (c->num_fields > 0) {
                c->fields = calloc(c->num_fields, sizeof(tl_dex_field));
            }
            if (static_fields_count > 0) {
                c->static_values = calloc(static_fields_count, sizeof(tl_dex_val));
            }

            uint32_t fid = 0;
            for (uint32_t f = 0; f < static_fields_count; f++) {
                fid += read_uleb128(&p);
                uint32_t flags = read_uleb128(&p);
                const char *fname = NULL, *ftype = NULL;
                dex_get_field_info(dex, fid, NULL, &fname, &ftype);
                c->fields[f] = (tl_dex_field){
                    .clazz = c, .name = fname, .type = ftype, .access_flags = flags, .is_static = true, .slot = f
                };
                if (fid < hdr->field_ids_size) dex->resolved_fields[fid] = &c->fields[f];
            }
            fid = 0;
            for (uint32_t f = 0; f < instance_fields_count; f++) {
                fid += read_uleb128(&p);
                uint32_t flags = read_uleb128(&p);
                const char *fname = NULL, *ftype = NULL;
                dex_get_field_info(dex, fid, NULL, &fname, &ftype);
                c->fields[static_fields_count + f] = (tl_dex_field){
                    .clazz = c, .name = fname, .type = ftype, .access_flags = flags, .is_static = false, .slot = f
                };
                if (fid < hdr->field_ids_size) dex->resolved_fields[fid] = &c->fields[static_fields_count + f];
            }

            c->num_methods = direct_methods_count + virtual_methods_count;
            if (c->num_methods > 0) {
                c->methods = calloc(c->num_methods, sizeof(tl_dex_method));
            }
            uint32_t mid = 0;
            for (uint32_t m = 0; m < direct_methods_count; m++) {
                mid += read_uleb128(&p);
                uint32_t flags = read_uleb128(&p);
                uint32_t code_off = read_uleb128(&p);
                const char *mname = NULL, *shorty = NULL;
                dex_get_method_info(dex, mid, NULL, &mname, &shorty);

                tl_dex_method *meth = &c->methods[m];
                meth->clazz = c;
                meth->name = mname;
                meth->shorty = shorty;
                meth->access_flags = flags;
                if (code_off != 0 && code_off + 16 <= size) {
                    const uint8_t *cp = dex->data + code_off;
                    meth->registers_size = (uint16_t)cp[0] | ((uint16_t)cp[1] << 8);
                    meth->ins_size       = (uint16_t)cp[2] | ((uint16_t)cp[3] << 8);
                    meth->outs_size      = (uint16_t)cp[4] | ((uint16_t)cp[5] << 8);
                    meth->insns_size     = (uint32_t)cp[12] | ((uint32_t)cp[13] << 8) | ((uint32_t)cp[14] << 16) | ((uint32_t)cp[15] << 24);
                    meth->insns          = (const uint16_t *)(cp + 16);
                }
                if (mid < hdr->method_ids_size) dex->resolved_methods[mid] = meth;
            }
            mid = 0;
            for (uint32_t m = 0; m < virtual_methods_count; m++) {
                mid += read_uleb128(&p);
                uint32_t flags = read_uleb128(&p);
                uint32_t code_off = read_uleb128(&p);
                const char *mname = NULL, *shorty = NULL;
                dex_get_method_info(dex, mid, NULL, &mname, &shorty);

                tl_dex_method *meth = &c->methods[direct_methods_count + m];
                meth->clazz = c;
                meth->name = mname;
                meth->shorty = shorty;
                meth->access_flags = flags;
                if (code_off != 0 && code_off + 16 <= size) {
                    const uint8_t *cp = dex->data + code_off;
                    meth->registers_size = (uint16_t)cp[0] | ((uint16_t)cp[1] << 8);
                    meth->ins_size       = (uint16_t)cp[2] | ((uint16_t)cp[3] << 8);
                    meth->outs_size      = (uint16_t)cp[4] | ((uint16_t)cp[5] << 8);
                    meth->insns_size     = (uint32_t)cp[12] | ((uint32_t)cp[13] << 8) | ((uint32_t)cp[14] << 16) | ((uint32_t)cp[15] << 24);
                    meth->insns          = (const uint16_t *)(cp + 16);
                }
                if (mid < hdr->method_ids_size) dex->resolved_methods[mid] = meth;
            }
        }
        context_add_class(ctx, c);
    }
    return dex;
}

static void dex_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    void (*log_fn)(const char *, ...) = (void (*)(const char *, ...))dlsym(RTLD_DEFAULT, "tl_log_line");
    if (log_fn) {
        log_fn("%s", buf);
    } else {
        printf("%s\n", buf);
        fflush(stdout);
    }
}

bool tl_dex_load_apk(tl_dex_context *ctx, const char *apk_path)
{
    tl_zip z;
    char zerr[128] = {0};
    if (!tl_zip_open(&z, apk_path, zerr, sizeof(zerr))) {
        dex_log("dex: could not open %s: %s", apk_path, zerr);
        return false;
    }

    int loaded = 0;
    for (size_t i = 0; i < z.count && ctx->num_dex_files < 16; i++) {
        const char *name = z.entries[i].name;
        if (!strcmp(name, "classes.dex") ||
            (strncmp(name, "classes", 7) == 0 && strstr(name, ".dex"))) {
            const uint8_t *data_ptr = NULL;
            size_t len = 0;
            bool owned = false;
            char err[128] = {0};
            if (tl_zip_data(&z, &z.entries[i], 64 * 1024 * 1024, &data_ptr, &len, &owned, err, sizeof(err))) {
                uint8_t *buf = malloc(len);
                if (buf) {
                    memcpy(buf, data_ptr, len);
                    tl_dex_file *df = parse_dex_buffer(ctx, buf, len);
                    if (df) {
                        ctx->dex_files[ctx->num_dex_files++] = df;
                        loaded++;
                    } else {
                        free(buf);
                    }
                }
                if (owned) free((void *)data_ptr);
            }
        }
    }
    tl_zip_close(&z);

    /* Link superclasses */
    for (int i = 0; i < ctx->num_classes; i++) {
        tl_dex_class *c = ctx->classes[i];
        if (c->super_descriptor) {
            c->super_class = tl_dex_find_class(ctx, c->super_descriptor);
        }
    }

    dex_log("dex: loaded %d DEX file(s), %d class definitions from %s",
            loaded, ctx->num_classes, apk_path);
    return loaded > 0;
}

/* ----------------------------------------------------- Object Allocation */

tl_dex_object *tl_dex_alloc_object(tl_dex_class *clazz)
{
    tl_dex_object *obj = calloc(1, sizeof(*obj));
    obj->clazz = clazz;
    int nfields = clazz ? clazz->num_instance_fields : 16;
    if (nfields < 16) nfields = 16;
    obj->fields = calloc(nfields, sizeof(tl_dex_val));
    return obj;
}

tl_dex_object *tl_dex_alloc_array(tl_dex_class *elem_class, uint32_t length, uint32_t elem_size)
{
    tl_dex_object *obj = calloc(1, sizeof(*obj));
    obj->clazz = elem_class;
    obj->array.length = length;
    obj->array.elem_size = elem_size ? elem_size : 4;
    obj->array.elements = calloc(length ? length : 1, obj->array.elem_size);
    return obj;
}

tl_dex_object *tl_dex_alloc_string(tl_dex_context *ctx, const char *utf8)
{
    tl_dex_class *s_class = tl_dex_find_class(ctx, "Ljava/lang/String;");
    tl_dex_object *obj = calloc(1, sizeof(*obj));
    obj->clazz = s_class;
    obj->str_utf8 = utf8 ? strdup(utf8) : strdup("");
    return obj;
}

/* ----------------------------------------------------- Dalvik Interpreter */

bool tl_dex_invoke(tl_dex_context *ctx, tl_dex_method *method, tl_dex_val *args, int nargs, tl_dex_val *ret)
{
    if (!method) return false;

    /* Native / Framework dispatch */
    if (method->native_func) {
        tl_dex_native_func fn = (tl_dex_native_func)method->native_func;
        tl_dex_object *this_obj = (nargs > 0) ? args[0].l : NULL;
        return fn(ctx, this_obj, args, nargs, ret);
    }

    if (!method->insns || method->insns_size == 0) return true;

    dex_log("invoke: %s->%s (pc_max=%d)", method->clazz ? method->clazz->descriptor : "unknown", method->name, method->insns_size);

    tl_dex_file *dex = method->clazz->dex;
    uint16_t reg_count = method->registers_size;
    if (reg_count < method->ins_size) reg_count = method->ins_size;
    tl_dex_val *v = calloc(reg_count + 8, sizeof(tl_dex_val));

    /* Map incoming parameters to the upper registers [reg_count - ins_size .. reg_count - 1] */
    uint16_t in_start = reg_count - method->ins_size;
    for (int i = 0; i < nargs && (in_start + i) < reg_count; i++) {
        v[in_start + i] = args[i];
    }

    tl_dex_val last_result = {0};
    const uint16_t *insns = method->insns;
    uint32_t pc = 0;
    bool success = true;

    while (pc < method->insns_size) {
        uint16_t inst = insns[pc];
        uint8_t opcode = inst & 0xff;
        uint8_t op_b = (inst >> 8) & 0xff;
        if (pc < 30 || pc % 50 == 0) {
            dex_log("  pc=%d op=0x%02x", pc, opcode);
        }

        switch (opcode) {
            case 0x00: /* nop */
                pc += 1;
                break;

            case 0x01: /* move vA, vB */
            case 0x07: /* move-object vA, vB */
                v[op_b & 0x0f] = v[(op_b >> 4) & 0x0f];
                pc += 1;
                break;

            case 0x02: /* move/from16 vAA, vBBBB */
            case 0x08: /* move-object/from16 vAA, vBBBB */
                v[op_b] = v[insns[pc + 1]];
                pc += 2;
                break;

            case 0x03: /* move/16 vAAAA, vBBBB */
            case 0x09: /* move-object/16 vAAAA, vBBBB */
                v[insns[pc + 1]] = v[insns[pc + 2]];
                pc += 3;
                break;

            case 0x04: /* move-wide vA, vB */
                v[op_b & 0x0f] = v[(op_b >> 4) & 0x0f];
                v[(op_b & 0x0f) + 1] = v[((op_b >> 4) & 0x0f) + 1];
                pc += 1;
                break;

            case 0x05: /* move-wide/from16 vAA, vBBBB */
                v[op_b] = v[insns[pc + 1]];
                v[op_b + 1] = v[insns[pc + 1] + 1];
                pc += 2;
                break;

            case 0x0a: /* move-result vAA */
            case 0x0b: /* move-result-wide vAA */
            case 0x0c: /* move-result-object vAA */
                v[op_b] = last_result;
                pc += 1;
                break;

            case 0x0e: /* return-void */
                goto done;

            case 0x0f: /* return vAA */
            case 0x11: /* return-object vAA */
                if (ret) *ret = v[op_b];
                goto done;

            case 0x10: /* return-wide vAA */
                if (ret) {
                    ret->j = v[op_b].j;
                }
                goto done;

            case 0x12: { /* const/4 vA, #+B */
                uint8_t a = op_b & 0x0f;
                int8_t b = (int8_t)(op_b & 0xf0) >> 4;
                v[a].i = b;
                pc += 1;
                break;
            }

            case 0x13: /* const/16 vAA, #+BBBB */
                v[op_b].i = (int16_t)insns[pc + 1];
                pc += 2;
                break;

            case 0x14: { /* const vAA, #+BBBBBBBB */
                uint32_t val = (uint32_t)insns[pc + 1] | ((uint32_t)insns[pc + 2] << 16);
                v[op_b].raw32 = val;
                pc += 3;
                break;
            }

            case 0x15: /* const-high16 vAA, #+BBBB0000 */
                v[op_b].raw32 = (uint32_t)insns[pc + 1] << 16;
                pc += 2;
                break;

            case 0x16: /* const-wide/16 vAA, #+BBBB */
                v[op_b].j = (int16_t)insns[pc + 1];
                pc += 2;
                break;

            case 0x17: { /* const-wide/32 vAA, #+BBBBBBBB */
                int32_t val = (int32_t)((uint32_t)insns[pc + 1] | ((uint32_t)insns[pc + 2] << 16));
                v[op_b].j = val;
                pc += 3;
                break;
            }

            case 0x18: { /* const-wide vAA, #+BBBBBBBBBBBBBBBB */
                uint64_t w0 = insns[pc + 1];
                uint64_t w1 = insns[pc + 2];
                uint64_t w2 = insns[pc + 3];
                uint64_t w3 = insns[pc + 4];
                v[op_b].raw64 = w0 | (w1 << 16) | (w2 << 32) | (w3 << 48);
                pc += 5;
                break;
            }

            case 0x19: /* const-wide/high16 vAA, #+BBBB000000000000 */
                v[op_b].raw64 = (uint64_t)insns[pc + 1] << 48;
                pc += 2;
                break;

            case 0x1a: { /* const-string vAA, string@BBBB */
                const char *s = dex_get_string(dex, insns[pc + 1]);
                v[op_b].l = tl_dex_alloc_string(ctx, s);
                pc += 2;
                break;
            }

            case 0x1c: { /* const-class vAA, type@BBBB */
                const char *tdesc = dex_get_type(dex, insns[pc + 1]);
                tl_dex_class *cl = tl_dex_find_class(ctx, tdesc);
                v[op_b].l = (tl_dex_object *)cl;
                pc += 2;
                break;
            }

            case 0x1d: /* monitor-enter */
            case 0x1e: /* monitor-exit */
            case 0x1f: /* check-cast */
                pc += (opcode == 0x1f) ? 2 : 1;
                break;

            case 0x20: { /* instance-of vA, vB, type@CCCC */
                uint8_t a = op_b & 0x0f;
                uint8_t b = (op_b >> 4) & 0x0f;
                v[a].i = (v[b].l != NULL) ? 1 : 0;
                pc += 2;
                break;
            }

            case 0x21: { /* array-length vA, vB */
                uint8_t a = op_b & 0x0f;
                uint8_t b = (op_b >> 4) & 0x0f;
                v[a].i = v[b].l ? (int32_t)v[b].l->array.length : 0;
                pc += 1;
                break;
            }

            case 0x22: { /* new-instance vAA, type@BBBB */
                const char *tdesc = dex_get_type(dex, insns[pc + 1]);
                tl_dex_class *cl = tl_dex_find_class(ctx, tdesc);
                v[op_b].l = tl_dex_alloc_object(cl);
                pc += 2;
                break;
            }

            case 0x23: { /* new-array vA, vB, type@CCCC */
                uint8_t a = op_b & 0x0f;
                uint8_t b = (op_b >> 4) & 0x0f;
                const char *tdesc = dex_get_type(dex, insns[pc + 1]);
                tl_dex_class *cl = tl_dex_find_class(ctx, tdesc);
                uint32_t len = (v[b].i > 0) ? (uint32_t)v[b].i : 0;
                v[a].l = tl_dex_alloc_array(cl, len, sizeof(void *));
                pc += 2;
                break;
            }

            case 0x26: { /* fill-array-data vAA, +BBBBBBBB */
                int32_t rel = (int32_t)((uint32_t)insns[pc + 1] | ((uint32_t)insns[pc + 2] << 16));
                const uint16_t *payload = insns + pc + rel;
                if (payload[0] == 0x0300 && v[op_b].l && v[op_b].l->array.elements) {
                    uint16_t elem_width = payload[1];
                    uint32_t count = (uint32_t)payload[2] | ((uint32_t)payload[3] << 8);
                    const uint8_t *src = (const uint8_t *)(payload + 4);
                    memcpy(v[op_b].l->array.elements, src, count * elem_width);
                }
                pc += 3;
                break;
            }

            case 0x28: /* goto +AA */
                pc += (int8_t)op_b;
                break;

            case 0x29: /* goto/16 +AAAA */
                pc += (int16_t)insns[pc + 1];
                break;

            case 0x2a: /* goto/32 +AAAAAAAA */
                pc += (int32_t)((uint32_t)insns[pc + 1] | ((uint32_t)insns[pc + 2] << 16));
                break;

            case 0x2d: /* cmpl-float vAA, vBB, vCC */
            case 0x2e: { /* cmpg-float vAA, vBB, vCC */
                uint16_t regs = insns[pc + 1];
                uint8_t b = regs & 0xff;
                uint8_t c = (regs >> 8) & 0xff;
                float fb = v[b].f;
                float fc = v[c].f;
                if (isnan(fb) || isnan(fc)) v[op_b].i = (opcode == 0x2d) ? -1 : 1;
                else if (fb > fc) v[op_b].i = 1;
                else if (fb < fc) v[op_b].i = -1;
                else v[op_b].i = 0;
                pc += 2;
                break;
            }

            case 0x2f: /* cmpl-double vAA, vBB, vCC */
            case 0x30: { /* cmpg-double vAA, vBB, vCC */
                uint16_t regs = insns[pc + 1];
                uint8_t b = regs & 0xff;
                uint8_t c = (regs >> 8) & 0xff;
                double db = v[b].d;
                double dc = v[c].d;
                if (isnan(db) || isnan(dc)) v[op_b].i = (opcode == 0x2f) ? -1 : 1;
                else if (db > dc) v[op_b].i = 1;
                else if (db < dc) v[op_b].i = -1;
                else v[op_b].i = 0;
                pc += 2;
                break;
            }

            case 0x31: { /* cmp-long vAA, vBB, vCC */
                uint16_t regs = insns[pc + 1];
                uint8_t b = regs & 0xff;
                uint8_t c = (regs >> 8) & 0xff;
                int64_t jb = v[b].j;
                int64_t jc = v[c].j;
                if (jb > jc) v[op_b].i = 1;
                else if (jb < jc) v[op_b].i = -1;
                else v[op_b].i = 0;
                pc += 2;
                break;
            }

            case 0x32: /* if-eq vA, vB, +CCCC */
            case 0x33: /* if-ne vA, vB, +CCCC */
            case 0x34: /* if-lt vA, vB, +CCCC */
            case 0x35: /* if-ge vA, vB, +CCCC */
            case 0x36: /* if-gt vA, vB, +CCCC */
            case 0x37: { /* if-le vA, vB, +CCCC */
                uint8_t a = op_b & 0x0f;
                uint8_t b = (op_b >> 4) & 0x0f;
                int16_t target = (int16_t)insns[pc + 1];
                bool take = false;
                if (opcode == 0x32) take = (v[a].i == v[b].i);
                else if (opcode == 0x33) take = (v[a].i != v[b].i);
                else if (opcode == 0x34) take = (v[a].i < v[b].i);
                else if (opcode == 0x35) take = (v[a].i >= v[b].i);
                else if (opcode == 0x36) take = (v[a].i > v[b].i);
                else if (opcode == 0x37) take = (v[a].i <= v[b].i);
                if (take) pc += target;
                else pc += 2;
                break;
            }

            case 0x38: /* if-eqz vAA, +BBBB */
            case 0x39: /* if-nez vAA, +BBBB */
            case 0x3a: /* if-ltz vAA, +BBBB */
            case 0x3b: /* if-gez vAA, +BBBB */
            case 0x3c: /* if-gtz vAA, +BBBB */
            case 0x3d: { /* if-lez vAA, +BBBB */
                int16_t target = (int16_t)insns[pc + 1];
                bool take = false;
                if (opcode == 0x38) take = (v[op_b].i == 0);
                else if (opcode == 0x39) take = (v[op_b].i != 0);
                else if (opcode == 0x3a) take = (v[op_b].i < 0);
                else if (opcode == 0x3b) take = (v[op_b].i >= 0);
                else if (opcode == 0x3c) take = (v[op_b].i > 0);
                else if (opcode == 0x3d) take = (v[op_b].i <= 0);
                if (take) pc += target;
                else pc += 2;
                break;
            }

            case 0x44: /* aget */
            case 0x45: /* aget-wide */
            case 0x46: /* aget-object */
            case 0x47: /* aget-boolean */
            case 0x48: /* aget-byte */
            case 0x49: /* aget-char */
            case 0x4a: { /* aget-short */
                uint16_t regs = insns[pc + 1];
                uint8_t arr_reg = regs & 0xff;
                uint8_t idx_reg = (regs >> 8) & 0xff;
                tl_dex_object *arr = v[arr_reg].l;
                int32_t idx = v[idx_reg].i;
                if (arr && arr->array.elements && idx >= 0 && (uint32_t)idx < arr->array.length) {
                    if (opcode == 0x45) {
                        v[op_b].j = ((int64_t *)arr->array.elements)[idx];
                    } else if (opcode == 0x46) {
                        v[op_b].l = ((tl_dex_object **)arr->array.elements)[idx];
                    } else {
                        v[op_b].i = ((int32_t *)arr->array.elements)[idx];
                    }
                }
                pc += 2;
                break;
            }

            case 0x4b: /* aput */
            case 0x4c: /* aput-wide */
            case 0x4d: /* aput-object */
            case 0x4e: /* aput-boolean */
            case 0x4f: /* aput-byte */
            case 0x50: /* aput-char */
            case 0x51: { /* aput-short */
                uint16_t regs = insns[pc + 1];
                uint8_t arr_reg = regs & 0xff;
                uint8_t idx_reg = (regs >> 8) & 0xff;
                tl_dex_object *arr = v[arr_reg].l;
                int32_t idx = v[idx_reg].i;
                if (arr && arr->array.elements && idx >= 0 && (uint32_t)idx < arr->array.length) {
                    if (opcode == 0x4c) {
                        ((int64_t *)arr->array.elements)[idx] = v[op_b].j;
                    } else if (opcode == 0x4d) {
                        ((tl_dex_object **)arr->array.elements)[idx] = v[op_b].l;
                    } else {
                        ((int32_t *)arr->array.elements)[idx] = v[op_b].i;
                    }
                }
                pc += 2;
                break;
            }

            case 0x52: /* iget */
            case 0x53: /* iget-wide */
            case 0x54: /* iget-object */
            case 0x55: /* iget-boolean */
            case 0x56: /* iget-byte */
            case 0x57: /* iget-char */
            case 0x58: { /* iget-short */
                uint8_t a = op_b & 0x0f;
                uint8_t b = (op_b >> 4) & 0x0f;
                tl_dex_field *f = dex_resolve_field(ctx, dex, insns[pc + 1]);
                tl_dex_object *obj = v[b].l;
                if (obj && obj->fields && f) {
                    v[a] = obj->fields[f->slot];
                } else {
                    v[a].raw64 = 0;
                }
                pc += 2;
                break;
            }

            case 0x59: /* iput */
            case 0x5a: /* iput-wide */
            case 0x5b: /* iput-object */
            case 0x5c: /* iput-boolean */
            case 0x5d: /* iput-byte */
            case 0x5e: /* iput-char */
            case 0x5f: { /* iput-short */
                uint8_t a = op_b & 0x0f;
                uint8_t b = (op_b >> 4) & 0x0f;
                tl_dex_field *f = dex_resolve_field(ctx, dex, insns[pc + 1]);
                tl_dex_object *obj = v[b].l;
                if (obj && obj->fields && f) {
                    obj->fields[f->slot] = v[a];
                }
                pc += 2;
                break;
            }

            case 0x60: /* sget */
            case 0x61: /* sget-wide */
            case 0x62: /* sget-object */
            case 0x63: /* sget-boolean */
            case 0x64: /* sget-byte */
            case 0x65: /* sget-char */
            case 0x66: { /* sget-short */
                tl_dex_field *f = dex_resolve_field(ctx, dex, insns[pc + 1]);
                if (f && f->clazz && f->clazz->static_values) {
                    v[op_b] = f->clazz->static_values[f->slot];
                } else {
                    v[op_b].raw64 = 0;
                }
                pc += 2;
                break;
            }

            case 0x67: /* sput */
            case 0x68: /* sput-wide */
            case 0x69: /* sput-object */
            case 0x6a: /* sput-boolean */
            case 0x6b: /* sput-byte */
            case 0x6c: /* sput-char */
            case 0x6d: { /* sput-short */
                tl_dex_field *f = dex_resolve_field(ctx, dex, insns[pc + 1]);
                if (f && f->clazz && f->clazz->static_values) {
                    f->clazz->static_values[f->slot] = v[op_b];
                }
                pc += 2;
                break;
            }

            case 0x6e: /* invoke-virtual */
            case 0x6f: /* invoke-super */
            case 0x70: /* invoke-direct */
            case 0x71: /* invoke-static */
            case 0x72: { /* invoke-interface */
                uint8_t count = (op_b >> 4) & 0x0f;
                uint16_t m_idx = insns[pc + 1];
                uint16_t arg_regs = insns[pc + 2];
                tl_dex_method *target = dex_resolve_method(ctx, dex, m_idx);

                uint8_t reg_list[5];
                reg_list[0] = arg_regs & 0x0f;
                reg_list[1] = (arg_regs >> 4) & 0x0f;
                reg_list[2] = (arg_regs >> 8) & 0x0f;
                reg_list[3] = (arg_regs >> 12) & 0x0f;
                reg_list[4] = op_b & 0x0f;

                tl_dex_val call_args[5] = {0};
                for (int i = 0; i < count && i < 5; i++) {
                    call_args[i] = v[reg_list[i]];
                }

                if (!target) {
                    dex_log("  invoke target NULL for idx=%d", m_idx);
                } else {
                    dex_log("  invoke %s->%s (native=%p)", target->clazz ? target->clazz->descriptor : "?", target->name, target->native_func);
                }

                tl_dex_invoke(ctx, target, call_args, count, &last_result);
                pc += 3;
                break;
            }

            case 0x74: /* invoke-virtual/range */
            case 0x75: /* invoke-super/range */
            case 0x76: /* invoke-direct/range */
            case 0x77: /* invoke-static/range */
            case 0x78: { /* invoke-interface/range */
                uint8_t count = op_b;
                uint16_t m_idx = insns[pc + 1];
                uint16_t first_reg = insns[pc + 2];
                tl_dex_method *target = dex_resolve_method(ctx, dex, m_idx);

                tl_dex_val *call_args = calloc(count ? count : 1, sizeof(tl_dex_val));
                for (int i = 0; i < count; i++) {
                    call_args[i] = v[first_reg + i];
                }
                tl_dex_invoke(ctx, target, call_args, count, &last_result);
                free(call_args);
                pc += 3;
                break;
            }

            /* Unary arithmetic & Conversions */
            case 0x7b: /* neg-int */    v[op_b & 0x0f].i = -v[(op_b >> 4) & 0x0f].i; pc += 1; break;
            case 0x7c: /* not-int */    v[op_b & 0x0f].i = ~v[(op_b >> 4) & 0x0f].i; pc += 1; break;
            case 0x7d: /* neg-long */   v[op_b & 0x0f].j = -v[(op_b >> 4) & 0x0f].j; pc += 1; break;
            case 0x7e: /* not-long */   v[op_b & 0x0f].j = ~v[(op_b >> 4) & 0x0f].j; pc += 1; break;
            case 0x7f: /* neg-float */  v[op_b & 0x0f].f = -v[(op_b >> 4) & 0x0f].f; pc += 1; break;
            case 0x80: /* neg-double */ v[op_b & 0x0f].d = -v[(op_b >> 4) & 0x0f].d; pc += 1; break;
            case 0x81: /* int-to-long */   v[op_b & 0x0f].j = v[(op_b >> 4) & 0x0f].i; pc += 1; break;
            case 0x82: /* int-to-float */  v[op_b & 0x0f].f = (float)v[(op_b >> 4) & 0x0f].i; pc += 1; break;
            case 0x83: /* int-to-double */ v[op_b & 0x0f].d = (double)v[(op_b >> 4) & 0x0f].i; pc += 1; break;
            case 0x84: /* long-to-int */   v[op_b & 0x0f].i = (int32_t)v[(op_b >> 4) & 0x0f].j; pc += 1; break;
            case 0x85: /* long-to-float */ v[op_b & 0x0f].f = (float)v[(op_b >> 4) & 0x0f].j; pc += 1; break;
            case 0x86: /* long-to-double */v[op_b & 0x0f].d = (double)v[(op_b >> 4) & 0x0f].j; pc += 1; break;
            case 0x87: /* float-to-int */  v[op_b & 0x0f].i = (int32_t)v[(op_b >> 4) & 0x0f].f; pc += 1; break;
            case 0x88: /* float-to-long */ v[op_b & 0x0f].j = (int64_t)v[(op_b >> 4) & 0x0f].f; pc += 1; break;
            case 0x89: /* float-to-double */v[op_b & 0x0f].d = (double)v[(op_b >> 4) & 0x0f].f; pc += 1; break;
            case 0x8a: /* double-to-int */ v[op_b & 0x0f].i = (int32_t)v[(op_b >> 4) & 0x0f].d; pc += 1; break;
            case 0x8b: /* double-to-long */v[op_b & 0x0f].j = (int64_t)v[(op_b >> 4) & 0x0f].d; pc += 1; break;
            case 0x8c: /* double-to-float */v[op_b & 0x0f].f = (float)v[(op_b >> 4) & 0x0f].d; pc += 1; break;

            /* Binary arithmetic (3 registers) */
            case 0x90: { /* add-int */
                uint16_t regs = insns[pc + 1];
                v[op_b].i = v[regs & 0xff].i + v[(regs >> 8) & 0xff].i;
                pc += 2;
                break;
            }
            case 0x91: { /* sub-int */
                uint16_t regs = insns[pc + 1];
                v[op_b].i = v[regs & 0xff].i - v[(regs >> 8) & 0xff].i;
                pc += 2;
                break;
            }
            case 0x92: { /* mul-int */
                uint16_t regs = insns[pc + 1];
                v[op_b].i = v[regs & 0xff].i * v[(regs >> 8) & 0xff].i;
                pc += 2;
                break;
            }
            case 0x93: { /* div-int */
                uint16_t regs = insns[pc + 1];
                int32_t d = v[(regs >> 8) & 0xff].i;
                v[op_b].i = d ? (v[regs & 0xff].i / d) : 0;
                pc += 2;
                break;
            }
            case 0x94: { /* rem-int */
                uint16_t regs = insns[pc + 1];
                int32_t d = v[(regs >> 8) & 0xff].i;
                v[op_b].i = d ? (v[regs & 0xff].i % d) : 0;
                pc += 2;
                break;
            }
            case 0x95: { /* and-int */
                uint16_t regs = insns[pc + 1];
                v[op_b].i = v[regs & 0xff].i & v[(regs >> 8) & 0xff].i;
                pc += 2;
                break;
            }
            case 0x96: { /* or-int */
                uint16_t regs = insns[pc + 1];
                v[op_b].i = v[regs & 0xff].i | v[(regs >> 8) & 0xff].i;
                pc += 2;
                break;
            }
            case 0x97: { /* xor-int */
                uint16_t regs = insns[pc + 1];
                v[op_b].i = v[regs & 0xff].i ^ v[(regs >> 8) & 0xff].i;
                pc += 2;
                break;
            }
            case 0x98: { /* shl-int */
                uint16_t regs = insns[pc + 1];
                v[op_b].i = v[regs & 0xff].i << (v[(regs >> 8) & 0xff].i & 0x1f);
                pc += 2;
                break;
            }
            case 0x99: { /* shr-int */
                uint16_t regs = insns[pc + 1];
                v[op_b].i = v[regs & 0xff].i >> (v[(regs >> 8) & 0xff].i & 0x1f);
                pc += 2;
                break;
            }

            case 0xa6: { /* add-float */
                uint16_t regs = insns[pc + 1];
                v[op_b].f = v[regs & 0xff].f + v[(regs >> 8) & 0xff].f;
                pc += 2;
                break;
            }
            case 0xa7: { /* sub-float */
                uint16_t regs = insns[pc + 1];
                v[op_b].f = v[regs & 0xff].f - v[(regs >> 8) & 0xff].f;
                pc += 2;
                break;
            }
            case 0xa8: { /* mul-float */
                uint16_t regs = insns[pc + 1];
                v[op_b].f = v[regs & 0xff].f * v[(regs >> 8) & 0xff].f;
                pc += 2;
                break;
            }
            case 0xa9: { /* div-float */
                uint16_t regs = insns[pc + 1];
                v[op_b].f = v[regs & 0xff].f / v[(regs >> 8) & 0xff].f;
                pc += 2;
                break;
            }
            case 0xab: { /* add-double */
                uint16_t regs = insns[pc + 1];
                v[op_b].d = v[regs & 0xff].d + v[(regs >> 8) & 0xff].d;
                pc += 2;
                break;
            }
            case 0xac: { /* sub-double */
                uint16_t regs = insns[pc + 1];
                v[op_b].d = v[regs & 0xff].d - v[(regs >> 8) & 0xff].d;
                pc += 2;
                break;
            }
            case 0xad: { /* mul-double */
                uint16_t regs = insns[pc + 1];
                v[op_b].d = v[regs & 0xff].d * v[(regs >> 8) & 0xff].d;
                pc += 2;
                break;
            }

            /* Binary arithmetic /2addr */
            case 0xb0: v[op_b & 0x0f].i += v[(op_b >> 4) & 0x0f].i; pc += 1; break; /* add-int/2addr */
            case 0xb1: v[op_b & 0x0f].i -= v[(op_b >> 4) & 0x0f].i; pc += 1; break; /* sub-int/2addr */
            case 0xb2: v[op_b & 0x0f].i *= v[(op_b >> 4) & 0x0f].i; pc += 1; break; /* mul-int/2addr */
            case 0xc6: v[op_b & 0x0f].f += v[(op_b >> 4) & 0x0f].f; pc += 1; break; /* add-float/2addr */
            case 0xc7: v[op_b & 0x0f].f -= v[(op_b >> 4) & 0x0f].f; pc += 1; break; /* sub-float/2addr */
            case 0xc8: v[op_b & 0x0f].f *= v[(op_b >> 4) & 0x0f].f; pc += 1; break; /* mul-float/2addr */
            case 0xc9: v[op_b & 0x0f].f /= v[(op_b >> 4) & 0x0f].f; pc += 1; break; /* div-float/2addr */

            /* Literal arithmetic */
            case 0xd0: /* add-int/lit16 */
                v[op_b & 0x0f].i = v[(op_b >> 4) & 0x0f].i + (int16_t)insns[pc + 1];
                pc += 2;
                break;
            case 0xd1: /* rsub-int */
                v[op_b & 0x0f].i = (int16_t)insns[pc + 1] - v[(op_b >> 4) & 0x0f].i;
                pc += 2;
                break;
            case 0xd2: /* mul-int/lit16 */
                v[op_b & 0x0f].i = v[(op_b >> 4) & 0x0f].i * (int16_t)insns[pc + 1];
                pc += 2;
                break;

            case 0xd8: /* add-int/lit8 */
                v[op_b].i = v[insns[pc + 1] & 0xff].i + (int8_t)((insns[pc + 1] >> 8) & 0xff);
                pc += 2;
                break;
            case 0xd9: /* rsub-int/lit8 */
                v[op_b].i = (int8_t)((insns[pc + 1] >> 8) & 0xff) - v[insns[pc + 1] & 0xff].i;
                pc += 2;
                break;
            case 0xda: /* mul-int/lit8 */
                v[op_b].i = v[insns[pc + 1] & 0xff].i * (int8_t)((insns[pc + 1] >> 8) & 0xff);
                pc += 2;
                break;
            case 0xdb: { /* div-int/lit8 */
                int8_t d = (int8_t)((insns[pc + 1] >> 8) & 0xff);
                v[op_b].i = d ? (v[insns[pc + 1] & 0xff].i / d) : 0;
                pc += 2;
                break;
            }
            case 0xde: /* and-int/lit8 */
                v[op_b].i = v[insns[pc + 1] & 0xff].i & (int8_t)((insns[pc + 1] >> 8) & 0xff);
                pc += 2;
                break;
            case 0xdf: /* or-int/lit8 */
                v[op_b].i = v[insns[pc + 1] & 0xff].i | (int8_t)((insns[pc + 1] >> 8) & 0xff);
                pc += 2;
                break;

            default:
                /* Advance by standard width */
                pc += 1;
                break;
        }
    }

done:
    dex_log("return: %s->%s", method->clazz ? method->clazz->descriptor : "unknown", method->name);
    free(v);
    return success;
}

/* ------------------------------------------------ Frame & Touch Dispatch */

void tl_dex_tick_frame(tl_dex_context *ctx, uint64_t frame_time_nanos)
{
    if (!ctx) return;

    /* 1. Tick Choreographer callback */
    if (ctx->choreographer_cb) {
        tl_dex_method *doFrame = tl_dex_find_method(ctx->choreographer_cb->clazz, "doFrame", "VJ");
        if (doFrame) {
            tl_dex_val args[2];
            args[0].l = ctx->choreographer_cb;
            args[1].j = (int64_t)frame_time_nanos;
            tl_dex_invoke(ctx, doFrame, args, 2, NULL);
        }
    }

    /* 2. Render view */
    tl_framework_render_view(ctx);
    ctx->frame_count++;
}

void tl_dex_send_touch(tl_dex_context *ctx, int action, float x, float y)
{
    if (!ctx || !ctx->current_view) return;

    tl_dex_class *c_me = tl_dex_find_class(ctx, "Landroid/view/MotionEvent;");
    tl_dex_object *ev = tl_dex_alloc_object(c_me);
    if (ev && ev->fields) {
        ev->fields[0].f = x;
        ev->fields[1].f = y;
        ev->fields[2].i = action;
    }

    tl_dex_method *onTouch = tl_dex_find_method(ctx->current_view->clazz, "onTouchEvent", "ZL");
    if (onTouch) {
        tl_dex_val args[2];
        args[0].l = ctx->current_view;
        args[1].l = ev;
        tl_dex_invoke(ctx, onTouch, args, 2, NULL);
    }
}
