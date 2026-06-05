#include "router_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps/ping/ping_sock.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/ip_addr.h"
#include "router_sta.h"
#include "router_runtime_config.h"
#include "sdkconfig.h"

static bool parse_bool(const char *text, bool *value)
{
    if (!text || !value) {
        return false;
    }
    if (strcmp(text, "on") == 0 || strcmp(text, "1") == 0 || strcmp(text, "true") == 0 ||
        strcmp(text, "yes") == 0 || strcmp(text, "enable") == 0 || strcmp(text, "enabled") == 0) {
        *value = true;
        return true;
    }
    if (strcmp(text, "off") == 0 || strcmp(text, "0") == 0 || strcmp(text, "false") == 0 ||
        strcmp(text, "no") == 0 || strcmp(text, "disable") == 0 || strcmp(text, "disabled") == 0) {
        *value = false;
        return true;
    }
    return false;
}

static void print_config(void)
{
    const router_runtime_config *cfg = router_runtime_config_get();
    printf("router.node_id=%u\n", (unsigned)cfg->node_id);
    printf("router.speed=%s\n", router_runtime_config_speed_name(cfg->speed));
    printf("router.w5500.dhcp=%s\n", cfg->w5500_dhcp ? "on" : "off");
    printf("router.w5500.ip=%s\n", cfg->w5500_ip);
    printf("router.w5500.netmask=%s\n", cfg->w5500_netmask);
    printf("router.w5500.gateway=%s\n", cfg->w5500_gateway);
    printf("router.mstp=%s", cfg->mstp_enable ? "on" : "off");
#if !CONFIG_ROUTER_STA_MSTP_ENABLE
    printf(" (compiled out)");
#endif
    printf("\n");
    printf("router.bip_net=%u\n", (unsigned)router_runtime_config_derive_net(0));
    printf("router.mstp_net=%u\n", (unsigned)router_runtime_config_derive_net(1));
    printf("note=settings apply after reboot\n");
}

static esp_err_t save_config(const router_runtime_config *cfg)
{
    esp_err_t err = router_runtime_config_save(cfg);
    if (err != ESP_OK) {
        printf("config save failed err=0x%x\n", (unsigned)err);
    } else {
        printf("saved; reboot to apply\n");
    }
    return err;
}

static int cmd_router_show(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    print_config();
    return 0;
}

static int cmd_router_set_node(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: router-set-node <1-999>\n");
        return 1;
    }
    long node = strtol(argv[1], NULL, 10);
    if (node < 1 || node > 999) {
        printf("node ID must be 1..999\n");
        return 1;
    }
    router_runtime_config cfg = *router_runtime_config_get();
    cfg.node_id = (uint16_t)node;
    return save_config(&cfg) == ESP_OK ? 0 : 1;
}

static int cmd_router_set_speed(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: router-set-speed slow|medium|fast\n");
        return 1;
    }
    router_sta_speed speed;
    if (!router_runtime_config_parse_speed(argv[1], &speed)) {
        printf("speed must be slow, medium, or fast\n");
        return 1;
    }
    router_runtime_config cfg = *router_runtime_config_get();
    cfg.speed = speed;
    return save_config(&cfg) == ESP_OK ? 0 : 1;
}

static int cmd_router_set_w5500_dhcp(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: router-set-w5500-dhcp on|off\n");
        return 1;
    }
    bool enabled;
    if (!parse_bool(argv[1], &enabled)) {
        printf("value must be on or off\n");
        return 1;
    }
    router_runtime_config cfg = *router_runtime_config_get();
    cfg.w5500_dhcp = enabled;
    return save_config(&cfg) == ESP_OK ? 0 : 1;
}

static bool set_string_field(char *dst, size_t dst_size, const char *value)
{
    if (!value || strlen(value) >= dst_size) {
        return false;
    }
    snprintf(dst, dst_size, "%s", value);
    return true;
}

static int cmd_router_set_w5500_ip(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: router-set-w5500-ip <ipv4>\n");
        return 1;
    }
    router_runtime_config cfg = *router_runtime_config_get();
    if (!set_string_field(cfg.w5500_ip, sizeof(cfg.w5500_ip), argv[1])) {
        printf("invalid IPv4 string\n");
        return 1;
    }
    cfg.w5500_dhcp = false;
    return save_config(&cfg) == ESP_OK ? 0 : 1;
}

static int cmd_router_set_w5500_mask(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: router-set-w5500-mask <ipv4-netmask>\n");
        return 1;
    }
    router_runtime_config cfg = *router_runtime_config_get();
    if (!set_string_field(cfg.w5500_netmask, sizeof(cfg.w5500_netmask), argv[1])) {
        printf("invalid netmask string\n");
        return 1;
    }
    cfg.w5500_dhcp = false;
    return save_config(&cfg) == ESP_OK ? 0 : 1;
}

static int cmd_router_set_w5500_gw(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: router-set-w5500-gw <ipv4-gateway>\n");
        return 1;
    }
    router_runtime_config cfg = *router_runtime_config_get();
    if (!set_string_field(cfg.w5500_gateway, sizeof(cfg.w5500_gateway), argv[1])) {
        printf("invalid gateway string\n");
        return 1;
    }
    cfg.w5500_dhcp = false;
    return save_config(&cfg) == ESP_OK ? 0 : 1;
}

static int cmd_router_set_mstp(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: router-set-mstp on|off\n");
        return 1;
    }
#if !CONFIG_ROUTER_STA_MSTP_ENABLE
    printf("MS/TP is compiled out of this firmware\n");
    return 1;
#else
    bool enabled;
    if (!parse_bool(argv[1], &enabled)) {
        printf("value must be on or off\n");
        return 1;
    }
    router_runtime_config cfg = *router_runtime_config_get();
    cfg.mstp_enable = enabled;
    return save_config(&cfg) == ESP_OK ? 0 : 1;
#endif
}

static int cmd_router_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("reboot requested\n");
    fflush(stdout);
    router_sta_request_reboot();
    return 0;
}

typedef struct {
    SemaphoreHandle_t done;
} router_ping_ctx;

static const char *router_ping_ipaddr_to_string(const ip_addr_t *addr, char *buf, size_t buf_len)
{
    const char *text = ipaddr_ntoa(addr);
    if (!text) {
        snprintf(buf, buf_len, "?");
    } else {
        snprintf(buf, buf_len, "%s", text);
    }
    return buf;
}

static void router_ping_success(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    uint32_t seqno = 0;
    uint32_t ttl = 0;
    uint32_t elapsed_ms = 0;
    uint32_t recv_len = 0;
    ip_addr_t target = IPADDR4_INIT(IPADDR_ANY);
    char addr_text[48];

    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &recv_len, sizeof(recv_len));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target, sizeof(target));

    printf("PING_REPLY from=%s seq=%lu bytes=%lu ttl=%lu time_ms=%lu\n",
           router_ping_ipaddr_to_string(&target, addr_text, sizeof(addr_text)),
           (unsigned long)seqno,
           (unsigned long)recv_len,
           (unsigned long)ttl,
           (unsigned long)elapsed_ms);
}

static void router_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    uint32_t seqno = 0;
    ip_addr_t target = IPADDR4_INIT(IPADDR_ANY);
    char addr_text[48];

    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target, sizeof(target));

    printf("PING_TIMEOUT from=%s seq=%lu\n",
           router_ping_ipaddr_to_string(&target, addr_text, sizeof(addr_text)),
           (unsigned long)seqno);
}

static void router_ping_end(esp_ping_handle_t hdl, void *args)
{
    router_ping_ctx *ctx = (router_ping_ctx *)args;
    uint32_t transmitted = 0;
    uint32_t received = 0;
    uint32_t duration_ms = 0;
    ip_addr_t target = IPADDR4_INIT(IPADDR_ANY);
    char addr_text[48];

    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &duration_ms, sizeof(duration_ms));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target, sizeof(target));

    printf("PING_DONE target=%s transmitted=%lu received=%lu lost=%lu duration_ms=%lu\n",
           router_ping_ipaddr_to_string(&target, addr_text, sizeof(addr_text)),
           (unsigned long)transmitted,
           (unsigned long)received,
           (unsigned long)(transmitted - received),
           (unsigned long)duration_ms);

    if (ctx && ctx->done) {
        xSemaphoreGive(ctx->done);
    }
}

static bool router_ping_resolve_target(const char *target_text, ip_addr_t *target)
{
    if (!target_text || !target) {
        return false;
    }
    if (ipaddr_aton(target_text, target)) {
        return true;
    }

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *result = NULL;
    int rc = getaddrinfo(target_text, NULL, &hints, &result);
    if (rc != 0 || !result) {
        printf("DNS lookup failed target=%s rc=%d\n", target_text, rc);
        return false;
    }

    const struct sockaddr_in *addr = (const struct sockaddr_in *)result->ai_addr;
    inet_addr_to_ip4addr(ip_2_ip4(target), &addr->sin_addr);
    IP_SET_TYPE_VAL(*target, IPADDR_TYPE_V4);
    freeaddrinfo(result);
    return true;
}

static int cmd_router_ping(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        printf("usage: router-ping <ip-or-host> [count]\n");
        return 1;
    }

    long count = 4;
    if (argc == 3) {
        count = strtol(argv[2], NULL, 10);
        if (count < 1 || count > 20) {
            printf("count must be 1..20\n");
            return 1;
        }
    }

    ip_addr_t target = IPADDR4_INIT(IPADDR_ANY);
    if (!router_ping_resolve_target(argv[1], &target)) {
        return 1;
    }

    router_ping_ctx ctx = {
        .done = xSemaphoreCreateBinary(),
    };
    if (!ctx.done) {
        printf("ping semaphore allocation failed\n");
        return 1;
    }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr = target;
    config.count = (uint32_t)count;
    config.interval_ms = 1000;
    config.timeout_ms = 1000;
    config.data_size = 32;

    esp_ping_callbacks_t callbacks = {
        .cb_args = &ctx,
        .on_ping_success = router_ping_success,
        .on_ping_timeout = router_ping_timeout,
        .on_ping_end = router_ping_end,
    };

    esp_ping_handle_t ping = NULL;
    esp_err_t err = esp_ping_new_session(&config, &callbacks, &ping);
    if (err != ESP_OK) {
        printf("ping session create failed err=0x%x\n", (unsigned)err);
        vSemaphoreDelete(ctx.done);
        return 1;
    }

    char addr_text[48];
    printf("PING_START target=%s count=%ld\n",
           router_ping_ipaddr_to_string(&target, addr_text, sizeof(addr_text)),
           count);
    err = esp_ping_start(ping);
    if (err != ESP_OK) {
        printf("ping start failed err=0x%x\n", (unsigned)err);
        esp_ping_delete_session(ping);
        vSemaphoreDelete(ctx.done);
        return 1;
    }

    TickType_t wait_ticks = pdMS_TO_TICKS((uint32_t)count * 1500u + 2000u);
    if (xSemaphoreTake(ctx.done, wait_ticks) != pdTRUE) {
        printf("ping wait timeout; stopping session\n");
        esp_ping_stop(ping);
    }

    esp_ping_delete_session(ping);
    vSemaphoreDelete(ctx.done);
    return 0;
}

static void register_cmd(const char *command, const char *help, esp_console_cmd_func_t func)
{
    const esp_console_cmd_t cmd = {
        .command = command,
        .help = help,
        .hint = NULL,
        .func = func,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

void router_cli_start(void)
{
#if !CONFIG_ROUTER_STA_CLI_ENABLE
    return;
#else
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "router>";
    repl_config.max_cmdline_length = 128;

    register_cmd("router-show", "Show persisted router settings", cmd_router_show);
    register_cmd("router-set-node", "Set node ID: router-set-node <1-999>", cmd_router_set_node);
    register_cmd("router-node", "Alias for router-set-node", cmd_router_set_node);
    register_cmd("router.node_id", "Alias for router-set-node", cmd_router_set_node);
    register_cmd("router-set-speed", "Set HaLow speed: router-set-speed slow|medium|fast", cmd_router_set_speed);
    register_cmd("router-speed", "Alias for router-set-speed", cmd_router_set_speed);
    register_cmd("router.speed", "Alias for router-set-speed", cmd_router_set_speed);
    register_cmd("router-set-w5500-dhcp", "Set W5500 DHCP: router-set-w5500-dhcp on|off", cmd_router_set_w5500_dhcp);
    register_cmd("router-w5500-dhcp", "Alias for router-set-w5500-dhcp", cmd_router_set_w5500_dhcp);
    register_cmd("router.w5500.dhcp", "Alias for router-set-w5500-dhcp", cmd_router_set_w5500_dhcp);
    register_cmd("router-set-w5500-ip", "Set W5500 static IP and disable DHCP", cmd_router_set_w5500_ip);
    register_cmd("router-w5500-ip", "Alias for router-set-w5500-ip", cmd_router_set_w5500_ip);
    register_cmd("router.w5500.ip", "Alias for router-set-w5500-ip", cmd_router_set_w5500_ip);
    register_cmd("router-set-w5500-mask", "Set W5500 static netmask and disable DHCP", cmd_router_set_w5500_mask);
    register_cmd("router-w5500-mask", "Alias for router-set-w5500-mask", cmd_router_set_w5500_mask);
    register_cmd("router.w5500.netmask", "Alias for router-set-w5500-mask", cmd_router_set_w5500_mask);
    register_cmd("router-set-w5500-gw", "Set W5500 static gateway and disable DHCP", cmd_router_set_w5500_gw);
    register_cmd("router-w5500-gw", "Alias for router-set-w5500-gw", cmd_router_set_w5500_gw);
    register_cmd("router.w5500.gateway", "Alias for router-set-w5500-gw", cmd_router_set_w5500_gw);
    register_cmd("router-set-mstp", "Set MS/TP enable: router-set-mstp on|off", cmd_router_set_mstp);
    register_cmd("router-mstp", "Alias for router-set-mstp", cmd_router_set_mstp);
    register_cmd("router.mstp", "Alias for router-set-mstp", cmd_router_set_mstp);
    register_cmd("router-reboot", "Reboot the ESP32", cmd_router_reboot);
    register_cmd("router-ping", "Ping an IP/host: router-ping <ip-or-host> [count]", cmd_router_ping);
    register_cmd("ping", "Alias for router-ping", cmd_router_ping);
    esp_console_register_help_command();

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#else
#error Unsupported console type
#endif

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    printf("ROUTER_CLI ready; type help or router-show\n");
#endif
}
