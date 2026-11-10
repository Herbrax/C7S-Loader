#pragma once

/* RemoteJS: talk to a Switch 2 that is already running the injected RemoteJS
 * loader. The loader opens a tiny TCP "eval server" on port 9026 over Wi-Fi:
 * you send it a blob of JavaScript, it evaluates it and replies with the
 * marker "Executed successfully" (or an error line). This is NOT LDN - by the
 * time we get here LDN has been stopped and infrastructure Wi-Fi is back up.
 *
 * Flow mirrors civswitch-ldn-nx's installer.c:
 *   - scan the local Wi-Fi subnet for a host answering on :9026 to probe "1"
 *   - send a .js file's bytes, wait for "Executed successfully"
 */

#include <stdbool.h>
#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RJS_IDLE = 0,
    RJS_SCANNING,
    RJS_FOUND,
    RJS_SENDING,
    RJS_DONE,
    RJS_ERROR,
} RjsPhase;

void remote_js_init(void);
void remote_js_shutdown(void);

/* Async. Finds a loader on :9026 over the local Wi-Fi. */
void remote_js_scan(void);
/* Async. Sends the named .js file (in C7S_DIR) to the loader. Scans first if
 * no loader is known yet. */
void remote_js_send(const char *filename);
/* Forget the found loader and go back to idle (does not interrupt a running
 * scan/send). */
void remote_js_reset(void);

RjsPhase     remote_js_phase(void);
const char  *remote_js_phase_name(void);
const char  *remote_js_status(void);
bool         remote_js_busy(void);
bool         remote_js_have_loader(void);
const char  *remote_js_loader_ip_str(void); /* "192.168.x.y" or "" */

/* Lists .js files living alongside the .C7S files. Returns count. */
int remote_js_list(char out[][128], int max);

#ifdef __cplusplus
}
#endif
