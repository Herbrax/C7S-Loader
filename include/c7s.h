#pragma once

#include <stdbool.h>
#include <stddef.h>

#define C7S_MAX_BYTES (1024u * 1024u)
#define C7S_SEPARATOR "---BEGIN---"
#define C7S_END_MARKER "---END---"

typedef struct {
    char *data;
    char **lines;
    int line_count;
    int declared_count;
    bool header_valid;
    bool count_mismatch;
    char title[128];
    char filename[128];
    char error[192];
} C7sPayload;

bool c7s_parse_file(const char *path, C7sPayload *out);
void c7s_free(C7sPayload *payload);
bool c7s_name_ok(const char *name);
