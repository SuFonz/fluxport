#include "server_internal.h"

#include "log.h"
#include "protocol.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <errno.h>
#endif

#define SERVER_RESULT_MSG_CAP 160
#define SERVER_DROP_LOG_MS 5000
#define SERVER_CLOSE_GRACE_MS 3000
#define SERVER_SHUTDOWN_GRACE_MS 1000

static void server_accept_cb(fp_loop *loop, fp_socket_t fd,
                             short revents, void *arg);
static void server_client_event_cb(fp_loop *loop, fp_socket_t fd,
                                   short revents, void *arg);
static void server_client_read(server_client *c);
static void server_client_handle_frame(server_client *c,
                                       const fp_frame *frame);
static void server_tick(fp_server *srv);
static void server_sweep_dead(fp_server *srv);
static void server_client_destroy(server_client *c);

static int net_would_block(void)
{
#ifdef _WIN32
    int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAEINTR ||
           err == WSAEINPROGRESS;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

void fp_server_note_drop(server_client *c, const char *what)
{
    int64_t now;

    if (c == NULL) {
        return;
    }
    c->dropped++;
    c->dropped_recent++;
    now = fp_now_ms();
    if (c->dropped_recent > 0 &&
        now - c->last_drop_log_ms >= SERVER_DROP_LOG_MS) {
        FP_LOGW(c->tag, "dropped %lu datagrams (%s), total %lu",
                (unsigned long)c->dropped_recent,
                what != NULL ? what : "backpressure",
                (unsigned long)c->dropped);
        c->dropped_recent = 0;
        c->last_drop_log_ms = now;
    }
}

void fp_server_client_set_dead(server_client *c, const char *reason)
{
    if (c == NULL || c->dead) {
        return;
    }
    c->dead = 1;
    c->state = SC_DEAD;
    FP_LOGI(c->tag, "closed: %s",
            reason != NULL ? reason : "connection closed");
    if (c->fd != FP_SOCKET_INVALID) {
        fp_loop_del(c->srv->loop, c->fd);
        fp_socket_close(c->fd);
        c->fd = FP_SOCKET_INVALID;
    }
}

void fp_server_client_update_events(server_client *c)
{
    short events = 0;

    if (c == NULL || c->dead || c->fd == FP_SOCKET_INVALID) {
        return;
    }
    if (c->state != SC_CLOSING) {
        events |= FP_POLLIN;
    }
    if (!fp_sendq_empty(&c->sendq)) {
        events |= FP_POLLOUT;
    }
    if (events == 0) {
        fp_server_client_set_dead(c, "closing complete");
        return;
    }
    if (fp_loop_mod(c->srv->loop, c->fd, events) != 0) {
        fp_server_client_set_dead(c, "poll update failed");
    }
}

int fp_server_client_send(server_client *c, uint8_t type, uint32_t stream_id,
                          const void *payload, size_t len, int critical)
{
    int rc;

    if (c == NULL || c->dead || c->fd == FP_SOCKET_INVALID) {
        return -1;
    }
    rc = fp_sendq_frame(&c->sendq, type, stream_id, payload, len);
    if (rc == FP_SENDQ_OK) {
        fp_server_client_update_events(c);
        return c->dead ? -1 : 0;
    }
    if (rc == FP_SENDQ_FULL || rc == FP_SENDQ_ENOMEM) {
        if (critical) {
            FP_LOGE(c->tag, "send queue full while sending %s",
                    fp_frame_type_name(type));
            fp_server_client_set_dead(c, "send queue limit reached");
            return -1;
        }
        fp_server_note_drop(c, "send queue full");
        return 1;
    }
    fp_server_client_set_dead(c, "send queue failure");
    return -1;
}

void fp_server_client_flush(server_client *c)
{
    if (c == NULL || c->dead) {
        return;
    }
    while (!fp_sendq_empty(&c->sendq)) {
        const uint8_t *data = fp_sendq_data(&c->sendq);
        size_t pending = fp_sendq_pending(&c->sendq);
        int rc = fp_tcp_send(c->fd, data, pending);

        if (rc > 0) {
            fp_sendq_consume(&c->sendq, (size_t)rc);
            continue;
        }
        if (rc == FP_IO_AGAIN) {
            break;
        }
        fp_server_client_set_dead(c, "control connection write failed");
        return;
    }
    if (c->state == SC_CLOSING && fp_sendq_empty(&c->sendq)) {
        fp_server_client_set_dead(c, "closing complete");
        return;
    }
    fp_server_client_update_events(c);
}

void fp_server_client_set_closing(server_client *c, int64_t grace_ms)
{
    if (c == NULL || c->dead) {
        return;
    }
    c->state = SC_CLOSING;
    c->close_deadline_ms = fp_now_ms() + grace_ms;
    if (fp_sendq_empty(&c->sendq)) {
        fp_server_client_set_dead(c, "closing complete");
        return;
    }
    fp_server_client_update_events(c);
}

static void client_protocol_error(server_client *c, const char *fmt, ...)
{
    char msg[192];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    FP_LOGW(c->tag, "protocol error: %s", msg);
    fp_server_client_set_dead(c, "protocol error");
}

static void client_reject_hello(server_client *c, uint8_t status,
                                const char *msg)
{
    fp_buf buf;
    int rc;

    fp_buf_init(&buf);
    if (fp_build_hello_ack(&buf, status, msg) != 0) {
        fp_buf_free(&buf);
        fp_server_client_set_dead(c, "cannot encode HELLO_ACK");
        return;
    }
    rc = fp_server_client_send(c, FP_FRAME_HELLO_ACK, 0,
                               buf.data, buf.len, 1);
    fp_buf_free(&buf);
    if (rc == 0) {
        fp_server_client_set_closing(c, SERVER_CLOSE_GRACE_MS);
    }
}

static void result_msg(fp_tunnel_result *result, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(result->msg, SERVER_RESULT_MSG_CAP, fmt, ap);
    va_end(ap);
}

static void handle_tunnel_add(server_client *c, const fp_frame *frame)
{
    fp_tunnel_spec *specs;
    fp_tunnel_result *results;
    fp_buf out;
    size_t count = 0;
    size_t i;
    size_t failed = 0;
    char err[192];

    if (frame->stream_id != 0) {
        client_protocol_error(c, "TUNNEL_ADD with stream id %u",
                              (unsigned)frame->stream_id);
        return;
    }
    specs = (fp_tunnel_spec *)calloc(FP_MAX_TUNNELS, sizeof(*specs));
    if (specs == NULL) {
        fp_server_client_set_dead(c, "out of memory");
        return;
    }
    if (fp_decode_tunnel_add(frame->payload, frame->len, specs,
                             FP_MAX_TUNNELS, &count,
                             err, sizeof(err)) != 0) {
        FP_LOGW(c->tag, "bad TUNNEL_ADD: %s", err);
        free(specs);
        fp_server_client_set_dead(c, "malformed TUNNEL_ADD");
        return;
    }
    results = (fp_tunnel_result *)calloc(count > 0 ? count : 1,
                                         sizeof(*results));
    if (results == NULL) {
        free(specs);
        fp_server_client_set_dead(c, "out of memory");
        return;
    }
    for (i = 0; i < count; i++) {
        results[i].index = (uint16_t)i;
        results[i].status = FP_STATUS_INTERNAL;
        result_msg(&results[i], "not processed");
        if (fp_server_tunnel_add(c, &specs[i], (uint16_t)i,
                                 &results[i]) != 0) {
            results[i].status = FP_STATUS_INTERNAL;
            result_msg(&results[i], "internal error");
        }
        if (results[i].status != FP_STATUS_OK) {
            failed++;
        }
    }
    fp_buf_init(&out);
    if (fp_build_tunnel_result(&out, results, count) != 0) {
        fp_buf_free(&out);
        free(results);
        free(specs);
        fp_server_client_set_dead(c, "cannot encode TUNNEL_RESULT");
        return;
    }
    if (fp_server_client_send(c, FP_FRAME_TUNNEL_RESULT, 0,
                              out.data, out.len, 1) != 0) {
        fp_buf_free(&out);
        free(results);
        free(specs);
        return;
    }
    fp_buf_free(&out);
    c->state = SC_READY;
    c->last_ping_ms = fp_now_ms();
    FP_LOGI(c->tag, "configured %lu tunnel%s (%lu failed)",
            (unsigned long)count, count == 1 ? "" : "s",
            (unsigned long)failed);
    free(results);
    free(specs);
}

static void handle_udp_open(server_client *c, const fp_frame *frame)
{
    char target[FP_MAX_ADDR_TEXT];
    char err[192];
    uint32_t sid = frame->stream_id;

    if (sid == 0 || (sid & FP_STREAM_R_BIT) != 0) {
        client_protocol_error(c, "UDP_OPEN with invalid stream id 0x%08x",
                              (unsigned)sid);
        return;
    }
    if (fp_decode_open(frame->payload, frame->len, target, sizeof(target),
                       err, sizeof(err)) != 0) {
        FP_LOGW(c->tag, "bad UDP_OPEN: %s", err);
        fp_server_client_set_dead(c, "malformed UDP_OPEN");
        return;
    }
    if (fp_server_session_open_l(c, sid, target, err, sizeof(err)) != 0) {
        fp_buf closebuf;
        FP_LOGD(c->tag, "cannot open L stream %u (%s): %s",
                (unsigned)sid, target, err);
        fp_buf_init(&closebuf);
        if (fp_build_close(&closebuf, FP_CLOSE_ERROR) == 0) {
            (void)fp_server_client_send(c, FP_FRAME_UDP_CLOSE, sid,
                                        closebuf.data, closebuf.len, 0);
        }
        fp_buf_free(&closebuf);
    }
}

static void handle_udp_close(server_client *c, const fp_frame *frame)
{
    int reason = FP_CLOSE_NORMAL;
    char err[128];

    if (frame->stream_id == 0) {
        client_protocol_error(c, "UDP_CLOSE with zero stream id");
        return;
    }
    if (fp_decode_close(frame->payload, frame->len,
                        &reason, err, sizeof(err)) != 0) {
        FP_LOGW(c->tag, "bad UDP_CLOSE: %s", err);
        fp_server_client_set_dead(c, "malformed UDP_CLOSE");
        return;
    }
    fp_server_session_client_close(c, frame->stream_id, reason);
}

static void handle_goaway(server_client *c, const fp_frame *frame)
{
    char reason[FP_MAX_MSG + 1];
    char err[128];

    if (fp_decode_goaway(frame->payload, frame->len,
                         reason, sizeof(reason), err, sizeof(err)) == 0 &&
        reason[0] != '\0') {
        FP_LOGI(c->tag, "peer says goodbye: %s", reason);
    } else {
        FP_LOGI(c->tag, "peer sent GOAWAY");
    }
    fp_server_client_set_dead(c, "peer sent GOAWAY");
}

static void handle_ready_frame(server_client *c, const fp_frame *frame)
{
    switch (frame->type) {
    case FP_FRAME_PING:
        if (frame->stream_id != 0) {
            client_protocol_error(c, "PING with nonzero stream id");
            return;
        }
        (void)fp_server_client_send(c, FP_FRAME_PONG, 0, NULL, 0, 1);
        return;
    case FP_FRAME_PONG:
        return;
    case FP_FRAME_UDP_OPEN:
        handle_udp_open(c, frame);
        return;
    case FP_FRAME_UDP_DATA:
        if (frame->stream_id == 0) {
            client_protocol_error(c, "UDP_DATA with zero stream id");
            return;
        }
        fp_server_session_client_data(c, frame->stream_id,
                                      frame->payload, frame->len);
        return;
    case FP_FRAME_UDP_CLOSE:
        handle_udp_close(c, frame);
        return;
    case FP_FRAME_GOAWAY:
        handle_goaway(c, frame);
        return;
    case FP_FRAME_HELLO:
    case FP_FRAME_HELLO_ACK:
    case FP_FRAME_TUNNEL_ADD:
    case FP_FRAME_TUNNEL_RESULT:
        client_protocol_error(c, "unexpected %s in ready state",
                              fp_frame_type_name(frame->type));
        return;
    default:
        client_protocol_error(c, "unknown frame type 0x%02x",
                              (unsigned)frame->type);
        return;
    }
}

static void server_client_handle_frame(server_client *c,
                                       const fp_frame *frame)
{
    if (c->dead) {
        return;
    }
    if (c->state == SC_HANDSHAKE) {
        fp_hello hello;
        char err[192];
        fp_buf ack;
        int rc;

        if (frame->type != FP_FRAME_HELLO) {
            client_protocol_error(c, "expected HELLO, got %s",
                                  fp_frame_type_name(frame->type));
            return;
        }
        if (fp_decode_hello(frame->payload, frame->len,
                            &hello, err, sizeof(err)) != 0) {
            FP_LOGW(c->tag, "bad HELLO: %s", err);
            client_reject_hello(c, FP_STATUS_MALFORMED, err);
            return;
        }
        if (hello.version != FP_PROTO_VERSION) {
            client_reject_hello(c, FP_STATUS_BAD_VERSION,
                                "unsupported protocol version");
            return;
        }
        if (!fp_constant_time_eq(c->srv->cfg.token,
                                 strlen(c->srv->cfg.token),
                                 hello.token, strlen(hello.token))) {
            FP_LOGW(c->tag, "authentication failed from %s",
                    c->peer_text);
            client_reject_hello(c, FP_STATUS_BAD_TOKEN, "invalid token");
            return;
        }
        snprintf(c->name, sizeof(c->name), "%s", hello.name);
        fp_buf_init(&ack);
        if (fp_build_hello_ack(&ack, FP_STATUS_OK, "welcome") != 0) {
            fp_buf_free(&ack);
            fp_server_client_set_dead(c, "cannot encode HELLO_ACK");
            return;
        }
        rc = fp_server_client_send(c, FP_FRAME_HELLO_ACK, 0,
                                   ack.data, ack.len, 1);
        fp_buf_free(&ack);
        if (rc != 0) {
            return;
        }
        c->state = SC_WAIT_TUNNELS;
        c->last_ping_ms = fp_now_ms();
        FP_LOGI(c->tag, "client '%s' authenticated from %s",
                c->name[0] != '\0' ? c->name : "(unnamed)",
                c->peer_text);
        return;
    }
    if (c->state == SC_WAIT_TUNNELS) {
        switch (frame->type) {
        case FP_FRAME_TUNNEL_ADD:
            handle_tunnel_add(c, frame);
            return;
        case FP_FRAME_PING:
            (void)fp_server_client_send(c, FP_FRAME_PONG, 0, NULL, 0, 1);
            return;
        case FP_FRAME_PONG:
            return;
        case FP_FRAME_GOAWAY:
            handle_goaway(c, frame);
            return;
        default:
            client_protocol_error(c, "unexpected %s before TUNNEL_ADD",
                                  fp_frame_type_name(frame->type));
            return;
        }
    }
    if (c->state == SC_READY) {
        handle_ready_frame(c, frame);
        return;
    }
    /* CLOSING: ignore further input; the flush/tick path closes. */
}

static void server_client_read(server_client *c)
{
    int iterations;

    if (c->dead) {
        return;
    }
    for (iterations = 0; iterations < 16; iterations++) {
        int n = fp_tcp_recv(c->fd, c->srv->rbuf, sizeof(c->srv->rbuf));
        int parsed;

        if (n > 0) {
            c->last_recv_ms = fp_now_ms();
            if (fp_parser_push(&c->parser, c->srv->rbuf, (size_t)n) != 0) {
                client_protocol_error(c, "frame buffer limit exceeded");
                return;
            }
            for (;;) {
                fp_frame frame;
                parsed = fp_parser_next(&c->parser, &frame);
                if (parsed == 0) {
                    break;
                }
                if (parsed < 0) {
                    client_protocol_error(c, "parser: %s",
                                          fp_parser_error(&c->parser));
                    return;
                }
                server_client_handle_frame(c, &frame);
                if (c->dead) {
                    return;
                }
            }
            if ((size_t)n < sizeof(c->srv->rbuf)) {
                return;
            }
            continue;
        }
        if (n == 0) {
            fp_server_client_set_dead(c, "peer closed the connection");
            return;
        }
        if (n == FP_IO_AGAIN) {
            return;
        }
        fp_server_client_set_dead(c, "control connection read failed");
        return;
    }
}

static void server_client_event_cb(fp_loop *loop, fp_socket_t fd,
                                   short revents, void *arg)
{
    server_client *c = (server_client *)arg;

    (void)loop;
    (void)fd;
    if (c == NULL || c->dead) {
        return;
    }
    if ((revents & FP_POLLNVAL) != 0) {
        fp_server_client_set_dead(c, "invalid descriptor");
        return;
    }
    if ((revents & FP_POLLOUT) != 0) {
        fp_server_client_flush(c);
    }
    if (c->dead) {
        return;
    }
    if ((revents & (FP_POLLIN | FP_POLLHUP | FP_POLLERR)) != 0) {
        server_client_read(c);
    }
}

static void server_client_destroy(server_client *c)
{
    fp_server *srv;

    if (c == NULL) {
        return;
    }
    srv = c->srv;
    fp_server_session_close_all(c, 0);
    fp_server_tunnel_close_all(c, 0);
    if (c->fd != FP_SOCKET_INVALID) {
        fp_loop_del(srv->loop, c->fd);
        fp_socket_close(c->fd);
        c->fd = FP_SOCKET_INVALID;
    }
    fp_parser_free(&c->parser);
    fp_sendq_free(&c->sendq);
    if (srv->nclients > 0) {
        srv->nclients--;
    }
    free(c);
}

static void server_sweep_dead(fp_server *srv)
{
    server_client **cursor = &srv->clients;

    while (*cursor != NULL) {
        server_client *c = *cursor;
        if (!c->dead) {
            cursor = &c->next;
            continue;
        }
        *cursor = c->next;
        server_client_destroy(c);
    }
}

static void server_tick(fp_server *srv)
{
    int64_t now = fp_now_ms();
    server_client *c;

    for (c = srv->clients; c != NULL; c = c->next) {
        if (c->dead) {
            continue;
        }
        if (c->state == SC_HANDSHAKE || c->state == SC_WAIT_TUNNELS) {
            if (now - c->connected_ms > FP_HANDSHAKE_TIMEOUT_MS) {
                fp_server_client_set_dead(c, "handshake timeout");
                continue;
            }
        }
        if (c->state == SC_CLOSING) {
            if (fp_sendq_empty(&c->sendq) ||
                now >= c->close_deadline_ms) {
                fp_server_client_set_dead(c, "closing complete");
            }
            continue;
        }
        if (c->state == SC_READY && srv->cfg.keepalive_s > 0) {
            int64_t keepalive_ms = (int64_t)srv->cfg.keepalive_s * 1000;
            if (now - c->last_recv_ms > keepalive_ms * 2) {
                fp_server_client_set_dead(c, "heartbeat timeout");
                continue;
            }
            if (now - c->last_ping_ms >= keepalive_ms) {
                c->last_ping_ms = now;
                (void)fp_server_client_send(c, FP_FRAME_PING, 0,
                                            NULL, 0, 1);
            }
        }
    }
    fp_server_session_tick(srv, now);
    server_sweep_dead(srv);
}

static void reject_connection(fp_socket_t fd, const char *reason)
{
    fp_sendq queue;
    fp_buf payload;
    int rc;

    fp_buf_init(&payload);
    if (fp_build_hello_ack(&payload, FP_STATUS_TOO_MANY_CLIENTS,
                           reason) != 0) {
        fp_buf_free(&payload);
        fp_socket_close(fd);
        return;
    }
    if (fp_sendq_init(&queue, FP_MAX_FRAME + 64) == 0) {
        if (fp_sendq_frame(&queue, FP_FRAME_HELLO_ACK, 0,
                           payload.data, payload.len) == FP_SENDQ_OK) {
            while (!fp_sendq_empty(&queue)) {
                size_t pending = fp_sendq_pending(&queue);
                rc = fp_tcp_send(fd, fp_sendq_data(&queue), pending);
                if (rc <= 0) {
                    break;
                }
                fp_sendq_consume(&queue, (size_t)rc);
            }
        }
        fp_sendq_free(&queue);
    }
    fp_buf_free(&payload);
    fp_socket_close(fd);
}

static void server_accept_cb(fp_loop *loop, fp_socket_t fd,
                             short revents, void *arg)
{
    fp_server *srv = (fp_server *)arg;
    int accepted = 0;

    (void)loop;
    (void)fd;
    if ((revents & FP_POLLNVAL) != 0) {
        FP_LOGE("server", "listener descriptor became invalid");
        return;
    }
    while (accepted < 16) {
        struct sockaddr_storage ss;
        fp_socklen_t sslen = (fp_socklen_t)sizeof(ss);
        fp_socket_t client_fd;
        server_client *c;
        char peer_text[FP_ENDPOINT_TEXT_MAX];

        memset(&ss, 0, sizeof(ss));
        client_fd = accept(srv->listen_fd, (struct sockaddr *)&ss, &sslen);
        if (client_fd == FP_SOCKET_INVALID) {
            if (net_would_block()) {
                return;
            }
            FP_LOGE("server", "accept failed");
            return;
        }
        accepted++;
        if (srv->nclients >= (size_t)srv->cfg.max_clients) {
            FP_LOGW("server", "rejecting connection from a new client: "
                    "limit of %d reached", srv->cfg.max_clients);
            reject_connection(client_fd, "too many clients");
            continue;
        }
        if (fp_set_nonblocking(client_fd) != 0) {
            FP_LOGW("server", "cannot make client socket non-blocking");
            fp_socket_close(client_fd);
            continue;
        }
        (void)fp_net_set_nodelay(client_fd);
        c = (server_client *)calloc(1, sizeof(*c));
        if (c == NULL) {
            FP_LOGE("server", "out of memory for client");
            fp_socket_close(client_fd);
            continue;
        }
        c->srv = srv;
        c->fd = client_fd;
        c->id = ++srv->next_client_id;
        snprintf(c->tag, sizeof(c->tag), "conn#%llu",
                 (unsigned long long)c->id);
        c->peer.len = sslen;
        memcpy(&c->peer.ss, &ss, sizeof(ss));
        if (fp_endpoint_format(&c->peer, peer_text,
                               sizeof(peer_text)) != 0) {
            snprintf(peer_text, sizeof(peer_text), "?");
        }
        snprintf(c->peer_text, sizeof(c->peer_text), "%s", peer_text);
        c->state = SC_HANDSHAKE;
        c->connected_ms = fp_now_ms();
        c->last_recv_ms = c->connected_ms;
        c->last_ping_ms = c->connected_ms;
        c->last_drop_log_ms = c->connected_ms;
        c->next_r_stream = 1;
        fp_parser_init(&c->parser);
        if (fp_sendq_init(&c->sendq, FP_SENDQ_LIMIT) != 0) {
            fp_socket_close(client_fd);
            c->fd = FP_SOCKET_INVALID;
            fp_parser_free(&c->parser);
            free(c);
            FP_LOGE("server", "cannot allocate send queue");
            continue;
        }
        c->next = srv->clients;
        srv->clients = c;
        srv->nclients++;
        if (fp_loop_add(srv->loop, client_fd, FP_POLLIN,
                        server_client_event_cb, c) != 0) {
            srv->clients = c->next;
            srv->nclients--;
            c->next = NULL;
            fp_sendq_free(&c->sendq);
            fp_parser_free(&c->parser);
            fp_socket_close(client_fd);
            c->fd = FP_SOCKET_INVALID;
            free(c);
            FP_LOGE("server", "cannot watch client socket");
            continue;
        }
        FP_LOGI(c->tag, "accepted connection from %s", c->peer_text);
    }
}

static void server_force_close_all(fp_server *srv)
{
    server_client *c;

    for (c = srv->clients; c != NULL; c = c->next) {
        fp_server_client_set_dead(c, "server shutdown");
    }
    server_sweep_dead(srv);
}

static void server_shutdown(fp_server *srv)
{
    int64_t deadline;
    server_client *c;

    if (srv->listener_watched) {
        fp_loop_del(srv->loop, srv->listen_fd);
        srv->listener_watched = 0;
    }
    if (srv->listen_fd != FP_SOCKET_INVALID) {
        fp_socket_close(srv->listen_fd);
        srv->listen_fd = FP_SOCKET_INVALID;
    }
    if (srv->clients == NULL) {
        return;
    }
    FP_LOGI("server", "shutting down, notifying %lu client%s",
            (unsigned long)srv->nclients, srv->nclients == 1 ? "" : "s");
    for (c = srv->clients; c != NULL; c = c->next) {
        fp_buf buf;
        fp_buf_init(&buf);
        if (fp_build_goaway(&buf, "server shutting down") == 0) {
            (void)fp_server_client_send(c, FP_FRAME_GOAWAY, 0,
                                        buf.data, buf.len, 0);
        }
        fp_buf_free(&buf);
        fp_server_client_set_closing(c, SERVER_SHUTDOWN_GRACE_MS);
    }
    deadline = fp_now_ms() + SERVER_SHUTDOWN_GRACE_MS;
    while (srv->clients != NULL && fp_now_ms() < deadline) {
        (void)fp_loop_run_once(srv->loop, 50);
        server_tick(srv);
    }
    server_force_close_all(srv);
}

fp_server *fp_server_new(const fp_server_config *cfg,
                         char *err, size_t errcap)
{
    fp_server *srv;
    char verr[192];

    if (cfg == NULL) {
        return NULL;
    }
    if (fp_server_config_validate(cfg, verr, sizeof(verr)) != 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "%s", verr);
        }
        return NULL;
    }
    srv = (fp_server *)calloc(1, sizeof(*srv));
    if (srv == NULL) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "out of memory");
        }
        return NULL;
    }
    srv->cfg = *cfg;
    srv->listen_fd = FP_SOCKET_INVALID;
    srv->next_client_id = 0;
    srv->loop = fp_loop_new();
    if (srv->loop == NULL) {
        free(srv);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "cannot create event loop");
        }
        return NULL;
    }
    srv->last_tick_ms = fp_now_ms();
    return srv;
}

int fp_server_start(fp_server *srv, char *err, size_t errcap)
{
    fp_socket_t fd = FP_SOCKET_INVALID;
    char lerr[192];

    if (srv == NULL) {
        return -1;
    }
    if (srv->listen_fd != FP_SOCKET_INVALID) {
        return 0;
    }
    if (fp_tcp_listen(srv->cfg.listen_host, srv->cfg.listen_port,
                      64, &fd, lerr, sizeof(lerr)) != 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "%s", lerr);
        }
        return -1;
    }
    if (fp_loop_add(srv->loop, fd, FP_POLLIN,
                    server_accept_cb, srv) != 0) {
        fp_socket_close(fd);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "cannot watch listening socket");
        }
        return -1;
    }
    srv->listen_fd = fd;
    srv->listener_watched = 1;
    FP_LOGI("server", "listening on %s:%u", srv->cfg.listen_host,
            (unsigned)srv->cfg.listen_port);
    if (srv->cfg.token[0] == '\0') {
        FP_LOGW("server", "no token configured; any client may connect");
    }
    return 0;
}

void fp_server_run(fp_server *srv)
{
    if (srv == NULL) {
        return;
    }
    while (!srv->stopping && !fp_stop_requested()) {
        int rc = fp_loop_run_once(srv->loop, 200);
        int64_t now;
        if (rc < 0) {
            FP_LOGE("server", "event loop failed");
            break;
        }
        now = fp_now_ms();
        if (now - srv->last_tick_ms >= 200) {
            server_tick(srv);
            srv->last_tick_ms = now;
        }
    }
    server_shutdown(srv);
}

void fp_server_stop(fp_server *srv)
{
    if (srv != NULL) {
        srv->stopping = 1;
    }
}

void fp_server_free(fp_server *srv)
{
    if (srv == NULL) {
        return;
    }
    if (srv->listener_watched) {
        fp_loop_del(srv->loop, srv->listen_fd);
        srv->listener_watched = 0;
    }
    if (srv->listen_fd != FP_SOCKET_INVALID) {
        fp_socket_close(srv->listen_fd);
        srv->listen_fd = FP_SOCKET_INVALID;
    }
    while (srv->clients != NULL) {
        server_client *c = srv->clients;
        srv->clients = c->next;
        fp_server_client_set_dead(c, "server freed");
        server_client_destroy(c);
    }
    fp_loop_free(srv->loop);
    free(srv);
}
