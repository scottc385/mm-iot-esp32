#include "router_bacnet_trace.h"

#include <stdbool.h>
#include <stdio.h>

#include "bacnet/bacenum.h"
#include "bacnet/npdu.h"
#include "router_log_control.h"
#include "router_service.h"

static const char *apdu_type_name(uint8_t pdu_type)
{
    switch (pdu_type) {
    case PDU_TYPE_CONFIRMED_SERVICE_REQUEST:
        return "confirmed-req";
    case PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST:
        return "unconfirmed-req";
    case PDU_TYPE_SIMPLE_ACK:
        return "simple-ack";
    case PDU_TYPE_COMPLEX_ACK:
        return "complex-ack";
    case PDU_TYPE_SEGMENT_ACK:
        return "segment-ack";
    case PDU_TYPE_ERROR:
        return "error";
    case PDU_TYPE_REJECT:
        return "reject";
    case PDU_TYPE_ABORT:
        return "abort";
    default:
        return "unknown";
    }
}

static const char *service_name(uint8_t pdu_type, uint8_t service)
{
    if (pdu_type == PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST) {
        switch (service) {
        case SERVICE_UNCONFIRMED_I_AM:
            return "i-am";
        case SERVICE_UNCONFIRMED_WHO_IS:
            return "who-is";
        default:
            return "unconfirmed";
        }
    }

    switch (service) {
    case SERVICE_CONFIRMED_READ_PROPERTY:
        return "read-property";
    case SERVICE_CONFIRMED_READ_PROP_MULTIPLE:
        return "read-property-multiple";
    case SERVICE_CONFIRMED_WRITE_PROPERTY:
        return "write-property";
    default:
        return "confirmed";
    }
}

static uint8_t service_offset(const uint8_t *apdu, size_t apdu_len, uint8_t pdu_type)
{
    bool segmented = apdu_len > 0 && (apdu[0] & 0x08u) != 0;

    switch (pdu_type) {
    case PDU_TYPE_CONFIRMED_SERVICE_REQUEST:
        return segmented ? 5u : 3u;
    case PDU_TYPE_COMPLEX_ACK:
        return segmented ? 4u : 2u;
    case PDU_TYPE_SIMPLE_ACK:
    case PDU_TYPE_ERROR:
    case PDU_TYPE_REJECT:
    case PDU_TYPE_ABORT:
        return 2u;
    case PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST:
        return 1u;
    default:
        return 0xffu;
    }
}

static int invoke_id(const uint8_t *apdu, size_t apdu_len, uint8_t pdu_type)
{
    if (!apdu || apdu_len < 2) {
        return -1;
    }

    switch (pdu_type) {
    case PDU_TYPE_CONFIRMED_SERVICE_REQUEST:
        return apdu_len >= 3 ? apdu[2] : -1;
    case PDU_TYPE_SIMPLE_ACK:
    case PDU_TYPE_COMPLEX_ACK:
    case PDU_TYPE_SEGMENT_ACK:
    case PDU_TYPE_ERROR:
    case PDU_TYPE_REJECT:
    case PDU_TYPE_ABORT:
        return apdu[1];
    default:
        return -1;
    }
}

void router_bacnet_trace_npdu(const char *dir,
                              const char *transport,
                              const uint8_t *npdu,
                              size_t npdu_len)
{
    if (!router_log_get(ROUTER_LOG_BACNET) || !npdu || npdu_len == 0) {
        return;
    }

    BACNET_ADDRESS daddr = {0};
    BACNET_ADDRESS saddr = {0};
    BACNET_NPDU_DATA npdu_data = {0};
    int apdu_offset = bacnet_npdu_decode(npdu, npdu_len, &daddr, &saddr, &npdu_data);
    if (apdu_offset <= 0) {
        printf("BACNET %s %s npdu_len=%u decode=fail\n",
               dir ? dir : "?",
               transport ? transport : "?",
               (unsigned)npdu_len);
        return;
    }

    if (npdu_data.network_layer_message || (size_t)apdu_offset >= npdu_len) {
        printf("BACNET %s %s netmsg=0x%02x dnet=%u snet=%u npdu_len=%u\n",
               dir ? dir : "?",
               transport ? transport : "?",
               (unsigned)npdu_data.network_message_type,
               (unsigned)daddr.net,
               (unsigned)saddr.net,
               (unsigned)npdu_len);
        return;
    }

    const uint8_t *apdu = &npdu[apdu_offset];
    size_t apdu_len = npdu_len - (size_t)apdu_offset;
    uint8_t pdu_type = apdu[0] & 0xF0u;
    bool segmented = (apdu[0] & 0x08u) != 0;
    bool more = (apdu[0] & 0x04u) != 0;
    uint8_t svc_offset = service_offset(apdu, apdu_len, pdu_type);
    uint8_t service = svc_offset < apdu_len ? apdu[svc_offset] : 0xffu;
    int invoke = invoke_id(apdu, apdu_len, pdu_type);

    tb_router_iam_info iam = {0};
    if (tb_router_npdu_decode_iam(npdu, npdu_len, &iam)) {
        printf("BACNET %s %s i-am device=%lu dnet=%u snet=%u sadr_len=%u apdu_len=%u seg=%u more=%u\n",
               dir ? dir : "?",
               transport ? transport : "?",
               (unsigned long)iam.device_id,
               (unsigned)daddr.net,
               (unsigned)iam.snet,
               (unsigned)iam.sadr_len,
               (unsigned)apdu_len,
               segmented ? 1u : 0u,
               more ? 1u : 0u);
        return;
    }

    printf("BACNET %s %s %s service=%s(%u) invoke=%d dnet=%u snet=%u npdu_len=%u apdu_len=%u seg=%u more=%u\n",
           dir ? dir : "?",
           transport ? transport : "?",
           apdu_type_name(pdu_type),
           service_name(pdu_type, service),
           (unsigned)service,
           invoke,
           (unsigned)daddr.net,
           (unsigned)saddr.net,
           (unsigned)npdu_len,
           (unsigned)apdu_len,
           segmented ? 1u : 0u,
           more ? 1u : 0u);
}

