#include "c7s.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool c7s_name_ok(const char *name) {
    if (!name || !name[0]) return false;
    size_t len = strlen(name);
    if (len >= 120) return false;
    if (name[0] == '.') return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        unsigned char c = *p;
        if (c < 0x20 || c == 0x7f) return false;
        if (c == '/' || c == '\\' || c == ':') return false;
    }
    if (strstr(name, "..")) return false;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return strcasecmp(dot, ".c7s") == 0;
}

static bool is_end_marker(const char *line) {
    size_t n = strlen(C7S_END_MARKER);
    if (strncmp(line, C7S_END_MARKER, n) != 0) return false;
    for (const char *p = line + n; *p; p++) {
        if (*p != ' ' && *p != '\t') return false;
    }
    return true;
}

static bool parse_header(const char *line, char *title, size_t title_size, int *count) {
    const char *sep = NULL;
    for (const char *p = line; (p = strstr(p, " - ")) != NULL; p += 3) sep = p;
    if (!sep) return false;
    size_t len = (size_t)(sep - line);
    if (len == 0 || len >= title_size) return false;
    int n = atoi(sep + 3);
    if (n <= 0) return false;
    memcpy(title, line, len);
    title[len] = '\0';
    *count = n;
    return true;
}

bool c7s_parse_file(const char *path, C7sPayload *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->filename, sizeof(out->filename), "%s", base_name(path));

    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(out->error, sizeof(out->error), "cannot open %s", path);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        snprintf(out->error, sizeof(out->error), "cannot seek");
        return false;
    }
    long size = ftell(f);
    if (size < 0 || (unsigned long)size > C7S_MAX_BYTES) {
        fclose(f);
        snprintf(out->error, sizeof(out->error), "file too large");
        return false;
    }
    rewind(f);
    char *data = malloc((size_t)size + 1);
    if (!data) {
        fclose(f);
        snprintf(out->error, sizeof(out->error), "out of memory");
        return false;
    }
    size_t got = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(data);
        snprintf(out->error, sizeof(out->error), "short read");
        return false;
    }
    data[size] = '\0';
    out->data = data;

    char *scan = data;
    if ((size_t)size >= 3 && (unsigned char)scan[0] == 0xEF &&
        (unsigned char)scan[1] == 0xBB && (unsigned char)scan[2] == 0xBF) {
        scan += 3;
    }
    size_t newline_count = 0;
    for (char *p = scan; *p; p++) {
        if (*p == '\n') newline_count++;
    }

    size_t max_lines = newline_count + 2;
    char **raw = malloc(max_lines * sizeof(char *));
    if (!raw) {
        free(data);
        out->data = NULL;
        snprintf(out->error, sizeof(out->error), "out of memory");
        return false;
    }
    int raw_count = 0;
    char *start = scan;
    for (char *q = scan;; q++) {
        if (*q == '\n' || *q == '\0') {
            bool end = (*q == '\0');
            *q = '\0';
            if (q > start && q[-1] == '\r') q[-1] = '\0';
            if ((size_t)raw_count < max_lines) raw[raw_count++] = start;
            start = q + 1;
            if (end) break;
        }
    }
    while (raw_count > 0 && raw[raw_count - 1][0] == '\0') raw_count--;

    int first_body = 0;
    if (raw_count >= 2 && strcmp(raw[1], C7S_SEPARATOR) == 0) {
        out->header_valid = parse_header(raw[0], out->title, sizeof(out->title), &out->declared_count);
        first_body = 2;
    } else {
        out->header_valid = false;
        first_body = 0;
    }

    int body_count = 0;
    for (int i = first_body; i < raw_count; i++) {
        if (is_end_marker(raw[i])) break;
        if (raw[i][0] != '\0') body_count++;
    }
    char **lines = malloc((size_t)(body_count ? body_count : 1) * sizeof(char *));
    if (!lines) {
        free(raw);
        free(data);
        out->data = NULL;
        snprintf(out->error, sizeof(out->error), "out of memory");
        return false;
    }
    int n = 0;
    for (int i = first_body; i < raw_count; i++) {
        if (is_end_marker(raw[i])) break;
        if (raw[i][0] != '\0') lines[n++] = raw[i];
    }
    free(raw);

    out->lines = lines;
    out->line_count = n;
    if (out->header_valid && out->declared_count != out->line_count) out->count_mismatch = true;
    return true;
}

void c7s_free(C7sPayload *payload) {
    if (!payload) return;
    free(payload->lines);
    free(payload->data);
    memset(payload, 0, sizeof(*payload));
}
