#include "mstp_port.h"

#include <stdio.h>
#include <string.h>

#include "bacnet/datalink/dlmstp.h"
#include "bacnet/datalink/mstp.h"
#include "bacnet/datalink/mstpdef.h"
#include "bacnet/npdu.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifndef CONFIG_ROUTER_STA_MSTP_UART_NUM
#define CONFIG_ROUTER_STA_MSTP_UART_NUM 1
#endif
#ifndef CONFIG_ROUTER_STA_MSTP_TX_GPIO
#define CONFIG_ROUTER_STA_MSTP_TX_GPIO 10
#endif
#ifndef CONFIG_ROUTER_STA_MSTP_RX_GPIO
#define CONFIG_ROUTER_STA_MSTP_RX_GPIO 11
#endif
#ifndef CONFIG_ROUTER_STA_MSTP_DE_GPIO
#define CONFIG_ROUTER_STA_MSTP_DE_GPIO 12
#endif
#ifndef CONFIG_ROUTER_STA_MSTP_RE_N_GPIO
#define CONFIG_ROUTER_STA_MSTP_RE_N_GPIO 13
#endif
#ifndef CONFIG_ROUTER_STA_MSTP_BAUD
#define CONFIG_ROUTER_STA_MSTP_BAUD 38400
#endif
#ifndef CONFIG_ROUTER_STA_MSTP_MAC
#define CONFIG_ROUTER_STA_MSTP_MAC 3
#endif
#ifndef CONFIG_ROUTER_STA_MSTP_MAX_MASTER
#define CONFIG_ROUTER_STA_MSTP_MAX_MASTER 127
#endif
#ifndef CONFIG_ROUTER_STA_MSTP_MAX_INFO_FRAMES
#define CONFIG_ROUTER_STA_MSTP_MAX_INFO_FRAMES 1
#endif
#ifndef CONFIG_ROUTER_STA_MSTP_RX_STREAM_SIZE
#define CONFIG_ROUTER_STA_MSTP_RX_STREAM_SIZE 4096
#endif
#ifndef CONFIG_ROUTER_STA_MSTP_PDU_QUEUE_LEN
#define CONFIG_ROUTER_STA_MSTP_PDU_QUEUE_LEN 16
#endif
#ifndef CONFIG_ROUTER_STA_MSTP_DRAIN_MAX
#define CONFIG_ROUTER_STA_MSTP_DRAIN_MAX 8
#endif
#ifndef CONFIG_ROUTER_STA_MSTP_LOG_FRAMES
#define CONFIG_ROUTER_STA_MSTP_LOG_FRAMES 0
#endif
#ifndef CONFIG_ROUTER_STA_MSTP_FSM_TASK_STACK_SIZE
#define CONFIG_ROUTER_STA_MSTP_FSM_TASK_STACK_SIZE 4096
#endif
#ifndef CONFIG_ROUTER_STA_MSTP_UART_TASK_STACK_SIZE
#define CONFIG_ROUTER_STA_MSTP_UART_TASK_STACK_SIZE 3072
#endif

#define UART_EVENT_QUEUE_SIZE 20
#define MSTP_PDU_QUEUE_LEN CONFIG_ROUTER_STA_MSTP_PDU_QUEUE_LEN

typedef struct {
    uint16_t len;
    uint8_t sadr_len;
    uint8_t sadr;
    uint8_t buf[MSTP_PORT_MAX_NPDU];
} mstp_pdu_slot;

static mstp_port *s_port_stats;
static struct mstp_port_struct_t s_mstp;
static struct dlmstp_user_data_t s_user;
static struct dlmstp_rs485_driver s_driver;
static uint8_t s_rx_buf[DLMSTP_MPDU_MAX];
static uint8_t s_tx_buf[DLMSTP_MPDU_MAX];
static StreamBufferHandle_t s_rx_stream;
static QueueHandle_t s_uart_evt_queue;
static TaskHandle_t s_uart_task;
static TaskHandle_t s_fsm_task;
static QueueHandle_t s_pdu_free_q;
static QueueHandle_t s_pdu_ready_q;
static mstp_pdu_slot s_pdu_slots[MSTP_PDU_QUEUE_LEN];
static bool s_inited;
static bool s_tx_in_progress;
static uint64_t s_last_activity_us;

unsigned long mstimer_now(void)
{
    return (unsigned long)(esp_timer_get_time() / 1000ULL);
}

static uint64_t now_us(void)
{
    return esp_timer_get_time();
}

static void set_rx_enabled(bool enabled)
{
#if CONFIG_ROUTER_STA_MSTP_RE_N_GPIO >= 0
    gpio_set_level(CONFIG_ROUTER_STA_MSTP_RE_N_GPIO, enabled ? 0 : 1);
#endif
}

static void set_tx_enabled(bool enabled)
{
#if CONFIG_ROUTER_STA_MSTP_DE_GPIO >= 0
    gpio_set_level(CONFIG_ROUTER_STA_MSTP_DE_GPIO, enabled ? 1 : 0);
#endif
    set_rx_enabled(!enabled);
}

static void rs485_init(void)
{
    const int uart_num = CONFIG_ROUTER_STA_MSTP_UART_NUM;
    uart_config_t cfg = {
        .baud_rate = CONFIG_ROUTER_STA_MSTP_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(uart_num, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(uart_num,
                                 CONFIG_ROUTER_STA_MSTP_TX_GPIO,
                                 CONFIG_ROUTER_STA_MSTP_RX_GPIO,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(uart_num,
                                        2048,
                                        0,
                                        UART_EVENT_QUEUE_SIZE,
                                        &s_uart_evt_queue,
                                        0));

#if CONFIG_ROUTER_STA_MSTP_DE_GPIO >= 0
    gpio_config_t de_conf = {
        .pin_bit_mask = 1ULL << CONFIG_ROUTER_STA_MSTP_DE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&de_conf));
#endif
#if CONFIG_ROUTER_STA_MSTP_RE_N_GPIO >= 0
    gpio_config_t re_conf = {
        .pin_bit_mask = 1ULL << CONFIG_ROUTER_STA_MSTP_RE_N_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&re_conf));
#endif
    set_tx_enabled(false);
}

static void rs485_send(const uint8_t *payload, uint16_t payload_len)
{
    if (!payload || payload_len == 0) {
        return;
    }

    const int uart_num = CONFIG_ROUTER_STA_MSTP_UART_NUM;
    s_tx_in_progress = true;
    set_tx_enabled(true);
    int written = uart_write_bytes(uart_num, payload, payload_len);
    (void)uart_wait_tx_done(uart_num, pdMS_TO_TICKS(2000));
    set_tx_enabled(false);
    s_tx_in_progress = false;
    s_last_activity_us = now_us();
    if (written > 0 && s_port_stats) {
        s_port_stats->tx_bytes += (uint32_t)written;
    }
}

static bool rs485_read(uint8_t *buf)
{
    if (!buf || !s_rx_stream) {
        return false;
    }
    size_t got = xStreamBufferReceive(s_rx_stream, buf, 1, 0);
    if (got == 1) {
        s_last_activity_us = now_us();
        if (s_port_stats) {
            s_port_stats->rx_bytes++;
        }
        return true;
    }
    return false;
}

static bool rs485_transmitting(void)
{
    return s_tx_in_progress;
}

static uint32_t rs485_baud_rate(void)
{
    return CONFIG_ROUTER_STA_MSTP_BAUD;
}

static bool rs485_baud_rate_set(uint32_t baud)
{
    return uart_set_baudrate(CONFIG_ROUTER_STA_MSTP_UART_NUM, baud) == ESP_OK;
}

static uint32_t rs485_silence_ms(void)
{
    if (s_last_activity_us == 0) {
        return 0;
    }
    return (uint32_t)((now_us() - s_last_activity_us) / 1000ULL);
}

static void rs485_silence_reset(void)
{
    s_last_activity_us = now_us();
}

static void rs485_silence_reset_arg(void *arg)
{
    (void)arg;
    rs485_silence_reset();
}

static uint32_t rs485_silence_ms_arg(void *arg)
{
    (void)arg;
    return rs485_silence_ms();
}

static void uart_event_task(void *arg)
{
    (void)arg;
    uart_event_t event;
    uint8_t buf[128];
    const int uart_num = CONFIG_ROUTER_STA_MSTP_UART_NUM;

    while (true) {
        if (xQueueReceive(s_uart_evt_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (event.type == UART_DATA) {
            size_t remaining = event.size;
            while (remaining > 0) {
                size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
                int read = uart_read_bytes(uart_num, buf, chunk, 0);
                if (read <= 0) {
                    break;
                }
                size_t sent = s_rx_stream ? xStreamBufferSend(s_rx_stream, buf, (size_t)read, 0) : 0;
                if (sent < (size_t)read && s_port_stats) {
                    s_port_stats->rx_drops += (uint32_t)((size_t)read - sent);
                }
                remaining -= (size_t)read;
            }
        } else if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
            uart_flush_input(uart_num);
            xQueueReset(s_uart_evt_queue);
        }
    }
}

static void mstp_fsm_task(void *arg)
{
    (void)arg;
    while (true) {
        int slot_idx = -1;
        if (s_pdu_free_q && xQueueReceive(s_pdu_free_q, &slot_idx, pdMS_TO_TICKS(1)) != pdTRUE) {
            slot_idx = -1;
        }

        if (slot_idx < 0 || slot_idx >= MSTP_PDU_QUEUE_LEN) {
            (void)dlmstp_receive(NULL, NULL, 0, 5);
            continue;
        }

        mstp_pdu_slot *slot = &s_pdu_slots[slot_idx];
        BACNET_ADDRESS src = {0};
        uint16_t pdu_len = dlmstp_receive(&src, slot->buf, sizeof(slot->buf), 5);
        if (pdu_len > 0) {
            slot->len = pdu_len;
            slot->sadr_len = src.mac_len;
            slot->sadr = src.mac_len > 0 ? src.mac[0] : 0;
            if (s_pdu_ready_q && xQueueSend(s_pdu_ready_q, &slot_idx, 0) == pdTRUE) {
                if (s_port_stats) {
                    UBaseType_t queued = uxQueueMessagesWaiting(s_pdu_ready_q);
                    if ((uint32_t)queued > s_port_stats->pdu_queue_depth_max) {
                        s_port_stats->pdu_queue_depth_max = (uint32_t)queued;
                    }
                }
                continue;
            }
            if (s_port_stats) {
                s_port_stats->pdu_drops++;
            }
        }
        if (s_pdu_free_q) {
            (void)xQueueSend(s_pdu_free_q, &slot_idx, 0);
        }
    }
}

bool mstp_port_open(mstp_port *port, uint16_t net)
{
    if (!port || net == 0 || s_inited) {
        return false;
    }

    memset(port, 0, sizeof(*port));
    port->net = net;
    port->mac = (uint8_t)CONFIG_ROUTER_STA_MSTP_MAC;
    port->baud = (uint32_t)CONFIG_ROUTER_STA_MSTP_BAUD;
    s_port_stats = port;

    s_rx_stream = xStreamBufferCreate(CONFIG_ROUTER_STA_MSTP_RX_STREAM_SIZE, 1);
    s_pdu_free_q = xQueueCreate(MSTP_PDU_QUEUE_LEN, sizeof(int));
    s_pdu_ready_q = xQueueCreate(MSTP_PDU_QUEUE_LEN, sizeof(int));
    if (!s_rx_stream || !s_pdu_free_q || !s_pdu_ready_q) {
        printf("MSTP_OPEN queue allocation failed\n");
        return false;
    }
    for (int i = 0; i < MSTP_PDU_QUEUE_LEN; i++) {
        (void)xQueueSend(s_pdu_free_q, &i, 0);
    }

    memset(&s_mstp, 0, sizeof(s_mstp));
    memset(&s_user, 0, sizeof(s_user));
    memset(&s_driver, 0, sizeof(s_driver));

    rs485_init();
    s_driver.init = rs485_init;
    s_driver.send = rs485_send;
    s_driver.read = rs485_read;
    s_driver.transmitting = rs485_transmitting;
    s_driver.baud_rate = rs485_baud_rate;
    s_driver.baud_rate_set = rs485_baud_rate_set;
    s_driver.silence_milliseconds = rs485_silence_ms;
    s_driver.silence_reset = rs485_silence_reset;

    s_user.RS485_Driver = &s_driver;
    s_mstp.UserData = &s_user;
    s_mstp.InputBuffer = s_rx_buf;
    s_mstp.InputBufferSize = sizeof(s_rx_buf);
    s_mstp.OutputBuffer = s_tx_buf;
    s_mstp.OutputBufferSize = sizeof(s_tx_buf);
    s_mstp.This_Station = port->mac;
    s_mstp.Nmax_info_frames = (uint8_t)CONFIG_ROUTER_STA_MSTP_MAX_INFO_FRAMES;
    s_mstp.Nmax_master = (uint8_t)CONFIG_ROUTER_STA_MSTP_MAX_MASTER;
    s_mstp.Tframe_abort = DEFAULT_Tframe_abort;
    s_mstp.Treply_delay = DEFAULT_Treply_delay;
    s_mstp.Treply_timeout = DEFAULT_Treply_timeout;
    s_mstp.Tusage_timeout = DEFAULT_Tusage_timeout;
    s_mstp.SlaveNodeEnabled = false;
    s_mstp.ZeroConfigEnabled = false;
    s_mstp.SilenceTimer = rs485_silence_ms_arg;
    s_mstp.SilenceTimerReset = rs485_silence_reset_arg;

    if (!dlmstp_init((char *)&s_mstp)) {
        printf("MSTP_OPEN dlmstp_init failed\n");
        return false;
    }
    dlmstp_set_baud_rate(port->baud);
    dlmstp_set_mac_address(port->mac);
    dlmstp_set_max_master((uint8_t)CONFIG_ROUTER_STA_MSTP_MAX_MASTER);
    dlmstp_set_max_info_frames((uint8_t)CONFIG_ROUTER_STA_MSTP_MAX_INFO_FRAMES);

    s_last_activity_us = now_us();
    s_inited = true;
    port->active = true;

    xTaskCreate(uart_event_task,
                "mstp_uart_rx",
                CONFIG_ROUTER_STA_MSTP_UART_TASK_STACK_SIZE,
                NULL,
                12,
                &s_uart_task);
    xTaskCreate(mstp_fsm_task,
                "mstp_fsm",
                CONFIG_ROUTER_STA_MSTP_FSM_TASK_STACK_SIZE,
                NULL,
                10,
                &s_fsm_task);

    printf("MSTP_OPEN net=%u mac=%u uart=%d tx=%d rx=%d de=%d re_n=%d baud=%lu max_master=%u max_info=%u\n",
           (unsigned)net,
           (unsigned)port->mac,
           CONFIG_ROUTER_STA_MSTP_UART_NUM,
           CONFIG_ROUTER_STA_MSTP_TX_GPIO,
           CONFIG_ROUTER_STA_MSTP_RX_GPIO,
           CONFIG_ROUTER_STA_MSTP_DE_GPIO,
           CONFIG_ROUTER_STA_MSTP_RE_N_GPIO,
           (unsigned long)port->baud,
           (unsigned)CONFIG_ROUTER_STA_MSTP_MAX_MASTER,
           (unsigned)CONFIG_ROUTER_STA_MSTP_MAX_INFO_FRAMES);
    return true;
}

static void mstp_router_send_npdu(tb_router_service *svc,
                                  void *user_ctx,
                                  tb_router_port *router_port,
                                  const uint8_t *npdu,
                                  size_t npdu_len,
                                  const BACNET_ADDRESS *daddr)
{
    (void)svc;
    (void)user_ctx;
    mstp_port *port = (mstp_port *)tb_router_port_transport_state(router_port);
    if (!port || !port->active || !npdu || npdu_len == 0) {
        return;
    }

    BACNET_ADDRESS dest = {0};
    dest.mac_len = 1;
    if (daddr && daddr->len > 0) {
        dest.mac[0] = daddr->adr[0];
    } else {
        dest.mac[0] = MSTP_BROADCAST_ADDRESS;
    }

    BACNET_ADDRESS src = {0};
    BACNET_NPDU_DATA npdu_data = {0};
    int offset = bacnet_npdu_decode(npdu, npdu_len, &dest, &src, &npdu_data);
    if (offset < 0) {
        npdu_data.data_expecting_reply = false;
    }

    int sent = dlmstp_send_pdu(&dest, &npdu_data, (uint8_t *)npdu, (unsigned)npdu_len);
    if (sent > 0) {
        port->tx_pdu++;
#if CONFIG_ROUTER_STA_MSTP_LOG_FRAMES
        printf("TX_MSTP net=%u dst=%u npdu_len=%u tx_pdu=%lu\n",
               (unsigned)port->net,
               (unsigned)dest.mac[0],
               (unsigned)npdu_len,
               (unsigned long)port->tx_pdu);
#endif
    } else {
        port->tx_errors++;
        printf("TX_MSTP_ERR net=%u dst=%u npdu_len=%u errors=%lu\n",
               (unsigned)port->net,
               (unsigned)dest.mac[0],
               (unsigned)npdu_len,
               (unsigned long)port->tx_errors);
    }
}

const tb_router_transport_ops k_mstp_router_ops = {
    .name = "mstp",
    .send_npdu = mstp_router_send_npdu,
};

void mstp_port_poll(mstp_port *port,
                    tb_router_service *svc,
                    uint8_t router_port_id,
                    uint32_t now_ms)
{
    if (!port || !port->active || !svc || !s_pdu_ready_q || !s_pdu_free_q) {
        return;
    }

    for (unsigned drained = 0; drained < CONFIG_ROUTER_STA_MSTP_DRAIN_MAX; drained++) {
        int slot_idx = -1;
        if (xQueueReceive(s_pdu_ready_q, &slot_idx, 0) != pdTRUE) {
            return;
        }
        if (slot_idx < 0 || slot_idx >= MSTP_PDU_QUEUE_LEN) {
            continue;
        }

        mstp_pdu_slot *slot = &s_pdu_slots[slot_idx];
        uint8_t sadr[1] = { slot->sadr };
        port->rx_pdu++;
#if CONFIG_ROUTER_STA_MSTP_LOG_FRAMES
        printf("RX_MSTP net=%u src=%s%u npdu_len=%u rx_pdu=%lu\n",
               (unsigned)port->net,
               slot->sadr_len ? "" : "-",
               slot->sadr_len ? (unsigned)slot->sadr : 0U,
               (unsigned)slot->len,
               (unsigned long)port->rx_pdu);
#endif
        int rc = tb_router_service_handle_frame(svc,
                                                router_port_id,
                                                slot->buf,
                                                slot->len,
                                                slot->sadr_len ? sadr : NULL,
                                                slot->sadr_len ? 1U : 0U,
                                                0,
                                                now_ms);
        if (rc >= 0) {
            port->rx_router_accepted++;
        }
#if CONFIG_ROUTER_STA_MSTP_LOG_FRAMES
        printf("RX_MSTP_ROUTER rc=%d accepted=%lu\n", rc, (unsigned long)port->rx_router_accepted);
#endif
        (void)xQueueSend(s_pdu_free_q, &slot_idx, 0);
    }
}

void mstp_port_print_status(const mstp_port *port)
{
    if (!port || !port->active) {
        return;
    }
    UBaseType_t ready = s_pdu_ready_q ? uxQueueMessagesWaiting(s_pdu_ready_q) : 0;
    UBaseType_t free_slots = s_pdu_free_q ? uxQueueMessagesWaiting(s_pdu_free_q) : 0;
    printf("MSTP_STATUS net=%u mac=%u baud=%lu tx_pdu=%lu rx_pdu=%lu router_rx=%lu tx_bytes=%lu rx_bytes=%lu tx_errors=%lu rx_drops=%lu pdu_drops=%lu ready_q=%lu/%u free_q=%lu depth_max=%lu drain_max=%u log_frames=%u\n",
           (unsigned)port->net,
           (unsigned)port->mac,
           (unsigned long)port->baud,
           (unsigned long)port->tx_pdu,
           (unsigned long)port->rx_pdu,
           (unsigned long)port->rx_router_accepted,
           (unsigned long)port->tx_bytes,
           (unsigned long)port->rx_bytes,
           (unsigned long)port->tx_errors,
           (unsigned long)port->rx_drops,
           (unsigned long)port->pdu_drops,
           (unsigned long)ready,
           (unsigned)MSTP_PDU_QUEUE_LEN,
           (unsigned long)free_slots,
           (unsigned long)port->pdu_queue_depth_max,
           (unsigned)CONFIG_ROUTER_STA_MSTP_DRAIN_MAX,
           (unsigned)CONFIG_ROUTER_STA_MSTP_LOG_FRAMES);
}
