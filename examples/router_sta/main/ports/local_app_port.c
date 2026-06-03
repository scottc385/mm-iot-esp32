#include "local_app_port.h"

#include <stdio.h>
#include <string.h>

#include "bacnet/bacdef.h"
#include "bacnet/bacenum.h"
#include "bacnet/iam.h"
#include "bacnet/npdu.h"
#include "mmosal.h"

#define LOCAL_APP_MAX_NPDU 256

static void encode_object_id(uint8_t *out, uint16_t object_type, uint32_t instance)
{
    uint32_t value = ((uint32_t)object_type << 22) | (instance & 0x3FFFFFU);
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

void local_app_port_init(local_app_port *app,
                         tb_router_service *router_service,
                         uint8_t port_id,
                         uint16_t net,
                         uint32_t device_id,
                         const char *object_name)
{
    if (!app) {
        return;
    }
    memset(app, 0, sizeof(*app));
    app->active = (router_service != NULL && port_id != 0 && net != 0 && device_id <= 0x3FFFFFU);
    app->router_service = router_service;
    app->port_id = port_id;
    app->net = net;
    app->device_id = device_id;
    app->object_name = object_name && object_name[0] ? object_name : "esp32-local-app";
}

static bool local_app_submit(local_app_port *app, const uint8_t *npdu, size_t npdu_len)
{
    if (!app || !app->active || !app->router_service || !npdu || npdu_len == 0) {
        return false;
    }
    uint8_t local_sadr[1] = { app->port_id };
    int rc = tb_router_service_handle_frame(app->router_service,
                                            app->port_id,
                                            npdu,
                                            npdu_len,
                                            local_sadr,
                                            sizeof(local_sadr),
                                            0,
                                            mmosal_get_time_ms());
    return rc >= 0;
}

static void local_app_submit_iam(local_app_port *app)
{
    uint8_t npdu[LOCAL_APP_MAX_NPDU];
    BACNET_NPDU_DATA npdu_data = {0};
    BACNET_ADDRESS dest = {0};

    npdu_data.protocol_version = BACNET_PROTOCOL_VERSION;
    npdu_data.hop_count = 0xFF;
    dest.net = BACNET_BROADCAST_NETWORK;

    int offset = npdu_encode_pdu(npdu, &dest, NULL, &npdu_data);
    if (offset <= 0 || (size_t)offset + 14 > sizeof(npdu)) {
        return;
    }

    uint8_t *apdu = &npdu[offset];
    apdu[0] = PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST;
    apdu[1] = SERVICE_UNCONFIRMED_I_AM;
    apdu[2] = 0xC4;
    encode_object_id(&apdu[3], OBJECT_DEVICE, app->device_id);
    apdu[7] = 0x22;
    apdu[8] = 0x05;
    apdu[9] = 0xC4;
    apdu[10] = 0x91;
    apdu[11] = 0x03;
    apdu[12] = 0x21;
    apdu[13] = 0xF5;

    size_t npdu_len = (size_t)offset + 14;
    if (local_app_submit(app, npdu, npdu_len)) {
        printf("LOCAL_APP_IAM net=%u device=%lu name=%s\n",
               (unsigned)app->net,
               (unsigned long)app->device_id,
               app->object_name);
    }
}

static void local_app_send_npdu(void *user_ctx,
                                tb_router_port *router_port,
                                const uint8_t *pdu,
                                size_t len,
                                const BACNET_ADDRESS *daddr)
{
    (void)user_ctx;
    (void)daddr;
    local_app_port *app = (local_app_port *)tb_router_port_transport_state(router_port);
    if (!app || !app->active || !pdu || len < 2) {
        return;
    }

    BACNET_ADDRESS dest = {0};
    BACNET_ADDRESS source = {0};
    BACNET_NPDU_DATA npdu = {0};
    int offset = bacnet_npdu_decode(pdu, len, &dest, &source, &npdu);
    if (offset < 0 || (size_t)offset >= len) {
        return;
    }

    const uint8_t *apdu = &pdu[offset];
    size_t apdu_len = len - (size_t)offset;
    printf("LOCAL_APP_IN net=%u npdu_len=%u first=%02x:%02x\n",
           (unsigned)app->net,
           (unsigned)len,
           apdu_len > 0 ? apdu[0] : 0,
           apdu_len > 1 ? apdu[1] : 0);

    if (apdu_len >= 2 &&
        (apdu[0] & 0xF0) == PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST &&
        apdu[1] == SERVICE_UNCONFIRMED_WHO_IS) {
        local_app_submit_iam(app);
    }
}

const tb_router_transport_ops k_local_app_router_ops = {
    .name = "local_app",
    .send_npdu = local_app_send_npdu,
};
