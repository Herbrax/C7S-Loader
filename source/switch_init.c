#include <switch.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "switch_init.h"

u32 __nx_fs_num_sessions = 4;

static const SocketInitConfig s_socket_config = {
    .tcp_tx_buf_size = 256 * 1024,
    .tcp_rx_buf_size = 256 * 1024,
    .tcp_tx_buf_max_size = 1024 * 1024,
    .tcp_rx_buf_max_size = 1024 * 1024,
    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0xA500,
    .sb_efficiency = 4,
    .num_bsd_sessions = 3,
    .bsd_service_type = BsdServiceType_User,
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
    struct in_addr host;
    snprintf(buf, size, "unknown");
    host.s_addr = (in_addr_t)gethostid();
    if (host.s_addr && host.s_addr != htonl(INADDR_LOOPBACK)) {
        inet_ntop(AF_INET, &host, buf, (socklen_t)size);
    }
}
