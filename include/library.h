#pragma once

#include <stdbool.h>

typedef struct {
    char name[128];
    char title[128];
    int declared;
    int lines;
    bool header;
    bool mismatch;
    long long size;
} C7sEntry;

int c7s_list(C7sEntry *out, int max);
