#include "client_internal.h"

#include "log.h"
#include "tunnel_table.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLIENT_DROP_LOG_MS 5000
#define CLIENT_CLOSE_GRACE_MS 1000

static void client_on_event(fp_loop *loop, fp_socket_t fd,
                            short revents, void *arg);
static void client_read(fp_client *cl);
static void client_handle_frame(fp_client *cl, const fp_frame *frame);
static void client_tick(fp_client *cl);
static void client_cleanup(fp_client *cl);

void fp_client_note_drop(fp_client *cl, const char *what)
{
    int64_t now;

    if (cl == NULL) {
        return;
    }
    cl->dropped++;
    cl->dropped_recent++;
    now = fp_now_ms();
    if (cl->dropped_recent > 0 &&
        now - cl->last_drop_log_ms >= CLIENT_DROP_LOG_MS) {
        FP_LOGW(cl->tag, "dropped %lu UDP packets (%s), total %lu",
                cl->dropped_recent,
                what != NULL ? what : "backpressure",
                (unsigned long)cl->dropped);
        cl->dropped_recent = 0;
        cl->last_drop_log_ms = now;
    }
}

void fp_client_set_dead(fp_client *cl, int exit_code, const char *reason)
{
    if (cl == NULL || cl->dead) {
        return;
    }
    cl->dead = 1;
    cl->state = CC_DEAD;
    if (exit_code > cl->exit_code) {
        cl->exit_code = exit_code;
    }
    FP_LOGI(cl->tag, "closed: %s",
            reason != NULL ? reason : "connection closed");
    if (cl->fd != FP_SOCKET_INVALID) {
        fp_loop_del(cl->loop, cl->fd);
        fp_socket_close(cl->fd);
        cl->fd = FP_SOCKET_INVALID;
    }
}

void fp_client_update_events(fp_client *cl)
{
    short events = 0;

    if (cl == NULL || cl->dead || cl->fd == FP_SOCKET_INVALID) {
        return;
    }
    if (cl->state != CC_CLOSING) {
        events |= FP_POLLIN;
    }
    if (!fp_sendq_empty(&cl->sendq)) {
        events |= FP_POLLOUT;
    }
    if (events == 0) {
        fp_client_set_dead(cl, cl->exit_code, "closing complete");
        return;
    }
    if (fp_loop_mod(cl->loop, cl->fd, events) != 0) {
        fp_client_set_dead(cl, 2, "poll update failed");
    }
}

int fp_client_send(fp_client *cl, uint8_t type, uint32_t stream_id,
                   const void *payload, size_t len, int critical)
{
    int rc;

    if (cl == NULL || cl->dead || cl->fd == FP_SOCKET_INVALID) {
        return -1;
    }
    rc = fp_sendq_frame(&cl->sendq, type, stream_id, payload, len);
    if (rc == FP_SENDQ_OK) {
        fp_client_update_events(cl);
        return cl->dead ? -1 : 0;
    }
    if (rc == FP_SENDQ_FULL || rc == FP_SENDQ_ENOMEM) {
        if (critical) {
            FP_LOGE(cl->tag, "send queue full while sending %s",
                    fp_frame_type_name(type));
            fp_client_set_dead(cl, 2, "send queue limit reached");
            return -1;
        }
        fp_client_note_drop(cl, "send queue full");
        return 1;
    }
    fp_client_set_dead(cl, 2, "send queue failure");
    return -1;
}

void fp_client_flush(fp_client *cl)
{
    if (cl == NULL || cl->dead) {
        return;
    }
    while (!fp_sendq_empty(&cl->sendq)) {
        const uint8_t *data = fp_sendq_data(&cl->sendq);
        size_t pending = fp_sendq_pending(&cl->sendq);
        int rc = fp_tcp_send(cl->fd, data, pending);

        if (rc > 0) {
            fp_sendq_consume(&cl->sendq, (size_t)rc);
            continue;
        }
        if (rc == FP_IO_AGAIN) {
            break;
        }
        fp_client_set_dead(cl, 2, "control connection write failed");
        return;
    }
    if (cl->state == CC_CLOSING && fp_sendq_empty(&cl->sendq)) {
        fp_client_set_dead(cl, cl->exit_code, "closing complete");
        return;
    }
    fp_client_update_events(cl);
}

void fp_client_set_closing(fp_client *cl, int64_t grace_ms)
{
    if (cl == NULL || cl->dead) {
        return;
    }
    cl->state = CC_CLOSING;
    cl->close_deadline_ms = fp_now_ms() + grace_ms;
    if (fp_sendq_empty(&cl->sendq)) {
        fp_client_set_dead(cl, cl->exit_code, "closing complete");
        return;
    }
    fp_client_update_events(cl);
}

static void client_protocol_error(fp_client *cl, const char *fmt, ...)
{
    char msg[192];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    FP_LOGW(cl->tag, "protocol error: %s", msg);
    fp_client_set_dead(cl, 2, "protocol error");
}

static int client_send_tunnel_add(fp_client *cl)
{
    fp_tunnel_spec *specs;
    fp_buf buf;
    size_t i;
    int rc;

    specs = (fp_tunnel_spec *)calloc(cl->ntunnels > 0 ? cl->ntunnels : 1,
                                     sizeof(*specs));
    if (specs == NULL) {
        fp_client_set_dead(cl, 2, "out of memory");
        return -1;
    }
    for (i = 0; i < cl->ntunnels; i++) {
        specs[i] = cl->tunnels[i].spec;
    }
    fp_buf_init(&buf);
    rc = fp_build_tunnel_add(&buf, specs, cl->ntunnels);
    free(specs);
    if (rc != 0) {
        fp_buf_free(&buf);
        fp_client_set_dead(cl, 2, "cannot encode TUNNEL_ADD");
        return -1;
    }
    rc = fp_client_send(cl, FP_FRAME_TUNNEL_ADD, 0,
                        buf.data, buf.len, 1);
    fp_buf_free(&buf);
    if (rc != 0) {
        return -1;
    }
    cl->state = CC_RESULT_WAIT;
    FP_LOGD(cl->tag, "sent %lu tunnel%s to server",
            (unsigned long)cl->ntunnels, cl->ntunnels == 1 ? "" : "s");
    return 0;
}

static void client_handle_hello_ack(fp_client *cl, const fp_frame *frame)
{
    fp_hello_ack ack;
    char err[192];

    if (cl->state != CC_ACK_WAIT) {
        client_protocol_error(cl, "unexpected HELLO_ACK in state %d",
                              cl->state);
        return;
    }
    if (fp_decode_hello_ack(frame->payload, frame->len,
                            &ack, err, sizeof(err)) != 0) {
        client_protocol_error(cl, "bad HELLO_ACK: %s", err);
        return;
    }
    if (ack.version != FP_PROTO_VERSION) {
        client_protocol_error(cl, "server protocol version %u, expected %u",
                              (unsigned)ack.version,
                              (unsigned)FP_PROTO_VERSION);
        return;
    }
    if (ack.status != FP_STATUS_OK) {
        FP_LOGE(cl->tag, "server rejected authentication: %s (%s)",
                fp_status_text(ack.status),
                ack.msg[0] != '\0' ? ack.msg : "no details");
        fp_client_set_dead(cl, 2, "authentication failed");
        return;
    }
    FP_LOGI(cl->tag, "authenticated with server%s%s",
            ack.msg[0] != '\0' ? ": " : "",
            ack.msg[0] != '\0' ? ack.msg : "");
    (void)client_send_tunnel_add(cl);
}

static void client_handle_tunnel_result(fp_client *cl,
                                        const fp_frame *frame)
{
    fp_tunnel_result *results;
    unsigned char *seen;
    size_t count = 0;
    size_t i;
    char err[192];

    if (cl->state != CC_RESULT_WAIT) {
        client_protocol_error(cl, "unexpected TUNNEL_RESULT in state %d",
                              cl->state);
        return;
    }
    results = (fp_tunnel_result *)calloc(cl->ntunnels > 0 ? cl->ntunnels : 1,
                                         sizeof(*results));
    seen = (unsigned char *)calloc(cl->ntunnels > 0 ? cl->ntunnels : 1, 1);
    if (results == NULL || seen == NULL) {
        free(results);
        free(seen);
        fp_client_set_dead(cl, 2, "out of memory");
        return;
    }
    if (fp_decode_tunnel_result(frame->payload, frame->len, results,
                                cl->ntunnels, &count,
                                err, sizeof(err)) != 0) {
        free(results);
        free(seen);
        client_protocol_error(cl, "bad TUNNEL_RESULT: %s", err);
        return;
    }
    if (count != cl->ntunnels) {
        free(results);
        free(seen);
        client_protocol_error(cl, "server returned %lu results for %lu "
                              "tunnels", (unsigned long)count,
                              (unsigned long)cl->ntunnels);
        return;
    }
    for (i = 0; i < count; i++) {
        size_t index = results[i].index;

        if (index >= cl->ntunnels || seen[index]) {
            free(results);
            free(seen);
            client_protocol_error(cl, "invalid result index %lu",
                                  (unsigned long)index);
            return;
        }
        seen[index] = 1;
        cl->tunnels[index].status = results[i].status;
        snprintf(cl->tunnels[index].msg, sizeof(cl->tunnels[index].msg),
                 "%s", results[i].msg);
        if (results[i].status == FP_STATUS_OK) {
            cl->tunnels[index].ok = 1;
        } else {
            cl->tunnels[index].ok = 0;
            cl->tunnels_failed++;
            FP_LOGW(cl->tag, "tunnel %lu (%s %s:%u -> %s:%u) failed: %s "
                    "(%s)", (unsigned long)index + 1,
                    fp_tunnel_dir_name(cl->tunnels[index].spec.dir),
                    cl->tunnels[index].spec.listen_addr,
                    (unsigned)cl->tunnels[index].spec.listen_port,
                    cl->tunnels[index].spec.target_host,
                    (unsigned)cl->tunnels[index].spec.target_port,
                    fp_status_text(results[i].status),
                    results[i].msg[0] != '\0' ? results[i].msg : "-");
        }
    }
    free(results);
    free(seen);
    fp_client_tunnels_bind_l(cl);
    cl->state = CC_RUNNING;
    cl->handshake_done = 1;
    cl->last_ping_ms = fp_now_ms();
    cl->last_tick_ms = fp_now_ms();
    FP_LOGI(cl->tag, "tunnel setup complete: %lu configured, %lu failed",
            (unsigned long)cl->ntunnels,
            (unsigned long)cl->tunnels_failed);
    if (cl->cfg.strict && cl->tunnels_failed > 0) {
        fp_buf goaway;
        int rc;
        fp_buf_init(&goaway);
        cl->fatal_exit = 3;
        if (fp_build_goaway(&goaway, "strict tunnel setup failed") == 0) {
            rc = fp_client_send(cl, FP_FRAME_GOAWAY, 0,
                                goaway.data, goaway.len, 1);
            fp_buf_free(&goaway);
            if (rc == 0) {
                fp_client_flush(cl);
            }
        } else {
            fp_buf_free(&goaway);
        }
        fp_client_set_dead(cl, 3, "strict mode: some tunnels failed");
        return;
    }
    if (cl->tunnels_failed > 0) {
        FP_LOGW(cl->tag, "continuing with %lu working tunnel%s",
                (unsigned long)(cl->ntunnels - cl->tunnels_failed),
                (cl->ntunnels - cl->tunnels_failed) == 1 ? "" : "s");
    }
}

static void client_handle_udp_open(fp_client *cl, const fp_frame *frame)
{
    char target[FP_MAX_ADDR_TEXT];
    char err[192];
    client_session *s;
    uint32_t sid = frame->stream_id;

    if (sid == 0 || (sid & FP_STREAM_R_BIT) == 0) {
        client_protocol_error(cl, "UDP_OPEN with invalid stream id 0x%08x",
                              (unsigned)sid);
        return;
    }
    if (fp_decode_open(frame->payload, frame->len, target, sizeof(target),
                       err, sizeof(err)) != 0) {
        client_protocol_error(cl, "bad UDP_OPEN: %s", err);
        return;
    }
    if (fp_client_session_open_r(cl, sid, target, &s,
                                 err, sizeof(err)) != 0) {
        fp_buf closebuf;
        FP_LOGD(cl->tag, "cannot open R stream %u (%s): %s",
                (unsigned)sid, target, err);
        fp_buf_init(&closebuf);
        if (fp_build_close(&closebuf, FP_CLOSE_ERROR) == 0) {
            (void)fp_client_send(cl, FP_FRAME_UDP_CLOSE, sid,
                                 closebuf.data, closebuf.len, 0);
        }
        fp_buf_free(&closebuf);
    }
}

static void client_handle_udp_close(fp_client *cl, const fp_frame *frame)
{
    int reason = FP_CLOSE_NORMAL;
    char err[128];

    if (frame->stream_id == 0) {
        client_protocol_error(cl, "UDP_CLOSE with zero stream id");
        return;
    }
    if (fp_decode_close(frame->payload, frame->len,
                        &reason, err, sizeof(err)) != 0) {
        client_protocol_error(cl, "bad UDP_CLOSE: %s", err);
        return;
    }
    fp_client_session_peer_close(cl, frame->stream_id, reason);
}

static void client_handle_goaway(fp_client *cl, const fp_frame *frame)
{
    char reason[FP_MAX_MSG + 1];
    char err[128];
    int code = cl->tunnels_failed > 0 ? 3 : 0;

    if (fp_decode_goaway(frame->payload, frame->len,
                         reason, sizeof(reason), err, sizeof(err)) == 0 &&
        reason[0] != '\0') {
        FP_LOGI(cl->tag, "server says goodbye: %s", reason);
    } else {
        FP_LOGI(cl->tag, "server sent GOAWAY");
    }
    fp_client_set_dead(cl, code, "server sent GOAWAY");
}

static void client_handle_frame(fp_client *cl, const fp_frame *frame)
{
    if (cl->dead) {
        return;
    }
    if (frame->type == FP_FRAME_PING) {
        (void)fp_client_send(cl, FP_FRAME_PONG, 0, NULL, 0, 1);
        return;
    }
    if (frame->type == FP_FRAME_PONG) {
        return;
    }
    if (frame->type == FP_FRAME_GOAWAY) {
        client_handle_goaway(cl, frame);
        return;
    }
    switch (cl->state) {
    case CC_ACK_WAIT:
        if (frame->type != FP_FRAME_HELLO_ACK) {
            client_protocol_error(cl, "expected HELLO_ACK, got %s",
                                  fp_frame_type_name(frame->type));
            return;
        }
        client_handle_hello_ack(cl, frame);
        return;
    case CC_RESULT_WAIT:
        if (frame->type != FP_FRAME_TUNNEL_RESULT) {
            client_protocol_error(cl, "expected TUNNEL_RESULT, got %s",
                                  fp_frame_type_name(frame->type));
            return;
        }
        client_handle_tunnel_result(cl, frame);
        return;
    case CC_RUNNING:
        break;
    default:
        return;
    }
    switch (frame->type) {
    case FP_FRAME_UDP_OPEN:
        client_handle_udp_open(cl, frame);
        return;
    case FP_FRAME_UDP_DATA:
        if (frame->stream_id == 0) {
            client_protocol_error(cl, "UDP_DATA with zero stream id");
            return;
        }
        fp_client_session_data(cl, frame->stream_id,
                               frame->payload, frame->len);
        return;
    case FP_FRAME_UDP_CLOSE:
        client_handle_udp_close(cl, frame);
        return;
    case FP_FRAME_HELLO:
    case FP_FRAME_HELLO_ACK:
    case FP_FRAME_TUNNEL_ADD:
    case FP_FRAME_TUNNEL_RESULT:
        client_protocol_error(cl, "unexpected %s in running state",
                              fp_frame_type_name(frame->type));
        return;
    default:
        client_protocol_error(cl, "unknown frame type 0x%02x",
                              (unsigned)frame->type);
        return;
    }
}

static void client_read(fp_client *cl)
{
    int iterations;

    if (cl->dead) {
        return;
    }
    for (iterations = 0; iterations < 16; iterations++) {
        int n = fp_tcp_recv(cl->fd, cl->rbuf, sizeof(cl->rbuf));
        int parsed;

        if (n > 0) {
            cl->last_recv_ms = fp_now_ms();
            if (fp_parser_push(&cl->parser, cl->rbuf, (size_t)n) != 0) {
                client_protocol_error(cl, "frame buffer limit exceeded");
                return;
            }
            for (;;) {
                fp_frame frame;
                parsed = fp_parser_next(&cl->parser, &frame);
                if (parsed == 0) {
                    break;
                }
                if (parsed < 0) {
                    client_protocol_error(cl, "parser: %s",
                                          fp_parser_error(&cl->parser));
                    return;
                }
                client_handle_frame(cl, &frame);
                if (cl->dead) {
                    return;
                }
            }
            if ((size_t)n < sizeof(cl->rbuf)) {
                return;
            }
            continue;
        }
        if (n == 0) {
            if (cl->stopping || cl->state == CC_CLOSING) {
                fp_client_set_dead(cl, cl->exit_code,
                                   "server closed the connection");
            } else {
                fp_client_set_dead(cl, 2,
                                   "server closed the connection");
            }
            return;
        }
        if (n == FP_IO_AGAIN) {
            return;
        }
        fp_client_set_dead(cl, 2, "control connection read failed");
        return;
    }
}

static void client_on_event(fp_loop *loop, fp_socket_t fd,
                            short revents, void *arg)
{
    fp_client *cl = (fp_client *)arg;

    (void)loop;
    (void)fd;
    if (cl == NULL || cl->dead) {
        return;
    }
    if ((revents & FP_POLLNVAL) != 0) {
        fp_client_set_dead(cl, 2, "invalid descriptor");
        return;
    }
    if ((revents & FP_POLLOUT) != 0) {
        fp_client_flush(cl);
    }
    if (cl->dead) {
        return;
    }
    if ((revents & (FP_POLLIN | FP_POLLHUP | FP_POLLERR)) != 0) {
        client_read(cl);
    }
}

static void client_tick(fp_client *cl)
{
    int64_t now;

    if (cl == NULL || cl->dead) {
        return;
    }
    now = fp_now_ms();
    if (cl->state == CC_RUNNING && cl->cfg.keepalive_s > 0) {
        int64_t keepalive_ms = (int64_t)cl->cfg.keepalive_s * 1000;
        if (now - cl->last_recv_ms > keepalive_ms * 2) {
            fp_client_set_dead(cl, 2, "heartbeat timeout");
            return;
        }
        if (now - cl->last_ping_ms >= keepalive_ms) {
            cl->last_ping_ms = now;
            (void)fp_client_send(cl, FP_FRAME_PING, 0, NULL, 0, 1);
        }
    }
    if (cl->state == CC_CLOSING &&
        (fp_sendq_empty(&cl->sendq) || now >= cl->close_deadline_ms)) {
        fp_client_set_dead(cl, cl->exit_code, "closing complete");
        return;
    }
    fp_client_session_tick(cl, now);
}

static int client_connect(fp_client *cl, char *err, size_t errcap)
{
    fp_socket_t fd = FP_SOCKET_INVALID;
    fp_endpoint peer;
    fp_buf hello;
    char cerr[192];
    int rc;

    if (fp_tcp_connect(cl->cfg.server_host, cl->cfg.server_port,
                       cl->cfg.connect_timeout_s * 1000,
                       &fd, &peer, cerr, sizeof(cerr)) != 0) {
        snprintf(err, errcap, "cannot connect to %s:%u: %s",
                 cl->cfg.server_host, (unsigned)cl->cfg.server_port, cerr);
        return -1;
    }
    (void)fp_net_set_nodelay(fd);
    if (fp_loop_add(cl->loop, fd, FP_POLLIN, client_on_event, cl) != 0) {
        fp_socket_close(fd);
        snprintf(err, errcap, "cannot watch control connection");
        return -1;
    }
    cl->fd = fd;
    cl->state = CC_ACK_WAIT;
    cl->connected_ms = fp_now_ms();
    cl->last_recv_ms = cl->connected_ms;
    cl->last_ping_ms = cl->connected_ms;
    cl->last_tick_ms = cl->connected_ms;
    cl->last_drop_log_ms = cl->connected_ms;
    fp_buf_init(&hello);
    if (fp_build_hello(&hello, cl->cfg.token, cl->cfg.name) != 0) {
        fp_buf_free(&hello);
        fp_client_set_dead(cl, 2, "cannot encode HELLO");
        snprintf(err, errcap, "cannot encode HELLO");
        return -1;
    }
    rc = fp_client_send(cl, FP_FRAME_HELLO, 0, hello.data, hello.len, 1);
    fp_buf_free(&hello);
    if (rc != 0) {
        snprintf(err, errcap, "cannot send HELLO");
        return -1;
    }
    FP_LOGI(cl->tag, "connected to %s:%u", cl->cfg.server_host,
            (unsigned)cl->cfg.server_port);
    (void)peer;
    return 0;
}

static int client_handshake(fp_client *cl)
{
    int64_t deadline = fp_now_ms() + FP_HANDSHAKE_TIMEOUT_MS;

    while (!cl->handshake_done && !cl->dead) {
        int64_t now = fp_now_ms();
        int rc;

        if (cl->stopping || fp_stop_requested()) {
            cl->stopping = 1;
            cl->handshake_done = 1;
            return 0;
        }
        if (now >= deadline) {
            fp_client_set_dead(cl, 2, "handshake timeout");
            break;
        }
        rc = fp_loop_run_once(cl->loop, 50);
        if (rc < 0) {
            fp_client_set_dead(cl, 2, "event loop failed");
            break;
        }
    }
    if (cl->handshake_done) {
        return cl->fatal_exit != 0 ? cl->fatal_exit : 0;
    }
    return cl->exit_code != 0 ? cl->exit_code : 2;
}

static void client_cleanup(fp_client *cl)
{
    if (cl == NULL || cl->shutdown_done) {
        return;
    }
    fp_client_session_close_all(cl, 0);
    fp_client_tunnels_close_all(cl);
    if (cl->fd != FP_SOCKET_INVALID) {
        fp_loop_del(cl->loop, cl->fd);
        fp_socket_close(cl->fd);
        cl->fd = FP_SOCKET_INVALID;
    }
    cl->shutdown_done = 1;
}

fp_client *fp_client_new(const fp_client_config *cfg,
                         const fp_tunnel_spec *tunnels, size_t ntunnels,
                         char *err, size_t errcap)
{
    fp_client *cl;
    size_t i;
    size_t j;
    char verr[192];

    if (cfg == NULL) {
        return NULL;
    }
    if (ntunnels > FP_MAX_TUNNELS) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "too many tunnels");
        }
        return NULL;
    }
    if (fp_client_config_validate(cfg, 0, verr, sizeof(verr)) != 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "%s", verr);
        }
        return NULL;
    }
    for (i = 0; i < ntunnels; i++) {
        const fp_tunnel_spec *s = &tunnels[i];
        if (s->dir != FP_DIR_L && s->dir != FP_DIR_R) {
            if (err != NULL && errcap > 0) {
                snprintf(err, errcap, "tunnel %lu has invalid direction",
                         (unsigned long)i + 1);
            }
            return NULL;
        }
        if (s->listen_addr[0] == '\0' || s->target_host[0] == '\0' ||
            s->listen_port == 0 || s->target_port == 0 ||
            fp_endpoint_is_wildcard(s->target_host)) {
            if (err != NULL && errcap > 0) {
                snprintf(err, errcap, "tunnel %lu is invalid",
                         (unsigned long)i + 1);
            }
            return NULL;
        }
        for (j = 0; j < i; j++) {
            if (tunnels[j].dir == s->dir &&
                tunnels[j].listen_port == s->listen_port &&
                tunnels[j].target_port == s->target_port &&
                strcmp(tunnels[j].listen_addr, s->listen_addr) == 0 &&
                strcmp(tunnels[j].target_host, s->target_host) == 0) {
                if (err != NULL && errcap > 0) {
                    snprintf(err, errcap, "duplicate tunnel %lu",
                             (unsigned long)i + 1);
                }
                return NULL;
            }
        }
    }
    cl = (fp_client *)calloc(1, sizeof(*cl));
    if (cl == NULL) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "out of memory");
        }
        return NULL;
    }
    cl->cfg = *cfg;
    cl->fd = FP_SOCKET_INVALID;
    cl->state = CC_INIT;
    cl->next_l_stream = 1;
    snprintf(cl->tag, sizeof(cl->tag), "client");
    cl->loop = fp_loop_new();
    if (cl->loop == NULL) {
        free(cl);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "cannot create event loop");
        }
        return NULL;
    }
    fp_parser_init(&cl->parser);
    if (fp_sendq_init(&cl->sendq, FP_SENDQ_LIMIT) != 0) {
        fp_loop_free(cl->loop);
        fp_parser_free(&cl->parser);
        free(cl);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "cannot allocate send queue");
        }
        return NULL;
    }
    cl->tunnels = (client_tunnel *)calloc(ntunnels > 0 ? ntunnels : 1,
                                          sizeof(*cl->tunnels));
    if (cl->tunnels == NULL) {
        fp_sendq_free(&cl->sendq);
        fp_parser_free(&cl->parser);
        fp_loop_free(cl->loop);
        free(cl);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "out of memory");
        }
        return NULL;
    }
    cl->ntunnels = ntunnels;
    for (i = 0; i < ntunnels; i++) {
        cl->tunnels[i].client = cl;
        cl->tunnels[i].spec = tunnels[i];
        cl->tunnels[i].status = FP_STATUS_INTERNAL;
        cl->tunnels[i].ok = 0;
        snprintf(cl->tunnels[i].msg, sizeof(cl->tunnels[i].msg), "pending");
        cl->tunnels[i].listen_fd = FP_SOCKET_INVALID;
    }
    return cl;
}

int fp_client_start(fp_client *cl, char *err, size_t errcap)
{
    int rc;

    if (cl == NULL) {
        return 2;
    }
    if (err == NULL) {
        err = (char *)"";
        errcap = 0;
    }
    if (cl->dead) {
        return cl->exit_code != 0 ? cl->exit_code : 2;
    }
    if (client_connect(cl, err, errcap) != 0) {
        client_cleanup(cl);
        return 2;
    }
    rc = client_handshake(cl);
    if (rc != 0) {
        client_cleanup(cl);
        return rc;
    }
    if (cl->stopping || cl->dead) {
        client_cleanup(cl);
        return cl->exit_code;
    }
    return 0;
}

int fp_client_run(fp_client *cl)
{
    if (cl == NULL) {
        return 1;
    }
    if (cl->dead) {
        client_cleanup(cl);
        return cl->exit_code;
    }
    if (!cl->handshake_done || cl->state != CC_RUNNING) {
        client_cleanup(cl);
        return cl->exit_code != 0 ? cl->exit_code : 1;
    }
    while (!cl->stopping && !fp_stop_requested() && !cl->dead) {
        int rc = fp_loop_run_once(cl->loop, 200);
        int64_t now;

        if (rc < 0) {
            fp_client_set_dead(cl, 2, "event loop failed");
            break;
        }
        now = fp_now_ms();
        if (now - cl->last_tick_ms >= 200) {
            client_tick(cl);
            cl->last_tick_ms = now;
        }
    }
    if (!cl->dead) {
        fp_client_stop(cl);
        client_cleanup(cl);
    } else {
        client_cleanup(cl);
    }
    if (cl->exit_code == 0 && cl->tunnels_failed > 0) {
        cl->exit_code = 3;
    }
    return cl->exit_code;
}

void fp_client_stop(fp_client *cl)
{
    if (cl != NULL) {
        cl->stopping = 1;
    }
}

void fp_client_free(fp_client *cl)
{
    if (cl == NULL) {
        return;
    }
    client_cleanup(cl);
    fp_sendq_free(&cl->sendq);
    fp_parser_free(&cl->parser);
    fp_loop_free(cl->loop);
    free(cl->tunnels);
    free(cl);
}
