#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lwip/sockets.h"
#include "router_service.h"
#include "router_transport.h"
#include "tbx.h"

#define UDP_TBX_PORT_MAX_FRAME 1600

typedef struct udp_tbx_port {
    int sock;
    struct sockaddr_in peer;
    uint8_t origin_id[TBX_ORIGIN_ID_LEN];
    uint32_t tx_frames;
    uint32_t rx_frames;
    uint32_t rx_bad;
    uint32_t rx_loop;
    uint32_t rx_router_accepted;
    uint32_t tx_errors;
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
void udp_tbx_port_print_status(const udp_tbx_port *port);
