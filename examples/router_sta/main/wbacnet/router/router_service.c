#include "router_service.h"

#include "bacnet/bacdef.h"
#include "bacnet/bacdcode.h"
#include "bacnet/iam.h"
#include "bacnet/npdu.h"
#include "router_core.h"
#include "router_policy.h"

#include <string.h>

#define TB_ROUTER_SERVICE_MAX_PORTS 10
#define TB_ROUTER_SERVICE_CORE_STORAGE_SIZE 4096
#define TB_ROUTER_SERVICE_PORT_STORAGE_SIZE 32

typedef struct {
    union {
        uint8_t bytes[TB_ROUTER_SERVICE_CORE_STORAGE_SIZE];
        uint64_t align_u64;
        void *align_ptr;
    } router_core_storage;
    union {
        uint8_t bytes[TB_ROUTER_SERVICE_PORT_STORAGE_SIZE * TB_ROUTER_SERVICE_MAX_PORTS];
        uint64_t align_u64;
        void *align_ptr;
    } port_storage;
    size_t port_count;
    void *user_ctx;
} tb_router_service_impl;

_Static_assert(sizeof(router_ctx) <= TB_ROUTER_SERVICE_CORE_STORAGE_SIZE,
               "TB_ROUTER_SERVICE_CORE_STORAGE_SIZE is too small for router_ctx");
_Static_assert(TB_ROUTER_SRC_FLAG_NET_OWNER == BR_SRC_FLAG_NET_OWNER,
               "router service NET_OWNER flag must match router core");
_Static_assert(sizeof(tb_router_service_impl) <= TB_ROUTER_SERVICE_STORAGE_SIZE,
               "TB_ROUTER_SERVICE_STORAGE_SIZE is too small for tb_router_service_impl");

struct tb_router_port {
    bool active;
    uint8_t port_id;
    uint16_t net;
    tb_router_transport_kind kind;
    uint32_t caps;
    void *transport_state;
    const tb_router_transport_ops *ops;
};

_Static_assert(sizeof(tb_router_port) <= TB_ROUTER_SERVICE_PORT_STORAGE_SIZE,
               "TB_ROUTER_SERVICE_PORT_STORAGE_SIZE is too small for tb_router_port");

static tb_router_service_impl *service_impl(tb_router_service *svc)
{
    return svc ? (tb_router_service_impl *)svc->opaque.bytes : NULL;
}

static const tb_router_service_impl *service_impl_const(const tb_router_service *svc)
{
    return svc ? (const tb_router_service_impl *)svc->opaque.bytes : NULL;
}

static router_ctx *router_core(tb_router_service *svc)
{
    tb_router_service_impl *impl = service_impl(svc);
    return impl ? (router_ctx *)impl->router_core_storage.bytes : NULL;
}

static tb_router_port *router_port_at(tb_router_service *svc, size_t index)
{
    tb_router_service_impl *impl = service_impl(svc);
    if (!impl || index >= TB_ROUTER_SERVICE_MAX_PORTS) {
        return NULL;
    }
    return (tb_router_port *)&impl->port_storage.bytes[index * TB_ROUTER_SERVICE_PORT_STORAGE_SIZE];
}

static const tb_router_port *router_port_at_const(const tb_router_service *svc, size_t index)
{
    const tb_router_service_impl *impl = service_impl_const(svc);
    if (!impl || index >= TB_ROUTER_SERVICE_MAX_PORTS) {
        return NULL;
    }
    return (const tb_router_port *)&impl->port_storage.bytes[index * TB_ROUTER_SERVICE_PORT_STORAGE_SIZE];
}

static tb_router_port *find_port(tb_router_service *svc, uint8_t port_id)
{
    tb_router_service_impl *impl = service_impl(svc);
    if (!impl) {
        return NULL;
    }
    for (size_t i = 0; i < impl->port_count; i++) {
        tb_router_port *port = router_port_at(svc, i);
        if (port && port->active && port->port_id == port_id) {
            return port;
        }
    }
    return NULL;
}

static void router_service_emit(void *arg,
                                uint8_t out_port,
                                const uint8_t *pdu,
                                size_t len,
                                const BACNET_ADDRESS *daddr)
{
    tb_router_service *svc = (tb_router_service *)arg;
    tb_router_port *port = find_port(svc, out_port);
    if (!port || !port->ops || !port->ops->send_npdu) {
        return;
    }
    tb_router_service_impl *impl = service_impl(svc);
    port->ops->send_npdu(svc, impl ? impl->user_ctx : NULL, port, pdu, len, daddr);
}

static tb_router_route_snapshot make_route_snapshot(const port_info *port,
                                                    const dnet_entry *entry,
                                                    uint32_t now_ms)
{
    tb_router_route_snapshot route = {
        .port_id = port->port_id,
        .port_net = port->net,
        .dnet = entry->net,
        .age_ms = (entry->last_ms != 0 && entry->last_ms <= now_ms) ? (now_ms - entry->last_ms) : 0,
        .configured_static = entry->configured_static,
        .have_next_hop = entry->nh_len > 0,
        .next_hop_len = entry->nh_len,
        .have_peer = entry->peer_have_ipv6,
        .peer_len = entry->peer_have_ipv6 ? TB_ROUTER_PEER_ID_MAX_LEN : 0,
        .have_peer_ipv6 = entry->peer_have_ipv6,
    };
    if (entry->nh_len > 0) {
        size_t copy_len = entry->nh_len;
        if (copy_len > sizeof(route.next_hop)) {
            copy_len = sizeof(route.next_hop);
        }
        memcpy(route.next_hop, entry->nh_adr, copy_len);
    }
    if (entry->peer_have_ipv6) {
        memcpy(route.peer, entry->peer_ipv6, sizeof(route.peer));
        memcpy(route.peer_ipv6, entry->peer_ipv6, sizeof(route.peer_ipv6));
    }
    return route;
}

static bool tb_router_service_start(tb_router_service *svc, uint32_t dnet_ttl_ms, int log_level);
static bool tb_router_service_add_static_dnet(tb_router_service *svc, uint8_t port_id, uint16_t dnet);

static void tb_router_service_init(tb_router_service *svc, void *user_ctx)
{
    if (!svc) {
        return;
    }
    memset(svc, 0, sizeof(*svc));
    tb_router_service_impl *impl = service_impl(svc);
    impl->user_ctx = user_ctx;
}

static tb_router_port *tb_router_service_add_port(tb_router_service *svc,
                                                  uint8_t port_id,
                                                  uint16_t net,
                                                  tb_router_transport_kind kind,
                                                  uint32_t caps,
                                                  void *transport_state,
                                                  const tb_router_transport_ops *ops)
{
    tb_router_service_impl *impl = service_impl(svc);
    if (!impl || impl->port_count >= TB_ROUTER_SERVICE_MAX_PORTS) {
        return NULL;
    }
    for (size_t i = 0; i < impl->port_count; i++) {
        tb_router_port *existing = router_port_at(svc, i);
        if (existing && existing->active && existing->port_id == port_id) {
            return NULL;
        }
    }

    tb_router_port *port = router_port_at(svc, impl->port_count++);
    memset(port, 0, sizeof(*port));
    port->active = true;
    port->port_id = port_id;
    port->net = net;
    port->kind = kind;
    port->caps = caps;
    port->transport_state = transport_state;
    port->ops = ops;
    return port;
}

bool tb_router_service_configure(tb_router_service *svc,
                                 void *user_ctx,
                                 const tb_router_port_config *ports,
                                 size_t port_count,
                                 uint32_t dnet_ttl_ms,
                                 int log_level)
{
    if (!svc || (!ports && port_count > 0)) {
        return false;
    }

    tb_router_service_init(svc, user_ctx);
    for (size_t i = 0; i < port_count; i++) {
        const tb_router_port_config *cfg = &ports[i];
        if (!tb_router_service_add_port(svc,
                                        cfg->port_id,
                                        cfg->net,
                                        cfg->kind,
                                        cfg->caps,
                                        cfg->transport_state,
                                        cfg->ops)) {
            return false;
        }
    }
    if (!tb_router_service_start(svc, dnet_ttl_ms, log_level)) {
        return false;
    }
    for (size_t i = 0; i < port_count; i++) {
        const tb_router_port_config *cfg = &ports[i];
        if (!cfg->static_dnets && cfg->static_dnet_count > 0) {
            return false;
        }
        for (size_t j = 0; j < cfg->static_dnet_count; j++) {
            if (!tb_router_service_add_static_dnet(svc, cfg->port_id, cfg->static_dnets[j])) {
                return false;
            }
        }
    }
    return true;
}

uint8_t tb_router_port_id(const tb_router_port *port)
{
    return port ? port->port_id : 0;
}

uint16_t tb_router_port_net(const tb_router_port *port)
{
    return port ? port->net : 0;
}

tb_router_transport_kind tb_router_port_kind(const tb_router_port *port)
{
    return port ? port->kind : 0;
}

uint32_t tb_router_port_caps(const tb_router_port *port)
{
    return port ? port->caps : 0;
}

void *tb_router_port_transport_state(const tb_router_port *port)
{
    return port ? port->transport_state : NULL;
}

static bool tb_router_service_start(tb_router_service *svc, uint32_t dnet_ttl_ms, int log_level)
{
    tb_router_service_impl *impl = service_impl(svc);
    if (!impl || impl->port_count > TB_ROUTER_SERVICE_MAX_PORTS) {
        return false;
    }
    router_ctx *router = router_core(svc);
    if (!router) {
        return false;
    }

    br_port_cfg ports[TB_ROUTER_SERVICE_MAX_PORTS];
    for (size_t i = 0; i < impl->port_count; i++) {
        const tb_router_port *port = router_port_at_const(svc, i);
        if (!port || !port->active) {
            return false;
        }
        ports[i] = (br_port_cfg){
            .net = port->net,
            .port_id = port->port_id,
        };
    }
    br_init(router, ports, impl->port_count);
    br_set_log_level(router, log_level);
    br_set_dnet_ttl(router, dnet_ttl_ms);
    return true;
}

void tb_router_service_set_log_level(tb_router_service *svc, int log_level)
{
    router_ctx *router = router_core(svc);
    if (!router) {
        return;
    }
    br_set_log_level(router, log_level);
}

int tb_router_service_handle_frame(tb_router_service *svc,
                                   uint8_t in_port,
                                   const uint8_t *pdu,
                                   size_t len,
                                   const uint8_t *sadr,
                                   size_t sadr_len,
                                   uint8_t src_flags,
                                   uint32_t now_ms)
{
    if (!svc) {
        return -1;
    }
    router_ctx *router = router_core(svc);
    br_set_time(router, now_ms);
    return br_handle_frame(router, in_port, pdu, len, sadr, sadr_len,
                           src_flags, router_service_emit, svc);
}

void tb_router_service_tick(tb_router_service *svc, uint32_t now_ms)
{
    router_ctx *router = router_core(svc);
    if (!router) {
        return;
    }
    br_tick(router, now_ms);
}

bool tb_router_service_is_remote_net(tb_router_service *svc, uint16_t net)
{
    if (net == 0 || net == BACNET_BROADCAST_NETWORK) {
        return false;
    }
    router_ctx *router = router_core(svc);
    if (!router) {
        return false;
    }
    return !router_policy_is_local_port_net(router, net);
}

static void add_route_hint(tb_router_service *svc, tb_router_route_hints *hints, uint16_t net)
{
    if (!hints || !tb_router_service_is_remote_net(svc, net)) {
        return;
    }
    for (size_t i = 0; i < hints->count; i++) {
        if (hints->nets[i] == net) {
            return;
        }
    }
    if (hints->count < TB_ROUTER_ROUTE_HINT_MAX_NETS) {
        hints->nets[hints->count++] = net;
    }
}

bool tb_router_npdu_is_iar(const uint8_t *npdu, size_t npdu_len)
{
    BACNET_ADDRESS daddr = {0};
    BACNET_ADDRESS saddr = {0};
    BACNET_NPDU_DATA npdu_data = {0};
    int offset = bacnet_npdu_decode(npdu, npdu_len, &daddr, &saddr, &npdu_data);
    if (offset <= 0) {
        return false;
    }
    return npdu_data.network_layer_message &&
        npdu_data.network_message_type == NETWORK_MESSAGE_I_AM_ROUTER_TO_NETWORK;
}

size_t tb_router_npdu_extract_iar_nets(const uint8_t *npdu,
                                       size_t npdu_len,
                                       uint16_t *nets,
                                       size_t max_nets)
{
    if (!npdu || !nets || max_nets == 0) {
        return 0;
    }
    BACNET_ADDRESS daddr = {0};
    BACNET_ADDRESS saddr = {0};
    BACNET_NPDU_DATA npdu_data = {0};
    int offset = bacnet_npdu_decode(npdu, npdu_len, &daddr, &saddr, &npdu_data);
    if (offset <= 0 ||
        !npdu_data.network_layer_message ||
        npdu_data.network_message_type != NETWORK_MESSAGE_I_AM_ROUTER_TO_NETWORK) {
        return 0;
    }
    size_t remaining = npdu_len - (size_t)offset;
    size_t count = 0;
    for (size_t i = 0; i + 1 < remaining && count < max_nets; i += 2) {
        uint16_t net = 0;
        if (decode_unsigned16(&npdu[offset + i], &net) > 0) {
            nets[count++] = net;
        }
    }
    return count;
}

bool tb_router_npdu_is_iar_owner(const uint8_t *npdu, size_t npdu_len, uint16_t owner_net)
{
    uint16_t nets[2] = {0};
    size_t count = tb_router_npdu_extract_iar_nets(npdu,
                                                   npdu_len,
                                                   nets,
                                                   sizeof(nets) / sizeof(nets[0]));
    return count == 1 && nets[0] == owner_net;
}

static bool npdu_is_unconfirmed_service(const uint8_t *npdu, size_t npdu_len, uint8_t service)
{
    BACNET_ADDRESS daddr = {0};
    BACNET_ADDRESS saddr = {0};
    BACNET_NPDU_DATA npdu_data = {0};
    int apdu_offset = bacnet_npdu_decode(npdu, npdu_len, &daddr, &saddr, &npdu_data);
    if (apdu_offset <= 0 ||
        npdu_data.network_layer_message ||
        (size_t)apdu_offset + 2 > npdu_len) {
        return false;
    }
    const uint8_t *apdu = npdu + apdu_offset;
    return ((apdu[0] & 0xF0) == PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST) &&
        apdu[1] == service;
}

bool tb_router_npdu_is_whois(const uint8_t *npdu, size_t npdu_len)
{
    return npdu_is_unconfirmed_service(npdu, npdu_len, SERVICE_UNCONFIRMED_WHO_IS);
}

bool tb_router_npdu_is_iam(const uint8_t *npdu, size_t npdu_len)
{
    return npdu_is_unconfirmed_service(npdu, npdu_len, SERVICE_UNCONFIRMED_I_AM);
}

bool tb_router_npdu_decode_iam(const uint8_t *npdu, size_t npdu_len, tb_router_iam_info *info)
{
    if (!npdu || !info) {
        return false;
    }

    BACNET_ADDRESS daddr = {0};
    BACNET_ADDRESS saddr = {0};
    BACNET_NPDU_DATA npdu_data = {0};
    int apdu_offset = bacnet_npdu_decode(npdu, npdu_len, &daddr, &saddr, &npdu_data);
    if (apdu_offset <= 0 ||
        npdu_data.network_layer_message ||
        (size_t)apdu_offset + 2 > npdu_len) {
        return false;
    }
    const uint8_t *apdu = npdu + apdu_offset;
    if (((apdu[0] & 0xF0) != PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST) ||
        apdu[1] != SERVICE_UNCONFIRMED_I_AM) {
        return false;
    }

    tb_router_iam_info decoded = {
        .snet = saddr.net,
        .sadr_len = saddr.len <= MAX_MAC_LEN ? saddr.len : 0,
    };
    if (decoded.sadr_len > 0) {
        memcpy(decoded.sadr, saddr.adr, decoded.sadr_len);
    }
    if (iam_decode_service_request(apdu + 2,
                                   &decoded.device_id,
                                   &decoded.max_apdu,
                                   &decoded.segmentation,
                                   &decoded.vendor_id) <= 0) {
        return false;
    }

    *info = decoded;
    return true;
}

bool tb_router_npdu_decode_addresses(const uint8_t *npdu,
                                     size_t npdu_len,
                                     BACNET_ADDRESS *daddr,
                                     BACNET_ADDRESS *saddr)
{
    if (!npdu) {
        return false;
    }
    BACNET_ADDRESS local_daddr = {0};
    BACNET_ADDRESS local_saddr = {0};
    BACNET_NPDU_DATA npdu_data = {0};
    int offset = bacnet_npdu_decode(npdu,
                                    npdu_len,
                                    daddr ? daddr : &local_daddr,
                                    saddr ? saddr : &local_saddr,
                                    &npdu_data);
    return offset > 0;
}

static size_t build_network_message(uint8_t *out,
                                    size_t out_len,
                                    uint8_t network_message_type,
                                    const uint16_t *nets,
                                    size_t net_count)
{
    if (!out) {
        return 0;
    }
    BACNET_NPDU_DATA npdu_data = {0};
    npdu_encode_npdu_network(&npdu_data,
                             network_message_type,
                             false,
                             MESSAGE_PRIORITY_NORMAL);
    BACNET_ADDRESS dest = {0};
    dest.net = BACNET_BROADCAST_NETWORK;
    dest.len = 0;
    int len = npdu_encode_pdu(out, &dest, NULL, &npdu_data);
    if (len <= 0 || (size_t)len > out_len) {
        return 0;
    }
    for (size_t i = 0; i < net_count; i++) {
        if ((size_t)len + 2 > out_len) {
            return 0;
        }
        len += encode_unsigned16(&out[len], nets[i]);
    }
    return (size_t)len;
}

size_t tb_router_npdu_build_whois_router(uint8_t *out, size_t out_len)
{
    return build_network_message(out,
                                 out_len,
                                 NETWORK_MESSAGE_WHO_IS_ROUTER_TO_NETWORK,
                                 NULL,
                                 0);
}

size_t tb_router_npdu_build_iar(uint8_t *out,
                                size_t out_len,
                                const uint16_t *nets,
                                size_t net_count)
{
    if (!nets || net_count == 0) {
        return 0;
    }
    return build_network_message(out,
                                 out_len,
                                 NETWORK_MESSAGE_I_AM_ROUTER_TO_NETWORK,
                                 nets,
                                 net_count);
}

bool tb_router_service_extract_route_hints(tb_router_service *svc,
                                           const uint8_t *npdu,
                                           size_t npdu_len,
                                           tb_router_route_hints *hints)
{
    if (!svc || !npdu || !hints) {
        return false;
    }
    memset(hints, 0, sizeof(*hints));

    BACNET_ADDRESS daddr = {0};
    BACNET_ADDRESS saddr = {0};
    BACNET_NPDU_DATA npdu_data = {0};
    int offset = bacnet_npdu_decode(npdu, npdu_len, &daddr, &saddr, &npdu_data);
    if (offset <= 0) {
        return false;
    }

    if (saddr.net != 0) {
        add_route_hint(svc, hints, saddr.net);
    }

    uint16_t iar_nets[TB_ROUTER_ROUTE_HINT_MAX_NETS];
    size_t iar_count = tb_router_npdu_extract_iar_nets(npdu,
                                                       npdu_len,
                                                       iar_nets,
                                                       sizeof(iar_nets) / sizeof(iar_nets[0]));
    for (size_t i = 0; i < iar_count; i++) {
        add_route_hint(svc, hints, iar_nets[i]);
    }

    return hints->count > 0;
}

static bool tb_router_service_add_static_dnet(tb_router_service *svc, uint8_t port_id, uint16_t dnet)
{
    router_ctx *router = router_core(svc);
    if (!router) {
        return false;
    }
    port_info *port = router_policy_find_port_by_id(router, port_id);
    dnet_entry *entry = port ? router_policy_add_dnet(port, dnet) : NULL;
    if (!entry) {
        return false;
    }
    entry->configured_static = true;
    return true;
}

bool tb_router_service_lookup_next_hop(tb_router_service *svc,
                                       uint8_t port_id,
                                       uint16_t dnet,
                                       BACNET_ADDRESS *out)
{
    router_ctx *router = router_core(svc);
    if (!router) {
        return false;
    }
    return br_lookup_next_hop(router, port_id, dnet, out);
}

bool tb_router_service_lookup_peer(tb_router_service *svc,
                                   uint16_t dnet,
                                   uint8_t *peer,
                                   uint8_t *peer_len)
{
    router_ctx *router = router_core(svc);
    if (!router) {
        return false;
    }
    return br_lookup_peer(router, dnet, peer, peer_len);
}

void tb_router_service_clear_peers(tb_router_service *svc)
{
    router_ctx *router = router_core(svc);
    if (!router) {
        return;
    }
    br_clear_peers(router);
}

static int take_iar_rx_port(tb_router_service *svc)
{
    router_ctx *router = router_core(svc);
    if (!router) {
        return -1;
    }
    return br_take_iar_rx_port(router);
}

static bool take_dnet_change(tb_router_service *svc, uint8_t *port_id, uint16_t *net)
{
    router_ctx *router = router_core(svc);
    if (!router) {
        return false;
    }
    return br_take_dnet_change(router, port_id, net);
}

void tb_router_service_drain_events(tb_router_service *svc, tb_router_service_event_fn fn, void *arg)
{
    if (!fn) {
        return;
    }

    uint8_t port_id = 0;
    uint16_t net = 0;
    while (take_dnet_change(svc, &port_id, &net)) {
        const tb_router_event event = {
            .type = TB_ROUTER_EVENT_DNET_CHANGE,
            .port_id = port_id,
            .net = net,
        };
        fn(arg, &event);
    }

    for (;;) {
        int iar_port = take_iar_rx_port(svc);
        if (iar_port < 0) {
            break;
        }
        const tb_router_event event = {
            .type = TB_ROUTER_EVENT_IAR_RX,
            .port_id = (uint8_t)iar_port,
            .net = 0,
        };
        fn(arg, &event);
    }
}

void tb_router_service_for_each_port(tb_router_service *svc, tb_router_service_port_fn fn, void *arg)
{
    router_ctx *router = router_core(svc);
    if (!router || !fn) {
        return;
    }
    for (size_t i = 0; i < router->nports; i++) {
        fn(arg, router->ports[i].port_id, router->ports[i].net);
    }
}

void tb_router_service_for_each_route(tb_router_service *svc,
                                      uint32_t now_ms,
                                      tb_router_service_route_fn fn,
                                      void *arg)
{
    router_ctx *router = router_core(svc);
    if (!router || !fn) {
        return;
    }
    for (size_t i = 0; i < router->nports; i++) {
        port_info *port = &router->ports[i];
        for (dnet_entry *e = port->dnets; e; e = e->next) {
            tb_router_route_snapshot route = make_route_snapshot(port, e, now_ms);
            fn(arg, &route);
        }
    }
}

void tb_router_service_for_each_port_route(tb_router_service *svc,
                                           uint32_t now_ms,
                                           tb_router_service_port_fn port_fn,
                                           tb_router_service_route_fn route_fn,
                                           void *arg)
{
    router_ctx *router = router_core(svc);
    if (!router) {
        return;
    }
    for (size_t i = 0; i < router->nports; i++) {
        port_info *port = &router->ports[i];
        if (port_fn) {
            port_fn(arg, port->port_id, port->net);
        }
        if (!route_fn) {
            continue;
        }
        for (dnet_entry *e = port->dnets; e; e = e->next) {
            tb_router_route_snapshot route = make_route_snapshot(port, e, now_ms);
            route_fn(arg, &route);
        }
    }
}

const char *tb_router_service_get_log(tb_router_service *svc, size_t *len_out)
{
    router_ctx *router = router_core(svc);
    if (!router) {
        if (len_out) {
            *len_out = 0;
        }
        return NULL;
    }
    return br_get_log(router, len_out);
}

void tb_router_service_clear_log(tb_router_service *svc)
{
    router_ctx *router = router_core(svc);
    if (!router) {
        return;
    }
    br_clear_log(router);
}
