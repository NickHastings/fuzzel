#include "xdg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <wchar.h>
#include <unistd.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>

#define LOG_MODULE "xdg"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "icon.h"

typedef tll(struct application) application_llist_t;

static wchar_t *
strdup_to_wchar(const char *s)
{
    size_t wlen = mbstowcs(NULL, s, 0);
    if (wlen == (size_t)-1)
        return NULL;

    wchar_t *ret = malloc((wlen + 1) * sizeof(wchar_t));
    mbstowcs(ret, s, wlen + 1);
    return ret;
}

static void
parse_desktop_file(int fd, const wchar_t *file_basename, const char *terminal,
                   application_llist_t *applications)
{
    FILE *f = fdopen(fd, "r");
    if (f == NULL)
        return;

    bool is_desktop_entry = false;

    wchar_t *wexec = NULL;
    wchar_t *name = NULL;
    wchar_t *generic_name = NULL;
    wchar_t *comment = NULL;
    wchar_list_t keywords = tll_init();
    wchar_list_t categories = tll_init();

    char *exec = NULL;
    char *path = NULL;
    char *icon = NULL;
    bool visible = true;
    bool use_terminal = false;

    while (true) {
        char *line = NULL;
        size_t sz = 0;
        ssize_t len = getline(&line, &sz, f);

        if (len == -1) {
            free(line);
            break;
        }

        if (len == 0) {
            free(line);
            continue;
        }

        if (line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }

        if (len == 0) {
            free(line);
            continue;
        }

        if (line[0] == '[' && line[len - 1] == ']') {
            if (strncasecmp(&line[1], "desktop entry", len - 2) == 0) {
                is_desktop_entry = true;
                free(line);
                continue;
            } else {
                free(line);
                break;
            }
        }

        const char *key = strtok(line, "=");
        char *value = strtok(NULL, "\n");

        if (key != NULL && value != NULL) {
            if (strcmp(key, "Name") == 0) {
                free(name);
                name = strdup_to_wchar(value);
            }

            else if (strcmp(key, "Exec") == 0) {
                free(exec);
                free(wexec);
                exec = strdup(value);
                wexec = strdup_to_wchar(value);
            }

            else if (strcmp(key, "Path") == 0) {
                free(path);
                path = strdup(value);
            }

            else if (strcmp(key, "GenericName") == 0) {
                free(generic_name);
                generic_name = strdup_to_wchar(value);
            }

            else if (strcmp(key, "Comment") == 0) {
                free(comment);
                comment = strdup_to_wchar(value);
            }

            else if (strcmp(key, "Keywords") == 0) {
                for (const char *kw = strtok(value, ";");
                     kw != NULL;
                     kw = strtok(NULL, ";"))
                {
                    wchar_t *wide_kw = strdup_to_wchar(kw);
                    if (wide_kw != NULL)
                        tll_push_back(keywords, wide_kw);
                }
            }

            else if (strcmp(key, "Categories") == 0) {
                for (const char *category = strtok(value, ";");
                     category != NULL;
                     category = strtok(NULL, ";"))
                {
                    wchar_t *wide_category = strdup_to_wchar(category);
                    if (wide_category != NULL)
                        tll_push_back(categories, wide_category);
                }
            }

            else if (strcmp(key, "Icon") == 0)
                icon = strdup(value);

            else if (strcmp(key, "Hidden") == 0 ||
                     strcmp(key, "NoDisplay") == 0)
            {
                if (strcmp(value, "true") == 0)
                    visible = false;
            }

            else if (strcmp(key, "Terminal") == 0) {
                if (strcmp(value, "true") == 0)
                    use_terminal = true;
            }
        }

        free(line);
    }

    fclose(f);

    if (is_desktop_entry && visible && name != NULL && exec != NULL) {
        bool already_added = false;
        tll_foreach(*applications, it) {
            if (wcscmp(it->item.title, name) == 0) {
                already_added = true;
                break;
            }
        }

        if (!already_added) {
            if (use_terminal && terminal != NULL) {
                char *exec_with_terminal = malloc(
                    strlen(terminal) + 1 + strlen(exec) + 1);
                strcpy(exec_with_terminal, terminal);
                strcat(exec_with_terminal, " ");
                strcat(exec_with_terminal, exec);
                free(exec);
                exec = exec_with_terminal;
            }

            tll_push_back(
                *applications,
                ((struct application){
                    .path = path,
                    .exec = exec,
                    .basename = wcsdup(file_basename),
                    .wexec = wexec,
                    .title = name,
                    .generic_name = generic_name,
                    .comment = comment,
                    .keywords = keywords,
                    .categories = categories,
                    .icon = {.name = icon != NULL ? strdup(icon) : NULL},
                    .count = 0}));
            free(icon);
            return;
        }
    }

    free(path);
    free(name);
    free(exec);
    free(wexec);
    free(generic_name);
    free(comment);
    free(icon);

    tll_free_and_free(keywords, free);
    tll_free_and_free(categories, free);
}

static void
scan_dir(int base_fd, const char *terminal, application_llist_t *applications)
{
    DIR *d = fdopendir(base_fd);
    if (d == NULL) {
        LOG_ERRNO("failed to open directory");
        return;
    }

    for (const struct dirent *e = readdir(d); e != NULL; e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;

        struct stat st;
        if (fstatat(base_fd, e->d_name, &st, 0) == -1) {
            LOG_WARN("%s: failed to stat", e->d_name);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            int dir_fd = openat(base_fd, e->d_name, O_RDONLY);
            if (dir_fd == -1) {
                LOG_ERRNO("%s: failed to open", e->d_name);
                continue;
            }

            scan_dir(dir_fd, terminal, applications);
            close(dir_fd);
        } else if (S_ISREG(st.st_mode)) {
            /* Skip files not ending with ".desktop" */
            const size_t desktop_len = strlen(".desktop");
            const size_t name_len = strlen(e->d_name);
            if (name_len < desktop_len)
                continue;

            if (strcmp(&e->d_name[name_len - desktop_len], ".desktop") != 0)
                continue;

            //LOG_DBG("%s", e->d_name);
            int fd = openat(base_fd, e->d_name, O_RDONLY);
            if (fd == -1)
                LOG_WARN("%s: failed to open", e->d_name);
            else {
                const char *file_basename = strrchr(e->d_name, '/');
                if (file_basename == NULL)
                    file_basename = e->d_name;
                else
                    file_basename++;

                const char *extension = strchr(file_basename, '.');
                if (extension == NULL)
                    extension = file_basename + strlen(file_basename);

                const char *src = file_basename;
                mbstate_t ps = {0};
                int chars = mbsnrtowcs(NULL, &src, extension - file_basename, 0, &ps);
                assert(chars >= 0);

                wchar_t wfile_basename[chars + 1];
                src = file_basename;
                memset(&ps, 0, sizeof(ps));
                mbsnrtowcs(wfile_basename, &src, extension - file_basename, chars, &ps);
                wfile_basename[chars] = L'\0';

                parse_desktop_file(fd, wfile_basename, terminal, applications);
                close(fd);
            }
        }

    }

    closedir(d);
}

static int
sort_application_by_title(const void *_a, const void *_b)
{
    const struct application *a = _a;
    const struct application *b = _b;
    return wcscmp(a->title, b->title);
}

void
xdg_find_programs(const char *terminal, struct application_list *applications)
{
    application_llist_t apps = tll_init();

    xdg_data_dirs_t dirs = xdg_data_dirs();
    tll_foreach(dirs, it) {
        char path[strlen(it->item) + 1 + strlen("applications") + 1];
        sprintf(path, "%s/applications", it->item);

        int fd = open(path, O_RDONLY);
        if (fd != -1) {
            scan_dir(fd, terminal, &apps);
            close(fd);
        }
    }

    applications->count = tll_length(apps);
    applications->v = malloc(tll_length(apps) * sizeof(applications->v[0]));

    size_t i = 0;
    tll_foreach(apps, it) {
        applications->v[i++] = it->item;
        tll_remove(apps, it);
    }
    tll_free(apps);

    qsort(applications->v, applications->count, sizeof(applications->v[0]),
          &sort_application_by_title);

    xdg_data_dirs_destroy(dirs);
}

xdg_data_dirs_t
xdg_data_dirs(void)
{
    xdg_data_dirs_t ret = tll_init();

    const char *xdg_data_home = getenv("XDG_DATA_HOME");
    if (xdg_data_home != NULL)
        tll_push_back(ret, strdup(xdg_data_home));
    else {
        static const char *const local = ".local/share";
        const struct passwd *pw = getpwuid(getuid());

        char *path = malloc(strlen(pw->pw_dir) + 1 + strlen(local) + 1);
        sprintf(path, "%s/%s", pw->pw_dir, local);
        tll_push_back(ret, path);
    }

    const char *_xdg_data_dirs = getenv("XDG_DATA_DIRS");

    if (_xdg_data_dirs != NULL) {

        char *ctx = NULL;
        char *copy = strdup(_xdg_data_dirs);

        for (const char *tok = strtok_r(copy, ":", &ctx);
             tok != NULL;
             tok = strtok_r(NULL, ":", &ctx))
        {
            tll_push_back(ret, strdup(tok));
        }

        free(copy);
    } else {
        tll_push_back(ret, strdup("/usr/local/share"));
        tll_push_back(ret, strdup("/usr/share"));
    }

    return ret;
}

void
xdg_data_dirs_destroy(xdg_data_dirs_t dirs)
{
    tll_foreach(dirs, it) {
        free(it->item);
        tll_remove(dirs, it);
    }
}

const char *
xdg_cache_dir(void)
{
    const char *xdg_cache_home = getenv("XDG_CACHE_HOME");
    if (xdg_cache_home != NULL)
        return xdg_cache_home;

    static char path[PATH_MAX];
    const struct passwd *pw = getpwuid(getuid());
    snprintf(path, sizeof(path), "%s/.cache", pw->pw_dir);
    return path;
}
