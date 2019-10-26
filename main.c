#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <wctype.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>

#include <locale.h>

#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>

#include <cairo.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wayland-util.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <xdg-output-unstable-v1.h>
#include <wlr-layer-shell-unstable-v1.h>

#define LOG_MODULE "fuzzel"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "application.h"
#include "dmenu.h"
#include "fdm.h"
#include "font.h"
#include "match.h"
#include "render.h"
#include "shm.h"
#include "tllist.h"
#include "version.h"
#include "wayland.h"
#include "xdg.h"

static struct rgba
hex_to_rgba(uint32_t color)
{
    return (struct rgba){
        .r = (double)((color >> 24) & 0xff) / 255.0,
        .g = (double)((color >> 16) & 0xff) / 255.0,
        .b = (double)((color >>  8) & 0xff) / 255.0,
        .a = (double)((color >>  0) & 0xff) / 255.0,
    };
}

static void
read_cache(struct application_list *apps)
{
    const char *path = xdg_cache_dir();
    if (path == NULL) {
        LOG_WARN("failed to get cache directory: not saving popularity cache");
        return;
    }

    int cache_dir_fd = open(path, O_DIRECTORY);
    if (cache_dir_fd == -1) {
        LOG_ERRNO("%s: failed to open", path);
        return;
    }

    struct stat st;
    int fd = -1;

    if (fstatat(cache_dir_fd, "fuzzel", &st, 0) == -1 ||
        (fd = openat(cache_dir_fd, "fuzzel", O_RDONLY)) == -1)
    {
        close(cache_dir_fd);
        LOG_ERRNO("%s/fuzzel: failed to open", path);
        return;
    }
    close(cache_dir_fd);

    char *text = mmap(
        NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);

    if (text == MAP_FAILED) {
        LOG_ERRNO("%s/fuzzel: failed to mmap", path);
        return;
    }

    size_t app_idx = 0;

    /* Loop lines */
    char *lineptr = NULL;
    for (char *line = strtok_r(text, "\n", &lineptr);
         line != NULL;
         line = strtok_r(NULL, "\n", &lineptr))
    {
        /* Parse each line ("<title>|<count>") */
        char *ptr = NULL;
        const char *title = strtok_r(line, "|", &ptr);
        const char *count_str = strtok_r(NULL, "|", &ptr);

        int count;
        sscanf(count_str, "%u", &count);

        size_t wlen = mbstowcs(NULL, title, 0);
        wchar_t wtitle[wlen + 1];
        mbstowcs(wtitle, title, wlen + 1);

        for (; app_idx < apps->count; app_idx++) {
            int cmp = wcscmp(apps->v[app_idx].title, wtitle);

            if (cmp == 0) {
                apps->v[app_idx].count = count;
                app_idx++;
                break;
            } else if (cmp > 0)
                break;
        }
    }

    munmap(text, st.st_size);
}

static void
write_cache(const struct application_list *apps)
{
    const char *path = xdg_cache_dir();
    if (path == NULL) {
        LOG_WARN("failed to get cache directory: not saving popularity cache");
        return;
    }

    int cache_dir_fd = open(path, O_DIRECTORY);
    if (cache_dir_fd == -1) {
        LOG_ERRNO("%s: failed to open", path);
        return;
    }

    int fd = openat(cache_dir_fd, "fuzzel", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    close(cache_dir_fd);

    if (fd == -1) {
        LOG_ERRNO("%s/fuzzel: failed to open", path);
        return;
    }

    for (size_t i = 0; i < apps->count; i++) {
        if (apps->v[i].count == 0)
            continue;

        char count_as_str[11];
        sprintf(count_as_str, "%u", apps->v[i].count);
        const size_t count_len = strlen(count_as_str);

        size_t clen = wcstombs(NULL, apps->v[i].title, 0);
        char ctitle[clen + 1];
        wcstombs(ctitle, apps->v[i].title, clen + 1);


        if (write(fd, ctitle, clen) != clen ||
            write(fd, "|", 1) != 1 ||
            write(fd, count_as_str, count_len) != count_len ||
            write(fd, "\n", 1) != 1)
        {
            LOG_ERRNO("failed to write cache");
            break;
        }
    }

    close(fd);
}

static void
print_usage(const char *prog_name)
{
    printf("Usage: %s [OPTION]...\n", prog_name);
    printf("\n");
    printf("Options:\n");
    printf("  -o,--output=OUTPUT         output (monitor) to display on (none)\n"
           "  -f,--font=FONT             font name and style in fontconfig format (monospace)\n"
           "  -i,--icon-theme=NAME       icon theme name (\"hicolor\")\n"
           "  -g,--geometry=WxH          window WIDTHxHEIGHT, in pixels (500x300)\n"
           "  -b,--background-color=HEX  background color (000000ff)\n"
           "  -t,--text-color=HEX        text color (ffffffff)\n"
           "  -m,--match-color=HEX       color of matched substring (cc9393ff)\n"
           "  -s,--selection-color=HEX   background color of selected item (333333ff)\n"
           "  -B,--border-width=INT      width of border, in pixels (1)\n"
           "  -r,--border-radius=INT     amount of corner \"roundness\" (10)\n"
           "  -C,--border-color=HEX      border color (ffffffff)\n"
           "  -d,--dmenu                 dmenu compatibility mode\n"
           "  -v,--version               show the version number and quit\n");
    printf("\n");
    printf("Colors must be specified as a 32-bit hexadecimal RGBA quadruple.\n");
}

int
main(int argc, char *const *argv)
{
    static const struct option longopts[] = {
        {"output"  ,         required_argument, 0, 'o'},
        {"font",             required_argument, 0, 'f'},
        {"icon-theme",       required_argument, 0, 'i'},
        {"geometry",         required_argument, 0, 'g'},
        {"background-color", required_argument, 0, 'b'},
        {"text-color",       required_argument, 0, 't'},
        {"match-color",      required_argument, 0, 'm'},
        {"selection-color",  required_argument, 0, 's'},
        {"border-width",     required_argument, 0, 'B'},
        {"border-radius",    required_argument, 0, 'r'},
        {"border-color",     required_argument, 0, 'C'},
        {"dmenu",            no_argument,       0, 'd'},
        {"version",          no_argument,       0, 'v'},
        {"help",             no_argument,       0, 'h'},
        {NULL,               no_argument,       0, 0},
    };

    const char *output_name = NULL;
    const char *font_name = "monospace";
    const char *icon_theme = "hicolor";

    int width = 500;
    int height = 300;
    const int x_margin = 20;
    const int y_margin = 4;
    int border_width = 1;
    int border_radius = 10;
    uint32_t text_color = 0xffffffff;
    uint32_t match_color = 0xcc9393ff;
    uint32_t background = 0x000000ff;
    uint32_t selection_color = 0x333333ff;
    uint32_t border_color = 0xffffffff;
    bool dmenu_mode = false;

    while (true) {
        int c = getopt_long(argc, argv, ":o:f:i:g:b:t:m:s:B:r:C:dvh", longopts, NULL);
        if (c == -1)
            break;

        switch (c) {
        case 'o':
            output_name = optarg;
            break;

        case 'f':
            font_name = optarg;
            break;

        case 'i':
            icon_theme = optarg;
            break;

        case 'g':
            if (sscanf(optarg, "%dx%d", &width, &height) != 2) {
                LOG_ERR("%s: invalid geometry (must be <width>x<height>)", optarg);
                return EXIT_FAILURE;
            }
            break;

        case 'b':
            if (sscanf(optarg, "%08x", &background) != 1) {
                LOG_ERR("%s: invalid color", optarg);
                return EXIT_FAILURE;
            }
            break;

        case 't':
            if (sscanf(optarg, "%08x", &text_color) != 1) {
                LOG_ERR("%s: invalid color", optarg);
                return EXIT_FAILURE;
            }
            break;

        case 'm':
            if (sscanf(optarg, "%x", &match_color) != 1) {
                LOG_ERR("%s: invalid color", optarg);
                return EXIT_FAILURE;
            }
            break;

        case 's':
            if (sscanf(optarg, "%x", &selection_color) != 1) {
                LOG_ERR("%s: invalid color", optarg);
                return EXIT_FAILURE;
            }
            break;

        case 'B':
            if (sscanf(optarg, "%d", &border_width) != 1) {
                LOG_ERR(
                    "%s: invalid border width (must be an integer)", optarg);
                return EXIT_FAILURE;
            }
            break;

        case 'r':
            if (sscanf(optarg, "%d", &border_radius) != 1) {
                LOG_ERR("%s: invalid border radius (must be an integer)", optarg);
                return EXIT_FAILURE;
            }
            break;

        case 'C':
            if (sscanf(optarg, "%x", &border_color) != 1) {
                LOG_ERR("%s: invalid color", optarg);
                return EXIT_FAILURE;
            }
            break;

        case 'd':
            dmenu_mode = true;
            break;

        case 'v':
            printf("fuzzel version %s\n", FUZZEL_VERSION);
            return EXIT_SUCCESS;

        case 'h':
            print_usage(argv[0]);
            return EXIT_SUCCESS;

        case ':':
            fprintf(stderr, "error: -%c: missing required argument\n", optopt);
            return EXIT_FAILURE;

        case '?':
            fprintf(stderr, "error: -%c: invalid option\n", optopt);
            return EXIT_FAILURE;
        }
    }

    int ret = EXIT_FAILURE;

    setlocale(LC_ALL, "");

    struct application_list applications = {0};
    struct fdm *fdm = NULL;
    struct prompt *prompt = NULL;
    struct matches *matches = NULL;
    struct render *render = NULL;
    struct wayland *wayl = NULL;

    struct font *font = font_from_name(font_name);
    if (font == NULL)
        goto out;

    LOG_DBG("height: %d, ascent: %d, descent: %d",
            font->fextents.height, font->fextents.ascent, font->fextents.descent);

    const double line_height = 2 * y_margin + font->fextents.height;
    size_t max_matches = (height - 2 * border_width - line_height) / line_height;
    LOG_DBG("max matches: %d", max_matches);

    struct options options = {
        .width = width,
        .height = height,
        .x_margin = x_margin,
        .y_margin = y_margin,
        .border_size = border_width,
        .border_radius = border_radius,
        .background_color = hex_to_rgba(background),
        .border_color = hex_to_rgba(border_color),
        .text_color = hex_to_rgba(text_color),
        .match_color = hex_to_rgba(match_color),
        .selection_color = hex_to_rgba(selection_color),
    };
    render = render_init(font, options);

    if (dmenu_mode)
        dmenu_load_entries(&applications);
    else
        xdg_find_programs(icon_theme, font->fextents.height, &applications);
    read_cache(&applications);

    if ((fdm = fdm_init()) == NULL)
        goto out;

    if ((prompt = prompt_init(L"> ")) == NULL)
        goto out;

    if ((matches = matches_init(&applications, max_matches)) == NULL)
        goto out;

    matches_update(matches, prompt);

    if ((wayl = wayl_init(fdm, render, prompt, matches, width, height, output_name, dmenu_mode)) == NULL)
        goto out;

    while (true) {
        wayl_flush(wayl);
        if (!fdm_poll(fdm))
            break;
    }

    if (wayl_update_cache(wayl))
        write_cache(&applications);

    ret = wayl_exit_code(wayl);

out:
    shm_fini();

    wayl_destroy(wayl);
    render_destroy(render);
    matches_destroy(matches);
    prompt_destroy(prompt);
    fdm_destroy(fdm);

    for (size_t i = 0; i < applications.count; i++) {
        struct application *app = &applications.v[i];

        free(app->path);
        free(app->exec);
        free(app->title);
        free(app->comment);
        if (app->icon.type == ICON_SURFACE)
            cairo_surface_destroy(app->icon.surface);
        else if (app->icon.type == ICON_SVG)
            g_object_unref(app->icon.svg);
    }
    free(applications.v);

    cairo_debug_reset_static_data();

    return ret;
}
