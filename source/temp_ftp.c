/*
 * TEMP-FTP: diagnostic-only server on port 2122.
 * Credentials: switch / chiral. Root: sdmc:.
 *
 * DELETE THIS FILE, include/temp_ftp.h, and the temp_ftp_start() call in
 * source/main.c BEFORE RELEASE.
 */
#include "temp_ftp.h"

#include <switch.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEMP_FTP_PORT 2122
#define TEMP_FTP_ROOT "sdmc:"

typedef struct {
    int ctrl;
    int pasv;
    char cwd[512];
} FtpClient;

static volatile bool g_stop;
static volatile bool g_running;
static int g_server = -1;
/* The control/data sockets of the one client accept() is servicing, so a
 * stop can shut them down and wake client_loop() out of a blocking recv().
 * Only ftp_thread accepts, so at most one client is live at a time. */
static volatile int g_client_ctrl = -1;
static volatile int g_client_pasv = -1;

static void reply(int fd, const char *line) {
    send(fd, line, strlen(line), 0);
}

static int pasv_open(uint16_t *port_out) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof(a);
    if (getsockname(fd, (struct sockaddr *)&a, &len) < 0) {
        close(fd);
        return -1;
    }
    *port_out = ntohs(a.sin_port);
    return fd;
}

static int data_accept(FtpClient *c) {
    if (c->pasv < 0) return -1;
    /* Poll so a stop (or a client that never dials in) can't park us here
     * forever holding the data socket. */
    int fd = -1;
    for (int waited = 0; waited < 10000 && !g_stop; waited += 200) {
        struct pollfd pfd = {.fd = c->pasv, .events = POLLIN};
        int pr = poll(&pfd, 1, 200);
        if (pr > 0) { fd = accept(c->pasv, NULL, NULL); break; }
        if (pr < 0) break;
    }
    close(c->pasv);
    c->pasv = -1;
    g_client_pasv = -1;
    return fd;
}

static void path_join(FtpClient *c, const char *arg, char *out, size_t cap) {
    if (arg[0] == '/') snprintf(out, cap, "%s%s", TEMP_FTP_ROOT, arg);
    else snprintf(out, cap, "%s/%s", c->cwd, arg);
}

static void send_file(int data, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) send(data, buf, n, 0);
    fclose(f);
}

static void send_list(int data, const char *path) {
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char line[600];
        snprintf(line, sizeof(line), "%s\r\n", e->d_name);
        send(data, line, strlen(line), 0);
    }
    closedir(d);
}

static void client_loop(FtpClient *c) {
    char line[1024];
    char cmd[32];
    char arg[768];
    reply(c->ctrl, "220 C7S-Loader temp FTP\r\n");
    snprintf(c->cwd, sizeof(c->cwd), "%s", TEMP_FTP_ROOT);
    for (;;) {
        size_t len = 0;
        while (len + 1 < sizeof(line)) {
            char ch;
            ssize_t r = recv(c->ctrl, &ch, 1, 0);
            if (r <= 0) return;
            if (ch == '\n') break;
            if (ch != '\r') line[len++] = ch;
        }
        line[len] = '\0';
        if (!len) continue;
        arg[0] = '\0';
        cmd[0] = '\0';
        sscanf(line, "%31s %767[^\n]", cmd, arg);
        for (char *p = cmd; *p; p++) *p = (char)toupper((unsigned char)*p);

        if (!strcmp(cmd, "USER")) {
            reply(c->ctrl, "331 Any password\r\n");
        } else if (!strcmp(cmd, "PASS")) {
            reply(c->ctrl, "230 Logged in\r\n");
        } else if (!strcmp(cmd, "SYST")) {
            reply(c->ctrl, "215 UNIX Type: L8\r\n");
        } else if (!strcmp(cmd, "FEAT")) {
            reply(c->ctrl, "211-Features:\r\n PASV\r\n EPSV\r\n SIZE\r\n211 End\r\n");
        } else if (!strcmp(cmd, "OPTS")) {
            reply(c->ctrl, "200 OK\r\n");
        } else if (!strcmp(cmd, "TYPE")) {
            reply(c->ctrl, "200 Type set\r\n");
        } else if (!strcmp(cmd, "PWD") || !strcmp(cmd, "XPWD")) {
            char out[600];
            snprintf(out, sizeof(out), "257 \"%s\"\r\n", c->cwd);
            reply(c->ctrl, out);
        } else if (!strcmp(cmd, "CWD")) {
            char path[768];
            path_join(c, arg, path, sizeof(path));
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                snprintf(c->cwd, sizeof(c->cwd), "%s", path);
                reply(c->ctrl, "250 OK\r\n");
            } else {
                reply(c->ctrl, "550 No such directory\r\n");
            }
        } else if (!strcmp(cmd, "PASV") || !strcmp(cmd, "EPSV")) {
            if (c->pasv >= 0) close(c->pasv);
            uint16_t port = 0;
            c->pasv = pasv_open(&port);
            g_client_pasv = c->pasv;
            if (c->pasv < 0) {
                reply(c->ctrl, "425 Can't open passive\r\n");
            } else if (!strcmp(cmd, "EPSV")) {
                char out[128];
                snprintf(out, sizeof(out), "229 Entering Extended Passive Mode (|||%u|)\r\n", (unsigned)port);
                reply(c->ctrl, out);
            } else {
                struct sockaddr_in local;
                socklen_t ll = sizeof(local);
                getsockname(c->ctrl, (struct sockaddr *)&local, &ll);
                char *ip = inet_ntoa(local.sin_addr);
                int a, b, cc, d;
                sscanf(ip, "%d.%d.%d.%d", &a, &b, &cc, &d);
                char out[128];
                snprintf(out, sizeof(out), "227 Entering Passive Mode (%d,%d,%d,%d,%u,%u)\r\n",
                         a, b, cc, d, port >> 8, port & 255);
                reply(c->ctrl, out);
            }
        } else if (!strcmp(cmd, "LIST") || !strcmp(cmd, "NLST")) {
            reply(c->ctrl, "150 Opening data connection\r\n");
            int data = data_accept(c);
            if (data < 0) {
                reply(c->ctrl, "425 No data connection\r\n");
                continue;
            }
            send_list(data, c->cwd);
            close(data);
            reply(c->ctrl, "226 Done\r\n");
        } else if (!strcmp(cmd, "RETR")) {
            char path[768];
            path_join(c, arg, path, sizeof(path));
            FILE *f = fopen(path, "rb");
            if (!f) {
                reply(c->ctrl, "550 Not found\r\n");
                continue;
            }
            fclose(f);
            reply(c->ctrl, "150 Opening data connection\r\n");
            int data = data_accept(c);
            if (data < 0) {
                reply(c->ctrl, "425 No data connection\r\n");
                continue;
            }
            send_file(data, path);
            close(data);
            reply(c->ctrl, "226 Done\r\n");
        } else if (!strcmp(cmd, "SIZE")) {
            char path[768];
            path_join(c, arg, path, sizeof(path));
            struct stat st;
            if (stat(path, &st) == 0) {
                char out[128];
                snprintf(out, sizeof(out), "213 %lld\r\n", (long long)st.st_size);
                reply(c->ctrl, out);
            } else {
                reply(c->ctrl, "550 Not found\r\n");
            }
        } else if (!strcmp(cmd, "QUIT")) {
            reply(c->ctrl, "221 Bye\r\n");
            return;
        } else {
            reply(c->ctrl, "502 Not implemented\r\n");
        }
    }
}

static void *ftp_thread(void *arg) {
    (void)arg;
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) return NULL;
    int one = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(TEMP_FTP_PORT);
    if (bind(server, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(server, 2) < 0) {
        close(server);
        g_running = false;
        return NULL;
    }
    g_server = server;
    /* Poll the listener with a timeout instead of blocking in accept(). On
     * libnx a close()/shutdown() from another thread does not reliably wake
     * a blocked accept(), so without this the thread could sit here holding
     * a socket when socketExit() runs on the way out -- the exact crash we
     * are avoiding. Polling lets the thread notice g_stop within 200ms and
     * close its own socket. */
    while (!g_stop) {
        struct pollfd pfd = {.fd = server, .events = POLLIN};
        int pr = poll(&pfd, 1, 200);
        if (pr <= 0) continue;  /* timeout or error -> re-check g_stop */
        int fd = accept(server, NULL, NULL);
        if (fd < 0) continue;
        FtpClient c;
        memset(&c, 0, sizeof(c));
        c.ctrl = fd;
        c.pasv = -1;
        g_client_ctrl = fd;
        client_loop(&c);
        g_client_ctrl = -1;
        g_client_pasv = -1;
        if (c.pasv >= 0) close(c.pasv);
        close(fd);
    }
    close(server);
    g_server = -1;
    g_running = false;
    return NULL;
}

void temp_ftp_start(void) {
    g_stop = false;
    g_running = true;
    pthread_t th;
    if (pthread_create(&th, NULL, ftp_thread, NULL) == 0) {
        pthread_detach(th);
    } else {
        g_running = false;
    }
}

void temp_ftp_stop(void) {
    g_stop = true;
    /* The listener exits its poll loop on its own; here we only wake a
     * client parked in recv() on a connected socket (shutdown() is reliable
     * for connected sockets). The owning thread closes the fds. */
    int c = g_client_ctrl;
    if (c >= 0) shutdown(c, SHUT_RDWR);
    int p = g_client_pasv;
    if (p >= 0) shutdown(p, SHUT_RDWR);
}

bool temp_ftp_running(void) { return g_running; }
