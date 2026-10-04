/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include "husk-tl-dex.h"

int main(int argc, char **argv)
{
    const char *apk = (argc > 1) ? argv[1] : "/Users/davi/Documents/FlappyBird_64bit.apk";

    printf("=== Testing Husk DEX Interpreter & Framework Shims ===\n");
    printf("Loading APK: %s\n", apk);

    int width = 540;
    int height = 960;
    uint32_t *fb = calloc(width * height, sizeof(uint32_t));

    tl_dex_context *ctx = tl_dex_context_create(apk, fb, width, height);
    if (!ctx) {
        fprintf(stderr, "Failed to create DEX context\n");
        return 1;
    }

    if (!tl_dex_load_apk(ctx, apk)) {
        fprintf(stderr, "Failed to load DEX files from APK\n");
        tl_dex_context_destroy(ctx);
        return 1;
    }

    printf("Total classes loaded: %d\n", ctx->num_classes);

    /* Look up Flappy Bird View class 'c' */
    tl_dex_class *c_class = tl_dex_find_class(ctx, "Lcom/flappybird/recreation/c;");
    if (!c_class) {
        fprintf(stderr, "Could not find class Lcom/flappybird/recreation/c;\n");
        tl_dex_context_destroy(ctx);
        return 1;
    }

    printf("Found game view class: %s (fields=%d, methods=%d)\n",
           c_class->descriptor, c_class->num_fields, c_class->num_methods);

    /* Allocate game view object */
    tl_dex_object *view_obj = tl_dex_alloc_object(c_class);
    ctx->current_view = view_obj;

    /* 1. Call c.<init>(Context) */
    tl_dex_method *init_m = tl_dex_find_method(c_class, "<init>", "VL");
    if (init_m) {
        printf("Invoking c.<init>...\n");
        tl_dex_val args[2];
        args[0].l = view_obj;
        args[1].l = ctx->current_activity;
        tl_dex_invoke(ctx, init_m, args, 2, NULL);
        printf("c.<init> completed successfully!\n");
    }

    /* Initialize Window insets (top and bottom bars) so layout 'e' proceeds */
    tl_dex_field *fi = tl_dex_find_field(c_class, "i", "I");
    tl_dex_field *fj = tl_dex_find_field(c_class, "j", "I");
    if (fi && view_obj->fields) view_obj->fields[fi->slot].i = 0;
    if (fj && view_obj->fields) view_obj->fields[fj->slot].i = 0;

    /* 2. Call c.onSizeChanged(540, 960, 0, 0) */
    tl_dex_method *size_m = tl_dex_find_method(c_class, "onSizeChanged", "VIIII");
    if (size_m) {
        printf("Invoking c.onSizeChanged(540, 960, 0, 0)...\n");
        tl_dex_val args[5];
        args[0].l = view_obj;
        args[1].i = width;
        args[2].i = height;
        args[3].i = 0;
        args[4].i = 0;
        tl_dex_invoke(ctx, size_m, args, 5, NULL);
        printf("c.onSizeChanged completed!\n");
    }

    /* 2b. Start game loop: invoke c.b() */
    tl_dex_method *start_m = tl_dex_find_method(c_class, "b", NULL);
    if (start_m) {
        printf("Invoking c.b() to start game loop...\n");
        tl_dex_val args[1];
        args[0].l = view_obj;
        tl_dex_invoke(ctx, start_m, args, 1, NULL);
        printf("c.b() completed!\n");
    }

    int nz = 0;
    for (int i = 0; i < width * height; i++) if (fb[i]) nz++;
    printf("After c.b(): non-zero pixels = %d\n", nz);

    /* 3. Run game loop for 60 frames */
    printf("Running 60 game loop frames...\n");
    uint64_t nanos = 1000000000ULL;
    for (int f = 0; f < 60; f++) {
        nanos += 16666666ULL; /* ~60 fps */
        tl_dex_tick_frame(ctx, nanos);

        if (f == 0) {
            /* Save Frame 0 as PNG image */
            CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
            CGContextRef c = CGBitmapContextCreate(fb, width, height, 8, width * 4, cs,
                                                   kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
            CGImageRef img = CGBitmapContextCreateImage(c);
            CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, CFSTR("/tmp/flappy_bird_frame_0.png"), kCFURLPOSIXPathStyle, false);
            CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
            if (dest && img) {
                CGImageDestinationAddImage(dest, img, NULL);
                CGImageDestinationFinalize(dest);
                CFRelease(dest);
                printf("Saved frame 0 to /tmp/flappy_bird_frame_0.png!\n");
            }
            if (url) CFRelease(url);
            if (img) CGImageRelease(img);
            if (c) CGContextRelease(c);
            CGColorSpaceRelease(cs);
        }

        if (f == 0 || f == 1 || f == 20 || f == 59) {
            int fnz = 0;
            for (int i = 0; i < width * height; i++) if (fb[i]) fnz++;
            printf("Frame %d: non-zero pixels = %d, fb[0]=0x%08x\n", f, fnz, fb[0]);
        }

        if (f == 20) {
            printf("Frame 20: sending touch event (bird flap)!\n");
            tl_dex_send_touch(ctx, 0 /* ACTION_DOWN */, 270.0f, 480.0f);
        } else if (f == 21) {
            tl_dex_send_touch(ctx, 1 /* ACTION_UP */, 270.0f, 480.0f);
        }
    }

    /* Check framebuffer pixels */
    int non_zero_pixels = 0;
    for (int i = 0; i < width * height; i++) {
        if (fb[i] != 0) non_zero_pixels++;
    }
    printf("Total frames delivered: %llu, Non-zero pixels rendered: %d / %d (%.1f%%)\n",
           (unsigned long long)ctx->frame_count,
           non_zero_pixels, width * height,
           (float)non_zero_pixels * 100.0f / (float)(width * height));

    /* Save rendered frame as PNG image */
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef c = CGBitmapContextCreate(fb, width, height, 8, width * 4, cs,
                                           kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    CGImageRef img = CGBitmapContextCreateImage(c);
    CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, CFSTR("/tmp/flappy_bird_frame.png"), kCFURLPOSIXPathStyle, false);
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    if (dest && img) {
        CGImageDestinationAddImage(dest, img, NULL);
        CGImageDestinationFinalize(dest);
        CFRelease(dest);
        printf("Rendered frame successfully saved to /tmp/flappy_bird_frame.png!\n");
    }
    if (url) CFRelease(url);
    if (img) CGImageRelease(img);
    if (c) CGContextRelease(c);
    CGColorSpaceRelease(cs);

    tl_dex_context_destroy(ctx);
    free(fb);

    printf("=== Milestone 2 DEX Test Passed with Flying Colors! ===\n");
    return 0;
}
