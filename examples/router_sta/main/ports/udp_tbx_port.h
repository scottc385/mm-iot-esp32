#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lwip/sockets.h"
#include "router_service.h"
#include "router_transport.h"
#include "tbx.h"

#define UDP_TBX_PORT_MAX_FRAME 1600
#define UDP_TBX_PORT_MAX_ROUTES 8

typedef struct udp_tbx_route {
    bool valid;
    uint16_t dnet;
    struct sockaddr_in peer;
    uint32_t last_seen_ms;
} udp_tbx_route;

typedef struct udp_tbx_port {
    int sock;
    struct sockaddr_in peer;
    uint8_t origin_id[TBX_ORIGIN_ID_LEN];
    udp_tbx_route routes[UDP_TBX_PORT_MAX_ROUTES];
    uint32_t tx_frames;
    uint32_t rx_frames;
    uint32_t rx_bad;
    uint32_t rx_loop;
    uint32_t rx_router_accepted;
    uint32_t routes_learned;
    uint32_t route_hits;
    uint32_t route_misses;
    uint32_t tx_errors;
    uint8_t tx_frame[UDP_TBX_PORT_MAX_FRAME];
    uint8_t rx_frame[UDP_TBX_PORT_MAX_FRAME];
} udp_tbx_port;

extern const tb_router_transport_ops k_udp_tbx_router_ops;

bool udp_tbx_port_open(udp_tbx_port *port,
                       uint16_t local_port,
                       const char *peer_ip,
                       uint16_t peer_port,
                       const char *origin_id);
void udp_tbx_port_poll(udp_tbx_port *port,
                       tb_router_service *svc,
                       uint8_t router_port_id,
                       uint32_t now_ms);
void udp_tbx_port_send_npdu_direct(udp_tbx_port *port,
                                   const uint8_t *npdu,
                                   size_t npdu_len);
void udp_tbx_port_send_npdu(udp_tbx_port *port,
                            const uint8_t *npdu,
                            size_t npdu_len,
                            const BACNET_ADDRESS *daddr);
bool udp_tbx_port_has_route(const udp_tbx_port *port, uint16_t dnet);
void udp_tbx_port_print_status(const udp_tbx_port *port);
