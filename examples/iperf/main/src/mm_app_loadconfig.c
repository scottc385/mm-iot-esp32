/*
 * Copyright 2021-2023 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Morse Micro load configuration helper
 *
 * This file contains helper routines to load commonly used configuration settings
 * such as SSID, password, IP address settings and country code from the config store.
 * If a particular setting is not found, defaults are used.  It is safe to call these
 * functions if none of the settings are available in config store.
 */

#include "mmwlan.h"
#include "mmipal.h"
#include "mmosal.h"
#include "mm_app_loadconfig.h"
#include "mm_app_regdb.h"


#define COUNTRY_CODE "US"
#ifndef COUNTRY_CODE
#error COUNTRY_CODE must be defined to the appropriate 2 character country code. \
       See mm_app_regdb.c for valid options.
#endif

#ifndef COUNTRY_CODE
#define COUNTRY_CODE "??"
#endif

/* Default SSID  */
#ifndef SSID
/** SSID of the AP to connect to. (Do not quote; it will be stringified.) */
#define SSID                            Halow-pi
#endif

/* Default passphrase  */
/* Default security type  */
#ifndef SECURITY_TYPE
/** Security type (@see mmwlan_security_type). */
#define SECURITY_TYPE                   MMWLAN_OPEN
#endif

/* Configure the STA to use DHCP, this overrides any static configuration.
 * If the @c ip.dhcp_enabled is set in the config store that will take priority */
// #define ENABLE_DHCP                     (1)

/* Static Network configuration */
#ifndef STATIC_LOCAL_IP
/** Statically configured IP address (if ENABLE_DHCP is not set). */
#define STATIC_LOCAL_IP                 "192.168.50.2"
#endif
#ifndef STATIC_GATEWAY
/** Statically configured gateway address (if ENABLE_DHCP is not set). */
#define STATIC_GATEWAY                  "192.168.50.1"
#endif
#ifndef STATIC_NETMASK
/** Statically configured netmask (if ENABLE_DHCP is not set). */
#define STATIC_NETMASK                  "255.255.255.0"
#endif

/* Static Network configuration */
#ifndef STATIC_LOCAL_IP6
/** Statically configured IP address (if ENABLE_AUTOCONFIG is not set). */
#define STATIC_LOCAL_IP6                 "FE80::2"
#endif

/*
 * Narrow RF/debug iperf attempts to the same explicit channel sets used by
 * sta_connect. This avoids relying on a full regulatory-domain scan while
 * sweeping the Pi AP through channel/BW combinations.
 */
#define IPERF_TEST_PRESET_1MHZ_LOW   1
#define IPERF_TEST_PRESET_1MHZ_MID   2
#define IPERF_TEST_PRESET_1MHZ_HIGH  3
#define IPERF_TEST_PRESET_2MHZ_MID   4
#define IPERF_TEST_PRESET_2MHZ_HIGH  5
#define IPERF_TEST_PRESET_4MHZ_LOW   6
#define IPERF_TEST_PRESET_4MHZ_MID   7
#define IPERF_TEST_PRESET_4MHZ_HIGH  8

/* Default: channel 13, 908.5 MHz, 1 MHz. */
#ifndef IPERF_TEST_CHANNEL_PRESET
#define IPERF_TEST_CHANNEL_PRESET IPERF_TEST_PRESET_1MHZ_LOW
#endif

#if IPERF_TEST_CHANNEL_PRESET == IPERF_TEST_PRESET_1MHZ_LOW
#define IPERF_TEST_CHANNEL_LABEL "channel=13 freq=908500000Hz bw=1MHz"
static const struct mmwlan_s1g_channel iperf_test_channels[] = {
    { 908500000, 10000, false, 68, 1, 13, 1, 36, 0, 0, 0 },
};
#elif IPERF_TEST_CHANNEL_PRESET == IPERF_TEST_PRESET_1MHZ_MID
#define IPERF_TEST_CHANNEL_LABEL "channel=25 freq=914500000Hz bw=1MHz"
static const struct mmwlan_s1g_channel iperf_test_channels[] = {
    { 914500000, 10000, false, 68, 1, 25, 1, 36, 0, 0, 0 },
};
#elif IPERF_TEST_CHANNEL_PRESET == IPERF_TEST_PRESET_1MHZ_HIGH
#define IPERF_TEST_CHANNEL_LABEL "channel=41 freq=922500000Hz bw=1MHz"
static const struct mmwlan_s1g_channel iperf_test_channels[] = {
    { 922500000, 10000, false, 68, 1, 41, 1, 36, 0, 0, 0 },
};
#elif IPERF_TEST_CHANNEL_PRESET == IPERF_TEST_PRESET_2MHZ_MID
#define IPERF_TEST_CHANNEL_LABEL "channel=26 freq=915000000Hz bw=2MHz primary=channel25/914500000Hz"
static const struct mmwlan_s1g_channel iperf_test_channels[] = {
    { 914500000, 10000, false, 68, 1, 25, 1, 36, 0, 0, 0 },
    { 915000000, 10000, false, 69, 2, 26, 2, 36, 0, 0, 0 },
};
#elif IPERF_TEST_CHANNEL_PRESET == IPERF_TEST_PRESET_2MHZ_HIGH
#define IPERF_TEST_CHANNEL_LABEL "channel=42 freq=923000000Hz bw=2MHz primary=channel41/922500000Hz"
static const struct mmwlan_s1g_channel iperf_test_channels[] = {
    { 922500000, 10000, false, 68, 1, 41, 1, 36, 0, 0, 0 },
    { 923000000, 10000, false, 69, 2, 42, 2, 36, 0, 0, 0 },
};
#elif IPERF_TEST_CHANNEL_PRESET == IPERF_TEST_PRESET_4MHZ_LOW
#define IPERF_TEST_CHANNEL_LABEL "channel=24 freq=914000000Hz bw=4MHz primary=channel26/915000000Hz"
static const struct mmwlan_s1g_channel iperf_test_channels[] = {
    { 914500000, 10000, false, 68, 1, 25, 1, 36, 0, 0, 0 },
    { 915000000, 10000, false, 69, 2, 26, 2, 36, 0, 0, 0 },
    { 914000000, 10000, false, 70, 3, 24, 4, 36, 0, 0, 0 },
};
#elif IPERF_TEST_CHANNEL_PRESET == IPERF_TEST_PRESET_4MHZ_MID
#define IPERF_TEST_CHANNEL_LABEL "channel=32 freq=918000000Hz bw=4MHz primary=channel34/919000000Hz"
static const struct mmwlan_s1g_channel iperf_test_channels[] = {
    { 918500000, 10000, false, 68, 1, 33, 1, 36, 0, 0, 0 },
    { 919000000, 10000, false, 69, 2, 34, 2, 36, 0, 0, 0 },
    { 918000000, 10000, false, 70, 3, 32, 4, 36, 0, 0, 0 },
};
#elif IPERF_TEST_CHANNEL_PRESET == IPERF_TEST_PRESET_4MHZ_HIGH
#define IPERF_TEST_CHANNEL_LABEL "channel=40 freq=922000000Hz bw=4MHz primary=channel42/923000000Hz"
static const struct mmwlan_s1g_channel iperf_test_channels[] = {
    { 922500000, 10000, false, 68, 1, 41, 1, 36, 0, 0, 0 },
    { 923000000, 10000, false, 69, 2, 42, 2, 36, 0, 0, 0 },
    { 922000000, 10000, false, 70, 3, 40, 4, 36, 0, 0, 0 },
};
#else
#error Unsupported IPERF_TEST_CHANNEL_PRESET
#endif

static const struct mmwlan_s1g_channel_list iperf_test_channel_list = {
    .country_code = "US",
    .num_channels = (sizeof(iperf_test_channels) / sizeof(iperf_test_channels[0])),
    .channels = iperf_test_channels,
};


/** Stringify macro. Do not use directly; use @ref STRINGIFY(). */
#define _STRINGIFY(x) #x
/** Convert the content of the given macro to a string. */
#define STRINGIFY(x) _STRINGIFY(x)

void load_mmipal_init_args(struct mmipal_init_args *args)
{
    /* Load default static IP in case we don't find the key */
    (void)mmosal_safer_strcpy(args->ip_addr, STATIC_LOCAL_IP, sizeof(args->ip_addr));

    /* Load default netmask in case we don't find the key */
    (void)mmosal_safer_strcpy(args->netmask, STATIC_NETMASK, sizeof(args->netmask));

    /* Load default gateway in case we don't find the key */
    (void)mmosal_safer_strcpy(args->gateway_addr, STATIC_GATEWAY, sizeof(args->gateway_addr));

#ifdef ENABLE_DHCP
    args->mode = MMIPAL_DHCP;
#else
    args->mode = MMIPAL_STATIC;
#endif

    if (args->mode == MMIPAL_DHCP)
    {
        printf("Initialize IPv4 using DHCP...\n");
    }
    else if (args->mode == MMIPAL_DHCP_OFFLOAD)
    {
        printf("Initialize IPv4 using DHCP offload...\n");
    }
    else
    {
        printf("Initialize IPv4 with static IP: %s...\n", args->ip_addr);
    }

    /* Load default static IPv6 in case we don't find the key */
    (void)mmosal_safer_strcpy(args->ip6_addr, STATIC_LOCAL_IP6, sizeof(args->ip6_addr));

    /* We set this as the by default IPv6 is set to disabled in @ref MMIPAL_INIT_ARGS_DEFAULT */
    args->ip6_mode = MMIPAL_IP6_AUTOCONFIG;

    if (args->ip6_mode == MMIPAL_IP6_AUTOCONFIG)
    {
        printf("Initialize IPv6 using Autoconfig...\n");
    }
    else
    {
        printf("Initialize IPv6 with static IP %s\n", args->ip6_addr);
    }
}

const struct mmwlan_s1g_channel_list* load_channel_list(void)
{
    printf("IPERF test channel: country=%s " IPERF_TEST_CHANNEL_LABEL "\n",
           iperf_test_channel_list.country_code);
    return &iperf_test_channel_list;
}

void load_mmwlan_sta_args(struct mmwlan_sta_args *sta_config)
{
    /* Load SSID */
    (void)mmosal_safer_strcpy((char*)sta_config->ssid, STRINGIFY(SSID), sizeof(sta_config->ssid));
    sta_config->ssid_len = strlen((char*)sta_config->ssid);

    sta_config->passphrase[0] = '\0';
    sta_config->passphrase_len = 0;

    /* Load security type */
    sta_config->security_type = SECURITY_TYPE;
}

void load_mmwlan_settings(void)
{
}
