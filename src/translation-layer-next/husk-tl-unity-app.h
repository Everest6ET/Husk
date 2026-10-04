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

int  husk_unity_state(void);
unsigned long husk_unity_frames(void);
void husk_unity_touch(int phase, int id, float x, float y);   /* phase 0 down, 1 move, 2 up, 3 cancel */
void husk_unity_set_paused(bool paused);

/* The app's package name from its manifest into `out`; false if it cannot be read. */
bool husk_unity_package_name(const char *apk, char *out, unsigned long out_len);

#ifdef __cplusplus
}
#endif

#endif
