#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>

#include <locale.h>
#include <threads.h>
#include <poll.h>

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
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

#define LOG_MODULE "f00sel"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "tllist.h"
#include "shm.h"
#include "render.h"
#include "application.h"
#include "match.h"
#include "xdg.h"
#include "font.h"

struct monitor {
    struct wl_output *output;
    struct zxdg_output_v1 *xdg;
    char *name;

    int x;
    int y;

    int width_mm;
    int height_mm;

    int width_px;
    int height_px;

    int scale;
};

struct wayland {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_surface *surface;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct zxdg_output_manager_v1 *xdg_output_manager;

    tll(struct monitor) monitors;
    const struct monitor *monitor;

    struct xkb_context *xkb;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
};

struct context {
    volatile enum { KEEP_RUNNING, EXIT_UPDATE_CACHE, EXIT} status;
    struct wayland wl;
    struct render *render;

    struct prompt prompt;
    struct application_list applications;

    struct match *matches;
    size_t match_count;
    size_t selected;

    struct {
        mtx_t mutex;
        cnd_t cond;
        int trigger;
        int pipe_read_fd;
        int pipe_write_fd;
        enum {REPEAT_STOP, REPEAT_START, REPEAT_EXIT} cmd;

        bool dont_re_repeat;
        int32_t delay;
        int32_t rate;
        uint32_t key;
    } repeat;

    bool frame_is_scheduled;
    struct buffer *pending;

    /* Window configuration */
    int width;
    int height;
};

static int max_matches;

static void refresh(struct context *c);
static void update_matches(struct context *c);

static void
shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
}

static const struct wl_shm_listener shm_listener = {
    .format = &shm_format,
};

static int
keyboard_repeater(void *arg)
{
    struct context *c = arg;

    while (true) {
        LOG_DBG("repeater: waiting for start");

        mtx_lock(&c->repeat.mutex);
        while (c->repeat.cmd == REPEAT_STOP)
            cnd_wait(&c->repeat.cond, &c->repeat.mutex);

        if (c->repeat.cmd == REPEAT_EXIT) {
            mtx_unlock(&c->repeat.mutex);
            return 0;
        }

    restart:

        LOG_DBG("repeater: started");
        assert(c->repeat.cmd == REPEAT_START);

        const long rate_delay = 1000000000 / c->repeat.rate;
        long delay = c->repeat.delay * 1000000;

        while (true) {
            assert(c->repeat.rate > 0);

            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);

            timeout.tv_nsec += delay;
            if (timeout.tv_nsec >= 1000000000) {
                timeout.tv_sec += timeout.tv_nsec / 1000000000;
                timeout.tv_nsec %= 1000000000;
            }

            int ret = cnd_timedwait(&c->repeat.cond, &c->repeat.mutex, &timeout);
            if (ret == thrd_success) {
                if (c->repeat.cmd == REPEAT_START)
                    goto restart;
                else if (c->repeat.cmd == REPEAT_STOP) {
                    mtx_unlock(&c->repeat.mutex);
                    break;
                } else if (c->repeat.cmd == REPEAT_EXIT) {
                    mtx_unlock(&c->repeat.mutex);
                    return 0;
                }
            }

            assert(ret == thrd_timedout);
            assert(c->repeat.cmd == REPEAT_START);
            LOG_DBG("repeater: repeat: %u", c->repeat.key);
            write(c->repeat.pipe_write_fd, &c->repeat.key, sizeof(c->repeat.key));

            delay = rate_delay;
        }

    }

    assert(false);
    return 1;
}

static void
keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
                uint32_t format, int32_t fd, uint32_t size)
{
    struct context *c = data;

    char *map_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

    c->wl.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    c->wl.xkb_keymap = xkb_keymap_new_from_string(
        c->wl.xkb, map_str, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);

    /* TODO: initialize in enter? */
    c->wl.xkb_state = xkb_state_new(c->wl.xkb_keymap);

    munmap(map_str, size);
    close(fd);
}

static void
keyboard_enter(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
               struct wl_surface *surface, struct wl_array *keys)
{
    LOG_DBG("enter");
#if 0
    uint32_t *key;
    wl_array_for_each(key, keys)
        xkb_state_update_key(xkb_state, *key, 1);
#endif
}

static void
keyboard_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
               struct wl_surface *surface)
{
    struct context *c = data;
    c->status = EXIT;
}

static size_t
prompt_next_char(const struct prompt *prompt)
{
    int clen = mblen(&prompt->text[prompt->cursor], MB_CUR_MAX);
    if (clen < 0) {
        LOG_ERRNO("prompt: %s", prompt->text);
        return prompt->cursor;
    }

    return prompt->cursor + clen;
}

static size_t
prompt_prev_char(const struct prompt *prompt)
{
    if (prompt->cursor == 0)
        return 0;

    size_t cursor = prompt->cursor;
    while (cursor > 0) {
        int clen = mblen(&prompt->text[--cursor], MB_CUR_MAX);
        if (clen >= 0)
            break;
    }

    return cursor;
}

static size_t
prompt_prev_word(const struct prompt *prompt)
{
    size_t prev_char = prompt_prev_char(prompt);
    const char *space = &prompt->text[prev_char];

    /* Ignore initial spaces */
    while (space >= prompt->text && *space == ' ')
        space--;

    while (space >= prompt->text && *space != ' ')
        space--;

    return space - prompt->text + 1;
}

static size_t
prompt_next_word(const struct prompt *prompt)
{
    const char *space = strchr(&prompt->text[prompt->cursor], ' ');
    if (space == NULL)
        return strlen(prompt->text);
    else
        return space - prompt->text + 1;
}

static bool
push_argv(char ***argv, size_t *size, char *arg, size_t *argc)
{
    if (arg != NULL && arg[0] == '%')
        return true;

    if (*argc >= *size) {
        size_t new_size = *size > 0 ? 2 * *size : 10;
        char **new_argv = realloc(*argv, new_size * sizeof(new_argv[0]));

        if (new_argv == NULL)
            return false;

        *argv = new_argv;
        *size = new_size;
    }

    (*argv)[(*argc)++] = arg;
    return true;
}

static bool
tokenize_cmdline(char *cmdline, char ***argv)
{
    *argv = NULL;
    size_t argv_size = 0;

    bool first_token_is_quoted = cmdline[0] == '"' || cmdline[0] == '\'';
    char delim = first_token_is_quoted ? cmdline[0] : ' ';

    char *p = first_token_is_quoted ? &cmdline[1] : &cmdline[0];

    size_t idx = 0;
    while (*p != '\0') {
        char *end = strchr(p, delim);
        if (end == NULL) {
            if (delim != ' ') {
                LOG_ERR("unterminated %s quote\n", delim == '"' ? "double" : "single");
                free(*argv);
                return false;
            }

            if (!push_argv(argv, &argv_size, p, &idx) ||
                !push_argv(argv, &argv_size, NULL, &idx))
            {
                goto err;
            } else
                return true;
        }

        *end = '\0';

        if (!push_argv(argv, &argv_size, p, &idx))
            goto err;

        p = end + 1;
        while (*p == delim)
            p++;

        while (*p == ' ')
            p++;

        if (*p == '"' || *p == '\'') {
            delim = *p;
            p++;
        } else
            delim = ' ';
    }

    if (!push_argv(argv, &argv_size, NULL, &idx))
        goto err;

    return true;

err:
    free(*argv);
    return false;
}

static void
keyboard_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
             uint32_t time, uint32_t key, uint32_t state)
{
    static bool mod_masks_initialized = false;
    static xkb_mod_mask_t ctrl = -1;
    static xkb_mod_mask_t alt = -1;
    //static xkb_mod_mask_t shift = -1;

    struct context *c = data;

    if (!mod_masks_initialized) {
        mod_masks_initialized = true;
        ctrl = 1 << xkb_keymap_mod_get_index(c->wl.xkb_keymap, "Control");
        alt = 1 << xkb_keymap_mod_get_index(c->wl.xkb_keymap, "Mod1");
        //shift = 1 << xkb_keymap_mod_get_index(c->wl.xkb_keymap, "Shift");
    }

    if (state == XKB_KEY_UP) {
        mtx_lock(&c->repeat.mutex);
        if (c->repeat.key == key) {
            if (c->repeat.cmd != REPEAT_EXIT) {
                c->repeat.cmd = REPEAT_STOP;
                cnd_signal(&c->repeat.cond);
            }
        }
        mtx_unlock(&c->repeat.mutex);
        return;
    }

    key += 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(c->wl.xkb_state, key);

    xkb_mod_mask_t mods = xkb_state_serialize_mods(
        c->wl.xkb_state, XKB_STATE_MODS_EFFECTIVE);
    xkb_mod_mask_t consumed = xkb_state_key_get_consumed_mods(c->wl.xkb_state, key);
    xkb_mod_mask_t significant = ctrl | alt /*| shift*/;
    xkb_mod_mask_t effective_mods = mods & ~consumed & significant;

#if 0
    for (size_t i = 0; i < 32; i++) {
        if (mods & (1 << i)) {
            LOG_DBG("%s", xkb_keymap_mod_get_name(c->wl.xkb_keymap, i));
        }
    }
#endif

    LOG_DBG("mod=0x%08x, consumed=0x%08x, significant=0x%08x, effective=0x%08x",
            mods, consumed, significant, effective_mods);

    if (sym == XKB_KEY_Home || (sym == XKB_KEY_a && effective_mods == ctrl)) {
        c->prompt.cursor = 0;
        refresh(c);
    }

    else if (sym == XKB_KEY_End || (sym == XKB_KEY_e && effective_mods == ctrl)) {
        c->prompt.cursor = strlen(c->prompt.text);
        refresh(c);
    }

    else if ((sym == XKB_KEY_b && effective_mods == alt) ||
             (sym == XKB_KEY_Left && effective_mods == ctrl)) {
        c->prompt.cursor = prompt_prev_word(&c->prompt);
        refresh(c);
    }

    else if ((sym == XKB_KEY_f && effective_mods == alt) ||
             (sym == XKB_KEY_Right && effective_mods == ctrl)) {

        c->prompt.cursor = prompt_next_word(&c->prompt);
        refresh(c);
    }

    else if ((sym == XKB_KEY_Escape && effective_mods == 0) ||
             (sym == XKB_KEY_g && effective_mods == ctrl)) {
        c->status = EXIT;
    }

    else if ((sym == XKB_KEY_p && effective_mods == ctrl) ||
             (sym == XKB_KEY_Up && effective_mods == 0)) {
        if (c->selected > 0) {
            c->selected--;
            refresh(c);
        }
    }

    else if ((sym == XKB_KEY_n && effective_mods == ctrl) ||
             (sym == XKB_KEY_Down && effective_mods == 0)) {
        if (c->selected + 1 < c->match_count) {
            c->selected++;
            refresh(c);
        }
    }

    else if (sym == XKB_KEY_Tab && effective_mods == 0) {
        if (c->selected + 1 < c->match_count) {
            c->selected++;
            refresh(c);
        } else if (sym == XKB_KEY_Tab) {
            /* Tab cycles */
            c->selected = 0;
            refresh(c);
        }
    }

    else if ((sym == XKB_KEY_b && effective_mods == ctrl) ||
             (sym == XKB_KEY_Left && effective_mods == 0)) {
        size_t new_cursor = prompt_prev_char(&c->prompt);
        if (new_cursor != c->prompt.cursor) {
            c->prompt.cursor = new_cursor;
            refresh(c);
        }
    }

    else if ((sym == XKB_KEY_f && effective_mods == ctrl) ||
             (sym == XKB_KEY_Right && effective_mods == 0)) {
        size_t new_cursor = prompt_next_char(&c->prompt);
        if (new_cursor != c->prompt.cursor) {
            c->prompt.cursor = new_cursor;
            refresh(c);
        }
    }

    else if ((sym == XKB_KEY_d && effective_mods == ctrl) ||
             (sym == XKB_KEY_Delete && effective_mods == 0)) {
        if (c->prompt.cursor < strlen(c->prompt.text)) {
            size_t next_char = prompt_next_char(&c->prompt);
            memmove(&c->prompt.text[c->prompt.cursor],
                    &c->prompt.text[next_char],
                    strlen(c->prompt.text) - next_char + 1);
            update_matches(c);
            refresh(c);
        }
    }

    else if (sym == XKB_KEY_BackSpace && effective_mods == 0) {
        if (c->prompt.cursor > 0) {
            size_t prev_char = prompt_prev_char(&c->prompt);
            c->prompt.text[prev_char] = '\0';
            c->prompt.cursor = prev_char;

            update_matches(c);
            refresh(c);
        }
    }

    else if (sym == XKB_KEY_BackSpace && (effective_mods == ctrl ||
                                          effective_mods == alt)) {
        size_t new_cursor = prompt_prev_word(&c->prompt);
        memmove(&c->prompt.text[new_cursor],
                &c->prompt.text[c->prompt.cursor],
                strlen(c->prompt.text) - c->prompt.cursor + 1);
        c->prompt.cursor = new_cursor;
        update_matches(c);
        refresh(c);
    }

    else if ((sym == XKB_KEY_d && effective_mods == alt) ||
             (sym == XKB_KEY_Delete && effective_mods == ctrl)) {
        size_t next_word = prompt_next_word(&c->prompt);
        memmove(&c->prompt.text[c->prompt.cursor],
                &c->prompt.text[next_word],
                strlen(c->prompt.text) - next_word + 1);
        update_matches(c);
        refresh(c);
    }

    else if (sym == XKB_KEY_k && effective_mods == ctrl) {
        c->prompt.text[c->prompt.cursor] = '\0';
        update_matches(c);
        refresh(c);
    }

    else if (sym == XKB_KEY_Return && effective_mods == 0) {
        c->status = EXIT;

        struct application *match = c->match_count > 0
            ? c->matches[c->selected].application
            : NULL;
        const char *execute = match != NULL ? match->exec : c->prompt.text;
        const char *path = match != NULL ? match->path : NULL;

        LOG_DBG("exec(%s)", execute);

        if (strchr(execute, '\\') != NULL) {
            LOG_ERR("unimplemented: escaped exec arguments: %s", execute);
            return;
        }

        /* Tokenize the command */
        char *copy = strdup(execute);
        char **argv;
        if (!tokenize_cmdline(copy, &argv)) {
            free(copy);
            return;
        }

        LOG_DBG("argv:");
        for (size_t i = 0; argv[i] != NULL; i++)
            LOG_DBG("  %zu: \"%s\"", i, argv[i]);

        int pipe_fds[2];
        if (pipe2(pipe_fds, O_CLOEXEC) == -1) {
            LOG_ERRNO("failed to create pipe");
            return;
        }

        pid_t pid = fork();
        if (pid == -1) {
            close(pipe_fds[0]);
            close(pipe_fds[1]);

            LOG_ERRNO("failed to fork");
            return;
        }

        if (pid == 0) {
            /* Child */

            /* Close read end */
            close(pipe_fds[0]);

            if (path != NULL) {
                if (chdir(path) == -1)
                    LOG_ERRNO("failed to chdir to %s", path);
            }

            /* Redirect stdin/stdout/stderr -> /dev/null */
            int devnull_r = open("/dev/null", O_RDONLY | O_CLOEXEC);
            int devnull_w = open("/dev/null", O_WRONLY | O_CLOEXEC);

            if (devnull_r == -1 || devnull_w == -1)
                goto child_err;

            if (dup2(devnull_r, STDIN_FILENO) == -1 ||
                dup2(devnull_w, STDOUT_FILENO) == -1 ||
                dup2(devnull_w, STDERR_FILENO) == -1)
            {
                goto child_err;
            }

            execvp(argv[0], argv);

        child_err:
            /* Signal error back to parent process */
            write(pipe_fds[1], &errno, sizeof(errno));
            _exit(1);
        } else {
            /* Parent */

            free(copy);
            free(argv);

            /* Close write end */
            close(pipe_fds[1]);

            int _errno;
            static_assert(sizeof(_errno) == sizeof(errno), "errno size mismatch");

            ssize_t ret = read(pipe_fds[0], &_errno, sizeof(_errno));
            if (ret == -1)
                LOG_ERRNO("failed to read from pipe");
            else if (ret == sizeof(_errno))
                LOG_ERRNO_P("%s: failed to execute", _errno, execute);
            else {
                LOG_DBG("%s: fork+exec succeeded", execute);
                if (match != NULL) {
                    c->status = EXIT_UPDATE_CACHE;
                    match->count++;
                }
            }

            close(pipe_fds[0]);
        }
    }

    else if (effective_mods == 0) {
        char buf[128] = {0};
        int count = xkb_state_key_get_utf8(c->wl.xkb_state, key, buf, sizeof(buf));

        if (count == 0)
            return;

        const size_t new_len = strlen(c->prompt.text) + count + 1;
        char *new_text = realloc(c->prompt.text, new_len);
        if (new_text == NULL)
            return;

        memmove(&new_text[c->prompt.cursor + count],
                &new_text[c->prompt.cursor],
                strlen(new_text) - c->prompt.cursor + 1);
        memcpy(&new_text[c->prompt.cursor], buf, count);

        c->prompt.text = new_text;
        c->prompt.cursor += count;

        LOG_DBG("prompt: \"%s\" (cursor=%zu, length=%zu)",
                c->prompt.text, c->prompt.cursor, new_len);

        update_matches(c);
        refresh(c);
    }

    mtx_lock(&c->repeat.mutex);
    if (!c->repeat.dont_re_repeat) {
        if (c->repeat.cmd != REPEAT_EXIT) {
            c->repeat.cmd = REPEAT_START;
            c->repeat.key = key - 8;
            cnd_signal(&c->repeat.cond);
        }
    }
    mtx_unlock(&c->repeat.mutex);

}

static void
keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial,
                   uint32_t mods_depressed, uint32_t mods_latched,
                   uint32_t mods_locked, uint32_t group)
{
    struct context *c = data;

    LOG_DBG("modifiers: depressed=0x%x, latched=0x%x, locked=0x%x, group=%u",
            mods_depressed, mods_latched, mods_locked, group);

    xkb_state_update_mask(
        c->wl.xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
}

static void
keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
                     int32_t rate, int32_t delay)
{
    struct context *c = data;
    LOG_DBG("keyboard repeat: rate=%d, delay=%d", rate, delay);
    c->repeat.rate = rate;
    c->repeat.delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = &keyboard_keymap,
    .enter = &keyboard_enter,
    .leave = &keyboard_leave,
    .key = &keyboard_key,
    .modifiers = &keyboard_modifiers,
    .repeat_info = &keyboard_repeat_info,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
                         enum wl_seat_capability caps)
{
    struct context *c = data;

    if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD))
        return;

    if (c->wl.keyboard != NULL)
        wl_keyboard_release(c->wl.keyboard);

    c->wl.keyboard = wl_seat_get_keyboard(wl_seat);
    wl_keyboard_add_listener(c->wl.keyboard, &keyboard_listener, c);
}

static void
seat_handle_name(void *data, struct wl_seat *wl_seat, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_handle_capabilities,
    .name = seat_handle_name,
};

static void
output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
                int32_t physical_width, int32_t physical_height,
                int32_t subpixel, const char *make, const char *model,
                int32_t transform)
{
    struct monitor *mon = data;
    mon->width_mm = physical_width;
    mon->height_mm = physical_height;
}

static void
output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
            int32_t width, int32_t height, int32_t refresh)
{
}

static void
output_done(void *data, struct wl_output *wl_output)
{
}

static void
output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
    struct monitor *mon = data;
    mon->scale = factor;
}

static const struct wl_output_listener output_listener = {
    .geometry = &output_geometry,
    .mode = &output_mode,
    .done = &output_done,
    .scale = &output_scale,
};

static void
xdg_output_handle_logical_position(
    void *data, struct zxdg_output_v1 *xdg_output, int32_t x, int32_t y)
{
    struct monitor *mon = data;
    mon->x = x;
    mon->y = y;
}

static void
xdg_output_handle_logical_size(void *data, struct zxdg_output_v1 *xdg_output,
                               int32_t width, int32_t height)
{
    struct monitor *mon = data;
    mon->width_px = width;
    mon->height_px = height;
}

static void
xdg_output_handle_done(void *data, struct zxdg_output_v1 *xdg_output)
{
}

static void
xdg_output_handle_name(void *data, struct zxdg_output_v1 *xdg_output,
                       const char *name)
{
    struct monitor *mon = data;
    mon->name = strdup(name);
}

static void
xdg_output_handle_description(void *data, struct zxdg_output_v1 *xdg_output,
                              const char *description)
{
}

static struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = xdg_output_handle_logical_position,
    .logical_size = xdg_output_handle_logical_size,
    .done = xdg_output_handle_done,
    .name = xdg_output_handle_name,
    .description = xdg_output_handle_description,
};

static void
handle_global(void *data, struct wl_registry *registry,
              uint32_t name, const char *interface, uint32_t version)
{
    struct context *c = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        c->wl.compositor = wl_registry_bind(
            c->wl.registry, name, &wl_compositor_interface, 4);
    }

    else if (strcmp(interface, wl_shm_interface.name) == 0) {
        c->wl.shm = wl_registry_bind(c->wl.registry, name, &wl_shm_interface, 1);
        wl_shm_add_listener(c->wl.shm, &shm_listener, &c->wl);
        wl_display_roundtrip(c->wl.display);
    }

    else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct wl_output *output = wl_registry_bind(
            c->wl.registry, name, &wl_output_interface, 3);

        tll_push_back(c->wl.monitors, ((struct monitor){.output = output}));

        struct monitor *mon = &tll_back(c->wl.monitors);
        wl_output_add_listener(output, &output_listener, mon);

        /*
         * The "output" interface doesn't give us the monitors'
         * identifiers (e.g. "LVDS-1"). Use the XDG output interface
         * for that.
         */

        assert(c->wl.xdg_output_manager != NULL);
        mon->xdg = zxdg_output_manager_v1_get_xdg_output(
            c->wl.xdg_output_manager, mon->output);

        zxdg_output_v1_add_listener(mon->xdg, &xdg_output_listener, mon);
        wl_display_roundtrip(c->wl.display);
    }

    else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        c->wl.layer_shell = wl_registry_bind(
            c->wl.registry, name, &zwlr_layer_shell_v1_interface, 1);
    }

    else if (strcmp(interface, wl_seat_interface.name) == 0) {
        c->wl.seat = wl_registry_bind(c->wl.registry, name, &wl_seat_interface, 4);
        wl_seat_add_listener(c->wl.seat, &seat_listener, c);
        wl_display_roundtrip(c->wl.display);
    }

    else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        c->wl.xdg_output_manager = wl_registry_bind(
            c->wl.registry, name, &zxdg_output_manager_v1_interface, 2);
    }
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    LOG_WARN("global removed: %u", name);
}

static const struct wl_registry_listener registry_listener = {
    .global = &handle_global,
    .global_remove = &handle_global_remove,
};

static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                        uint32_t serial, uint32_t w, uint32_t h)
{
    zwlr_layer_surface_v1_ack_configure(surface, serial);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
};

static int
sort_match_by_count(const void *_a, const void *_b)
{
    const struct match *a = _a;
    const struct match *b = _b;
    return b->application->count - a->application->count;
}

static void
update_matches(struct context *c)
{
    /* Nothing entered; all programs found matches */
    if (strlen(c->prompt.text) == 0) {

        for (size_t i = 0; i < c->applications.count; i++) {
            c->matches[i] = (struct match){
                .application = &c->applications.v[i],
                .start_title = -1,
                .start_comment = -1};
        }

        /* Sort */
        c->match_count = c->applications.count;
        qsort(c->matches, c->match_count, sizeof(c->matches[0]), &sort_match_by_count);

        /* Limit count (don't render outside window) */
        if (c->match_count > max_matches)
            c->match_count = max_matches;

        if (c->selected >= c->match_count && c->selected > 0)
            c->selected = c->match_count - 1;
        return;
    }

    c->match_count = 0;
    for (size_t i = 0; i < c->applications.count; i++) {
        struct application *app = &c->applications.v[i];
        size_t start_title = -1;
        size_t start_comment = -1;

        const char *m = strcasestr(app->title, c->prompt.text);
        if (m != NULL)
            start_title = m - app->title;

        if (app->comment != NULL) {
            m = strcasestr(app->comment, c->prompt.text);
            if (m != NULL)
                start_comment = m - app->comment;
        }

        if (start_title == -1 && start_comment == -1)
            continue;

        c->matches[c->match_count++] = (struct match){
            .application = app,
            .start_title = start_title,
            .start_comment = start_comment};
    }

    /* Sort */
    qsort(c->matches, c->match_count, sizeof(c->matches[0]), &sort_match_by_count);

    /* Limit count (don't render outside window) */
    if (c->match_count > max_matches)
        c->match_count = max_matches;

    if (c->selected >= c->match_count && c->selected > 0)
        c->selected = c->match_count - 1;
}

static void frame_callback(
    void *data, struct wl_callback *wl_callback, uint32_t callback_data);

static const struct wl_callback_listener frame_listener = {
    .done = &frame_callback,
};

static void
commit_buffer(struct context *c, struct buffer *buf)
{
    assert(buf->busy);

    wl_surface_set_buffer_scale(c->wl.surface, c->wl.monitor->scale);
    wl_surface_attach(c->wl.surface, buf->wl_buf, 0, 0);
    wl_surface_damage(c->wl.surface, 0, 0, buf->width, buf->height);

    struct wl_callback *cb = wl_surface_frame(c->wl.surface);
    wl_callback_add_listener(cb, &frame_listener, c);

    c->frame_is_scheduled = true;
    wl_surface_commit(c->wl.surface);
}

static void
frame_callback(void *data, struct wl_callback *wl_callback, uint32_t callback_data)
{
    struct context *c = data;

    c->frame_is_scheduled = false;
    wl_callback_destroy(wl_callback);

    if (c->pending != NULL) {
        commit_buffer(c, c->pending);
        c->pending = NULL;
    }
}

static void
refresh(struct context *c)
{
    LOG_DBG("refresh");

    struct buffer *buf = shm_get_buffer(c->wl.shm, c->width, c->height);

    /* Background + border */
    render_background(c->render, buf);

    /* Window content */
    render_prompt(c->render, buf, &c->prompt);
    render_match_list(c->render, buf, c->matches, c->match_count,
                      strlen(c->prompt.text), c->selected);

    cairo_surface_flush(buf->cairo_surface);

    if (c->frame_is_scheduled) {
        /* There's already a frame being drawn - delay current frame
         * (overwriting any previous pending frame) */

        if (c->pending != NULL)
            c->pending->busy = false;

        c->pending = buf;
    } else {
        /* No pending frames - render immediately */
        assert(c->pending == NULL);
        commit_buffer(c, buf);
    }
}

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

    int fd = openat(cache_dir_fd, "f00sel", O_RDONLY);
    close(cache_dir_fd);

    if (fd == -1) {
        LOG_ERRNO("%s/f00sel: failed to open", path);
        return;
    }

    FILE *s = fdopen(fd, "r");
    //close(fd);
    assert(s != NULL);

    errno = 0;
    while (true) {
        char *line = NULL;
        size_t len = 0;
        ssize_t res = getline(&line, &len, s);
        if (res == -1) {
            free(line);
            if (errno != 0)
                LOG_ERRNO("%s/f00sel: failed to read", path);
            break;
        }

        LOG_INFO("%s", line);

        char *title = NULL;
        int count;
        if (sscanf(line, "%m[^|]|%u", &title, &count) != 2) {
            free(title);
            free(line);
            continue;
        }

        /* TODO: this is slow */
        for (size_t i = 0; i < apps->count; i++) {
            if (strcmp(apps->v[i].title, title) == 0) {
                apps->v[i].count = count;
                break;
            }
        }

        free(title);
        free(line);
    }

    fclose(s);
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

    int fd = openat(cache_dir_fd, "f00sel", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    close(cache_dir_fd);

    if (fd == -1) {
        LOG_ERRNO("%s/f00sel: failed to open", path);
        return;
    }

    for (size_t i = 0; i < apps->count; i++) {
        if (apps->v[i].count == 0)
            continue;

        char count_as_str[11];
        sprintf(count_as_str, "%u", apps->v[i].count);

        write(fd, apps->v[i].title, strlen(apps->v[i].title));
        write(fd, "|", 1);
        write(fd, count_as_str, strlen(count_as_str));
        write(fd, "\n", 1);
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
           "  -g,--geometry=WxH          window WIDTHxHEIGHT, in pixels (500x300)\n"
           "  -b,--background-color=HEX  background color (000000ff)\n"
           "  -t,--text-color=HEX        text color (ffffffff)\n"
           "  -m,--match-color=HEX       color of matched substring (cc9393ff)\n"
           "  -s,--selection-color=HEX   background color of selected item (333333ff)\n"
           "  -B,--border-width=INT      width of border, in pixels (1)\n"
           "  -r,--border-radius=INT     amount of corner \"roundness\" (10)\n"
           "  -C,--border-color=HEX      border color (ffffffff)\n");
    printf("\n");
    printf("Colors must be specified as a 32-bit hexadecimal RGBA quadruple.\n");
}

int
main(int argc, char *const *argv)
{
    static const struct option longopts[] = {
        {"output"  ,         required_argument, 0, 'o'},
        {"font",             required_argument, 0, 'f'},
        {"geometry",         required_argument, 0, 'g'},
        {"background-color", required_argument, 0, 'b'},
        {"text-color",       required_argument, 0, 't'},
        {"match-color",      required_argument, 0, 'm'},
        {"selection-color",  required_argument, 0, 's'},
        {"border-width",     required_argument, 0, 'B'},
        {"border-radius",    required_argument, 0, 'r'},
        {"border-color",     required_argument, 0, 'C'},
        {"help",             no_argument,       0, 'h'},
        {NULL,               no_argument,       0, 0},
    };

    const char *output_name = NULL;
    const char *font_name = "monospace";

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

    while (true) {
        int c = getopt_long(argc, argv, ":o:f:g:b:t:m:s:B:r:C:h", longopts, NULL);
        if (c == -1)
            break;

        switch (c) {
        case 'o':
            output_name = optarg;
            break;

        case 'f':
            font_name = optarg;
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

    int repeat_pipe_fds[2] = {-1, -1};
    if (pipe2(repeat_pipe_fds, O_CLOEXEC) == -1) {
        LOG_ERRNO("failed to create pipe for repeater thread");
        return ret;
    }

    struct context c = {
        .status = KEEP_RUNNING,
        .wl = {0},
        .prompt = {
            .text = calloc(1, 1),
            .cursor = 0
        },
        .repeat = {
            .pipe_read_fd = repeat_pipe_fds[0],
            .pipe_write_fd = repeat_pipe_fds[1],
            .cmd = REPEAT_STOP,
        },
    };

    mtx_init(&c.repeat.mutex, mtx_plain);
    cnd_init(&c.repeat.cond);

    thrd_t keyboard_repeater_id;
    thrd_create(&keyboard_repeater_id, &keyboard_repeater, &c);

    cairo_scaled_font_t *font = font_from_name(font_name);
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(surf);
    cairo_font_extents_t fextents;
    cairo_set_scaled_font(cr, font);
    cairo_font_extents(cr, &fextents);
    cairo_destroy(cr);
    cairo_surface_destroy(surf);

    LOG_DBG("height: %f, ascent: %f, descent: %f",
            fextents.height, fextents.ascent, fextents.descent);

    xdg_find_programs(fextents.height, &c.applications);
    c.matches = malloc(c.applications.count * sizeof(c.matches[0]));
    read_cache(&c.applications);

    c.wl.display = wl_display_connect(NULL);
    if (c.wl.display == NULL) {
        LOG_ERR("failed to connect to wayland; no compositor running?");
        goto out;
    }

    c.wl.registry = wl_display_get_registry(c.wl.display);
    if (c.wl.registry == NULL) {
        LOG_ERR("failed to get wayland registry");
        goto out;
    }

    wl_registry_add_listener(c.wl.registry, &registry_listener, &c);
    wl_display_roundtrip(c.wl.display);

    if (c.wl.compositor == NULL) {
        LOG_ERR("no compositor");
        goto out;
    }
    if (c.wl.layer_shell == NULL) {
        LOG_ERR("no layer shell interface");
        goto out;
    }
    if (c.wl.shm == NULL) {
        LOG_ERR("no shared memory buffers interface");
        goto out;
    }

    if (tll_length(c.wl.monitors) == 0) {
        LOG_ERR("no monitors");
        goto out;
    }

    tll_foreach(c.wl.monitors, it) {
        const struct monitor *mon = &it->item;
        LOG_INFO("monitor: %s: %dx%d+%d+%d (%dx%dmm)",
                 mon->name, mon->width_px, mon->height_px,
                 mon->x, mon->y, mon->width_mm, mon->height_mm);

        if (output_name != NULL && strcmp(output_name, mon->name) == 0) {
            c.wl.monitor = mon;
            break;
        } else if (output_name == NULL) {
            /* Use last monitor found */
            c.wl.monitor = mon;
        }
    }

    if (c.wl.monitor == NULL && output_name != NULL) {
        LOG_ERR("%s: no output with that name found", output_name);
        goto out;
    }

    if (c.wl.monitor == NULL) {
        LOG_ERR("no outputs found");
        goto out;
    }

    c.wl.surface = wl_compositor_create_surface(c.wl.compositor);
    if (c.wl.surface == NULL) {
        LOG_ERR("failed to create panel surface");
        goto out;
    }

#if 0
    c.wl.pointer.surface = wl_compositor_create_surface(c.wl.compositor);
    if (c.wl.pointer.surface == NULL) {
        LOG_ERR("failed to create cursor surface");
        goto out;
    }

    c.wl.pointer.theme = wl_cursor_theme_load(NULL, 24, c.wl.shm);
    if (c.wl.pointer.theme == NULL) {
        LOG_ERR("failed to load cursor theme");
        goto out;
    }
#endif

    c.wl.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        c.wl.layer_shell, c.wl.surface, c.wl.monitor->output,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP, "f00sel");

    if (c.wl.layer_surface == NULL) {
        LOG_ERR("failed to create layer shell surface");
        goto out;
    }

    const int scale = c.wl.monitor->scale;

    width /= scale; width *= scale;
    height /= scale; height *= scale;

    c.width = width;
    c.height = height;

    zwlr_layer_surface_v1_set_size(c.wl.layer_surface, width / scale, height / scale);
    zwlr_layer_surface_v1_set_keyboard_interactivity(c.wl.layer_surface, 1);

    zwlr_layer_surface_v1_add_listener(
        c.wl.layer_surface, &layer_surface_listener, &c.wl);

    /* Trigger a 'configure' event, after which we'll have the width */
    wl_surface_commit(c.wl.surface);
    wl_display_roundtrip(c.wl.display);

    const double line_height = 2 * y_margin + fextents.height;
    max_matches = (height - 2 * border_width - line_height) / line_height;
    LOG_DBG("max matches: %d", max_matches);

    struct options options = {
        .width = c.width,
        .height = c.height,
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
    c.render = render_init(font, options);

    update_matches(&c);
    refresh(&c);

    wl_display_dispatch_pending(c.wl.display);
    wl_display_flush(c.wl.display);

    while (c.status == KEEP_RUNNING) {
        struct pollfd fds[] = {
            {.fd = wl_display_get_fd(c.wl.display), .events = POLLIN},
            {.fd = c.repeat.pipe_read_fd, .events = POLLIN},
        };

        wl_display_flush(c.wl.display);
        poll(fds, 2, -1);

        if (fds[0].revents & POLLHUP) {
            LOG_WARN("disconnected from wayland");
            break;
        }

        if (fds[1].revents & POLLIN) {
            uint32_t key;
            read(c.repeat.pipe_read_fd, &key, sizeof(key));

            c.repeat.dont_re_repeat = true;
            keyboard_key(&c, NULL, 0, 0, key, XKB_KEY_DOWN);
            c.repeat.dont_re_repeat = false;
        }

        if (fds[0].revents & POLLIN)
            wl_display_dispatch(c.wl.display);
    }

    if (c.status == EXIT_UPDATE_CACHE)
        write_cache(&c.applications);

    ret = EXIT_SUCCESS;

out:
    /* Signal stop to repeater thread */
    mtx_lock(&c.repeat.mutex);
    c.repeat.cmd = REPEAT_EXIT;
    cnd_signal(&c.repeat.cond);
    mtx_unlock(&c.repeat.mutex);

    render_destroy(c.render);
    shm_fini();

    free(c.prompt.text);
    for (size_t i = 0; i < c.applications.count; i++) {
        struct application *app = &c.applications.v[i];

        free(app->path);
        free(app->exec);
        free(app->title);
        free(app->comment);
        if (app->icon.type == ICON_SURFACE)
            cairo_surface_destroy(app->icon.surface);
        else if (app->icon.type == ICON_SVG)
            g_object_unref(app->icon.svg);
    }
    free(c.applications.v);
    free(c.matches);

    tll_foreach(c.wl.monitors, it) {
        free(it->item.name);
        if (it->item.xdg)
            zxdg_output_v1_destroy(it->item.xdg);
        if (it->item.output != NULL)
            wl_output_destroy(it->item.output);
        tll_remove(c.wl.monitors, it);
    }

    if (c.wl.xdg_output_manager != NULL)
        zxdg_output_manager_v1_destroy(c.wl.xdg_output_manager);

    if (c.wl.layer_surface != NULL)
        zwlr_layer_surface_v1_destroy(c.wl.layer_surface);
    if (c.wl.layer_shell != NULL)
        zwlr_layer_shell_v1_destroy(c.wl.layer_shell);
    if (c.wl.keyboard != NULL)
        wl_keyboard_destroy(c.wl.keyboard);
    if (c.wl.surface != NULL)
        wl_surface_destroy(c.wl.surface);
    if (c.wl.seat != NULL)
        wl_seat_destroy(c.wl.seat);
    if (c.wl.compositor != NULL)
        wl_compositor_destroy(c.wl.compositor);
    if (c.wl.shm != NULL)
        wl_shm_destroy(c.wl.shm);
    if (c.wl.registry != NULL)
        wl_registry_destroy(c.wl.registry);
    if (c.wl.display != NULL)
        wl_display_disconnect(c.wl.display);
    if (c.wl.xkb_state != NULL)
        xkb_state_unref(c.wl.xkb_state);
    if (c.wl.xkb_keymap != NULL)
        xkb_keymap_unref(c.wl.xkb_keymap);
    if (c.wl.xkb != NULL)
        xkb_context_unref(c.wl.xkb);

    thrd_join(keyboard_repeater_id, NULL);
    cnd_destroy(&c.repeat.cond);
    mtx_destroy(&c.repeat.mutex);
    close(c.repeat.pipe_read_fd);
    close(c.repeat.pipe_write_fd);
    cairo_debug_reset_static_data();

    return ret;
}
