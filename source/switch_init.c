#include <switch.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "switch_init.h"

/* Match chiral-ftp, which runs the same libmicrohttpd + bsd sockets on the
 * same platform and exits cleanly. 8 fs sessions, and the socket driver's
 * buffers sized the way chiral/mtheall's ftpd size them. We intentionally do
 * NOT set __nx_heap_size: under hbloader the heap comes from the homebrew ABI
 * anyway, and capping it starves the socket driver's transfer memory. */
u32 __nx_fs_num_sessions = 8;

static const SocketInitConfig s_socket_config = {
    .tcp_tx_buf_size     = 1 * 1024 * 1024,
    .tcp_rx_buf_size     = 1 * 1024 * 1024,
    .tcp_tx_buf_max_size = 4 * 1024 * 1024,
    .tcp_rx_buf_max_size = 4 * 1024 * 1024,
    .udp_tx_buf_size     = 0x2400,
    .udp_rx_buf_size     = 0xA500,
    .sb_efficiency       = 8,
    .num_bsd_sessions    = 3,
    .bsd_service_type    = BsdServiceType_User,
};

void userAppInit(void) {
    appletLockExit();
    appletSetAutoSleepDisabled(true);
    nifmInitialize(NifmServiceType_User);
    socketInitialize(&s_socket_config);
}

void userAppExit(void) {
    socketExit();
    nifmExit();
    appletSetAutoSleepDisabled(false);
    appletUnlockExit();
}

void local_ip(char *buf, size_t size) {
    snprintf(buf, size, "unknown");
    u32 ip = 0, mask = 0, gw = 0, dns1 = 0, dns2 = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&ip, &mask, &gw, &dns1, &dns2)) && ip) {
        struct in_addr addr;
        addr.s_addr = ip;
        if (inet_ntop(AF_INET, &addr, buf, (socklen_t)size)) return;
    }
    ip = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ip)) && ip) {
        struct in_addr addr;
        addr.s_addr = ip;
        if (inet_ntop(AF_INET, &addr, buf, (socklen_t)size)) return;
    }
    struct in_addr host;
    host.s_addr = htonl((in_addr_t)gethostid());
    if (host.s_addr && host.s_addr != htonl(INADDR_LOOPBACK)) {
        inet_ntop(AF_INET, &host, buf, (socklen_t)size);
    }
}
