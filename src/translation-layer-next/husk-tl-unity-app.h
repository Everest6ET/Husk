/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What the app calls to run a Unity game through the native runtime.
 *
 * Starting is asynchronous: the engine takes seconds to load, and the UI must stay up
 * meanwhile. The progress is in the log (husk_tl_attempt_log) and in husk_unity_state.
 * An engine cannot be unloaded, so a game that has been started stays loaded for the life
 * of the process; leaving its screen pauses it.
 */
#ifndef HUSK_TL_UNITY_APP_H
#define HUSK_TL_UNITY_APP_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { HUSK_UNITY_IDLE = 0, HUSK_UNITY_STARTING = 1, HUSK_UNITY_RUNNING = 2, HUSK_UNITY_FAILED = 3, HUSK_UNITY_ENDED = 4 };

/*
 * Start the game in `apk`. `metal_layer` is the CAMetalLayer it draws into, `width`/`height` its
 * size in pixels. `angle_dylib` is the bundled libANGLE-shared.dylib; `ca_bundle` a PEM file of root
 * certificates. Returns false if a game is already started or the arguments are unusable.
 */
bool husk_unity_launch(const char *apk, const char *data_dir, void *metal_layer, int width, int height,
                       const char *angle_dylib, const char *ca_bundle);

/* The same for a cocos2d-x game (Geometry Dash): a landscape surface, with sound. Status, touch and pause go through the calls below. */
bool husk_cocos_launch(const char *apk, const char *data_dir, void *metal_layer, int width, int height,
                       const char *angle_dylib, const char *ca_bundle);

/* The same for a game built on Google's GameActivity (Minecraft): a landscape surface, with sound and multi-touch. */
bool husk_gameactivity_launch(const char *apk, const char *data_dir, void *metal_layer, int width, int height,
                              const char *angle_dylib, const char *ca_bundle);

/* Soft keyboard for a cocos2d-x game. The handler is told (on the game's GL thread) 0 = toggle, 1 = show, 2 = hide. */
void husk_cocos_set_keyboard_handler(void (*handler)(int action));
void husk_cocos_insert_text(const char *utf8);
void husk_cocos_delete_backward(void);
void husk_cocos_key_down(int keycode);
void husk_cocos_request_text(void (*cb)(const char *utf8));    /* what the game's text field holds now; cb runs on the GL thread */

/* A link the game wants opened (terms of use, social buttons). The handler runs on the game's GL thread. */
void husk_cocos_set_open_url_handler(void (*handler)(const char *url));

/* The APK of the game started this session, or NULL. An engine cannot be loaded twice, nor two games at once. */
const char *husk_native_loaded_apk(void);

int  husk_unity_state(void);
unsigned long husk_unity_frames(void);
typedef struct husk_unity_perf { double fps, mean_ms, max_ms; } husk_unity_perf;
void husk_unity_perf_snapshot(husk_unity_perf *out);          /* since the last call */
void husk_unity_touch(int phase, int id, float x, float y);   /* phase 0 down, 1 move, 2 up, 3 cancel */
void husk_unity_set_paused(bool paused);

/* The app's package name from its manifest into `out`; false if it cannot be read. */
bool husk_unity_package_name(const char *apk, char *out, unsigned long out_len);

#ifdef __cplusplus
}
#endif

#endif
