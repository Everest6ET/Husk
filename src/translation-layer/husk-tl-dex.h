/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HUSK_TL_DEX_H
#define HUSK_TL_DEX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tl_dex_file tl_dex_file;
typedef struct tl_dex_class tl_dex_class;
typedef struct tl_dex_method tl_dex_method;
typedef struct tl_dex_field tl_dex_field;
typedef struct tl_dex_object tl_dex_object;
typedef struct tl_dex_context tl_dex_context;

typedef union {
    int32_t  i;
    int64_t  j;
    float    f;
    double   d;
    tl_dex_object *l;
    uint32_t raw32;
    uint64_t raw64;
} tl_dex_val;

struct tl_dex_field {
    tl_dex_class *clazz;
    const char *name;
    const char *type;
    uint32_t access_flags;
    bool is_static;
    uint32_t slot;          /* offset in object or static table */
};

struct tl_dex_method {
    tl_dex_class *clazz;
    const char *name;
    const char *shorty;
    const char *signature;
    uint32_t access_flags;
    uint16_t registers_size;
    uint16_t ins_size;
    uint16_t outs_size;
    uint32_t insns_size;
    const uint16_t *insns;
    /* Native function pointer if ACC_NATIVE or framework builtin */
    void *native_func;
};

struct tl_dex_class {
    tl_dex_context *ctx;
    tl_dex_file *dex;
    const char *descriptor;     /* e.g. "Lcom/flappybird/recreation/c;" */
    const char *super_descriptor;
    tl_dex_class *super_class;
    uint32_t access_flags;

    int num_fields;
    tl_dex_field *fields;
    int num_instance_fields;
    int num_static_fields;

    int num_methods;
    tl_dex_method *methods;

    /* Static storage */
    tl_dex_val *static_values;
    bool initialized;

    tl_dex_class *next_hash;
    /* Native bridge handler if custom (e.g. Canvas, View, Bitmap) */
    void *native_class_data;
};

struct tl_dex_object {
    tl_dex_class *clazz;
    uint32_t flags;
    union {
        /* Object fields */
        tl_dex_val *fields;
        /* Array data */
        struct {
            uint32_t length;
            uint32_t elem_size;
            void *elements;
        } array;
        /* Java string value */
        char *str_utf8;
        /* Native object wrapper (e.g. Canvas*, Bitmap*, View*) */
        void *native_ptr;
    };
};

#define TL_DEX_CLASS_HASH_SIZE 16384

struct tl_dex_context {
    int num_dex_files;
    tl_dex_file *dex_files[16];

    int num_classes;
    int class_capacity;
    tl_dex_class **classes;
    tl_dex_class *class_hash[TL_DEX_CLASS_HASH_SIZE];

    /* Display surface for drawing */
    uint32_t *framebuffer;
    int fb_width;
    int fb_height;

    /* Active game view and activity */
    tl_dex_object *current_view;
    tl_dex_object *current_activity;

    /* Registered Choreographer frame callback */
    tl_dex_object *choreographer_cb;

    /* Total frames delivered */
    uint64_t frame_count;

    /* Asset lookup from APK */
    const char *apk_path;

    /* Framework private state */
    void *framework_data;
};

/* Context lifecycle */
tl_dex_context *tl_dex_context_create(const char *apk_path, uint32_t *fb, int width, int height);
void tl_dex_context_destroy(tl_dex_context *ctx);

/* Load classes*.dex from the given APK */
bool tl_dex_load_apk(tl_dex_context *ctx, const char *apk_path);

/* Find class by descriptor */
tl_dex_class *tl_dex_find_class(tl_dex_context *ctx, const char *descriptor);

/* Find method on class or superclasses */
tl_dex_method *tl_dex_find_method(tl_dex_class *clazz, const char *name, const char *shorty);

/* Find field on class or superclasses */
tl_dex_field *tl_dex_find_field(tl_dex_class *clazz, const char *name, const char *type);

/* Object allocation */
tl_dex_object *tl_dex_alloc_object(tl_dex_class *clazz);
tl_dex_object *tl_dex_alloc_array(tl_dex_class *elem_class, uint32_t length, uint32_t elem_size);
tl_dex_object *tl_dex_alloc_string(tl_dex_context *ctx, const char *utf8);

/* Bytecode execution */
bool tl_dex_invoke(tl_dex_context *ctx, tl_dex_method *method, tl_dex_val *args, int nargs, tl_dex_val *ret);

/* Frame tick and input event dispatch */
void tl_dex_tick_frame(tl_dex_context *ctx, uint64_t frame_time_nanos);
void tl_dex_send_touch(tl_dex_context *ctx, int action, float x, float y);

#ifdef __cplusplus
}
#endif

#endif /* HUSK_TL_DEX_H */
