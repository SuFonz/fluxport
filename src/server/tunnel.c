#include "server_internal.h"

#include "log.h"
#include "tunnel_table.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TUNNEL_RESULT_MSG_CAP 160

static void set_result(fp_tunnel_result *result, uint8_t status,
                       const char *message)
{
    const char *text = message != NULL ? message : "";

    result->status = status;
    snprintf(result->msg, TUNNEL_RESULT_MSG_CAP, "%.*s",
             TUNNEL_RESULT_MSG_CAP - 1, text);
}

static int same_spec(const fp_tunnel_spec *a, const fp_tunnel_spec *b)
{
    return a->dir == b->dir &&
           a->listen_port == b->listen_port &&
           a->target_port == b->target_port &&
           strcmp(a->listen_addr, b->listen_addr) == 0 &&
           strcmp(a->target_host, b->target_host) == 0;
}

int fp_server_tunnel_add(server_client *c, const fp_tunnel_spec *spec,
                         uint16_t index, fp_tunnel_result *result)
{
    server_tunnel *t;
    server_tunnel *it;
    char err[192];

    if (result == NULL) {
        return -1;
    }
    result->index = index;
    result->msg[0] = '\0';
    if (c == NULL || spec == NULL) {
        set_result(result, FP_STATUS_INTERNAL, "internal error");
        return -1;
    }
    if (spec->dir != FP_DIR_L && spec->dir != FP_DIR_R) {
        set_result(result, FP_STATUS_BAD_TUNNEL, "invalid direction");
        return 0;
    }
    if (spec->listen_addr[0] == '\0' || spec->target_host[0] == '\0' ||
        spec->listen_port == 0 || spec->target_port == 0) {
        set_result(result, FP_STATUS_BAD_TUNNEL, "invalid tunnel parameters");
        return 0;
    }
    if (fp_endpoint_is_wildcard(spec->target_host)) {
        set_result(result, FP_STATUS_BAD_TUNNEL,
                   "target address must be concrete");
        return 0;
    }
    if (c->ntunnels >= FP_MAX_TUNNELS) {
        set_result(result, FP_STATUS_BAD_TUNNEL, "too many tunnels");
        return 0;
    }
    for (it = c->tunnels; it != NULL; it = it->next) {
        if (same_spec(&it->spec, spec)) {
            set_result(result, FP_STATUS_DUPLICATE,
                       "duplicate tunnel on this connection");
            return 0;
        }
    }
    if (spec->dir == FP_DIR_R &&
        !fp_server_config_port_allowed(&c->srv->cfg, spec->listen_port)) {
        set_result(result, FP_STATUS_PORT_DENIED,
                   "listen port is not allowed by server policy");
        return 0;
    }
    t = (server_tunnel *)calloc(1, sizeof(*t));
    if (t == NULL) {
        set_result(result, FP_STATUS_INTERNAL, "out of memory");
        return 0;
    }
    t->client = c;
    t->spec = *spec;
    t->index = index;
    t->listen_fd = FP_SOCKET_INVALID;
    if (spec->dir == FP_DIR_R) {
        if (fp_udp_bind(spec->listen_addr, spec->listen_port, 0,
                        &t->listen_fd, err, sizeof(err)) != 0) {
            set_result(result, FP_STATUS_BIND_FAILED, err);
            free(t);
            return 0;
        }
        if (fp_loop_add(c->srv->loop, t->listen_fd, FP_POLLIN,
                        fp_server_session_on_r_udp, t) != 0) {
            fp_socket_close(t->listen_fd);
            t->listen_fd = FP_SOCKET_INVALID;
            set_result(result, FP_STATUS_INTERNAL,
                       "cannot watch R listener");
            free(t);
            return 0;
        }
        t->watcher_added = 1;
    }
    t->next = c->tunnels;
    c->tunnels = t;
    c->ntunnels++;
    set_result(result, FP_STATUS_OK, "ok");
    if (spec->dir == FP_DIR_R) {
        FP_LOGI(c->tag, "R tunnel %s:%u -> %s:%u listening",
                spec->listen_addr, (unsigned)spec->listen_port,
                spec->target_host, (unsigned)spec->target_port);
    } else {
        FP_LOGI(c->tag, "L tunnel %s:%u -> %s:%u",
                spec->listen_addr, (unsigned)spec->listen_port,
                spec->target_host, (unsigned)spec->target_port);
    }
    return 0;
}

server_tunnel *fp_server_tunnel_find_l(server_client *c,
                                       const char *host, uint16_t port)
{
    server_tunnel *t;

    if (c == NULL || host == NULL) {
        return NULL;
    }
    for (t = c->tunnels; t != NULL; t = t->next) {
        if (t->spec.dir == FP_DIR_L &&
            t->spec.target_port == port &&
            strcmp(t->spec.target_host, host) == 0) {
            return t;
        }
    }
    return NULL;
}

void fp_server_tunnel_close(server_tunnel *t, int notify)
{
    server_client *c;
    server_tunnel **cursor;

    if (t == NULL) {
        return;
    }
    c = t->client;
    if (t->watcher_added) {
        fp_loop_del(c->srv->loop, t->listen_fd);
        t->watcher_added = 0;
    }
    if (t->listen_fd != FP_SOCKET_INVALID) {
        fp_socket_close(t->listen_fd);
        t->listen_fd = FP_SOCKET_INVALID;
    }
    fp_server_session_close_tunnel(t, notify);
    if (c != NULL) {
        cursor = &c->tunnels;
        while (*cursor != NULL) {
            if (*cursor == t) {
                *cursor = t->next;
                break;
            }
            cursor = &(*cursor)->next;
        }
        if (c->ntunnels > 0) {
            c->ntunnels--;
        }
        FP_LOGD(c->tag, "closed %s tunnel %s:%u -> %s:%u",
                fp_tunnel_dir_name(t->spec.dir),
                t->spec.listen_addr, (unsigned)t->spec.listen_port,
                t->spec.target_host, (unsigned)t->spec.target_port);
    }
    free(t);
}

void fp_server_tunnel_close_all(server_client *c, int notify)
{
    server_tunnel *t;

    if (c == NULL) {
        return;
    }
    t = c->tunnels;
    while (t != NULL) {
        server_tunnel *next = t->next;
        fp_server_tunnel_close(t, notify);
        t = next;
    }
    c->tunnels = NULL;
    c->ntunnels = 0;
}
