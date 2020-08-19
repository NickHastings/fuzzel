#include "application.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#define LOG_MODULE "application"
#define LOG_ENABLE_DBG 0
#include "log.h"

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
    char *search_start = p;

    size_t idx = 0;
    while (*p != '\0') {
        char *end = strchr(search_start, delim);
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

        if (end > p && *(end - 1) == '\\')  {
            /* Escaped quote, remove one level of escaping and
             * continue searching for "our" closing quote */
            memmove(end - 1, end, strlen(end));
            end[strlen(end) - 1] = '\0';
            search_start = end;
            continue;
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
        search_start = p;
    }

    if (!push_argv(argv, &argv_size, NULL, &idx))
        goto err;

    return true;

err:
    free(*argv);
    return false;
}

bool
application_execute(const struct application *app, const struct prompt *prompt)
{
    const wchar_t *ptext = prompt_text(prompt);
    const size_t clen = wcstombs(NULL, ptext, 0);
    char cprompt[clen + 1];
    wcstombs(cprompt, ptext, clen + 1);

    const char *execute = app != NULL ? app->exec : cprompt;
    const char *path = app != NULL ? app->path : NULL;

    LOG_DBG("exec(%s)", execute);

    if (strchr(execute, '\\') != NULL) {
        LOG_ERR("unimplemented: escaped exec arguments: %s", execute);
        return false;
    }

    /* Tokenize the command */
    char *copy = strdup(execute);
    char **argv;
    if (!tokenize_cmdline(copy, &argv)) {
        free(copy);
        return false;
    }

    LOG_DBG("argv:");
    for (size_t i = 0; argv[i] != NULL; i++)
        LOG_DBG("  %zu: \"%s\"", i, argv[i]);

    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC) == -1) {
        LOG_ERRNO("failed to create pipe");
        return false;
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);

        LOG_ERRNO("failed to fork");
        return false;
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
        (void)!write(pipe_fds[1], &errno, sizeof(errno));
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
        }

        close(pipe_fds[0]);
        return ret == 0;
    }
}

struct application_list *
applications_init(void)
{
    struct application_list *apps = malloc(sizeof(*apps));
    *apps = (struct application_list){};
    return apps;
}

void
applications_destroy(struct application_list *apps)
{
    if (apps == NULL)
        return;

    for (size_t i = 0; i < apps->count; i++) {
        struct application *app = &apps->v[i];

        free(app->path);
        free(app->exec);
        free(app->title);
        free(app->comment);
        free(app->icon.name);
        if (app->icon.type == ICON_SURFACE)
            cairo_surface_destroy(app->icon.surface);
        else if (app->icon.type == ICON_SVG) {
#ifdef FUZZEL_ENABLE_SVG
            g_object_unref(app->icon.svg);
#endif
        }
    }
    free(apps->v);
    free(apps);
}
