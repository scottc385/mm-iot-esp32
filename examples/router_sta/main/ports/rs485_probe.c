#include "rs485_probe.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#define RS485_PROBE_RX_BUF_SIZE 512
#define RS485_PROBE_TX_BUF_SIZE 0
#define RS485_PROBE_PARSE_BUF_SIZE 1024
#define MSTP_PREAMBLE_0 0x55
#define MSTP_PREAMBLE_1 0xff
#define MSTP_MIN_FRAME_SIZE 8
#define MSTP_MAX_DATA_LEN 501

static bool s_started;
#if CONFIG_ROUTER_STA_RS485_PROBE_INTERVAL_MS > 0
static uint32_t s_last_tx_ms;
static uint32_t s_tx_count;
#endif
static uint32_t s_rx_count;
static uint32_t s_mstp_frame_count;
static uint32_t s_noise_count;
static uint32_t s_invalid_count;
static uint32_t s_token_count;
static uint32_t s_poll_for_master_count;
static uint32_t s_reply_to_poll_count;
static uint32_t s_data_frame_count;
#if CONFIG_ROUTER_STA_RS485_PROBE_SUMMARY_INTERVAL_MS > 0
static uint32_t s_last_summary_ms;
#endif
static uint8_t s_parse_buf[RS485_PROBE_PARSE_BUF_SIZE];
static size_t s_parse_len;

#if CONFIG_ROUTER_STA_RS485_PROBE_LOG_FRAMES
static const char *rs485_probe_frame_type_name(uint8_t type)
{
    switch (type) {
    case 0:
        return "token";
    case 1:
        return "poll-for-master";
    case 2:
        return "reply-to-poll";
    case 3:
        return "test-request";
    case 4:
        return "test-response";
    case 5:
        return "data-expecting-reply";
    case 6:
        return "data-not-expecting-reply";
    case 7:
        return "reply-postponed";
    default:
        return "unknown";
    }
}
#endif

static void rs485_probe_drop_prefix(size_t count)
{
    if (count >= s_parse_len) {
        s_parse_len = 0;
        return;
    }
    memmove(s_parse_buf, s_parse_buf + count, s_parse_len - count);
    s_parse_len -= count;
}

static void rs485_probe_parse_frames(void)
{
    while (s_parse_len >= 2) {
        size_t sync = 0;
        while (sync + 1 < s_parse_len &&
               !(s_parse_buf[sync] == MSTP_PREAMBLE_0 &&
                 s_parse_buf[sync + 1] == MSTP_PREAMBLE_1)) {
            sync++;
        }
        if (sync > 0) {
            s_noise_count += (uint32_t)sync;
#if CONFIG_ROUTER_STA_RS485_PROBE_LOG_FRAMES
            printf("RS485_RX_NOISE bytes=%u total=%" PRIu32 "\n",
                   (unsigned)sync,
                   s_noise_count);
#endif
            rs485_probe_drop_prefix(sync);
        }
        if (s_parse_len < MSTP_MIN_FRAME_SIZE) {
            return;
        }
        if (s_parse_buf[0] != MSTP_PREAMBLE_0 ||
            s_parse_buf[1] != MSTP_PREAMBLE_1) {
            rs485_probe_drop_prefix(1);
            continue;
        }

        uint8_t type = s_parse_buf[2];
#if CONFIG_ROUTER_STA_RS485_PROBE_LOG_FRAMES
        uint8_t dst = s_parse_buf[3];
        uint8_t src = s_parse_buf[4];
#endif
        uint16_t data_len =
            ((uint16_t)s_parse_buf[5] << 8) | (uint16_t)s_parse_buf[6];
#if CONFIG_ROUTER_STA_RS485_PROBE_LOG_FRAMES
        uint8_t header_crc = s_parse_buf[7];
#endif

        if (data_len > MSTP_MAX_DATA_LEN) {
            s_invalid_count++;
#if CONFIG_ROUTER_STA_RS485_PROBE_LOG_FRAMES
            uint8_t dst = s_parse_buf[3];
            uint8_t src = s_parse_buf[4];
            printf("RS485_MSTP_INVALID type=%u dst=%u src=%u len=%u reason=len\n",
                   type,
                   dst,
                   src,
                   data_len);
#endif
            rs485_probe_drop_prefix(1);
            continue;
        }

        size_t frame_len = MSTP_MIN_FRAME_SIZE + data_len;
        if (data_len > 0) {
            frame_len += 2; /* Data CRC. Header-only frames have no data CRC. */
        }
        if (s_parse_len < frame_len) {
            return;
        }

        s_mstp_frame_count++;
        switch (type) {
        case 0:
            s_token_count++;
            break;
        case 1:
            s_poll_for_master_count++;
            break;
        case 2:
            s_reply_to_poll_count++;
            break;
        case 5:
        case 6:
            s_data_frame_count++;
            break;
        default:
            break;
        }
#if CONFIG_ROUTER_STA_RS485_PROBE_LOG_FRAMES
        printf("RS485_MSTP_FRAME count=%" PRIu32
               " type=%u name=%s dst=%u src=%u len=%u hcrc=0x%02x\n",
               s_mstp_frame_count,
               type,
               rs485_probe_frame_type_name(type),
               dst,
               src,
               data_len,
               header_crc);
#endif
        rs485_probe_drop_prefix(frame_len);
    }
}

static uart_port_t rs485_probe_uart_num(void)
{
    return (uart_port_t)CONFIG_ROUTER_STA_RS485_UART_NUM;
}

static void rs485_probe_set_tx_enable(bool enable)
{
    gpio_set_level(CONFIG_ROUTER_STA_RS485_DE_GPIO, enable ? 1 : 0);
#if CONFIG_ROUTER_STA_RS485_RE_N_GPIO >= 0
    gpio_set_level(CONFIG_ROUTER_STA_RS485_RE_N_GPIO, enable ? 1 : 0);
#endif
}

bool rs485_probe_start(void)
{
    if (s_started) {
        return true;
    }

    printf("RS485_PROBE start uart=%d tx=%d rx=%d de=%d re_n=%d baud=%d interval_ms=%d\n",
           CONFIG_ROUTER_STA_RS485_UART_NUM,
           CONFIG_ROUTER_STA_RS485_TX_GPIO,
           CONFIG_ROUTER_STA_RS485_RX_GPIO,
           CONFIG_ROUTER_STA_RS485_DE_GPIO,
           CONFIG_ROUTER_STA_RS485_RE_N_GPIO,
           CONFIG_ROUTER_STA_RS485_BAUD,
           CONFIG_ROUTER_STA_RS485_PROBE_INTERVAL_MS);

    gpio_config_t out_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_ROUTER_STA_RS485_DE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&out_cfg) != ESP_OK) {
        printf("RS485_PROBE gpio_config DE failed\n");
        return false;
    }
#if CONFIG_ROUTER_STA_RS485_RE_N_GPIO >= 0
    out_cfg.pin_bit_mask = 1ULL << CONFIG_ROUTER_STA_RS485_RE_N_GPIO;
    if (gpio_config(&out_cfg) != ESP_OK) {
        printf("RS485_PROBE gpio_config RE_N failed\n");
        return false;
    }
#endif
    rs485_probe_set_tx_enable(false);

    uart_config_t uart_config = {
        .baud_rate = CONFIG_ROUTER_STA_RS485_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_port_t uart_num = rs485_probe_uart_num();
    esp_err_t err = uart_param_config(uart_num, &uart_config);
    if (err != ESP_OK) {
        printf("RS485_PROBE uart_param_config failed err=0x%x\n", (unsigned)err);
        return false;
    }

    err = uart_set_pin(uart_num,
                       CONFIG_ROUTER_STA_RS485_TX_GPIO,
                       CONFIG_ROUTER_STA_RS485_RX_GPIO,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        printf("RS485_PROBE uart_set_pin failed err=0x%x\n", (unsigned)err);
        return false;
    }

    err = uart_driver_install(uart_num,
                              RS485_PROBE_RX_BUF_SIZE,
                              RS485_PROBE_TX_BUF_SIZE,
                              0,
                              NULL,
                              0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("RS485_PROBE uart_driver_install failed err=0x%x\n", (unsigned)err);
        return false;
    }
    uart_flush_input(uart_num);
    s_started = true;
    printf("RS485_PROBE ready\n");
    return true;
}

static void rs485_probe_poll_rx(void)
{
    uint8_t buf[64];
    uart_port_t uart_num = rs485_probe_uart_num();
    int len = uart_read_bytes(uart_num, buf, sizeof(buf), 0);
    if (len <= 0) {
        return;
    }

    s_rx_count += (uint32_t)len;
    if (s_parse_len + (size_t)len > sizeof(s_parse_buf)) {
        printf("RS485_RX_OVERFLOW bytes=%d parse_len=%u total=%" PRIu32 "\n",
               len,
               (unsigned)s_parse_len,
               s_rx_count);
        s_parse_len = 0;
    }
    memcpy(s_parse_buf + s_parse_len, buf, (size_t)len);
    s_parse_len += (size_t)len;
    rs485_probe_parse_frames();
}

#if CONFIG_ROUTER_STA_RS485_PROBE_INTERVAL_MS > 0
static void rs485_probe_send_pattern(void)
{
    static const uint8_t pattern[] = {0x55, 0xaa, 0x00, 0xff};
    uart_port_t uart_num = rs485_probe_uart_num();

    rs485_probe_set_tx_enable(true);
    int written = uart_write_bytes(uart_num, pattern, sizeof(pattern));
    uart_wait_tx_done(uart_num, pdMS_TO_TICKS(100));
    rs485_probe_set_tx_enable(false);

    s_tx_count++;
    printf("RS485_TX seq=%" PRIu32 " bytes=%d pattern=55:aa:00:ff\n",
           s_tx_count,
           written);
}
#endif

void rs485_probe_poll(uint32_t now_ms)
{
    if (!s_started) {
        return;
    }

    rs485_probe_poll_rx();

#if CONFIG_ROUTER_STA_RS485_PROBE_SUMMARY_INTERVAL_MS > 0
    if (s_last_summary_ms == 0 ||
        now_ms - s_last_summary_ms >=
            CONFIG_ROUTER_STA_RS485_PROBE_SUMMARY_INTERVAL_MS) {
        s_last_summary_ms = now_ms;
        printf("RS485_STATUS rx_bytes=%" PRIu32 " frames=%" PRIu32
               " token=%" PRIu32 " pfm=%" PRIu32 " reply_poll=%" PRIu32
               " data=%" PRIu32 " invalid=%" PRIu32 " noise=%" PRIu32
               " parse_buf=%u\n",
               s_rx_count,
               s_mstp_frame_count,
               s_token_count,
               s_poll_for_master_count,
               s_reply_to_poll_count,
               s_data_frame_count,
               s_invalid_count,
               s_noise_count,
               (unsigned)s_parse_len);
    }
#endif

#if CONFIG_ROUTER_STA_RS485_PROBE_INTERVAL_MS > 0
    if (s_last_tx_ms == 0 ||
        now_ms - s_last_tx_ms >= CONFIG_ROUTER_STA_RS485_PROBE_INTERVAL_MS) {
        s_last_tx_ms = now_ms;
        rs485_probe_send_pattern();
    }
#endif
}
