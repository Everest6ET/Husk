/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "husk-tl.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <path-to-apk>\n", argv[0]);
        return 1;
    }

    const char *apks[] = { argv[1] };
    printf("=== Starting Husk Translation Layer Attempt ===\n");
    printf("Target APK: %s\n", argv[1]);

    int rc = husk_tl_attempt_start(apks, 1, 5);
    if (rc != 0) {
        fprintf(stderr, "Error: husk_tl_attempt_start failed (%d)\n", rc);
        return 1;
    }

    int exit_code = 0;
    while (!husk_tl_attempt_done(&exit_code)) {
        char *log = husk_tl_attempt_log();
        if (log && *log) {
            fputs(log, stdout);
            fflush(stdout);
        }
        usleep(50 * 1000);
    }

    char *log = husk_tl_attempt_log();
    if (log && *log) {
        fputs(log, stdout);
        fflush(stdout);
    }

    printf("=== Attempt finished (exit_code=%d, frames=%d) ===\n",
           exit_code, husk_tl_attempt_frames());
    husk_tl_attempt_reset();
    return 0;
}
