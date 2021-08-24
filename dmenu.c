#include "dmenu.h"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>

#include <tllist.h>

#define LOG_MODULE "dmenu"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "char32.h"

void
dmenu_load_entries(struct application_list *applications)
{
    tll(char32_t *) entries = tll_init();

    char *line = NULL;
    size_t alloc_size = 0;

    errno = 0;
    while (true) {
        ssize_t len = getline(&line, &alloc_size, stdin);

        if (len == -1) {
            if (errno != 0)
                LOG_ERRNO("failed to read from stdin");
            break;
        }

        while (len > 0 && line[len - 1] == '\n')
            line[--len] = '\0';

        LOG_DBG("%s", line);

        char32_t *wline = ambstoc32(line);
        if (wline == NULL)
            continue;

        tll_push_back(entries, wline);
    }

    free(line);

    applications->v = calloc(tll_length(entries), sizeof(applications->v[0]));
    applications->count = tll_length(entries);

    size_t i = 0;
    tll_foreach(entries, it) {
        struct application *app = &applications->v[i++];
        app->title = it->item;
        app->icon.type = ICON_NONE;

        tll_remove(entries, it);
    }

    tll_free(entries);
}

bool
dmenu_execute(const struct application *app, ssize_t index,
              const struct prompt *prompt, enum dmenu_mode format)
{
    switch (format) {
    case DMENU_MODE_NONE:
        assert(false);
        return false;

    case DMENU_MODE_TEXT: {
        char *text = ac32tombs(app != NULL ? app->title : prompt_text(prompt));
        if (text != NULL)
            printf("%s\n", text);
        free(text);
        break;
    }

    case DMENU_MODE_INDEX:
        printf("%zd\n", index);
        break;
    }

    return true;
}
