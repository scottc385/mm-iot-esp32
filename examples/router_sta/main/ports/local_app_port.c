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

static bool decode_object_id(const uint8_t *buf, uint16_t *object_type, uint32_t *instance)
{
    if (!buf || !object_type || !instance) {
        return false;
    }
    uint32_t value = ((uint32_t)buf[0] << 24) |
        ((uint32_t)buf[1] << 16) |
        ((uint32_t)buf[2] << 8) |
        (uint32_t)buf[3];
    *object_type = (uint16_t)(value >> 22);
    *instance = value & 0x3FFFFFU;
    return true;
}

static size_t encode_character_string(uint8_t *out, size_t out_len, const char *value)
{
    size_t value_len = value ? strlen(value) : 0;
    size_t payload_len = value_len + 1; /* Character set byte plus string bytes. */
    if (!out || payload_len > 253) {
        return 0;
    }
    if (payload_len < 5) {
        if (out_len < 1 + payload_len) {
            return 0;
        }
        out[0] = (uint8_t)((7U << 4) | payload_len);
        out[1] = 0; /* ANSI X3.4 / UTF-8 compatible for our ASCII names. */
        memcpy(&out[2], value ? value : "", value_len);
        return 1 + payload_len;
    }
    if (out_len < 2 + payload_len) {
        return 0;
    }
    out[0] = (uint8_t)((7U << 4) | 5U);
    out[1] = (uint8_t)payload_len;
    out[2] = 0;
    memcpy(&out[3], value ? value : "", value_len);
    return 2 + payload_len;
}

static bool decode_character_string(const uint8_t *buf,
                                    size_t len,
                                    char *out,
                                    size_t out_len,
                                    size_t *consumed)
{
    if (!buf || !out || out_len == 0 || len < 2) {
        return false;
    }

    uint8_t tag = buf[0];
    if ((tag >> 4) != 7) {
        return false;
    }

    size_t offset = 1;
    size_t payload_len = tag & 0x0FU;
    if (payload_len == 5) {
        if (offset >= len) {
            return false;
        }
        payload_len = buf[offset++];
    }
    if (payload_len < 1 || offset + payload_len > len || buf[offset] != 0) {
        return false;
    }

    size_t value_len = payload_len - 1;
    if (value_len >= out_len) {
        return false;
    }

    memcpy(out, &buf[offset + 1], value_len);
    out[value_len] = '\0';
    if (consumed) {
        *consumed = offset + payload_len;
    }
    return true;
}

static bool parse_read_property_object_name(const uint8_t *apdu,
                                            size_t apdu_len,
                                            uint8_t *invoke_id,
                                            uint32_t *device_id)
{
    if (!apdu || apdu_len < 11 ||
        apdu[0] != PDU_TYPE_CONFIRMED_SERVICE_REQUEST ||
        apdu[3] != SERVICE_CONFIRMED_READ_PROPERTY ||
        apdu[4] != 0x0C ||
        apdu[9] != 0x19 ||
        apdu[10] != PROP_OBJECT_NAME) {
        return false;
    }

    uint16_t object_type = 0;
    uint32_t object_instance = 0;
    if (!decode_object_id(&apdu[5], &object_type, &object_instance) ||
        object_type != OBJECT_DEVICE) {
        return false;
    }
    if (invoke_id) {
        *invoke_id = apdu[2];
    }
    if (device_id) {
        *device_id = object_instance;
    }
    return true;
}

static bool parse_write_property_object_name(const uint8_t *apdu,
                                             size_t apdu_len,
                                             uint8_t *invoke_id,
                                             uint32_t *device_id,
                                             char *value,
                                             size_t value_len)
{
    if (!apdu || apdu_len < 14 ||
        apdu[0] != PDU_TYPE_CONFIRMED_SERVICE_REQUEST ||
        apdu[3] != SERVICE_CONFIRMED_WRITE_PROPERTY ||
        apdu[4] != 0x0C ||
        apdu[9] != 0x19 ||
        apdu[10] != PROP_OBJECT_NAME ||
        apdu[11] != 0x3E) {
        return false;
    }

    uint16_t object_type = 0;
    uint32_t object_instance = 0;
    if (!decode_object_id(&apdu[5], &object_type, &object_instance) ||
        object_type != OBJECT_DEVICE) {
        return false;
    }

    size_t consumed = 0;
    if (!decode_character_string(&apdu[12], apdu_len - 12, value, value_len, &consumed)) {
        return false;
    }
    if (12 + consumed >= apdu_len || apdu[12 + consumed] != 0x3F) {
        return false;
    }

    if (invoke_id) {
        *invoke_id = apdu[2];
    }
    if (device_id) {
        *device_id = object_instance;
    }
    return true;
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
    snprintf(app->object_name, sizeof(app->object_name), "%s",
             object_name && object_name[0] ? object_name : "esp32-local-app");
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

static void local_app_submit_read_property_ack(local_app_port *app,
                                               const BACNET_ADDRESS *request_source,
                                               uint8_t invoke_id)
{
    if (!request_source || request_source->net == 0) {
        return;
    }

    uint8_t npdu[LOCAL_APP_MAX_NPDU];
    BACNET_NPDU_DATA npdu_data = {0};
    BACNET_ADDRESS dest = *request_source;

    npdu_data.protocol_version = BACNET_PROTOCOL_VERSION;
    npdu_data.hop_count = 0xFF;

    int offset = npdu_encode_pdu(npdu, &dest, NULL, &npdu_data);
    if (offset <= 0 || (size_t)offset + 12 > sizeof(npdu)) {
        return;
    }

    uint8_t *apdu = &npdu[offset];
    apdu[0] = PDU_TYPE_COMPLEX_ACK;
    apdu[1] = invoke_id;
    apdu[2] = SERVICE_CONFIRMED_READ_PROPERTY;
    apdu[3] = 0x0C;
    encode_object_id(&apdu[4], OBJECT_DEVICE, app->device_id);
    apdu[8] = 0x19;
    apdu[9] = PROP_OBJECT_NAME;
    apdu[10] = 0x3E;

    size_t value_len = encode_character_string(&apdu[11], sizeof(npdu) - (size_t)offset - 12, app->object_name);
    if (value_len == 0) {
        return;
    }

    apdu[11 + value_len] = 0x3F;
    size_t npdu_len = (size_t)offset + 12 + value_len;
    if (local_app_submit(app, npdu, npdu_len)) {
        printf("LOCAL_APP_RP_ACK net=%u device=%lu invoke=%u name=%s\n",
               (unsigned)app->net,
               (unsigned long)app->device_id,
               (unsigned)invoke_id,
               app->object_name);
    }
}

static void local_app_submit_simple_ack(local_app_port *app,
                                        const BACNET_ADDRESS *request_source,
                                        uint8_t invoke_id,
                                        uint8_t service_choice)
{
    if (!request_source || request_source->net == 0) {
        return;
    }

    uint8_t npdu[LOCAL_APP_MAX_NPDU];
    BACNET_NPDU_DATA npdu_data = {0};
    BACNET_ADDRESS dest = *request_source;

    npdu_data.protocol_version = BACNET_PROTOCOL_VERSION;
    npdu_data.hop_count = 0xFF;

    int offset = npdu_encode_pdu(npdu, &dest, NULL, &npdu_data);
    if (offset <= 0 || (size_t)offset + 3 > sizeof(npdu)) {
        return;
    }

    uint8_t *apdu = &npdu[offset];
    apdu[0] = PDU_TYPE_SIMPLE_ACK;
    apdu[1] = invoke_id;
    apdu[2] = service_choice;

    size_t npdu_len = (size_t)offset + 3;
    if (local_app_submit(app, npdu, npdu_len)) {
        printf("LOCAL_APP_SIMPLE_ACK net=%u device=%lu invoke=%u service=%u\n",
               (unsigned)app->net,
               (unsigned long)app->device_id,
               (unsigned)invoke_id,
               (unsigned)service_choice);
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

    uint8_t invoke_id = 0;
    uint32_t device_id = 0;
    if (parse_read_property_object_name(apdu, apdu_len, &invoke_id, &device_id) &&
        device_id == app->device_id) {
        local_app_submit_read_property_ack(app, &source, invoke_id);
    }

    char object_name[LOCAL_APP_OBJECT_NAME_MAX] = {0};
    if (parse_write_property_object_name(apdu,
                                         apdu_len,
                                         &invoke_id,
                                         &device_id,
                                         object_name,
                                         sizeof(object_name)) &&
        device_id == app->device_id) {
        snprintf(app->object_name, sizeof(app->object_name), "%s", object_name);
        printf("LOCAL_APP_WP net=%u device=%lu name=%s\n",
               (unsigned)app->net,
               (unsigned long)app->device_id,
               app->object_name);
        local_app_submit_simple_ack(app, &source, invoke_id, SERVICE_CONFIRMED_WRITE_PROPERTY);
    }
}

const tb_router_transport_ops k_local_app_router_ops = {
    .name = "local_app",
    .send_npdu = local_app_send_npdu,
};
