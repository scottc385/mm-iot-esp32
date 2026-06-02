#include "router_core.h"
#include "router_policy.h"

#include "bacnet/bacdcode.h"
#include "bacnet/bacenum.h"
#include "bacnet/bactext.h"
#include "bacnet/bacapp.h"
#include "bacnet/iam.h"
#include "bacnet/rp.h"
#include "bacnet/wp.h"
#include "bacnet/npdu.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#ifndef ROUTER_CORE_ENABLE_LEGACY
#define ROUTER_CORE_ENABLE_LEGACY 1
#endif

void br_init(router_ctx *ctx, const br_port_cfg *ports, size_t nports)
{
    if (!ctx || !ports || nports > 8) return;
    memset(ctx, 0, sizeof(*ctx));
    for (size_t i = 0; i < nports; i++) {
        ctx->ports[i].net = ports[i].net;
        ctx->ports[i].port_id = ports[i].port_id;
    }
    ctx->nports = nports;
    ctx->dnet_ttl_ms = 0; /* 0 = never expire */
    ctx->log_level = 2;
    ctx->last_iar_rx_port = -1;
    ctx->last_dnet_change_port = -1;
    ctx->last_dnet_change_net = 0;
}

void br_set_time(router_ctx *ctx, uint32_t now_ms)
{
    if (!ctx) return;
    ctx->clock_ms = now_ms;
}

void br_set_dnet_ttl(router_ctx *ctx, uint32_t ttl_ms)
{
    if (!ctx) return;
    ctx->dnet_ttl_ms = ttl_ms;
}

void br_set_log_level(router_ctx *ctx, int level)
{
    if (!ctx) return;
    ctx->log_level = level;
}

int br_get_log_level(router_ctx *ctx)
{
    if (!ctx) return 0;
    return ctx->log_level;
}

const char *br_get_log(router_ctx *ctx, size_t *len_out)
{
    if (!ctx) return NULL;
    if (len_out) *len_out = ctx->log_len;
    if (ctx->log_len < sizeof(ctx->log_buf)) {
        ctx->log_buf[ctx->log_len] = '\0';
    }
    return ctx->log_buf;
}

void br_clear_log(router_ctx *ctx)
{
    if (!ctx) return;
    ctx->log_len = 0;
    if (sizeof(ctx->log_buf) > 0) ctx->log_buf[0] = '\0';
}

void br_clear_peers(router_ctx *ctx)
{
    if (!ctx) return;
    for (size_t i = 0; i < ctx->nports; i++) {
        for (dnet_entry *e = ctx->ports[i].dnets; e; e = e->next) {
            e->peer_have_ipv6 = false;
            memset(e->peer_ipv6, 0, sizeof(e->peer_ipv6));
        }
    }
    for (size_t i = 0; i < sizeof(ctx->peer_log) / sizeof(ctx->peer_log[0]); i++) {
        ctx->peer_log[i].valid = false;
        ctx->peer_log[i].dnet = 0;
        ctx->peer_log[i].tail[0] = 0;
        ctx->peer_log[i].tail[1] = 0;
    }
}

int br_take_iar_rx_port(router_ctx *ctx)
{
    if (!ctx) return -1;
    int port = ctx->last_iar_rx_port;
    ctx->last_iar_rx_port = -1;
    return port;
}

bool br_take_dnet_change(router_ctx *ctx, uint8_t *port_id, uint16_t *net)
{
    if (!ctx) return false;
    if (ctx->last_dnet_change_port < 0) return false;
    if (port_id) *port_id = (uint8_t)ctx->last_dnet_change_port;
    if (net) *net = ctx->last_dnet_change_net;
    ctx->last_dnet_change_port = -1;
    ctx->last_dnet_change_net = 0;
    return true;
}

bool br_lookup_next_hop(router_ctx *ctx, uint8_t port_id, uint16_t dnet, BACNET_ADDRESS *out)
{
    if (!ctx || !out) return false;
    port_info *p = router_policy_find_port_by_id(ctx, port_id);
    if (!p) return false;
    for (dnet_entry *e = p->dnets; e; e = e->next) {
        if (e->net == dnet && e->nh_len == 6) {
            out->len = 6;
            out->net = dnet;
            memcpy(out->adr, e->nh_adr, 6);
            return true;
        }
    }
    return false;
}

bool br_lookup_peer(router_ctx *ctx, uint16_t dnet, uint8_t *peer, uint8_t *peer_len)
{
    if (!ctx) return false;
    for (size_t i = 0; i < ctx->nports; i++) {
        for (dnet_entry *e = ctx->ports[i].dnets; e; e = e->next) {
            if (e->net == dnet && e->peer_have_ipv6) {
                if (peer && peer_len) {
                    *peer_len = 16;
                    memcpy(peer, e->peer_ipv6, 16);
                }
                return true;
            }
        }
    }
    return false;
}

static void rc_log(router_ctx *ctx, int level, const char *fmt, ...)
{
    if (!ctx || level > ctx->log_level) return;
    if (ctx->log_len >= sizeof(ctx->log_buf) - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int remaining = (int)(sizeof(ctx->log_buf) - ctx->log_len - 1);
    int written = vsnprintf(ctx->log_buf + ctx->log_len, (size_t)remaining, fmt, ap);
    va_end(ap);
    if (written < 0) return;
    if (written > remaining) written = remaining;
    ctx->log_len += (size_t)written;
}

static void format_msg_summary(router_ctx *ctx,
                               uint8_t in_port,
                               uint8_t out_port,
                               const BACNET_ADDRESS *saddr,
                               const BACNET_ADDRESS *daddr,
                               const uint8_t *apdu,
                               size_t apdu_len,
                               const char *path)
{
    if (!ctx || !apdu || apdu_len == 0) return;

    uint8_t pdu_type = apdu[0] & 0xF0;
    uint8_t service = (apdu_len > 1) ? apdu[1] : 0;

    const char *pdu_name = "PDU";
    const char *svc_name = "";
    char detail[96] = {0};

    if (pdu_type == PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST) {
        pdu_name = "Unconfirmed-REQ";
        if (service == SERVICE_UNCONFIRMED_I_AM) {
            uint32_t dev_id = 0;
            if (apdu_len >= 2) {
                iam_decode_service_request(apdu + 2, &dev_id, NULL, NULL, NULL);
            }
            svc_name = "i-am";
            snprintf(detail, sizeof(detail), "device,%" PRIu32, dev_id);
        } else if (service == SERVICE_UNCONFIRMED_WHO_IS) {
            svc_name = "who-is";
        }
    } else if (pdu_type == PDU_TYPE_CONFIRMED_SERVICE_REQUEST) {
        pdu_name = "Confirmed-REQ";
        if (service == SERVICE_CONFIRMED_READ_PROPERTY) {
            svc_name = "readProperty";
        }
        if (service == SERVICE_CONFIRMED_READ_PROPERTY && apdu_len > 4) {
            BACNET_READ_PROPERTY_DATA rpdata = {0};
            if (rp_decode_service_request(apdu + 4, (unsigned)apdu_len - 4, &rpdata) > 0) {
                snprintf(detail, sizeof(detail), "%s,%" PRIu32 " %s",
                         bactext_object_type_name(rpdata.object_type), rpdata.object_instance,
                         bactext_property_name(rpdata.object_property));
                if (rpdata.array_index != BACNET_ARRAY_ALL) {
                    char tmp[24];
                    snprintf(tmp, sizeof(tmp), "[%u]", (unsigned)rpdata.array_index);
                    strncat(detail, tmp, sizeof(detail) - strlen(detail) - 1);
                }
            }
        } else if (service == SERVICE_CONFIRMED_WRITE_PROPERTY) {
            svc_name = "writeProperty";
        }
        if (service == SERVICE_CONFIRMED_WRITE_PROPERTY && apdu_len > 4) {
            BACNET_WRITE_PROPERTY_DATA wpdata = {0};
            if (wp_decode_service_request(apdu + 4, (unsigned)apdu_len - 4, &wpdata) > 0) {
                snprintf(detail, sizeof(detail), "%s,%" PRIu32 " %s",
                         bactext_object_type_name(wpdata.object_type), wpdata.object_instance,
                         bactext_property_name(wpdata.object_property));
                if (wpdata.array_index != BACNET_ARRAY_ALL) {
                    char tmp[24];
                    snprintf(tmp, sizeof(tmp), "[%u]", (unsigned)wpdata.array_index);
                    strncat(detail, tmp, sizeof(detail) - strlen(detail) - 1);
                }
            }
        }
    } else if (pdu_type == PDU_TYPE_COMPLEX_ACK) {
        pdu_name = "Complex-ACK";
    } else if (pdu_type == PDU_TYPE_ERROR) {
        pdu_name = "Error";
    }

    rc_log(ctx, 4,
           "routed %u->%u %s snet=%u dnet=%u %s %s %s\n",
           in_port, out_port, path,
           saddr ? saddr->net : 0, daddr ? daddr->net : 0,
           pdu_name, svc_name, detail);
}

static void log_peer_change(router_ctx *ctx,
                            uint16_t dnet,
                            const uint8_t *old_peer,
                            const uint8_t *new_peer,
                            bool is_new);

static void forward_emit_observer(void *arg,
                                  uint8_t in_port,
                                  uint8_t out_port,
                                  const BACNET_ADDRESS *saddr,
                                  const BACNET_ADDRESS *daddr,
                                  const uint8_t *apdu,
                                  size_t apdu_len,
                                  const char *path)
{
    router_ctx *ctx = (router_ctx *)arg;
    format_msg_summary(ctx, in_port, out_port, saddr, daddr, apdu, apdu_len, path);
}

static void peer_change_observer(void *arg,
                                 uint16_t dnet,
                                 const uint8_t *old_peer,
                                 const uint8_t *new_peer,
                                 bool is_new)
{
    router_ctx *ctx = (router_ctx *)arg;
    log_peer_change(ctx, dnet, old_peer, new_peer, is_new);
}

static void nlm_observer(void *arg,
                         router_policy_nlm_event_t event,
                         uint8_t in_port,
                         bool learned_any)
{
    router_ctx *ctx = (router_ctx *)arg;
    if (!ctx) return;
    if (event == ROUTER_POLICY_NLM_IAR_RX) {
        if (learned_any) {
            rc_log(ctx, 4, "I-AR rx: learned dnets from port=%u\n", in_port);
        }
    } else if (event == ROUTER_POLICY_NLM_IAR_EMIT) {
        rc_log(ctx, 4, "I-AR emit port=%u\n", in_port);
    }
}

static void log_peer_change(router_ctx *ctx,
                            uint16_t dnet,
                            const uint8_t *old_peer,
                            const uint8_t *new_peer,
                            bool is_new)
{
    if (!ctx || ctx->log_level < 1 || !new_peer) return;
    uint8_t tail0 = new_peer[14];
    uint8_t tail1 = new_peer[15];
    bool found = false;
    for (size_t i = 0; i < sizeof(ctx->peer_log) / sizeof(ctx->peer_log[0]); i++) {
        if (ctx->peer_log[i].valid && ctx->peer_log[i].dnet == dnet) {
            found = true;
            if (ctx->peer_log[i].tail[0] == tail0 && ctx->peer_log[i].tail[1] == tail1) {
                return;
            }
            ctx->peer_log[i].tail[0] = tail0;
            ctx->peer_log[i].tail[1] = tail1;
            break;
        }
    }
    if (!found) {
        for (size_t i = 0; i < sizeof(ctx->peer_log) / sizeof(ctx->peer_log[0]); i++) {
            if (!ctx->peer_log[i].valid) {
                ctx->peer_log[i].valid = true;
                ctx->peer_log[i].dnet = dnet;
                ctx->peer_log[i].tail[0] = tail0;
                ctx->peer_log[i].tail[1] = tail1;
                found = true;
                break;
            }
        }
    }
    char old_tail[6] = "????";
    char new_tail[6] = "????";
    snprintf(new_tail, sizeof(new_tail), "%02x%02x", tail0, tail1);
    if (old_peer) {
        snprintf(old_tail, sizeof(old_tail), "%02x%02x", old_peer[14], old_peer[15]);
    }
    if (is_new) {
        rc_log(ctx, 1, "router learn dnet=%u peer=:%s\n", dnet, new_tail);
    } else {
        rc_log(ctx, 1, "router update dnet=%u peer :%s -> :%s\n", dnet, old_tail, new_tail);
    }
}

#if ROUTER_CORE_ENABLE_LEGACY
int br_handle_frame_legacy(router_ctx *ctx,
                           uint8_t in_port,
                           const uint8_t *pdu,
                           size_t len,
                           const uint8_t *sadr,
                           size_t sadr_len,
                           uint8_t src_flags,
                           br_emit_fn emit,
                           void *emit_arg)
{
    BACNET_ADDRESS daddr = {0}, saddr = {0};
    BACNET_NPDU_DATA npdu = {0};
    port_info *srcp = NULL;
    int offset = router_policy_prepare_frame(ctx, in_port, pdu, len, sadr, sadr_len,
                                             &daddr, &saddr, &npdu, &srcp);
    if (offset < 0) return -1;

    if (npdu.network_layer_message) {
        int apdu_len = (int)len - offset;
        return router_policy_handle_network_message(ctx, in_port, srcp, npdu.network_message_type,
                                                    pdu + offset, apdu_len, &saddr, sadr, sadr_len,
                                                    src_flags, emit, emit_arg, nlm_observer, ctx);
    }

    /* Application-frame policy (currently handles Unconfirmed I-Am learning). */
    router_policy_handle_application_frame(ctx, srcp, &daddr, &saddr, sadr, sadr_len,
                                           pdu + offset, (size_t)((int)len - offset),
                                           peer_change_observer, ctx);

    return router_policy_forward_data_frame(ctx, in_port, pdu, len, offset, &daddr, &saddr, &npdu,
                                            emit, emit_arg, forward_emit_observer, ctx);
}
#endif

int br_handle_frame(router_ctx *ctx,
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
                                                     nlm_observer, ctx,
                                                     peer_change_observer, ctx,
                                                     forward_emit_observer, ctx);
}

/* Legacy wrapper kept behind compile-time gate for parity testing. */
#if ROUTER_CORE_ENABLE_LEGACY
void br_tick_legacy(router_ctx *ctx, uint32_t now_ms)
{
    router_policy_tick(ctx, now_ms);
}
#endif

void br_tick(router_ctx *ctx, uint32_t now_ms)
{
    router_policy_tick(ctx, now_ms);
}
