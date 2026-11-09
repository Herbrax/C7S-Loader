#pragma once

#include <stdbool.h>

/* TEMP-FTP: diagnostic only. Delete this header and source/temp_ftp.c,
 * and the temp_ftp_start() call in main.c, before release. */
void temp_ftp_start(void);
void temp_ftp_stop(void);
/* True while the server thread is still alive (listener or an active
 * client). The exit path waits on this so socketExit() never runs while
 * this thread is parked in accept()/recv() holding a socket. */
bool temp_ftp_running(void);
