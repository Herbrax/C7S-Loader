#pragma once

#include <stdbool.h>
#include <switch.h>

typedef struct {
    char ssid[33];
    u8 node_count;
    u8 node_count_max;
    s16 channel;
    s16 link_level;
    s32 version;
    u32 host_ip;
    bool joinable;
    char problem[48];
} LdnLobbyInfo;

Result ldn_route_init(void);
int ldn_route_scan(void);
int ldn_route_scan_count(void);
const LdnLobbyInfo *ldn_route_scan_item(int index);
int ldn_route_join(int index);
void ldn_route_send_loader(void);
void ldn_route_stop(void);
void ldn_route_start(void);
void ldn_route_shutdown(void);
bool ldn_route_active(void);
bool ldn_route_connected(void);
const char *ldn_route_status(void);
const char *ldn_route_log(void);
