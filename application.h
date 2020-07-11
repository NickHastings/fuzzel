#pragma once

#include <stdbool.h>

#include <cairo.h>
#include <librsvg/rsvg.h>

#include "prompt.h"

enum icon_type { ICON_NONE, ICON_SURFACE, ICON_SVG };

struct icon {
    char *name;
    enum icon_type type;
    union {
        cairo_surface_t *surface;
        RsvgHandle *svg;
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
