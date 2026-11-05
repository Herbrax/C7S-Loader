#pragma once

#include <stdbool.h>
#include <stddef.h>

void update_init(const char *argv);
void update_cleanup(void);
bool update_new_path(char *out, size_t cap);
bool update_apply(char *err, size_t errlen);
