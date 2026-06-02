#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "bacnet/bacaddr.h"
#include "router_transport.h"

#define TB_ROUTER_SERVICE_STORAGE_SIZE 4608
#define TB_ROUTER_SRC_FLAG_NET_OWNER 0x01
#define TB_ROUTER_ROUTE_HINT_MAX_NETS 16

typedef struct tb_router_service tb_router_service;
typedef struct tb_router_route_snapshot tb_router_route_snapshot;
typedef struct tb_router_event tb_router_event;
typedef struct tb_router_route_hints tb_router_route_hints;
typedef struct tb_router_iam_info tb_router_iam_info;

typedef enum {
    TB_ROUTER_EVENT_DNET_CHANGE = 1,
    TB_ROUTER_EVENT_IAR_RX = 2,
} tb_router_event_type;

struct tb_router_service {
    union {
        uint8_t bytes[TB_ROUTER_SERVICE_STORAGE_SIZE];
        uint64_t align_u64;
        void *align_ptr;
    } opaque;
};

struct tb_router_route_snapshot {
    uint8_t port_id;
    uint16_t port_net;
    uint16_t dnet;
    uint32_t age_ms;
    bool configured_static;
    bool have_next_hop;
    uint8_t next_hop_len;
    uint8_t next_hop[6];
    bool have_peer;
    uint8_t peer_len;
    uint8_t peer[TB_ROUTER_PEER_ID_MAX_LEN];
    /* Compatibility alias for older Thread-oriented callers. Prefer peer/peer_len. */
    bool have_peer_ipv6;
    uint8_t peer_ipv6[TB_ROUTER_PEER_ID_MAX_LEN];
};

struct tb_router_event {
    tb_router_event_type type;
    uint8_t port_id;
    uint16_t net;
};

struct tb_router_route_hints {
    uint16_t nets[TB_ROUTER_ROUTE_HINT_MAX_NETS];
    size_t count;
};

struct tb_router_iam_info {
    uint16_t snet;
    uint8_t sadr[MAX_MAC_LEN];
    uint8_t sadr_len;
    uint32_t device_id;
    unsigned max_apdu;
    int segmentation;
    uint16_t vendor_id;
};

typedef void (*tb_router_service_port_fn)(void *arg, uint8_t port_id, uint16_t port_net);
typedef void (*tb_router_service_route_fn)(void *arg, const tb_router_route_snapshot *route);
typedef void (*tb_router_service_event_fn)(void *arg, const tb_router_event *event);

bool tb_router_service_configure(tb_router_service *svc,
                                 void *user_ctx,
                                 const tb_router_port_config *ports,
                                 size_t port_count,
                                 uint32_t dnet_ttl_ms,
                                 int log_level);
uint8_t tb_router_port_id(const tb_router_port *port);
uint16_t tb_router_port_net(const tb_router_port *port);
tb_router_transport_kind tb_router_port_kind(const tb_router_port *port);
uint32_t tb_router_port_caps(const tb_router_port *port);
void *tb_router_port_transport_state(const tb_router_port *port);
void tb_router_service_set_log_level(tb_router_service *svc, int log_level);
int tb_router_service_handle_frame(tb_router_service *svc,
                                   uint8_t in_port,
                                   const uint8_t *pdu,
                                   size_t len,
                                   const uint8_t *sadr,
                                   size_t sadr_len,
                                   uint8_t src_flags,
                                   uint32_t now_ms);
void tb_router_service_tick(tb_router_service *svc, uint32_t now_ms);
bool tb_router_service_is_remote_net(tb_router_service *svc, uint16_t net);
bool tb_router_service_extract_route_hints(tb_router_service *svc,
                                           const uint8_t *npdu,
                                           size_t npdu_len,
                                           tb_router_route_hints *hints);
bool tb_router_npdu_is_iar(const uint8_t *npdu, size_t npdu_len);
size_t tb_router_npdu_extract_iar_nets(const uint8_t *npdu,
                                       size_t npdu_len,
                                       uint16_t *nets,
                                       size_t max_nets);
bool tb_router_npdu_is_iar_owner(const uint8_t *npdu, size_t npdu_len, uint16_t owner_net);
bool tb_router_npdu_is_whois(const uint8_t *npdu, size_t npdu_len);
bool tb_router_npdu_is_iam(const uint8_t *npdu, size_t npdu_len);
bool tb_router_npdu_decode_iam(const uint8_t *npdu, size_t npdu_len, tb_router_iam_info *info);
bool tb_router_npdu_decode_addresses(const uint8_t *npdu,
                                     size_t npdu_len,
                                     BACNET_ADDRESS *daddr,
                                     BACNET_ADDRESS *saddr);
size_t tb_router_npdu_build_whois_router(uint8_t *out, size_t out_len);
size_t tb_router_npdu_build_iar(uint8_t *out,
                                size_t out_len,
                                const uint16_t *nets,
                                size_t net_count);
bool tb_router_service_lookup_next_hop(tb_router_service *svc,
                                       uint8_t port_id,
                                       uint16_t dnet,
                                       BACNET_ADDRESS *out);
bool tb_router_service_lookup_peer(tb_router_service *svc,
                                   uint16_t dnet,
                                   uint8_t *peer,
                                   uint8_t *peer_len);
void tb_router_service_clear_peers(tb_router_service *svc);
void tb_router_service_drain_events(tb_router_service *svc, tb_router_service_event_fn fn, void *arg);
void tb_router_service_for_each_port(tb_router_service *svc, tb_router_service_port_fn fn, void *arg);
void tb_router_service_for_each_route(tb_router_service *svc,
                                      uint32_t now_ms,
                                      tb_router_service_route_fn fn,
                                      void *arg);
void tb_router_service_for_each_port_route(tb_router_service *svc,
                                           uint32_t now_ms,
                                           tb_router_service_port_fn port_fn,
                                           tb_router_service_route_fn route_fn,
                                           void *arg);
const char *tb_router_service_get_log(tb_router_service *svc, size_t *len_out);
void tb_router_service_clear_log(tb_router_service *svc);
