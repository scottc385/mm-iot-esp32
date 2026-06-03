#include "router_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_err.h"
#include "esp_system.h"
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
