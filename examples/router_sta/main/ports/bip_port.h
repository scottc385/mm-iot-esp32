#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lwip/sockets.h"
#include "router_service.h"
#include "router_transport.h"

#define BIP_PORT_MAX_FRAME 1600

typedef struct bip_port {
    int sock;
    uint16_t udp_port;
    uint16_t net;
    uint32_t tx_frames;
    uint32_t rx_frames;
    uint32_t rx_bad;
    uint32_t rx_router_accepted;
    uint32_t tx_errors;
    uint8_t tx_frame[BIP_PORT_MAX_FRAME];
    uint8_t rx_frame[BIP_PORT_MAX_FRAME];
} bip_port;

extern const tb_router_transport_ops k_bip_router_ops;

bool bip_port_open(bip_port *port, uint16_t net, uint16_t udp_port);
void bip_port_send_npdu_broadcast(bip_port *port, const uint8_t *npdu, size_t npdu_len);
void bip_port_poll(bip_port *port,
                   tb_router_service *svc,
                   uint8_t router_port_id,
                   uint32_t now_ms);
void bip_port_print_status(const bip_port *port);
