#include "library.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "app.h"
#include "c7s.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

static int entry_cmp(const void *a, const void *b) {
    const C7sEntry *ea = a;
    const C7sEntry *eb = b;
    return strcasecmp(ea->name, eb->name);
}

int c7s_list(C7sEntry *out, int max) {
    if (!out || max <= 0) return 0;
    DIR *d = opendir(C7S_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d)) != NULL) {
        if (!c7s_name_ok(e->d_name)) continue;
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", C7S_DIR, e->d_name) >= (int)sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        C7sEntry *ent = &out[n];
        memset(ent, 0, sizeof(*ent));
        size_t nlen = strlen(e->d_name);
        if (nlen >= sizeof(ent->name)) nlen = sizeof(ent->name) - 1;
        memcpy(ent->name, e->d_name, nlen);
        ent->name[nlen] = '\0';
        ent->size = (long long)st.st_size;

        C7sPayload p;
        if (c7s_parse_file(path, &p)) {
            ent->header = p.header_valid;
            ent->types = p.types;
            ent->lines = p.line_count;
            snprintf(ent->title, sizeof(ent->title), "%s", p.title);
            snprintf(ent->desc, sizeof(ent->desc), "%s", p.desc);
            c7s_free(&p);
        }
        n++;
    }
    closedir(d);
    qsort(out, n, sizeof(*out), entry_cmp);
    return n;
}
