/*
 * HaLow field survey station application.
 *
 * This app keeps the ESP32 STA side intentionally simple: connect with the known-good
 * static configuration, run an iperf UDP server, and expose status through serial,
 * the boot button, and an optional PWM RGB LED.
 *
 * LED status legend:
 * - Dim green: connected and idle.
 * - Cyan fast blink: test traffic is currently being received.
 * - Green flash: recent test completed with acceptable loss/errors.
 * - Amber flash: recent test completed with high loss/errors or no useful traffic.
 * - Red slow blink: out of range or RSSI below CONFIG_FIELD_OUT_OF_RANGE_RSSI_DBM.
 *
 * If GPIO48 addressable LED output causes instability on a specific board,
 * build with sdkconfig.defaults.esp32s3.heltec_ht_hc01p_noled as a fallback.
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/rmt_tx.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#include "led_strip_encoder.h"
#include "mm_app_common.h"
#include "mmipal.h"
#include "mmiperf.h"
#include "mmosal.h"
#include "mmwlan.h"
#include "mmwlan_stats.h"

#ifndef IPERF_SERVER_PORT
#define IPERF_SERVER_PORT 5001
#endif

#ifndef CONFIG_FIELD_BUTTON_GPIO
#define CONFIG_FIELD_BUTTON_GPIO 0
#endif

#ifndef CONFIG_FIELD_TRIGGER_AP_IP
#define CONFIG_FIELD_TRIGGER_AP_IP "192.168.50.1"
#endif

#ifndef CONFIG_FIELD_TRIGGER_PORT
#define CONFIG_FIELD_TRIGGER_PORT 5010
#endif

#ifndef CONFIG_FIELD_OUT_OF_RANGE_RSSI_DBM
#define CONFIG_FIELD_OUT_OF_RANGE_RSSI_DBM -88
#endif

#ifndef CONFIG_FIELD_STATUS_INTERVAL_MS
#define CONFIG_FIELD_STATUS_INTERVAL_MS 5000
#endif

#define FIELD_TEST_RUNNING_WINDOW_MS 3000
#define FIELD_RECENT_RESULT_WINDOW_MS 10000
#define FIELD_ADDR_LED_RESOLUTION_HZ 10000000

#if CONFIG_FIELD_RGB_ADDR_ENABLE || CONFIG_FIELD_RGB_PWM_ENABLE
#define FIELD_RGB_READY 1
#else
#define FIELD_RGB_READY 0
#endif

/** Array of power-of-10 unit specifiers. */
static const char units[] = {' ', 'K', 'M', 'G', 'T'};

static mmiperf_handle_t g_iperf_handle = NULL;
static uint32_t g_test_seq = 0;
static uint32_t g_last_rx_frames = 0;
static uint32_t g_last_rx_change_ms = 0;
static uint32_t g_last_report_ms = 0;
static bool g_last_report_ok = false;
static uint32_t g_last_report_errors = 0;
static uint32_t g_last_report_rx = 0;
static uint32_t g_last_report_kbps = 0;

#if CONFIG_FIELD_RGB_ADDR_ENABLE
static rmt_channel_handle_t g_led_chan = NULL;
static rmt_encoder_handle_t g_led_encoder = NULL;
static uint8_t g_led_pixel[3];
#endif

static const char *report_type_str(enum mmiperf_report_type type)
{
    switch (type)
    {
    case MMIPERF_TCP_DONE_SERVER: return "TCP_DONE_SERVER";
    case MMIPERF_TCP_DONE_CLIENT: return "TCP_DONE_CLIENT";
    case MMIPERF_TCP_ABORTED_LOCAL: return "TCP_ABORTED_LOCAL";
    case MMIPERF_TCP_ABORTED_LOCAL_DATAERROR: return "TCP_ABORTED_LOCAL_DATAERROR";
    case MMIPERF_TCP_ABORTED_LOCAL_TXERROR: return "TCP_ABORTED_LOCAL_TXERROR";
    case MMIPERF_TCP_ABORTED_REMOTE: return "TCP_ABORTED_REMOTE";
    case MMIPERF_UDP_DONE_SERVER: return "UDP_DONE_SERVER";
    case MMIPERF_UDP_DONE_CLIENT: return "UDP_DONE_CLIENT";
    case MMIPERF_INTERRIM_REPORT: return "INTERIM_REPORT";
    default: return "UNKNOWN";
    }
}

static uint32_t format_bytes(uint64_t bytes, uint8_t *unit_index)
{
    *unit_index = 0;
    while (bytes >= 1000 && *unit_index < 4)
    {
        bytes /= 1000;
        (*unit_index)++;
    }
    return bytes;
}

static int32_t get_link_rssi(void)
{
    return mmwlan_get_rssi();
}

static void get_umac_stats(struct mmwlan_stats_umac_data *stats)
{
    memset(stats, 0, sizeof(*stats));
    (void)mmwlan_get_umac_stats(stats);
}

#if CONFIG_FIELD_RGB_ADDR_ENABLE
static void rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (g_led_chan == NULL || g_led_encoder == NULL)
    {
        return;
    }

    /* WS2812/SK6812 protocol uses GRB byte order. */
    g_led_pixel[0] = g;
    g_led_pixel[1] = r;
    g_led_pixel[2] = b;

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    if (rmt_transmit(g_led_chan, g_led_encoder, g_led_pixel, sizeof(g_led_pixel), &tx_config) == ESP_OK)
    {
        (void)rmt_tx_wait_all_done(g_led_chan, 100);
    }
}

static void rgb_init(void)
{
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = CONFIG_FIELD_RGB_ADDR_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = FIELD_ADDR_LED_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };

    if (rmt_new_tx_channel(&tx_chan_config, &g_led_chan) != ESP_OK)
    {
        printf("Addressable RGB LED init failed: GPIO%d channel unavailable\n",
               CONFIG_FIELD_RGB_ADDR_GPIO);
        g_led_chan = NULL;
        return;
    }

    led_strip_encoder_config_t encoder_config = {
        .resolution = FIELD_ADDR_LED_RESOLUTION_HZ,
    };
    if (rmt_new_led_strip_encoder(&encoder_config, &g_led_encoder) != ESP_OK)
    {
        printf("Addressable RGB LED encoder init failed\n");
        g_led_encoder = NULL;
        return;
    }

    if (rmt_enable(g_led_chan) != ESP_OK)
    {
        printf("Addressable RGB LED RMT enable failed\n");
        g_led_chan = NULL;
        g_led_encoder = NULL;
        return;
    }

    printf("Addressable RGB status LED enabled on GPIO%d\n", CONFIG_FIELD_RGB_ADDR_GPIO);
}
#elif CONFIG_FIELD_RGB_PWM_ENABLE
static uint32_t led_duty(uint8_t value)
{
#if CONFIG_FIELD_RGB_ACTIVE_LOW
    return 255U - value;
#else
    return value;
#endif
}

static void rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, led_duty(r));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, led_duty(g));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, led_duty(b));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
}

static void rgb_init(void)
{
    if (CONFIG_FIELD_RGB_RED_GPIO < 0 || CONFIG_FIELD_RGB_GREEN_GPIO < 0 ||
        CONFIG_FIELD_RGB_BLUE_GPIO < 0)
    {
        printf("RGB PWM enabled but one or more RGB GPIOs are invalid; disabling LED output\n");
        return;
    }

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t channels[3] = {
        { .gpio_num = CONFIG_FIELD_RGB_RED_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE,
          .channel = LEDC_CHANNEL_0, .intr_type = LEDC_INTR_DISABLE,
          .timer_sel = LEDC_TIMER_0, .duty = led_duty(0), .hpoint = 0 },
        { .gpio_num = CONFIG_FIELD_RGB_GREEN_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE,
          .channel = LEDC_CHANNEL_1, .intr_type = LEDC_INTR_DISABLE,
          .timer_sel = LEDC_TIMER_0, .duty = led_duty(0), .hpoint = 0 },
        { .gpio_num = CONFIG_FIELD_RGB_BLUE_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE,
          .channel = LEDC_CHANNEL_2, .intr_type = LEDC_INTR_DISABLE,
          .timer_sel = LEDC_TIMER_0, .duty = led_duty(0), .hpoint = 0 },
    };

    for (unsigned i = 0; i < 3; i++)
    {
        ledc_channel_config(&channels[i]);
    }
}
#else
static void rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    (void)r;
    (void)g;
    (void)b;
}

static void rgb_init(void)
{
    printf("RGB PWM status LED disabled in sdkconfig\n");
}
#endif

static void button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << CONFIG_FIELD_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static bool button_is_pressed(void)
{
    int level = gpio_get_level(CONFIG_FIELD_BUTTON_GPIO);
#if CONFIG_FIELD_BUTTON_ACTIVE_LOW
    return level == 0;
#else
    return level != 0;
#endif
}

static void send_trigger_packet(void)
{
    int32_t rssi = get_link_rssi();
    struct mmwlan_stats_umac_data stats;
    get_umac_stats(&stats);

    char msg[160];
    g_test_seq++;
    int len = snprintf(msg, sizeof(msg),
                       "FIELD_TEST_TRIGGER seq=%lu uptime_ms=%lu rssi=%ld umac_rssi=%d\n",
                       (unsigned long)g_test_seq, (unsigned long)mmosal_get_time_ms(),
                       (long)rssi, stats.rssi);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0)
    {
        printf("button trigger: socket failed errno=%d\n", errno);
        return;
    }

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(CONFIG_FIELD_TRIGGER_PORT);
    dest.sin_addr.s_addr = inet_addr(CONFIG_FIELD_TRIGGER_AP_IP);

    int sent = -1;
    if (connect(sock, (struct sockaddr *)&dest, sizeof(dest)) == 0)
    {
        sent = send(sock, msg, len, 0);
    }
    else
    {
        printf("button trigger: connect failed errno=%d target=%s:%d\n",
               errno, CONFIG_FIELD_TRIGGER_AP_IP, CONFIG_FIELD_TRIGGER_PORT);
    }
    close(sock);

    printf("button trigger: seq=%lu sent=%d target=%s:%d rssi=%ld umac_rssi=%d\n",
           (unsigned long)g_test_seq, sent, CONFIG_FIELD_TRIGGER_AP_IP,
           CONFIG_FIELD_TRIGGER_PORT, (long)rssi, stats.rssi);
}

static void button_task(void *arg)
{
    (void)arg;
    bool was_pressed = false;
    uint32_t press_start_ms = 0;

    while (true)
    {
        bool pressed = button_is_pressed();
        uint32_t now = mmosal_get_time_ms();

        if (pressed && !was_pressed)
        {
            press_start_ms = now;
        }
        else if (!pressed && was_pressed)
        {
            uint32_t held_ms = now - press_start_ms;
            if (held_ms >= 50 && held_ms < 3000)
            {
                send_trigger_packet();
            }
            else if (held_ms >= 3000)
            {
                printf("button long press: status requested\n");
            }
        }

        was_pressed = pressed;
        mmosal_task_sleep(25);
    }
}

static void iperf_report_handler(const struct mmiperf_report *report, void *arg,
                                 mmiperf_handle_t handle)
{
    (void)arg;
    (void)handle;

    uint8_t bytes_unit = 0;
    uint32_t bytes_formatted = format_bytes(report->bytes_transferred, &bytes_unit);

    printf("\nIperf Report\n");
    printf("  Report Type: %s (%d)\n", report_type_str(report->report_type), report->report_type);
    printf("  Remote Address: %s:%d\n", report->remote_addr, report->remote_port);
    printf("  Local Address:  %s:%d\n", report->local_addr, report->local_port);
    printf("  Transferred: %lu %cBytes, duration: %lu ms, bandwidth: %lu kbps\n",
           bytes_formatted, units[bytes_unit], (unsigned long)report->duration_ms,
           (unsigned long)report->bandwidth_kbitpsec);
    printf("  UDP Frames: tx=%lu rx=%lu out_of_sequence=%lu errors=%lu ipg_count=%lu ipg_sum_ms=%lu\n\n",
           (unsigned long)report->tx_frames, (unsigned long)report->rx_frames,
           (unsigned long)report->out_of_sequence_frames, (unsigned long)report->error_count,
           (unsigned long)report->ipg_count, (unsigned long)report->ipg_sum_ms);

    if (report->report_type == MMIPERF_UDP_DONE_SERVER)
    {
        g_last_report_ms = mmosal_get_time_ms();
        g_last_report_errors = report->error_count;
        g_last_report_rx = report->rx_frames;
        g_last_report_kbps = report->bandwidth_kbitpsec;
        g_last_report_ok = (report->rx_frames > 0) &&
                           (report->error_count <= 2 || report->error_count <= (report->rx_frames / 10));
        printf("Waiting for client to connect...\n");
    }
}

static void start_udp_server(void)
{
    struct mmiperf_server_args args = MMIPERF_SERVER_ARGS_DEFAULT;
    args.local_port = IPERF_SERVER_PORT;
    args.report_fn = iperf_report_handler;

    g_iperf_handle = mmiperf_start_udp_server(&args);
    if (g_iperf_handle == NULL)
    {
        printf("Failed to start iperf UDP server\n");
        return;
    }

    printf("\nField-test UDP server started on port %u\n", args.local_port);

    struct mmipal_ip_config ip_config;
    if (mmipal_get_ip_config(&ip_config) == MMIPAL_SUCCESS)
    {
        printf("AP command: iperf -c %s -p %u -i 1 -u -b 500k -t 10\n",
               ip_config.ip_addr, args.local_port);
    }
}

static const char *derive_status(int32_t rssi, bool link_up, bool running, uint32_t now_ms)
{
    if (!link_up)
    {
        return "LINK_DOWN";
    }

    if (running)
    {
        return "TEST_RUNNING";
    }

    if (rssi == INT32_MIN || rssi <= CONFIG_FIELD_OUT_OF_RANGE_RSSI_DBM)
    {
        return "OUT_OF_RANGE";
    }

    if (g_last_report_ms != 0 && (now_ms - g_last_report_ms) < FIELD_RECENT_RESULT_WINDOW_MS)
    {
        return g_last_report_ok ? "TEST_OK" : "TEST_FAIL";
    }

    return "IDLE_CONNECTED";
}

static void update_led(const char *status, uint32_t now_ms)
{
    bool blink_fast = ((now_ms / 250U) % 2U) == 0;
    bool blink_slow = ((now_ms / 750U) % 2U) == 0;

    if (strcmp(status, "TEST_RUNNING") == 0)
    {
        rgb_set(0, blink_fast ? 180 : 0, blink_fast ? 180 : 0); /* cyan blink */
    }
    else if (strcmp(status, "TEST_OK") == 0)
    {
        rgb_set(0, blink_fast ? 220 : 20, 0); /* green flash */
    }
    else if (strcmp(status, "TEST_FAIL") == 0)
    {
        rgb_set(blink_fast ? 220 : 20, 120, 0); /* amber flash */
    }
    else if (strcmp(status, "OUT_OF_RANGE") == 0)
    {
        rgb_set(blink_slow ? 220 : 0, 0, 0); /* red slow blink */
    }
    else if (strcmp(status, "LINK_DOWN") == 0)
    {
        rgb_set(blink_slow ? 220 : 0, 0, 0); /* red slow blink */
    }
    else
    {
        rgb_set(0, 40, 0); /* dim green */
    }
}

static void status_task(void *arg)
{
    (void)arg;
    uint32_t last_print_ms = 0;

    while (true)
    {
        uint32_t now = mmosal_get_time_ms();
        struct mmiperf_report report;
        bool running = false;

        if (g_iperf_handle != NULL && mmiperf_get_interim_report(g_iperf_handle, &report))
        {
            if (report.rx_frames != g_last_rx_frames)
            {
                g_last_rx_frames = report.rx_frames;
                g_last_rx_change_ms = now;
            }
            running = (now - g_last_rx_change_ms) < FIELD_TEST_RUNNING_WINDOW_MS;
        }

        int32_t rssi = get_link_rssi();
        bool link_up = app_link_is_up();
        struct mmwlan_stats_umac_data stats;
        get_umac_stats(&stats);
        const char *status = derive_status(rssi, link_up, running, now);
        update_led(status, now);

        if ((now - last_print_ms) >= CONFIG_FIELD_STATUS_INTERVAL_MS)
        {
            last_print_ms = now;
            printf("FIELD status=%s link=%s uptime_ms=%lu rssi=%ld umac_rssi=%d rx_frames=%lu last_kbps=%lu last_rx=%lu last_errors=%lu seq=%lu\n",
                   status, link_up ? "up" : "down", (unsigned long)now, (long)rssi, stats.rssi,
                   (unsigned long)g_last_rx_frames, (unsigned long)g_last_report_kbps,
                   (unsigned long)g_last_report_rx, (unsigned long)g_last_report_errors,
                   (unsigned long)g_test_seq);
        }

        mmosal_task_sleep(250);
    }
}

void app_main(void)
{
    printf("\n\nMorse Field Test STA (Built " __DATE__ " " __TIME__ ")\n\n");
    printf("button_gpio=%d trigger=%s:%d out_of_range_rssi=%d rgb=%s\n",
           CONFIG_FIELD_BUTTON_GPIO, CONFIG_FIELD_TRIGGER_AP_IP, CONFIG_FIELD_TRIGGER_PORT,
           CONFIG_FIELD_OUT_OF_RANGE_RSSI_DBM, FIELD_RGB_READY ? "enabled" : "disabled");

    rgb_init();
    rgb_set(0, 0, 80);
    button_init();

    mmosal_task_create(button_task, NULL, MMOSAL_TASK_PRI_LOW, 2048, "field_button");

    app_wlan_init();
    app_wlan_start();

    start_udp_server();
    mmosal_task_create(status_task, NULL, MMOSAL_TASK_PRI_LOW, 2048, "field_status");

    while (true)
    {
        mmosal_task_sleep(UINT32_MAX);
    }
}
