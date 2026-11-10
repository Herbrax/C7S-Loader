#pragma once

#include <stdbool.h>

typedef struct {
    char name[128];
    char title[128];
    char desc[256];
    int types;   /* bitmask of C7S_TYPE_* */
    int lines;
    bool header;
    long long size;
} C7sEntry;

int c7s_list(C7sEntry *out, int max);
