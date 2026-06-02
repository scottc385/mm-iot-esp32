#include "router_mgmt_command.h"

#include <stddef.h>
#include <string.h>

static void write_line(const tb_router_mgmt_handlers *handlers, void *ctx, const char *line)
{
    if (handlers && handlers->write_line) {
        handlers->write_line(ctx, line);
    }
}

static const char *trim_start(const char *line)
{
    while (line && (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n')) {
        line++;
    }
    return line ? line : "";
}

static size_t trimmed_len(const char *line)
{
    size_t len = strlen(line);
    while (len > 0 &&
           (line[len - 1] == ' ' || line[len - 1] == '\t' ||
            line[len - 1] == '\r' || line[len - 1] == '\n')) {
        len--;
    }
    return len;
}

static bool cmd_eq(const char *line, const char *cmd)
{
    size_t len = trimmed_len(line);
    return len == strlen(cmd) && strncmp(line, cmd, len) == 0;
}

static bool starts_with_token(const char *line, const char *token, const char **rest)
{
    size_t token_len = strlen(token);
    size_t line_len = trimmed_len(line);
    if (line_len < token_len || strncmp(line, token, token_len) != 0) {
        return false;
    }
    if (line_len > token_len && line[token_len] != ' ' && line[token_len] != '\t') {
        return false;
    }
    *rest = trim_start(line + token_len);
    return true;
}

static bool split_key_value(const char *line,
                            size_t line_len,
                            const char **key,
                            size_t *key_len,
                            const char **value,
                            size_t *value_len)
{
    const char *cursor = line;
    while ((size_t)(cursor - line) < line_len && *cursor != ' ' && *cursor != '\t') {
        cursor++;
    }
    *key = line;
    *key_len = (size_t)(cursor - line);
    cursor = trim_start(cursor);
    *value = cursor;
    *value_len = line_len - (size_t)(cursor - line);
    return *key_len > 0 && *value_len > 0;
}

static bool copy_token(char *out, size_t out_len, const char *token, size_t token_len)
{
    if (!out || out_len == 0 || token_len >= out_len) {
        return false;
    }
    memcpy(out, token, token_len);
    out[token_len] = '\0';
    return true;
}

void tb_router_mgmt_print_help(const tb_router_mgmt_handlers *handlers, void *ctx)
{
    write_line(handlers, ctx, "router CLI commands:");
    write_line(handlers, ctx, "  router status      show router summary, routes, and queue depth");
    write_line(handlers, ctx, "  router ports       show configured router ports");
    write_line(handlers, ctx, "  router routes      show learned/static DNET routes");
    write_line(handlers, ctx, "  transport status   show transport adapter status");
    write_line(handlers, ctx, "  interface status   show network interface status");
    write_line(handlers, ctx, "  interface halow status  show HaLow interface status");
    write_line(handlers, ctx, "  interface halow apply   print/apply configured HaLow settings");
    write_line(handlers, ctx, "  cfg show           show staged/current configuration");
    write_line(handlers, ctx, "  cfg get <key>      read a configuration value");
    write_line(handlers, ctx, "  cfg set <key> <v>  stage a configuration value");
    write_line(handlers, ctx, "  cfg save           persist staged configuration");
    write_line(handlers, ctx, "  status             alias for router status");
    write_line(handlers, ctx, "  tables             alias for router status");
    write_line(handlers, ctx, "  help               show this help");
    write_line(handlers, ctx, "  quit | exit        stop router-linux");
}

tb_router_mgmt_result tb_router_mgmt_dispatch(const char *line,
                                              const tb_router_mgmt_handlers *handlers,
                                              void *ctx)
{
    line = trim_start(line);
    if (trimmed_len(line) == 0) {
        return TB_ROUTER_MGMT_CONTINUE;
    }

    if (cmd_eq(line, "quit") || cmd_eq(line, "exit")) {
        return TB_ROUTER_MGMT_STOP;
    }
    if (cmd_eq(line, "help") || cmd_eq(line, "?")) {
        tb_router_mgmt_print_help(handlers, ctx);
    } else if (cmd_eq(line, "router status") ||
               cmd_eq(line, "status") ||
               cmd_eq(line, "tables")) {
        if (handlers && handlers->show_status) {
            handlers->show_status(ctx);
        }
    } else if (cmd_eq(line, "router ports") || cmd_eq(line, "ports")) {
        if (handlers && handlers->show_ports) {
            handlers->show_ports(ctx);
        }
    } else if (cmd_eq(line, "router routes") || cmd_eq(line, "routes")) {
        if (handlers && handlers->show_routes) {
            handlers->show_routes(ctx);
        }
    } else if (cmd_eq(line, "transport status") || cmd_eq(line, "transports")) {
        if (handlers && handlers->show_transport_status) {
            handlers->show_transport_status(ctx);
        }
    } else if (cmd_eq(line, "interface status") || cmd_eq(line, "interfaces")) {
        if (handlers && handlers->show_interface_status) {
            handlers->show_interface_status(ctx);
        } else {
            write_line(handlers, ctx, "interface status unsupported");
        }
    } else if (cmd_eq(line, "interface halow status")) {
        if (handlers && handlers->show_halow_interface_status) {
            handlers->show_halow_interface_status(ctx);
        } else {
            write_line(handlers, ctx, "interface halow status unsupported");
        }
    } else if (cmd_eq(line, "interface halow apply")) {
        if (!handlers || !handlers->apply_halow_interface || !handlers->apply_halow_interface(ctx)) {
            write_line(handlers, ctx, "interface halow apply unsupported");
        }
    } else if (cmd_eq(line, "cfg show")) {
        if (handlers && handlers->cfg_show) {
            handlers->cfg_show(ctx);
        } else {
            write_line(handlers, ctx, "cfg show unsupported");
        }
    } else if (cmd_eq(line, "cfg save")) {
        if (!handlers || !handlers->cfg_save || !handlers->cfg_save(ctx)) {
            write_line(handlers, ctx, "cfg save unsupported");
        }
    } else {
        const char *rest = NULL;
        char key[96];
        char value[160];
        if (starts_with_token(line, "cfg get", &rest)) {
            size_t key_len = trimmed_len(rest);
            if (key_len == 0 || !copy_token(key, sizeof(key), rest, key_len)) {
                write_line(handlers, ctx, "usage: cfg get <key>");
            } else if (!handlers || !handlers->cfg_get || !handlers->cfg_get(ctx, key)) {
                write_line(handlers, ctx, "cfg get unsupported");
            }
        } else if (starts_with_token(line, "cfg set", &rest)) {
            size_t rest_len = trimmed_len(rest);
            const char *key_start = NULL;
            const char *value_start = NULL;
            size_t key_len = 0;
            size_t value_len = 0;
            if (!split_key_value(rest, rest_len, &key_start, &key_len, &value_start, &value_len) ||
                !copy_token(key, sizeof(key), key_start, key_len) ||
                !copy_token(value, sizeof(value), value_start, value_len)) {
                write_line(handlers, ctx, "usage: cfg set <key> <value>");
            } else if (!handlers || !handlers->cfg_set || !handlers->cfg_set(ctx, key, value)) {
                write_line(handlers, ctx, "cfg set unsupported");
            }
        } else {
            write_line(handlers, ctx, "unknown router CLI command");
            tb_router_mgmt_print_help(handlers, ctx);
        }
    }

    return TB_ROUTER_MGMT_CONTINUE;
}
