#pragma once

#include <stdbool.h>

int httpd_start(unsigned short port);
void httpd_stop(void);
bool httpd_running(void);
unsigned short httpd_port(void);
