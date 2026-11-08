#pragma once

#include <stdbool.h>
#include <switch.h>

Result ldn_route_init(void);
void ldn_route_start(void);
void ldn_route_shutdown(void);
bool ldn_route_active(void);
const char *ldn_route_status(void);
const char *ldn_route_log(void);
