#ifndef ROUTER_MGMT_COMMAND_H
#define ROUTER_MGMT_COMMAND_H

#include <stdbool.h>

typedef enum tb_router_mgmt_result {
    TB_ROUTER_MGMT_CONTINUE = 0,
    TB_ROUTER_MGMT_STOP = 1,
} tb_router_mgmt_result;

typedef void (*tb_router_mgmt_callback)(void *ctx);
typedef void (*tb_router_mgmt_write_line)(void *ctx, const char *line);
typedef bool (*tb_router_mgmt_action)(void *ctx);
typedef bool (*tb_router_mgmt_cfg_get)(void *ctx, const char *key);
typedef bool (*tb_router_mgmt_cfg_set)(void *ctx, const char *key, const char *value);
typedef bool (*tb_router_mgmt_cfg_save)(void *ctx);

typedef struct tb_router_mgmt_handlers {
    tb_router_mgmt_callback show_status;
    tb_router_mgmt_callback show_ports;
    tb_router_mgmt_callback show_routes;
    tb_router_mgmt_callback show_transport_status;
    tb_router_mgmt_callback show_interface_status;
    tb_router_mgmt_callback show_halow_interface_status;
    tb_router_mgmt_action apply_halow_interface;
    tb_router_mgmt_callback cfg_show;
    tb_router_mgmt_cfg_get cfg_get;
    tb_router_mgmt_cfg_set cfg_set;
    tb_router_mgmt_cfg_save cfg_save;
    tb_router_mgmt_write_line write_line;
} tb_router_mgmt_handlers;

void tb_router_mgmt_print_help(const tb_router_mgmt_handlers *handlers, void *ctx);
tb_router_mgmt_result tb_router_mgmt_dispatch(const char *line,
                                              const tb_router_mgmt_handlers *handlers,
                                              void *ctx);

#endif
