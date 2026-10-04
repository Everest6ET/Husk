/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HUSK_TL_FRAMEWORK_H
#define HUSK_TL_FRAMEWORK_H

#include "husk-tl-dex.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native function handler type */
typedef bool (*tl_dex_native_func)(tl_dex_context *ctx, tl_dex_object *this_obj,
                                  tl_dex_val *args, int nargs, tl_dex_val *ret);

/* Look up a built-in / native framework method */
tl_dex_native_func tl_framework_lookup(const char *class_desc, const char *method_name, const char *shorty);

/* Framework initialisation and asset loading */
bool tl_framework_init(tl_dex_context *ctx);
void tl_framework_cleanup(tl_dex_context *ctx);

/* Direct canvas rendering for a frame */
void tl_framework_render_view(tl_dex_context *ctx);

#ifdef __cplusplus
}
#endif

#endif /* HUSK_TL_FRAMEWORK_H */
