#pragma once

#include <stdbool.h>
#include <stddef.h>

#define C7S_MAX_BYTES (4u * 1024u * 1024u)
#define C7S_SEPARATOR "---BEGIN---"
#define C7S_END_MARKER "---END---"

/* A file declares one or two of these in its header line. The tokens written
 * in the file are "Text", "Civ7LDN" and "Script" (see c7s.c). */
enum {
    C7S_TYPE_TEXT    = 1 << 0, /* typed over USB keyboard into a text field */
    C7S_TYPE_CIV7LDN = 1 << 1, /* injected into Civ 7 chat over LDN          */
    C7S_TYPE_SCRIPT  = 1 << 2, /* timed gamepad/keyboard macro (@key/@pad)   */
    C7S_TYPE_JS      = 1 << 3, /* combined RemoteJS payload + save template  */
};

typedef struct {
    char *data;
    char **lines;
    int line_count;
    bool header_valid; /* header parsed with a recognised type */
    int types;         /* bitmask of C7S_TYPE_* (1 or 2 set) */
    char title[128];
    char desc[256];
    char filename[128];
    char error[192];
} C7sPayload;

/* Convenience predicates over the type bitmask. */
static inline bool c7s_is_text(const C7sPayload *p) { return (p->types & C7S_TYPE_TEXT) != 0; }
static inline bool c7s_is_ldn(const C7sPayload *p) { return (p->types & C7S_TYPE_CIV7LDN) != 0; }
static inline bool c7s_is_script(const C7sPayload *p) { return (p->types & C7S_TYPE_SCRIPT) != 0; }
static inline bool c7s_is_js(const C7sPayload *p) { return (p->types & C7S_TYPE_JS) != 0; }

bool c7s_parse_file(const char *path, C7sPayload *out);
void c7s_free(C7sPayload *payload);
bool c7s_name_ok(const char *name);
bool c7s_upload_name_ok(const char *name);
