#include "c7s.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool name_safe(const char *name) {
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
    return true;
}

bool c7s_name_ok(const char *name) {
    if (!name_safe(name)) return false;
    const char *dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ".c7s") == 0;
}

/* Upload also accepts standalone .js payloads (stored next to the .C7S). */
bool c7s_upload_name_ok(const char *name) {
    if (!name_safe(name)) return false;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return strcasecmp(dot, ".c7s") == 0 || strcasecmp(dot, ".js") == 0;
}

static bool is_end_marker(const char *line) {
    size_t n = strlen(C7S_END_MARKER);
    if (strncmp(line, C7S_END_MARKER, n) != 0) return false;
    for (const char *p = line + n; *p; p++) {
        if (*p != ' ' && *p != '\t') return false;
    }
    return true;
}

static void rtrim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
}

/* Header line:  <Title> - <Type>[ - <Type2>] - desc:"<description>"
 * Fields separated by " - ". Type tokens (case-insensitive): Text, Civ7LDN,
 * Script. desc:"..." may sit anywhere and may itself contain " - " inside the
 * quotes. Unknown fields are ignored. Fills title + desc and returns the type
 * bitmask (0 if none recognised). */
static int parse_header(const char *line, char *title, size_t title_size,
                        char *desc, size_t desc_size) {
    title[0] = '\0';
    desc[0] = '\0';

    char work[512];
    snprintf(work, sizeof(work), "%s", line);

    /* Extract desc:"..." first, since its value can contain " - ". */
    char *d = strstr(work, "desc:\"");
    if (d) {
        char *v = d + 6;
        char *endq = strchr(v, '"');
        size_t dl = endq ? (size_t)(endq - v) : strlen(v);
        if (dl >= desc_size) dl = desc_size - 1;
        memcpy(desc, v, dl);
        desc[dl] = '\0';
        char *cut = d;                                   /* drop the whole field */
        if (cut - work >= 3 && strncmp(cut - 3, " - ", 3) == 0) cut -= 3;
        *cut = '\0';
    }

    int types = 0;
    char *p = work;
    char *sep = strstr(p, " - ");
    if (!sep) {                                          /* title only */
        snprintf(title, title_size, "%s", p);
        rtrim(title);
        return 0;
    }
    size_t tl = (size_t)(sep - p);
    if (tl >= title_size) tl = title_size - 1;
    memcpy(title, p, tl);
    title[tl] = '\0';
    rtrim(title);

    for (p = sep + 3; p && *p;) {
        char *next = strstr(p, " - ");
        size_t fl = next ? (size_t)(next - p) : strlen(p);
        char tok[64];
        if (fl >= sizeof(tok)) fl = sizeof(tok) - 1;
        memcpy(tok, p, fl);
        tok[fl] = '\0';
        rtrim(tok);
        if (strcasecmp(tok, "Text") == 0) types |= C7S_TYPE_TEXT;
        else if (strcasecmp(tok, "Civ7LDN") == 0) types |= C7S_TYPE_CIV7LDN;
        else if (strcasecmp(tok, "Script") == 0) types |= C7S_TYPE_SCRIPT;
        else if (strcasecmp(tok, "JS Payload") == 0) types |= C7S_TYPE_JS;
        if (!next) break;
        p = next + 3;
    }
    return types;
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
        out->types = parse_header(raw[0], out->title, sizeof(out->title),
                                  out->desc, sizeof(out->desc));
        out->header_valid = true;   /* has the <header>/---BEGIN--- structure */
        first_body = 2;
    } else {
        out->header_valid = false;
        first_body = 0;
    }

    int body_count = 0;
    for (int i = first_body; i < raw_count; i++) {
        if (is_end_marker(raw[i])) break;
        body_count++;
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
        lines[n++] = raw[i];
    }
    free(raw);

    out->lines = lines;
    out->line_count = n;

    /* Types come from the header. If the header declared none (legacy or
     * malformed file), fall back to a body sniff so the file is still usable:
     * a @-prefixed first line is a Script, otherwise Text. */
    if (out->types == 0) {
        if (out->line_count > 0 && out->lines[0][0] == '@') out->types = C7S_TYPE_SCRIPT;
        else if (out->line_count > 0) out->types = C7S_TYPE_TEXT;
    }
    return true;
}

void c7s_free(C7sPayload *payload) {
    if (!payload) return;
    free(payload->lines);
    free(payload->data);
    memset(payload, 0, sizeof(*payload));
}
