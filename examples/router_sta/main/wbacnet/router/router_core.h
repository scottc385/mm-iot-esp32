#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "bacnet/bacaddr.h"

typedef struct {
    uint16_t net;   /* directly attached network */
    uint8_t port_id; /* arbitrary id used by caller */
} br_port_cfg;

typedef struct dnet_entry {
    uint16_t net;
    uint32_t last_ms;
    bool configured_static;
    uint8_t nh_len;
    uint8_t nh_adr[6];
    bool peer_have_ipv6;
    uint8_t peer_ipv6[16];
    struct dnet_entry *next;
} dnet_entry;

typedef struct {
    uint16_t net;      /* directly attached */
    uint8_t port_id;
    dnet_entry *dnets; /* learned nets */
} port_info;

typedef struct router_ctx {
    port_info ports[8];
    size_t nports;
    uint32_t dnet_ttl_ms;
    uint32_t clock_ms;
    int log_level;
    struct {
        uint16_t dnet;
        uint8_t tail[2];
        bool valid;
    } peer_log[16];
    int8_t last_iar_rx_port;
    int8_t last_dnet_change_port;
    uint16_t last_dnet_change_net;
    char log_buf[1024];
    size_t log_len;
    uint8_t scratch[1500];
} router_ctx;

/* Source flags for learning/ownership */
#define BR_SRC_FLAG_NET_OWNER 0x01
#define BR_IAR_ADVISORY_STALE_MS 60000

typedef void (*br_emit_fn)(void *arg,
                           uint8_t out_port,
                           const uint8_t *pdu,
                           size_t len,
                           const BACNET_ADDRESS *daddr);

void br_init(router_ctx *ctx, const br_port_cfg *ports, size_t nports);
void br_set_time(router_ctx *ctx, uint32_t now_ms);
void br_set_dnet_ttl(router_ctx *ctx, uint32_t ttl_ms);
void br_set_log_level(router_ctx *ctx, int level);
int br_get_log_level(router_ctx *ctx);
const char *br_get_log(router_ctx *ctx, size_t *len_out);
void br_clear_log(router_ctx *ctx);
void br_clear_peers(router_ctx *ctx);
int br_take_iar_rx_port(router_ctx *ctx);
bool br_take_dnet_change(router_ctx *ctx, uint8_t *port_id, uint16_t *net);
bool br_lookup_next_hop(router_ctx *ctx, uint8_t port_id, uint16_t dnet, BACNET_ADDRESS *out);
bool br_lookup_peer(router_ctx *ctx, uint16_t dnet, uint8_t *peer, uint8_t *peer_len);

int br_handle_frame(router_ctx *ctx,
                    uint8_t in_port,
                    const uint8_t *pdu,
                    size_t len,
                    const uint8_t *sadr,
                    size_t sadr_len,
                    uint8_t src_flags,
                    br_emit_fn emit,
                    void *emit_arg);

void br_tick(router_ctx *ctx, uint32_t now_ms);
