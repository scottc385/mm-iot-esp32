/*
 * Copyright 2022-2023 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Example application to demonstrate MMWLAN API to connect to an AP.
 *
 * @note It is assumed that you have followed the steps in the @ref GETTING_STARTED guide and are
 * therefore familiar with how to build, flash, and monitor an application using the MM-IoT-SDK
 * framework.
 */

#include <endian.h>
#include <string.h>
#include "mmhal.h"
#include "mmosal.h"
#include "mmwlan.h"
#include "mmwlan_stats.h"
#include "mm_app_regdb.h"

#define COUNTRY_CODE "US"
#ifndef COUNTRY_CODE
#error COUNTRY_CODE must be defined to the appropriate 2 character country code. \
       See mm_app_regdb.c for valid options.
#endif

/** SSID of the AP to connect to. */
#define SSID "Halow-pi"

static unsigned scan_result_count;

static void dump_umac_stats(const char *tag)
{
    enum mmwlan_status status;
    struct mmwlan_stats_umac_data stats = {0};

    status = mmwlan_get_umac_stats(&stats);
    if (status != MMWLAN_SUCCESS)
    {
        printf("UMAC stats unavailable at %s: status %d\n", tag, status);
        return;
    }

    printf("UMAC stats [%s]: scans_complete=%u hw_restarts=%u timeouts=%lu scan_req_ts=%lu scan_complete_ts=%lu rssi=%d\n",
           tag, stats.num_scans_complete, stats.hw_restart_counter,
           (unsigned long)stats.timeouts_fired,
           (unsigned long)stats.connect_timestamp[MMWLAN_STATS_CONNECT_TIMESTAMP_SCAN_REQUESTED],
           (unsigned long)stats.connect_timestamp[MMWLAN_STATS_CONNECT_TIMESTAMP_SCAN_COMPLETE],
           stats.rssi);
}

static void print_scan_signal_summary(const struct mmwlan_scan_result *result)
{
    printf("SCAN signal summary: RSSI=%d dBm, SNR=n/a (noise not exposed by this SDK)\n",
           result->rssi);
}

static void dump_link_rssi(const char *tag)
{
    int32_t rssi = mmwlan_get_rssi();

    if (rssi == INT32_MIN)
    {
        printf("Link RSSI [%s]: unavailable\n", tag);
        return;
    }

    printf("Link RSSI [%s]: %ld dBm, SNR=n/a (noise not exposed by this SDK)\n",
           tag, (long)rssi);
}

/**
 * Link state callback. This is typically used to signal state to the network stack.
 */
static void link_state_change_hanndler(enum mmwlan_link_state link_state, void *arg)
{
    printf("Link went %s\n", (link_state == MMWLAN_LINK_DOWN) ? "Down" : "Up");

    if (link_state == MMWLAN_LINK_UP)
    {
        struct mmosal_semb *link_up_semaphore = (struct mmosal_semb *)arg;
        bool ok = mmosal_semb_give(link_up_semaphore);
        if (!ok)
        {
            printf("Failed to give link_up_semaphore\n");
            MMOSAL_ASSERT(false);
        }
    }
}

/**
 * Receive callback. This is invoked when a packet is received, which is typically passed to
 * the network stack.
 */
static void rx_handler(uint8_t *header, unsigned header_len,
                       uint8_t *payload, unsigned payload_len,
                       void *arg)
{
    struct __attribute__((packed)) dot3_header
    {
        uint8_t dest_addr[6];
        uint8_t src_addr[6];
        uint16_t ethertype;
    };

    struct dot3_header *hdr = (struct dot3_header *)header;

    MMOSAL_ASSERT(sizeof(*hdr) == header_len);

    printf("RX from %02x:%02x:%02x:%02x:%02x:%02x type 0x%04x\n",
           hdr->src_addr[0], hdr->src_addr[1], hdr->src_addr[2],
           hdr->src_addr[3], hdr->src_addr[4], hdr->src_addr[5],
           be16toh(hdr->ethertype));
}

/**
 * STA status callback. This is typically used to signal STA state to the application (e.g.,
 * to update UI).
 */
static void sta_status_handler(enum mmwlan_sta_state sta_state)
{
    const char *sta_state_desc[] = {
        "DISABLED",
        "CONNECTING",
        "CONNECTED",
    };
    printf("STA state: %s (%u)\n", sta_state_desc[sta_state], sta_state);
}

static void scan_result_handler(const struct mmwlan_scan_result *result, void *arg)
{
    (void)arg;
    scan_result_count++;

    printf("SCAN ssid='%.*s' bssid=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d freq=%lu bw=%u op_bw=%u\n",
           result->ssid_len, result->ssid,
           result->bssid[0], result->bssid[1], result->bssid[2],
           result->bssid[3], result->bssid[4], result->bssid[5],
           result->rssi, (unsigned long)result->channel_freq_hz,
           result->bw_mhz, result->op_bw_mhz);
    print_scan_signal_summary(result);
}

static void scan_complete_handler(enum mmwlan_scan_state scan_state, void *arg)
{
    struct mmosal_semb *scan_done_semaphore = (struct mmosal_semb *)arg;
    const char *desc[] = {
        "SUCCESSFUL",
        "TERMINATED",
        "RUNNING",
    };

    if (scan_state < (sizeof(desc) / sizeof(desc[0])))
    {
        printf("SCAN complete: %s (%u)\n", desc[scan_state], scan_state);
    }
    else
    {
        printf("SCAN complete: UNKNOWN (%u)\n", scan_state);
    }

    printf("SCAN result count: %u\n", scan_result_count);
    dump_umac_stats("explicit_scan_complete");

    if (scan_state != MMWLAN_SCAN_RUNNING)
    {
        bool ok = mmosal_semb_give(scan_done_semaphore);
        if (!ok)
        {
            printf("Failed to give scan_done_semaphore\n");
            MMOSAL_ASSERT(false);
        }
    }
}

static void sta_event_handler(const struct mmwlan_sta_event_cb_args *sta_event, void *arg)
{
    (void)arg;

    static const char *event_desc[] = {
        "SCAN_REQUEST",
        "SCAN_COMPLETE",
        "SCAN_ABORT",
        "AUTH_REQUEST",
        "ASSOC_REQUEST",
        "DEAUTH_TX",
        "CTRL_PORT_OPEN",
        "CTRL_PORT_CLOSED",
    };

    if (sta_event->event < (sizeof(event_desc) / sizeof(event_desc[0])))
    {
        printf("STA event: %s (%u)\n", event_desc[sta_event->event], sta_event->event);
        if (sta_event->event == MMWLAN_STA_EVT_SCAN_REQUEST)
        {
            scan_result_count = 0;
            dump_umac_stats("sta_scan_request");
        }
        else if (sta_event->event == MMWLAN_STA_EVT_SCAN_COMPLETE)
        {
            printf("STA scan result count: %u\n", scan_result_count);
            dump_umac_stats("sta_scan_complete");
        }
    }
    else
    {
        printf("STA event: UNKNOWN (%u)\n", sta_event->event);
    }
}

void app_print_version_info(void)
{
    enum mmwlan_status status;
    struct mmwlan_version version = {0};
    struct mmwlan_bcf_metadata bcf_metadata = {0};

    printf("-----------------------------------\n");

    status = mmwlan_get_bcf_metadata(&bcf_metadata);
    if (status == MMWLAN_SUCCESS)
    {
        printf("  BCF API version:         %u.%u.%u\n",
               bcf_metadata.version.major, bcf_metadata.version.minor, bcf_metadata.version.patch);
        if (bcf_metadata.build_version[0] != '\0')
        {
            printf("  BCF build version:       %s\n", bcf_metadata.build_version);
        }
        if (bcf_metadata.board_desc[0] != '\0')
        {
            printf("  BCF board description:   %s\n", bcf_metadata.board_desc);
        }
    }
    else
    {
        printf("  !! BCF metadata retrival failed !!\n");
    }

    status = mmwlan_get_version(&version);
    if (status != MMWLAN_SUCCESS)
    {
        printf("  !! Error occured whilst retrieving version info !!\n");
    }
    printf("  Morselib version:        %s\n", version.morselib_version);
    printf("  Morse firmware version:  %s\n", version.morse_fw_version);
    printf("  Morse chip ID:           0x%04lx\n", version.morse_chip_id);
    printf("  Morse chip name:         %s\n", version.morse_chip_id_string);
    printf("-----------------------------------\n");

    MMOSAL_ASSERT(status == MMWLAN_SUCCESS);
}

/**
 * Main entry point to the application. This will be invoked in a thread once operating system
 * and hardware initialization has completed. It may return, but it does not have to.
 */
void app_main(void)
{
    enum mmwlan_status status;
    struct mmwlan_sta_args sta_args = MMWLAN_STA_ARGS_INIT;
    const struct mmwlan_s1g_channel_list* channel_list;
    uint8_t mac_addr[MMWLAN_MAC_ADDR_LEN];
    struct mmosal_semb *link_up_semaphore;
    struct mmosal_semb *scan_done_semaphore;
    struct mmwlan_scan_req scan_req = MMWLAN_SCAN_REQ_INIT;
    bool ok;

    printf("\n\nMorse STA Demo (Built "__DATE__ " " __TIME__ ")\n\n");

    /* This semaphore is used by the link state callback to signal the link going up. We use
     * it in this main thread to block until the link goes up. */
    link_up_semaphore = mmosal_semb_create("link_up");
    MMOSAL_ASSERT(link_up_semaphore != NULL);
    scan_done_semaphore = mmosal_semb_create("scan_done");
    MMOSAL_ASSERT(scan_done_semaphore != NULL);

    /* Initialize subsystems, note that they must be called in this order. */
    mmhal_init();
    mmwlan_init();

    /* Load channel list. */
    channel_list = mmwlan_lookup_regulatory_domain(get_regulatory_db(), COUNTRY_CODE);
    if (channel_list == NULL)
    {
        printf("Could not find specified regulatory domain matching country code %s\n",
               COUNTRY_CODE);
        MMOSAL_ASSERT(false);
    }
    status = mmwlan_set_channel_list(channel_list);
    if (status != MMWLAN_SUCCESS)
    {
        printf("Failed to set country code %s\n", channel_list->country_code);
        MMOSAL_ASSERT(false);
    }

    /* Register callback to be invoked when the link goes up and down. We pass link_up_semaphore
     * as an opaque argument so that this can be used to signal link up. */
    status = mmwlan_register_link_state_cb(link_state_change_hanndler, link_up_semaphore);
    if (status != MMWLAN_SUCCESS)
    {
        printf("Failed to register %s callback\n", "link state");
        MMOSAL_ASSERT(false);
    }

    /* Register a callback to be invoked on receive. */
    status = mmwlan_register_rx_cb(rx_handler, NULL);
    if (status != MMWLAN_SUCCESS)
    {
        printf("Failed to register %s callback\n", "rx");
        MMOSAL_ASSERT(false);
    }

    /* Boot the WLAN interface so that we can retrieve the firmware version. */
    struct mmwlan_boot_args boot_args = MMWLAN_BOOT_ARGS_INIT;
    (void)mmwlan_boot(&boot_args);
    app_print_version_info();

    status = mmwlan_set_power_save_mode(MMWLAN_PS_DISABLED);
    if (status != MMWLAN_SUCCESS)
    {
        printf("Failed to disable power save: status %d\n", status);
        MMOSAL_ASSERT(false);
    }

    status = mmwlan_clear_umac_stats();
    if (status != MMWLAN_SUCCESS)
    {
        printf("Failed to clear UMAC stats: status %d\n", status);
    }

    status = mmwlan_get_mac_addr(mac_addr);
    if (status != MMWLAN_SUCCESS)
    {
        printf("Failed to get MAC address\n");
        MMOSAL_ASSERT(false);
    }

    /* Perform an explicit scan first so discovery can be debugged independently of association. */
    scan_req.scan_rx_cb = scan_result_handler;
    scan_req.scan_complete_cb = scan_complete_handler;
    scan_req.scan_cb_arg = scan_done_semaphore;
    scan_req.args.ssid_len = sizeof(SSID) - 1;
    memcpy(scan_req.args.ssid, SSID, scan_req.args.ssid_len);
    scan_req.args.dwell_time_ms = 200;
    scan_req.args.dwell_on_home_ms = 0;
    scan_result_count = 0;

    status = mmwlan_scan_request(&scan_req);
    if (status != MMWLAN_SUCCESS)
    {
        printf("Failed to start scan: status %d\n", status);
        MMOSAL_ASSERT(false);
    }

    ok = mmosal_semb_wait(scan_done_semaphore, 30000);
    if (!ok)
    {
        printf("Timed out waiting for scan completion\n");
        status = mmwlan_scan_abort();
        printf("Scan abort returned status %d\n", status);
    }

    /* Set up STA arguments and start connection to AP. */
    sta_args.ssid_len = sizeof(SSID) - 1;
    memcpy(sta_args.ssid, SSID, sta_args.ssid_len);
    /* Connect to an open AP with PMF disabled to match the current Pi test AP. */
    sta_args.security_type = MMWLAN_OPEN;
    sta_args.pmf_mode = MMWLAN_PMF_DISABLED;
    sta_args.scan_rx_cb = scan_result_handler;
    sta_args.sta_evt_cb = sta_event_handler;
    status = mmwlan_sta_enable(&sta_args, sta_status_handler);
    if (status != MMWLAN_SUCCESS)
    {
        printf("Failed to enable STA mode: status %d\n", status);
        MMOSAL_ASSERT(false);
    }

    /* Wait until the link comes up. */
    ok = mmosal_semb_wait(link_up_semaphore, UINT32_MAX);
    MMOSAL_ASSERT(ok);

    /* Send a packet. Note that this is just for demonstration purposes and normally this function
     * would be connected up to the IP stack (e.g., via a LWIP netif). */
    uint8_t arp_packet[] = {
        /* 802.3 header: dest_addr, source_addr, ethertype */
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5],
        0x08, 0x06,

        /* ARP payload */
        0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01,
        mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5],
        0xc0, 0xa8, 0x01, 0x02, /* Our IP address (192.168.1.2) */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xc0, 0xa8, 0x01, 0x01,  /* Target IP address (192.168.1.1) */
    };
    status = mmwlan_tx(arp_packet, sizeof(arp_packet));
    if (status != MMWLAN_SUCCESS)
    {
        printf("TX failed with status %d\n", status);
        MMOSAL_ASSERT(false);
    }

    while (true)
    {
        mmosal_task_sleep(5000);
        dump_link_rssi("connected");
        dump_umac_stats("connected");
    }
}
