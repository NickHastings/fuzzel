#pragma once

#include <stdbool.h>
#include <pixman.h>

#include <cairo.h>
#ifdef FUZZEL_ENABLE_SVG
#include <librsvg/rsvg.h>
#endif

#include "prompt.h"

enum icon_type { ICON_NONE, ICON_SURFACE, ICON_PNG, ICON_SVG };

struct icon {
    char *name;
    enum icon_type type;
    union {
        cairo_surface_t *surface;
#if defined(FUZZEL_ENABLE_PNG)
        struct {
            pixman_image_t *pix;
            bool has_scale_transform;
        } png;
#endif
#if defined(FUZZEL_ENABLE_SVG)
        RsvgHandle *svg;
#endif
    };
};

struct application {
    char *path;
    char *exec;
    wchar_t *title;
    wchar_t *comment;
    struct icon icon;
    unsigned count;
};

bool application_execute(
    const struct application *app, const struct prompt *prompt);

struct application_list {
    struct application *v;
    size_t count;
};

struct application_list *applications_init(void);
void applications_destroy(struct application_list *apps);
