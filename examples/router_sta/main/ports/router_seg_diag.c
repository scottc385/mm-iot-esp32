#include "router_seg_diag.h"

#include <stdio.h>
#include <string.h>

#include "bacnet/bacenum.h"
#include "bacnet/npdu.h"
#include "esp_log.h"

static const char *TAG = "router_seg";

static const char *pdu_type_name(uint8_t pdu_type)
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

static int apdu_invoke_id(const uint8_t *apdu, size_t apdu_len, uint8_t pdu_type)
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

static void format_addr(const BACNET_ADDRESS *addr, char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) {
        return;
    }
    if (!addr) {
        snprintf(buf, buf_len, "none");
        return;
    }

    size_t used = (size_t)snprintf(buf,
                                   buf_len,
                                   "net=%u len=%u adr=",
                                   (unsigned)addr->net,
                                   (unsigned)addr->len);
    if (used >= buf_len) {
        return;
    }
    if (addr->len == 0) {
        snprintf(buf + used, buf_len - used, "-");
        return;
    }
    for (uint8_t i = 0; i < addr->len && used < buf_len; i++) {
        used += (size_t)snprintf(buf + used,
                                 buf_len - used,
                                 "%s%02x",
                                 i == 0 ? "" : ":",
                                 addr->adr[i]);
    }
}

void router_seg_diag_log_npdu(const char *tag, const uint8_t *npdu, size_t npdu_len)
{
#if ROUTER_SEG_DIAG_ENABLE
    if (!npdu || npdu_len == 0) {
        ESP_LOGI(TAG, "%s npdu_len=%u empty", tag ? tag : "NPDU", (unsigned)npdu_len);
        return;
    }

    BACNET_ADDRESS daddr = {0};
    BACNET_ADDRESS saddr = {0};
    BACNET_NPDU_DATA npdu_data = {0};
    int apdu_offset = bacnet_npdu_decode(npdu, npdu_len, &daddr, &saddr, &npdu_data);
    if (apdu_offset <= 0) {
        ESP_LOGW(TAG,
                 "%s npdu_len=%u npdu_decode=fail first=%02x:%02x",
                 tag ? tag : "NPDU",
                 (unsigned)npdu_len,
                 npdu_len > 0 ? npdu[0] : 0,
                 npdu_len > 1 ? npdu[1] : 0);
        return;
    }

    char daddr_text[80];
    char saddr_text[80];
    format_addr(&daddr, daddr_text, sizeof(daddr_text));
    format_addr(&saddr, saddr_text, sizeof(saddr_text));

    if (npdu_data.network_layer_message) {
        ESP_LOGI(TAG,
                 "%s npdu_len=%u apdu_offset=%d network_msg=0x%02x daddr={%s} saddr={%s}",
                 tag ? tag : "NPDU",
                 (unsigned)npdu_len,
                 apdu_offset,
                 (unsigned)npdu_data.network_message_type,
                 daddr_text,
                 saddr_text);
        return;
    }

    if ((size_t)apdu_offset >= npdu_len) {
        ESP_LOGI(TAG,
                 "%s npdu_len=%u apdu_len=0 daddr={%s} saddr={%s}",
                 tag ? tag : "NPDU",
                 (unsigned)npdu_len,
                 daddr_text,
                 saddr_text);
        return;
    }

    const uint8_t *apdu = &npdu[apdu_offset];
    size_t apdu_len = npdu_len - (size_t)apdu_offset;
    uint8_t pdu_type = apdu[0] & 0xF0u;
    bool segmented = (apdu[0] & 0x08u) != 0;
    bool more_segments = (apdu[0] & 0x04u) != 0;
    int invoke = apdu_invoke_id(apdu, apdu_len, pdu_type);

    ESP_LOGI(TAG,
             "%s npdu_len=%u apdu_len=%u apdu_type=%s(0x%02x) invoke=%d segmented=%u more=%u daddr={%s} saddr={%s}",
             tag ? tag : "NPDU",
             (unsigned)npdu_len,
             (unsigned)apdu_len,
             pdu_type_name(pdu_type),
             (unsigned)pdu_type,
             invoke,
             segmented ? 1u : 0u,
             more_segments ? 1u : 0u,
             daddr_text,
             saddr_text);
#else
    (void)tag;
    (void)npdu;
    (void)npdu_len;
#endif
}

void router_seg_diag_log_addresses(const char *tag,
                                   const BACNET_ADDRESS *daddr,
                                   const BACNET_ADDRESS *saddr)
{
#if ROUTER_SEG_DIAG_ENABLE
    char daddr_text[80];
    char saddr_text[80];
    format_addr(daddr, daddr_text, sizeof(daddr_text));
    format_addr(saddr, saddr_text, sizeof(saddr_text));
    ESP_LOGI(TAG, "%s daddr={%s} saddr={%s}", tag ? tag : "ADDR", daddr_text, saddr_text);
#else
    (void)tag;
    (void)daddr;
    (void)saddr;
#endif
}

