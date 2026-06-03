#pragma once

#include <stddef.h>
#include <stdint.h>

#include "bacnet/bacaddr.h"

#define TB_ROUTER_PORT_CAP_UNICAST 0x01u
#define TB_ROUTER_PORT_CAP_BROADCAST 0x02u
#define TB_ROUTER_PORT_CAP_STATIC_PEERS 0x04u
#define TB_ROUTER_PORT_CAP_LEARNED_PEERS 0x08u
#define TB_ROUTER_PORT_CAP_ROUTE_META 0x10u
#define TB_ROUTER_PEER_ID_MAX_LEN 16u

typedef struct tb_router_port tb_router_port;
typedef struct tb_router_port_config tb_router_port_config;
typedef struct tb_router_service tb_router_service;

typedef enum {
    TB_ROUTER_TRANSPORT_BIP = 1,
    TB_ROUTER_TRANSPORT_MSTP = 2,
    TB_ROUTER_TRANSPORT_TBX_UDP = 3,
    TB_ROUTER_TRANSPORT_WAN = 4,
    TB_ROUTER_TRANSPORT_HALOW_MESH = 5,
    TB_ROUTER_TRANSPORT_LOCAL_APP = 6,
} tb_router_transport_kind;

static inline const char *tb_router_transport_kind_name(tb_router_transport_kind kind)
{
    switch (kind) {
        case TB_ROUTER_TRANSPORT_BIP:
            return "bacnet_ip";
        case TB_ROUTER_TRANSPORT_MSTP:
            return "mstp";
        case TB_ROUTER_TRANSPORT_TBX_UDP:
            return "udp_tbx";
        case TB_ROUTER_TRANSPORT_WAN:
            return "wan";
        case TB_ROUTER_TRANSPORT_HALOW_MESH:
            return "halow_mesh";
        case TB_ROUTER_TRANSPORT_LOCAL_APP:
            return "local_app";
        default:
            return "unknown";
    }
}

typedef struct {
    const char *name;
    void (*send_npdu)(tb_router_service *svc,
                      void *user_ctx,
                      tb_router_port *port,
                      const uint8_t *pdu,
                      size_t len,
                      const BACNET_ADDRESS *daddr);
} tb_router_transport_ops;

struct tb_router_port_config {
    uint8_t port_id;
    uint16_t net;
    tb_router_transport_kind kind;
    uint32_t caps;
    void *transport_state;
    const tb_router_transport_ops *ops;
    const uint16_t *static_dnets;
    size_t static_dnet_count;
};
