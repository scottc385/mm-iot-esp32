#include "router_policy.h"

#include "bacnet/npdu.h"

#include <stdlib.h>
#include <string.h>

port_info *router_policy_find_port_by_net(router_ctx *ctx, uint16_t net)
{
    if (!ctx) return NULL;
    for (size_t i = 0; i < ctx->nports; i++) {
        if (ctx->ports[i].net == net) return &ctx->ports[i];
        for (dnet_entry *e = ctx->ports[i].dnets; e; e = e->next) {
            if (e->net == net) return &ctx->ports[i];
        }
    }
    return NULL;
}

port_info *router_policy_find_port_by_id(router_ctx *ctx, uint8_t id)
{
    if (!ctx) return NULL;
    for (size_t i = 0; i < ctx->nports; i++) {
        if (ctx->ports[i].port_id == id) return &ctx->ports[i];
    }
    return NULL;
}

bool router_policy_is_local_port_net(router_ctx *ctx, uint16_t net)
{
    if (!ctx) return false;
    for (size_t i = 0; i < ctx->nports; i++) {
        if (ctx->ports[i].net == net) return true;
    }
    return false;
}

dnet_entry *router_policy_add_dnet(port_info *port, uint16_t net)
{
    if (!port) return NULL;
    for (dnet_entry *e = port->dnets; e; e = e->next) {
        if (e->net == net) {
            return e;
        }
    }
    dnet_entry *e = (dnet_entry *)malloc(sizeof(*e));
    if (!e) return NULL;
    e->net = net;
    e->last_ms = 0;
    e->configured_static = false;
    e->nh_len = 0;
    memset(e->nh_adr, 0, sizeof(e->nh_adr));
    e->peer_have_ipv6 = false;
    memset(e->peer_ipv6, 0, sizeof(e->peer_ipv6));
    e->next = port->dnets;
    port->dnets = e;
    return e;
}

dnet_entry *router_policy_find_dnet_entry(router_ctx *ctx, uint16_t net, uint8_t *port_id_out)
{
    if (!ctx) return NULL;
    for (size_t i = 0; i < ctx->nports; i++) {
        for (dnet_entry *e = ctx->ports[i].dnets; e; e = e->next) {
            if (e->net == net) {
                if (port_id_out) {
                    *port_id_out = ctx->ports[i].port_id;
                }
                return e;
            }
        }
    }
    return NULL;
}

void router_policy_remove_dnet_from_other_ports(router_ctx *ctx, uint16_t net, uint8_t keep_port_id)
{
    if (!ctx) return;
    for (size_t i = 0; i < ctx->nports; i++) {
        port_info *p = &ctx->ports[i];
        if (p->port_id == keep_port_id) continue;
        dnet_entry **pp = &p->dnets;
        while (*pp) {
            dnet_entry *e = *pp;
            if (e->net == net) {
                *pp = e->next;
                free(e);
                continue;
            }
            pp = &e->next;
        }
    }
}

bool router_policy_process_iar(router_ctx *ctx,
                               port_info *srcp,
                               const uint8_t *apdu,
                               int apdu_len,
                               const uint8_t *sadr,
                               size_t sadr_len,
                               uint8_t src_flags)
{
    if (!ctx || !srcp || !apdu || apdu_len <= 0) return false;
    bool net_owner = (src_flags & BR_SRC_FLAG_NET_OWNER) != 0;
    bool learned_any = false;
    for (int i = 0; i + 1 < apdu_len; i += 2) {
        uint16_t net = (uint16_t)((apdu[i] << 8) | apdu[i + 1]);
        if (router_policy_is_local_port_net(ctx, net)) {
            continue;
        }
        if (!net_owner) {
            uint8_t existing_port = 0;
            dnet_entry *existing = router_policy_find_dnet_entry(ctx, net, &existing_port);
            if (existing) {
                if (existing->configured_static) {
                    continue;
                }
                if (existing_port == srcp->port_id) {
                    if (net_owner && sadr && sadr_len == 16) {
                        existing->peer_have_ipv6 = true;
                        memcpy(existing->peer_ipv6, sadr, 16);
                    }
                    existing->last_ms = ctx->clock_ms;
                    continue;
                }
                if (existing->last_ms != 0 &&
                    (ctx->clock_ms - existing->last_ms) <= BR_IAR_ADVISORY_STALE_MS) {
                    continue;
                }
                router_policy_remove_dnet_from_other_ports(ctx, net, srcp->port_id);
            }
        } else {
            dnet_entry *existing = router_policy_find_dnet_entry(ctx, net, NULL);
            if (existing && existing->configured_static) {
                continue;
            }
            router_policy_remove_dnet_from_other_ports(ctx, net, srcp->port_id);
        }

        dnet_entry *e = router_policy_add_dnet(srcp, net);
        if (e) {
            if (net_owner && sadr && sadr_len == 16) {
                e->peer_have_ipv6 = true;
                memcpy(e->peer_ipv6, sadr, 16);
            }
            if (e->last_ms == 0) {
                ctx->last_dnet_change_port = (int8_t)srcp->port_id;
                ctx->last_dnet_change_net = net;
                learned_any = true;
            }
            e->last_ms = ctx->clock_ms;
        }
    }
    return learned_any;
}

static bool list_contains_net(const uint16_t *nets, size_t count, uint16_t net)
{
    for (size_t i = 0; i < count; i++) {
        if (nets[i] == net) return true;
    }
    return false;
}

static void router_policy_forward_nlm_to_other_ports(router_ctx *ctx,
                                                     uint8_t in_port,
                                                     uint8_t network_message_type,
                                                     const uint8_t *apdu,
                                                     int apdu_len,
                                                     const BACNET_ADDRESS *source,
                                                     br_emit_fn emit,
                                                     void *emit_arg)
{
    if (!ctx || !apdu || apdu_len < 0 || !emit) {
        return;
    }

    BACNET_NPDU_DATA n = {0};
    BACNET_ADDRESS dest = {0};
    BACNET_ADDRESS src = {0};
    n.network_layer_message = true;
    n.network_message_type = network_message_type;
    n.protocol_version = BACNET_PROTOCOL_VERSION;
    n.hop_count = 0xFF;
    dest.net = BACNET_BROADCAST_NETWORK;
    dest.len = 0;
    if (source) {
        src = *source;
    }

    int nlen = npdu_encode_pdu(ctx->scratch, &dest, &src, &n);
    if (nlen <= 0 || (size_t)nlen + (size_t)apdu_len > sizeof(ctx->scratch)) {
        return;
    }

    memcpy(ctx->scratch + nlen, apdu, (size_t)apdu_len);
    size_t out_len = (size_t)nlen + (size_t)apdu_len;
    for (size_t i = 0; i < ctx->nports; i++) {
        uint8_t out_port = ctx->ports[i].port_id;
        if (out_port == in_port) continue;
        emit(emit_arg, out_port, ctx->scratch, out_len, &dest);
    }
}

int router_policy_build_iar_response(router_ctx *ctx,
                                     uint8_t in_port,
                                     uint16_t requested_net,
                                     BACNET_ADDRESS *source,
                                     uint8_t *out,
                                     size_t out_cap,
                                     BACNET_ADDRESS *dest_out)
{
    if (!ctx || !source || !out || out_cap == 0 || !dest_out) return 0;

    port_info *in_port_info = router_policy_find_port_by_id(ctx, in_port);

    if (requested_net != BACNET_BROADCAST_NETWORK) {
        port_info *reachable_port = router_policy_find_port_by_net(ctx, requested_net);
        if (!reachable_port || reachable_port->port_id == in_port) {
            return 0;
        }
    }

    BACNET_NPDU_DATA n = {0};
    n.network_layer_message = true;
    n.network_message_type = NETWORK_MESSAGE_I_AM_ROUTER_TO_NETWORK;
    n.protocol_version = BACNET_PROTOCOL_VERSION;
    n.hop_count = 0xFF;

    BACNET_ADDRESS dest = {0};
    dest.net = BACNET_BROADCAST_NETWORK;
    dest.len = 0;

    int nlen = npdu_encode_pdu(out, &dest, source, &n);
    if (nlen <= 0 || (size_t)nlen >= out_cap) {
        return 0;
    }

    if (requested_net != BACNET_BROADCAST_NETWORK) {
        if ((size_t)nlen + 2 > out_cap) return 0;
        out[nlen++] = (requested_net >> 8) & 0xFF;
        out[nlen++] = requested_net & 0xFF;
    } else {
        uint16_t nets[16];
        size_t nets_count = 0;
        for (size_t i = 0; i < ctx->nports; i++) {
            if (ctx->ports[i].port_id != in_port &&
                ctx->ports[i].net != 0 &&
                !list_contains_net(nets, nets_count, ctx->ports[i].net) &&
                nets_count < (sizeof(nets) / sizeof(nets[0]))) {
                nets[nets_count++] = ctx->ports[i].net;
            }
            for (dnet_entry *e = ctx->ports[i].dnets; e; e = e->next) {
                if (in_port_info && e->net == in_port_info->net) continue;
                if (list_contains_net(nets, nets_count, e->net)) continue;
                if (nets_count >= (sizeof(nets) / sizeof(nets[0]))) break;
                nets[nets_count++] = e->net;
            }
        }
        for (size_t i = 0; i < nets_count; i++) {
            if ((size_t)nlen + 2 > out_cap) break;
            out[nlen++] = (nets[i] >> 8) & 0xFF;
            out[nlen++] = nets[i] & 0xFF;
        }
    }

    *dest_out = dest;
    return nlen;
}

void router_policy_process_iam(router_ctx *ctx,
                               port_info *srcp,
                               BACNET_ADDRESS *daddr,
                               const BACNET_ADDRESS *saddr,
                               const uint8_t *sadr,
                               size_t sadr_len,
                               router_policy_peer_change_fn peer_observer,
                               void *peer_observer_arg)
{
    if (!ctx || !srcp || !daddr || !saddr) return;

    if (saddr->net != 0 && saddr->net != srcp->net &&
        !router_policy_is_local_port_net(ctx, saddr->net)) {
        port_info *learn_port = srcp;
        if (ctx->nports >= 2 && srcp->port_id == 0 && saddr->len == 6) {
            port_info *other = NULL;
            for (size_t i = 0; i < ctx->nports; i++) {
                if (ctx->ports[i].port_id != srcp->port_id) {
                    other = &ctx->ports[i];
                    break;
                }
            }
            if (other) {
                learn_port = other;
            }
        }
        dnet_entry *e = router_policy_add_dnet(learn_port, saddr->net);
        if (e) {
            if (saddr->len == 6) {
                e->nh_len = 6;
                memcpy(e->nh_adr, saddr->adr, 6);
            } else if (sadr_len == 6 && sadr) {
                e->nh_len = 6;
                memcpy(e->nh_adr, sadr, 6);
            }
            if (sadr_len == 16 && sadr) {
                uint8_t old_peer[16];
                bool had_peer = e->peer_have_ipv6;
                if (had_peer) {
                    memcpy(old_peer, e->peer_ipv6, 16);
                }
                e->peer_have_ipv6 = true;
                memcpy(e->peer_ipv6, sadr, 16);
                if (peer_observer) {
                    if (!had_peer) {
                        peer_observer(peer_observer_arg, saddr->net, NULL, e->peer_ipv6, true);
                    } else if (memcmp(old_peer, e->peer_ipv6, 16) != 0) {
                        peer_observer(peer_observer_arg, saddr->net, old_peer, e->peer_ipv6, false);
                    }
                }
            }
            if (e->last_ms == 0) {
                ctx->last_dnet_change_port = (int8_t)learn_port->port_id;
                ctx->last_dnet_change_net = saddr->net;
            }
            e->last_ms = ctx->clock_ms;
        }
    }

    /* Forward local-only I-Am across the router so remote
       clients can discover the device. Treat as broadcast. */
    if (daddr->net == 0 && daddr->len == 0) {
        daddr->net = BACNET_BROADCAST_NETWORK;
    }
}

static void router_policy_learn_source_net(router_ctx *ctx,
                                           port_info *srcp,
                                           const BACNET_ADDRESS *saddr)
{
    if (!ctx || !srcp || !saddr) return;
    if (saddr->net == 0 || saddr->net == srcp->net ||
        router_policy_is_local_port_net(ctx, saddr->net)) {
        return;
    }

    uint8_t existing_port = 0;
    dnet_entry *existing = router_policy_find_dnet_entry(ctx, saddr->net, &existing_port);
    if (existing) {
        if (existing->configured_static) {
            return;
        }
        if (existing_port == srcp->port_id) {
            existing->last_ms = ctx->clock_ms;
            return;
        }
        router_policy_remove_dnet_from_other_ports(ctx, saddr->net, srcp->port_id);
    }

    dnet_entry *entry = router_policy_add_dnet(srcp, saddr->net);
    if (entry) {
        if (entry->last_ms == 0) {
            ctx->last_dnet_change_port = (int8_t)srcp->port_id;
            ctx->last_dnet_change_net = saddr->net;
        }
        entry->last_ms = ctx->clock_ms;
    }
}

void router_policy_handle_application_frame(router_ctx *ctx,
                                            port_info *srcp,
                                            BACNET_ADDRESS *daddr,
                                            const BACNET_ADDRESS *saddr,
                                            const uint8_t *sadr,
                                            size_t sadr_len,
                                            const uint8_t *apdu,
                                            size_t apdu_len,
                                            router_policy_peer_change_fn peer_observer,
                                            void *peer_observer_arg)
{
    if (!ctx || !srcp || !daddr || !saddr || !apdu || apdu_len < 2) return;

    uint8_t pdu_type = apdu[0] & 0xF0;
    uint8_t service = apdu[1];
    if (pdu_type == PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST &&
        service == SERVICE_UNCONFIRMED_I_AM) {
        router_policy_process_iam(ctx, srcp, daddr, saddr, sadr, sadr_len,
                                  peer_observer, peer_observer_arg);
    }
}

int router_policy_prepare_frame(router_ctx *ctx,
                                uint8_t in_port,
                                const uint8_t *pdu,
                                size_t len,
                                const uint8_t *src_mac,
                                size_t src_mac_len,
                                BACNET_ADDRESS *daddr,
                                BACNET_ADDRESS *saddr,
                                BACNET_NPDU_DATA *npdu,
                                port_info **srcp_out)
{
    if (!ctx || !pdu || !daddr || !saddr || !npdu || !srcp_out) return -1;

    int offset = bacnet_npdu_decode(pdu, len, daddr, saddr, npdu);
    if (offset < 0) return -1;

    if (npdu->hop_count == 0) npdu->hop_count = 0xFF;

    port_info *srcp = router_policy_find_port_by_id(ctx, in_port);
    if (!srcp) return -1;

    if (saddr->net == 0 && saddr->len == 0) {
        saddr->net = srcp->net;
        if (src_mac && src_mac_len > 0) {
            size_t copy_len = src_mac_len;
            if (copy_len > sizeof(saddr->adr)) copy_len = sizeof(saddr->adr);
            saddr->len = (uint8_t)copy_len;
            memcpy(saddr->adr, src_mac, copy_len);
        } else {
            saddr->len = 0;
        }
    }

    *srcp_out = srcp;
    return offset;
}

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
                                         void *observer_arg)
{
    if (!ctx || !srcp || !apdu || apdu_len < 0) return -1;
    switch (network_message_type) {
        case NETWORK_MESSAGE_I_AM_ROUTER_TO_NETWORK: {
            bool learned_any = router_policy_process_iar(ctx, srcp, apdu, apdu_len,
                                                         sadr, sadr_len, src_flags);
            ctx->last_iar_rx_port = (int8_t)srcp->port_id;
            if (observer) {
                observer(observer_arg, ROUTER_POLICY_NLM_IAR_RX, in_port, learned_any);
            }
            router_policy_forward_nlm_to_other_ports(ctx, in_port,
                                                     NETWORK_MESSAGE_I_AM_ROUTER_TO_NETWORK,
                                                     apdu, apdu_len, saddr,
                                                     emit, emit_arg);
            return 0;
        }
        case NETWORK_MESSAGE_WHO_IS_ROUTER_TO_NETWORK: {
            uint16_t net = BACNET_BROADCAST_NETWORK;
            if (apdu_len >= 2) {
                net = (uint16_t)((apdu[0] << 8) | apdu[1]);
            }
            if (net != BACNET_BROADCAST_NETWORK) {
                port_info *reachable_port = router_policy_find_port_by_net(ctx, net);
                if (reachable_port && reachable_port->port_id == in_port) {
                    return 0;
                }
            }
            uint8_t out[64];
            BACNET_ADDRESS dest = {0};
            BACNET_ADDRESS source = {0};
            if (saddr) {
                source = *saddr;
            }
            int nlen = router_policy_build_iar_response(ctx, in_port, net, &source, out, sizeof(out), &dest);
            if (nlen > 0 && emit) {
                emit(emit_arg, in_port, out, (size_t)nlen, &dest);
                if (observer) {
                    observer(observer_arg, ROUTER_POLICY_NLM_IAR_EMIT, in_port, true);
                }
            } else {
                router_policy_forward_nlm_to_other_ports(ctx, in_port,
                                                         NETWORK_MESSAGE_WHO_IS_ROUTER_TO_NETWORK,
                                                         apdu, apdu_len, saddr,
                                                         emit, emit_arg);
            }
            return 0;
        }
        default:
            return 0;
    }
}

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
                                     void *observer_arg)
{
    if (!ctx || !pdu || !daddr || !saddr || !npdu || !emit) return -1;
    int apdu_len = (int)len - offset;
    if (apdu_len < 0) return -1;

    if (daddr->net == BACNET_BROADCAST_NETWORK || daddr->net == 0xFFFF) {
        if (npdu->hop_count != 0xFF && npdu->hop_count > 0) npdu->hop_count--;
        daddr->net = BACNET_BROADCAST_NETWORK;
        daddr->len = 0;
        int npdu_len = npdu_encode_pdu(ctx->scratch, daddr, saddr, npdu);
        memcpy(ctx->scratch + npdu_len, pdu + offset, (size_t)apdu_len);
        size_t outlen = (size_t)(npdu_len + apdu_len);
        for (size_t i = 0; i < ctx->nports; i++) {
            uint8_t out_port = ctx->ports[i].port_id;
            if (out_port == in_port) continue;
            emit(emit_arg, out_port, ctx->scratch, outlen, daddr);
            if (observer) {
                observer(observer_arg, in_port, out_port, saddr, daddr,
                         pdu + offset, (size_t)apdu_len, "bcast");
            }
        }
        return 0;
    }

    port_info *dstp = router_policy_find_port_by_net(ctx, daddr->net);
    if (!dstp) return -1;

    bool routed = (dstp->net != daddr->net);
    int npdu_len;
    if (routed) {
        if (npdu->hop_count != 0xFF && npdu->hop_count > 0) npdu->hop_count--;
        npdu_len = npdu_encode_pdu(ctx->scratch, daddr, saddr, npdu);
    } else {
        npdu_len = npdu_encode_pdu(ctx->scratch, NULL, saddr, npdu);
    }
    memcpy(ctx->scratch + npdu_len, pdu + offset, (size_t)apdu_len);
    size_t outlen = (size_t)(npdu_len + apdu_len);

    if (dstp->port_id == in_port) {
        return 0;
    }
    emit(emit_arg, dstp->port_id, ctx->scratch, outlen, daddr);
    if (observer) {
        observer(observer_arg, in_port, dstp->port_id, saddr, daddr,
                 pdu + offset, (size_t)apdu_len, routed ? "routed" : "local");
    }
    return 0;
}

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
                                              void *fwd_observer_arg)
{
    BACNET_ADDRESS daddr = {0}, saddr = {0};
    BACNET_NPDU_DATA npdu = {0};
    port_info *srcp = NULL;
    int offset = router_policy_prepare_frame(ctx, in_port, pdu, len, sadr, sadr_len,
                                             &daddr, &saddr, &npdu, &srcp);
    if (offset < 0) return -1;

    router_policy_learn_source_net(ctx, srcp, &saddr);

    if (npdu.network_layer_message) {
        int apdu_len = (int)len - offset;
        return router_policy_handle_network_message(ctx, in_port, srcp, npdu.network_message_type,
                                                    pdu + offset, apdu_len, &saddr, sadr, sadr_len,
                                                    src_flags, emit, emit_arg,
                                                    nlm_observer, nlm_observer_arg);
    }

    router_policy_handle_application_frame(ctx, srcp, &daddr, &saddr, sadr, sadr_len,
                                           pdu + offset, (size_t)((int)len - offset),
                                           peer_observer, peer_observer_arg);

    return router_policy_forward_data_frame(ctx, in_port, pdu, len, offset, &daddr, &saddr, &npdu,
                                            emit, emit_arg, fwd_observer, fwd_observer_arg);
}

int router_policy_handle_frame(router_ctx *ctx,
                               uint8_t in_port,
                               const uint8_t *pdu,
                               size_t len,
                               const uint8_t *sadr,
                               size_t sadr_len,
                               uint8_t src_flags,
                               br_emit_fn emit,
                               void *emit_arg)
{
    return router_policy_handle_frame_with_observers(ctx, in_port, pdu, len, sadr, sadr_len,
                                                     src_flags, emit, emit_arg,
                                                     NULL, NULL, NULL, NULL, NULL, NULL);
}

void router_policy_tick(router_ctx *ctx, uint32_t now_ms)
{
    if (!ctx || ctx->dnet_ttl_ms == 0) return;
    for (size_t i = 0; i < ctx->nports; i++) {
        dnet_entry **pp = &ctx->ports[i].dnets;
        while (*pp) {
            dnet_entry *e = *pp;
            if (!e->configured_static && now_ms - e->last_ms > ctx->dnet_ttl_ms) {
                *pp = e->next;
                free(e);
            } else {
                pp = &e->next;
            }
        }
    }
}
