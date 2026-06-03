#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "router_service.h"
#include "router_transport.h"

#define MSTP_PORT_MAX_NPDU 1600

typedef struct mstp_port {
    bool active;
    uint16_t net;
    uint8_t mac;
    uint32_t baud;
    uint32_t tx_pdu;
    uint32_t rx_pdu;
    uint32_t rx_router_accepted;
    uint32_t tx_errors;
    uint32_t rx_drops;
    uint32_t pdu_drops;
    uint32_t pdu_queue_depth_max;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
} mstp_port;

extern const tb_router_transport_ops k_mstp_router_ops;

bool mstp_port_open(mstp_port *port, uint16_t net);
void mstp_port_poll(mstp_port *port,
                    tb_router_service *svc,
                    uint8_t router_port_id,
                    uint32_t now_ms);
void mstp_port_print_status(const mstp_port *port);
