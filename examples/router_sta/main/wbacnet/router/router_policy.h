#pragma once

#include <stddef.h>
#include <stdint.h>

#include "router_core.h"
#include "bacnet/npdu.h"

int router_policy_handle_frame(router_ctx *ctx,
                               uint8_t in_port,
                               const uint8_t *pdu,
                               size_t len,
                               const uint8_t *sadr,
                               size_t sadr_len,
                               uint8_t src_flags,
                               br_emit_fn emit,
                               void *emit_arg);

void router_policy_tick(router_ctx *ctx, uint32_t now_ms);

/* Phase B shared policy helpers (pure lookups). */
port_info *router_policy_find_port_by_net(router_ctx *ctx, uint16_t net);
port_info *router_policy_find_port_by_id(router_ctx *ctx, uint8_t id);
bool router_policy_is_local_port_net(router_ctx *ctx, uint16_t net);
dnet_entry *router_policy_add_dnet(port_info *port, uint16_t net);
dnet_entry *router_policy_find_dnet_entry(router_ctx *ctx, uint16_t net, uint8_t *port_id_out);
void router_policy_remove_dnet_from_other_ports(router_ctx *ctx, uint16_t net, uint8_t keep_port_id);

bool router_policy_process_iar(router_ctx *ctx,
                               port_info *srcp,
                               const uint8_t *apdu,
                               int apdu_len,
                               const uint8_t *sadr,
                               size_t sadr_len,
                               uint8_t src_flags);

int router_policy_build_iar_response(router_ctx *ctx,
                                     uint8_t in_port,
                                     uint16_t requested_net,
                                     BACNET_ADDRESS *source,
                                     uint8_t *out,
                                     size_t out_cap,
                                     BACNET_ADDRESS *dest_out);

typedef void (*router_policy_emit_observer_fn)(void *arg,
                                               uint8_t in_port,
                                               uint8_t out_port,
                                               const BACNET_ADDRESS *saddr,
                                               const BACNET_ADDRESS *daddr,
                                               const uint8_t *apdu,
                                               size_t apdu_len,
                                               const char *path);

typedef enum {
    ROUTER_POLICY_NLM_IAR_RX = 1,
    ROUTER_POLICY_NLM_IAR_EMIT = 2,
} router_policy_nlm_event_t;

typedef void (*router_policy_nlm_observer_fn)(void *arg,
                                              router_policy_nlm_event_t event,
                                              uint8_t in_port,
                                              bool learned_any);

typedef void (*router_policy_peer_change_fn)(void *arg,
                                             uint16_t dnet,
                                             const uint8_t *old_peer,
                                             const uint8_t *new_peer,
                                             bool is_new);

int router_policy_handle_frame_with_observers(router_ctx *ctx,
                                              uint8_t in_port,
                                              const uint8_t *pdu,
                                              size_t len,
                                              const uint8_t *sadr,
                                              size_t sadr_len,
                                              uint8_t src_flags,
                                              br_emit_fn emit,
                                              void *emit_arg,
                                              router_policy_nlm_observer_fn nlm_observer,
                                              void *nlm_observer_arg,
                                              router_policy_peer_change_fn peer_observer,
                                              void *peer_observer_arg,
                                              router_policy_emit_observer_fn fwd_observer,
                                              void *fwd_observer_arg);

void router_policy_process_iam(router_ctx *ctx,
                               port_info *srcp,
                               BACNET_ADDRESS *daddr,
                               const BACNET_ADDRESS *saddr,
                               const uint8_t *sadr,
                               size_t sadr_len,
                               router_policy_peer_change_fn peer_observer,
                               void *peer_observer_arg);

void router_policy_handle_application_frame(router_ctx *ctx,
                                            port_info *srcp,
                                            BACNET_ADDRESS *daddr,
                                            const BACNET_ADDRESS *saddr,
                                            const uint8_t *sadr,
                                            size_t sadr_len,
                                            const uint8_t *apdu,
                                            size_t apdu_len,
                                            router_policy_peer_change_fn peer_observer,
                                            void *peer_observer_arg);

int router_policy_prepare_frame(router_ctx *ctx,
                                uint8_t in_port,
                                const uint8_t *pdu,
                                size_t len,
                                const uint8_t *src_mac,
                                size_t src_mac_len,
                                BACNET_ADDRESS *daddr,
                                BACNET_ADDRESS *saddr,
                                BACNET_NPDU_DATA *npdu,
                                port_info **srcp_out);

int router_policy_handle_network_message(router_ctx *ctx,
                                         uint8_t in_port,
                                         port_info *srcp,
                                         uint8_t network_message_type,
                                         const uint8_t *apdu,
                                         int apdu_len,
                                         const BACNET_ADDRESS *saddr,
                                         const uint8_t *sadr,
                                         size_t sadr_len,
                                         uint8_t src_flags,
                                         br_emit_fn emit,
                                         void *emit_arg,
                                         router_policy_nlm_observer_fn observer,
                                         void *observer_arg);

int router_policy_forward_data_frame(router_ctx *ctx,
                                     uint8_t in_port,
                                     const uint8_t *pdu,
                                     size_t len,
                                     int offset,
                                     BACNET_ADDRESS *daddr,
                                     BACNET_ADDRESS *saddr,
                                     BACNET_NPDU_DATA *npdu,
                                     br_emit_fn emit,
                                     void *emit_arg,
                                     router_policy_emit_observer_fn observer,
                                     void *observer_arg);
