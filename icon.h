#pragma once

#include <stdbool.h>

#include "application.h"
#include "tllist.h"

struct icon_dir {
    char *path;  /* Relative to theme's base path */
    int size;
    int min_size;
    int max_size;
    int scale;
    bool scalable;
};

struct icon_theme {
    char *name;
    char *path;
    tll(struct icon_dir) dirs;
};

typedef tll(struct icon_theme) icon_theme_list_t;

icon_theme_list_t icon_load_theme(const char *name);
void icon_themes_destroy(icon_theme_list_t themes);

bool icon_reload_application_icons(
    icon_theme_list_t themes, int icon_size,
    struct application_list *applications);
