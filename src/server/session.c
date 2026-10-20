#include "server_internal.h"

#include "log.h"
#include "protocol.h"
#include "tunnel_table.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <netinet/in.h>
#endif

#define SESSION_MAX_DATAGRAM 65507u

static void link_session(server_client *c, server_session *s)
{
    s->next = c->sessions;
    c->sessions = s;
    c->nsessions++;
    if (s->tunnel != NULL) {
        s->tunnel->nsessions++;
    }
    c->srv->nsessions++;
}

static void unlink_session(server_client *c, server_session *s)
{
    server_session **cursor = &c->sessions;

    while (*cursor != NULL) {
        if (*cursor == s) {
            *cursor = s->next;
            break;
        }
        cursor = &(*cursor)->next;
    }
    if (c->nsessions > 0) {
        c->nsessions--;
    }
    if (s->tunnel != NULL && s->tunnel->nsessions > 0) {
        s->tunnel->nsessions--;
    }
    if (c->srv->nsessions > 0) {
        c->srv->nsessions--;
    }
}

server_session *fp_server_session_lookup_id(server_client *c,
                                            uint32_t stream_id)
{
    server_session *s;

    if (c == NULL) {
        return NULL;
    }
    for (s = c->sessions; s != NULL; s = s->next) {
        if (s->stream_id == stream_id) {
            return s;
        }
    }
    return NULL;
}

static server_session *lookup_peer(server_tunnel *t,
                                   const fp_endpoint *peer)
{
    server_session *s;

    if (t == NULL || peer == NULL) {
        return NULL;
    }
    for (s = t->client->sessions; s != NULL; s = s->next) {
        if (s->tunnel == t && fp_endpoint_equal(&s->peer, peer)) {
            return s;
        }
    }
    return NULL;
}

static int udp_bind_any(int family, fp_socket_t *out,
                        char *err, size_t errcap)
{
    fp_socket_t fd;
    struct sockaddr_storage ss;
    fp_socklen_t len;
    int rc;

    fd = fp_socket_open(family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == FP_SOCKET_INVALID) {
        int e = fp_net_last_error();
        if (err != NULL && errcap > 0) {
            fp_net_error_text(e, err, errcap);
        }
        return -1;
    }
    memset(&ss, 0, sizeof(ss));
    if (family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = htonl(INADDR_ANY);
        sin->sin_port = 0;
        len = (fp_socklen_t)sizeof(*sin);
    } else if (family == AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = 0;
        len = (fp_socklen_t)sizeof(*sin6);
    } else {
        fp_socket_close(fd);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "unsupported address family");
        }
        return -1;
    }
    rc = bind(fd, (const struct sockaddr *)&ss, len);
    if (rc != 0) {
        int e = fp_net_last_error();
        if (err != NULL && errcap > 0) {
            fp_net_error_text(e, err, errcap);
        }
        fp_socket_close(fd);
        return -1;
    }
    *out = fd;
    return 0;
}

int fp_server_session_open_l(server_client *c, uint32_t stream_id,
                             const char *target_text,
                             char *err, size_t errcap)
{
    char host[FP_MAX_HOST + 1];
    uint16_t port;
    char perr[128];
    server_tunnel *t;
    fp_endpoint target;
    fp_socket_t fd = FP_SOCKET_INVALID;
    server_session *s;
    int rc;

    if (c == NULL || target_text == NULL) {
        return -1;
    }
    if (stream_id == 0 || (stream_id & FP_STREAM_R_BIT) != 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "invalid L stream id");
        }
        return -1;
    }
    if (fp_server_session_lookup_id(c, stream_id) != NULL) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "duplicate stream id %u",
                     (unsigned)stream_id);
        }
        return -1;
    }
    if (fp_hostport_parse(target_text, host, sizeof(host), &port,
                          perr, sizeof(perr)) != 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "%s", perr);
        }
        return -1;
    }
    if (fp_endpoint_is_wildcard(host)) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "target must be concrete");
        }
        return -1;
    }
    t = fp_server_tunnel_find_l(c, host, port);
    if (t == NULL) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap,
                     "target does not match a configured L tunnel");
        }
        return -1;
    }
    if (c->nsessions >= FP_MAX_SESSIONS ||
        c->srv->nsessions >= FP_MAX_SESSIONS) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "session limit reached");
        }
        return -1;
    }
    if (fp_endpoint_resolve(host, port, SOCK_DGRAM,
                            &target, perr, sizeof(perr)) != 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "%s", perr);
        }
        return -1;
    }
    if (udp_bind_any(target.ss.ss_family, &fd,
                     perr, sizeof(perr)) != 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "cannot bind UDP socket: %s", perr);
        }
        return -1;
    }
    rc = connect(fd, (const struct sockaddr *)&target.ss, target.len);
    if (rc != 0) {
        int e = fp_net_last_error();
        if (err != NULL && errcap > 0) {
            fp_net_error_text(e, err, errcap);
        }
        fp_socket_close(fd);
        return -1;
    }
    s = (server_session *)calloc(1, sizeof(*s));
    if (s == NULL) {
        fp_socket_close(fd);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "out of memory");
        }
        return -1;
    }
    s->client = c;
    s->tunnel = t;
    s->dir = FP_DIR_L;
    s->stream_id = stream_id;
    s->udp_fd = fd;
    s->owns_fd = 1;
    s->target = target;
    s->peer = target;
    s->last_active_ms = fp_now_ms();
    if (fp_loop_add(c->srv->loop, fd, FP_POLLIN,
                    fp_server_session_on_l_udp, s) != 0) {
        fp_socket_close(fd);
        free(s);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "cannot watch UDP socket");
        }
        return -1;
    }
    s->watcher_added = 1;
    link_session(c, s);
    return 0;
}

uint32_t fp_server_alloc_r_stream(server_client *c)
{
    size_t attempts;

    if (c == NULL) {
        return 0;
    }
    for (attempts = 0; attempts <= FP_MAX_SESSIONS; attempts++) {
        uint32_t n = c->next_r_stream & FP_STREAM_ID_MASK;
        uint32_t sid;
        c->next_r_stream = (c->next_r_stream + 1) & FP_STREAM_ID_MASK;
        if (n == 0) {
            continue;
        }
        sid = FP_STREAM_R_BIT | n;
        if (fp_server_session_lookup_id(c, sid) == NULL) {
            return sid;
        }
    }
    return 0;
}

int fp_server_session_open_r(server_client *c, server_tunnel *t,
                             const fp_endpoint *peer, uint32_t *stream_id,
                             server_session **out, char *err, size_t errcap)
{
    uint32_t sid;
    server_session *s;
    char target_text[FP_ENDPOINT_TEXT_MAX];
    fp_buf openbuf;
    int rc;

    if (c == NULL || t == NULL || peer == NULL || out == NULL) {
        return -1;
    }
    if (c->dead || c->state != SC_READY) {
        return -1;
    }
    if (c->nsessions >= FP_MAX_SESSIONS ||
        c->srv->nsessions >= FP_MAX_SESSIONS) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "session limit reached");
        }
        return -1;
    }
    if (fp_hostport_format(t->spec.target_host, t->spec.target_port,
                           target_text, sizeof(target_text)) != 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "target address is too long");
        }
        return -1;
    }
    sid = fp_server_alloc_r_stream(c);
    if (sid == 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "stream id space exhausted");
        }
        return -1;
    }
    s = (server_session *)calloc(1, sizeof(*s));
    if (s == NULL) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "out of memory");
        }
        return -1;
    }
    s->client = c;
    s->tunnel = t;
    s->dir = FP_DIR_R;
    s->stream_id = sid;
    s->udp_fd = t->listen_fd;
    s->owns_fd = 0;
    s->peer = *peer;
    s->last_active_ms = fp_now_ms();
    link_session(c, s);
    fp_buf_init(&openbuf);
    rc = fp_build_open(&openbuf, target_text);
    if (rc == 0) {
        rc = fp_server_client_send(c, FP_FRAME_UDP_OPEN, sid,
                                   openbuf.data, openbuf.len, 0);
    }
    fp_buf_free(&openbuf);
    if (rc != 0) {
        fp_server_session_close(c, s, FP_CLOSE_ERROR, 0);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "cannot send UDP_OPEN");
        }
        return -1;
    }
    *stream_id = sid;
    *out = s;
    return 0;
}

void fp_server_session_close(server_client *c, server_session *s,
                             int reason, int notify)
{
    if (c == NULL || s == NULL) {
        return;
    }
    if (notify && !c->dead && c->state == SC_READY) {
        uint8_t byte = (uint8_t)(reason < 0 ? FP_CLOSE_NORMAL : reason);
        (void)fp_server_client_send(c, FP_FRAME_UDP_CLOSE, s->stream_id,
                                    &byte, 1, 0);
    }
    if (s->watcher_added) {
        fp_loop_del(c->srv->loop, s->udp_fd);
        s->watcher_added = 0;
    }
    if (s->owns_fd && s->udp_fd != FP_SOCKET_INVALID) {
        fp_socket_close(s->udp_fd);
        s->udp_fd = FP_SOCKET_INVALID;
    }
    unlink_session(c, s);
    FP_LOGD(c->tag, "closed %s stream %u (reason %d)",
            fp_tunnel_dir_name(s->dir), (unsigned)s->stream_id, reason);
    free(s);
}

void fp_server_session_client_close(server_client *c, uint32_t stream_id,
                                    int reason)
{
    server_session *s = fp_server_session_lookup_id(c, stream_id);

    if (s == NULL) {
        FP_LOGD(c != NULL ? c->tag : "server",
                "UDP_CLOSE for unknown stream %u",
                (unsigned)stream_id);
        return;
    }
    fp_server_session_close(c, s, reason, 0);
}

void fp_server_session_close_all(server_client *c, int notify)
{
    if (c == NULL) {
        return;
    }
    while (c->sessions != NULL) {
        fp_server_session_close(c, c->sessions, FP_CLOSE_NORMAL, notify);
    }
}

void fp_server_session_close_tunnel(server_tunnel *t, int notify)
{
    server_client *c;
    server_session *s;

    if (t == NULL) {
        return;
    }
    c = t->client;
    s = c->sessions;
    while (s != NULL) {
        server_session *next = s->next;
        if (s->tunnel == t) {
            fp_server_session_close(c, s, FP_CLOSE_NORMAL, notify);
        }
        s = next;
    }
}

void fp_server_session_client_data(server_client *c, uint32_t stream_id,
                                   const uint8_t *payload, size_t len)
{
    server_session *s;
    int rc;
    static const uint8_t zero;

    if (c == NULL) {
        return;
    }
    s = fp_server_session_lookup_id(c, stream_id);
    if (s == NULL) {
        fp_server_note_drop(c, "data for unknown stream");
        return;
    }
    if (len > SESSION_MAX_DATAGRAM) {
        fp_server_note_drop(c, "oversized UDP datagram");
        return;
    }
    s->last_active_ms = fp_now_ms();
    if (payload == NULL && len == 0) {
        payload = &zero;
    }
    if (s->dir == FP_DIR_L) {
        rc = fp_udp_sendto(s->udp_fd, payload, len,
                           (const struct sockaddr *)&s->target.ss,
                           s->target.len);
    } else {
        rc = fp_udp_sendto(s->tunnel->listen_fd, payload, len,
                           (const struct sockaddr *)&s->peer.ss,
                           s->peer.len);
    }
    if (rc < 0) {
        fp_server_note_drop(c, "UDP send failed");
    }
}

void fp_server_session_tick(fp_server *srv, int64_t now)
{
    server_client *c;
    int64_t idle_ms;

    if (srv == NULL) {
        return;
    }
    idle_ms = (int64_t)srv->cfg.udp_idle_s * 1000;
    for (c = srv->clients; c != NULL; c = c->next) {
        server_session *s;
        if (c->dead) {
            continue;
        }
        s = c->sessions;
        while (s != NULL) {
            server_session *next = s->next;
            if (now - s->last_active_ms >= idle_ms) {
                fp_server_session_close(c, s, FP_CLOSE_TIMEOUT, 1);
            }
            s = next;
        }
    }
}

void fp_server_session_on_l_udp(fp_loop *loop, fp_socket_t fd,
                                short revents, void *arg)
{
    server_session *s = (server_session *)arg;
    server_client *c;
    int iterations;

    (void)loop;
    (void)fd;
    (void)revents;
    if (s == NULL) {
        return;
    }
    c = s->client;
    if (c->dead) {
        return;
    }
    for (iterations = 0; iterations < 32; iterations++) {
        fp_endpoint from;
        fp_socklen_t fromlen = (fp_socklen_t)sizeof(from.ss);
        int n;
        int rc;

        memset(&from, 0, sizeof(from));
        n = fp_udp_recvfrom(s->udp_fd, c->srv->udp_buf,
                            sizeof(c->srv->udp_buf), &from.ss, &fromlen);
        if (n < 0) {
            break;
        }
        from.len = fromlen;
        if (n > 0 && !fp_endpoint_equal(&from, &s->target)) {
            fp_server_note_drop(c, "unexpected UDP source");
            continue;
        }
        s->last_active_ms = fp_now_ms();
        rc = fp_server_client_send(c, FP_FRAME_UDP_DATA, s->stream_id,
                                   c->srv->udp_buf, (size_t)n, 0);
        if (rc == 1) {
            fp_server_note_drop(c, "send queue full");
        } else if (rc < 0) {
            return;
        }
    }
}

void fp_server_session_on_r_udp(fp_loop *loop, fp_socket_t fd,
                                short revents, void *arg)
{
    server_tunnel *t = (server_tunnel *)arg;
    server_client *c;
    int iterations;

    (void)loop;
    (void)fd;
    (void)revents;
    if (t == NULL) {
        return;
    }
    c = t->client;
    if (c == NULL || c->dead) {
        return;
    }
    for (iterations = 0; iterations < 32; iterations++) {
        fp_endpoint from;
        fp_socklen_t fromlen = (fp_socklen_t)sizeof(from.ss);
        int n;
        server_session *s;
        int rc;

        memset(&from, 0, sizeof(from));
        n = fp_udp_recvfrom(t->listen_fd, c->srv->udp_buf,
                            sizeof(c->srv->udp_buf), &from.ss, &fromlen);
        if (n < 0) {
            break;
        }
        from.len = fromlen;
        if ((size_t)n > SESSION_MAX_DATAGRAM) {
            fp_server_note_drop(c, "oversized UDP datagram");
            continue;
        }
        s = lookup_peer(t, &from);
        if (s == NULL) {
            uint32_t sid = 0;
            char err[192];
            if (fp_server_session_open_r(c, t, &from, &sid, &s,
                                         err, sizeof(err)) != 0) {
                fp_server_note_drop(c, "cannot open R session");
                continue;
            }
            FP_LOGD(c->tag, "new R stream %u for %s:%u",
                    (unsigned)sid, t->spec.target_host,
                    (unsigned)t->spec.target_port);
        }
        s->last_active_ms = fp_now_ms();
        rc = fp_server_client_send(c, FP_FRAME_UDP_DATA, s->stream_id,
                                   c->srv->udp_buf, (size_t)n, 0);
        if (rc == 1) {
            fp_server_note_drop(c, "send queue full");
        } else if (rc < 0) {
            return;
        }
    }
}
