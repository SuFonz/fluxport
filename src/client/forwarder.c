#include "client_internal.h"

#include "log.h"
#include "tunnel_table.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <netinet/in.h>
#endif

#define SESSION_MAX_DATAGRAM 65507u

static int udp_bind_any(int family, fp_socket_t *out,
                        char *err, size_t errcap)
{
    fp_socket_t fd;
    struct sockaddr_storage ss;
    fp_socklen_t len;

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
    if (bind(fd, (const struct sockaddr *)&ss, len) != 0) {
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

static void link_session(fp_client *cl, client_session *s)
{
    s->next = cl->sessions;
    cl->sessions = s;
    cl->nsessions++;
    if (s->tunnel != NULL) {
        s->tunnel->nsessions++;
    }
}

static void unlink_session(fp_client *cl, client_session *s)
{
    client_session **cursor = &cl->sessions;

    while (*cursor != NULL) {
        if (*cursor == s) {
            *cursor = s->next;
            break;
        }
        cursor = &(*cursor)->next;
    }
    if (cl->nsessions > 0) {
        cl->nsessions--;
    }
    if (s->tunnel != NULL && s->tunnel->nsessions > 0) {
        s->tunnel->nsessions--;
    }
}

client_session *fp_client_session_lookup_id(fp_client *cl, uint32_t sid)
{
    client_session *s;

    if (cl == NULL) {
        return NULL;
    }
    for (s = cl->sessions; s != NULL; s = s->next) {
        if (s->stream_id == sid) {
            return s;
        }
    }
    return NULL;
}

static client_session *lookup_peer(fp_client *cl, client_tunnel *t,
                                   const fp_endpoint *peer)
{
    client_session *s;

    for (s = cl->sessions; s != NULL; s = s->next) {
        if (s->tunnel == t && fp_endpoint_equal(&s->peer, peer)) {
            return s;
        }
    }
    return NULL;
}

uint32_t fp_client_alloc_l_stream(fp_client *cl)
{
    size_t attempts;

    if (cl == NULL) {
        return 0;
    }
    for (attempts = 0; attempts <= FP_MAX_SESSIONS; attempts++) {
        uint32_t id = cl->next_l_stream & FP_STREAM_ID_MASK;
        cl->next_l_stream = (cl->next_l_stream + 1) & FP_STREAM_ID_MASK;
        if (id == 0) {
            continue;
        }
        if (fp_client_session_lookup_id(cl, id) == NULL) {
            return id;
        }
    }
    return 0;
}

int fp_client_session_open_l(fp_client *cl, client_tunnel *t,
                             const fp_endpoint *peer, client_session **out,
                             char *err, size_t errcap)
{
    client_session *s;
    uint32_t sid;
    char target[FP_ENDPOINT_TEXT_MAX];
    fp_buf openbuf;
    int rc;

    if (cl == NULL || t == NULL || peer == NULL || out == NULL) {
        return -1;
    }
    if (cl->dead || cl->stopping || cl->state != CC_RUNNING ||
        !t->ok || t->spec.dir != FP_DIR_L ||
        t->listen_fd == FP_SOCKET_INVALID) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "L tunnel is not active");
        }
        return -1;
    }
    if (cl->nsessions >= FP_MAX_SESSIONS) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "session limit reached");
        }
        return -1;
    }
    sid = fp_client_alloc_l_stream(cl);
    if (sid == 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "stream id space exhausted");
        }
        return -1;
    }
    if (fp_hostport_format(t->spec.target_host, t->spec.target_port,
                           target, sizeof(target)) != 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "target address is too long");
        }
        return -1;
    }
    s = (client_session *)calloc(1, sizeof(*s));
    if (s == NULL) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "out of memory");
        }
        return -1;
    }
    s->client = cl;
    s->tunnel = t;
    s->dir = FP_DIR_L;
    s->stream_id = sid;
    s->peer = *peer;
    s->udp_fd = FP_SOCKET_INVALID;
    s->owns_fd = 0;
    s->last_active_ms = fp_now_ms();
    link_session(cl, s);
    fp_buf_init(&openbuf);
    rc = fp_build_open(&openbuf, target);
    if (rc == 0) {
        rc = fp_client_send(cl, FP_FRAME_UDP_OPEN, sid,
                            openbuf.data, openbuf.len, 1);
    }
    fp_buf_free(&openbuf);
    if (rc != 0) {
        fp_client_session_close(cl, s, FP_CLOSE_ERROR, 0);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "cannot send UDP_OPEN");
        }
        return -1;
    }
    *out = s;
    return 0;
}

static client_tunnel *find_r_tunnel(fp_client *cl, const char *host,
                                    uint16_t port)
{
    size_t i;

    for (i = 0; i < cl->ntunnels; i++) {
        client_tunnel *t = &cl->tunnels[i];
        if (t->ok && t->spec.dir == FP_DIR_R &&
            t->spec.target_port == port &&
            strcmp(t->spec.target_host, host) == 0) {
            return t;
        }
    }
    return NULL;
}

int fp_client_session_open_r(fp_client *cl, uint32_t sid,
                             const char *target_text,
                             client_session **out,
                             char *err, size_t errcap)
{
    char host[FP_MAX_HOST + 1];
    uint16_t port;
    char perr[128];
    client_tunnel *t;
    fp_endpoint target;
    fp_socket_t fd = FP_SOCKET_INVALID;
    client_session *s;

    if (cl == NULL || target_text == NULL || out == NULL) {
        return -1;
    }
    if (cl->dead || cl->stopping || cl->state != CC_RUNNING) {
        return -1;
    }
    if (sid == 0 || (sid & FP_STREAM_R_BIT) == 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "invalid R stream id");
        }
        return -1;
    }
    if (fp_client_session_lookup_id(cl, sid) != NULL) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "duplicate stream id");
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
    t = find_r_tunnel(cl, host, port);
    if (t == NULL) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "target does not match an R tunnel");
        }
        return -1;
    }
    if (cl->nsessions >= FP_MAX_SESSIONS) {
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
    s = (client_session *)calloc(1, sizeof(*s));
    if (s == NULL) {
        fp_socket_close(fd);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "out of memory");
        }
        return -1;
    }
    s->client = cl;
    s->tunnel = t;
    s->dir = FP_DIR_R;
    s->stream_id = sid;
    s->target = target;
    s->udp_fd = fd;
    s->owns_fd = 1;
    s->last_active_ms = fp_now_ms();
    if (fp_loop_add(cl->loop, fd, FP_POLLIN,
                    fp_client_on_session_udp, s) != 0) {
        fp_socket_close(fd);
        free(s);
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "cannot watch UDP socket");
        }
        return -1;
    }
    s->watcher_added = 1;
    link_session(cl, s);
    *out = s;
    return 0;
}

void fp_client_session_close(fp_client *cl, client_session *s,
                             int reason, int notify)
{
    if (cl == NULL || s == NULL) {
        return;
    }
    if (notify && !cl->dead && !cl->stopping &&
        cl->state == CC_RUNNING) {
        uint8_t byte = (uint8_t)(reason < 0 ? FP_CLOSE_NORMAL : reason);
        (void)fp_client_send(cl, FP_FRAME_UDP_CLOSE, s->stream_id,
                             &byte, 1, 0);
    }
    if (s->watcher_added) {
        fp_loop_del(cl->loop, s->udp_fd);
        s->watcher_added = 0;
    }
    if (s->owns_fd && s->udp_fd != FP_SOCKET_INVALID) {
        fp_socket_close(s->udp_fd);
        s->udp_fd = FP_SOCKET_INVALID;
    }
    unlink_session(cl, s);
    FP_LOGD(cl->tag, "closed %s stream %u (reason %d)",
            fp_tunnel_dir_name(s->dir), (unsigned)s->stream_id, reason);
    free(s);
}

void fp_client_session_peer_close(fp_client *cl, uint32_t sid, int reason)
{
    client_session *s = fp_client_session_lookup_id(cl, sid);

    if (s == NULL) {
        FP_LOGD(cl->tag, "UDP_CLOSE for unknown stream %u",
                (unsigned)sid);
        return;
    }
    fp_client_session_close(cl, s, reason, 0);
}

void fp_client_session_data(fp_client *cl, uint32_t sid,
                            const uint8_t *payload, size_t len)
{
    client_session *s;
    static const uint8_t zero;
    int rc;

    if (cl == NULL) {
        return;
    }
    s = fp_client_session_lookup_id(cl, sid);
    if (s == NULL) {
        fp_client_note_drop(cl, "data for unknown stream");
        return;
    }
    if (len > SESSION_MAX_DATAGRAM) {
        fp_client_note_drop(cl, "oversized UDP datagram");
        return;
    }
    s->last_active_ms = fp_now_ms();
    if (payload == NULL && len == 0) {
        payload = &zero;
    }
    if (s->dir == FP_DIR_L) {
        if (!s->tunnel->watcher_added ||
            s->tunnel->listen_fd == FP_SOCKET_INVALID) {
            fp_client_note_drop(cl, "L listener is gone");
            return;
        }
        rc = fp_udp_sendto(s->tunnel->listen_fd, payload, len,
                           (const struct sockaddr *)&s->peer.ss,
                           s->peer.len);
    } else {
        rc = fp_udp_sendto(s->udp_fd, payload, len,
                           (const struct sockaddr *)&s->target.ss,
                           s->target.len);
    }
    if (rc < 0) {
        fp_client_note_drop(cl, "UDP send failed");
    }
}

void fp_client_session_close_all(fp_client *cl, int notify)
{
    if (cl == NULL) {
        return;
    }
    while (cl->sessions != NULL) {
        fp_client_session_close(cl, cl->sessions, FP_CLOSE_NORMAL, notify);
    }
}

void fp_client_session_tick(fp_client *cl, int64_t now)
{
    client_session *s;
    int64_t idle_ms;

    if (cl == NULL) {
        return;
    }
    idle_ms = (int64_t)cl->cfg.udp_idle_s * 1000;
    s = cl->sessions;
    while (s != NULL) {
        client_session *next = s->next;
        if (now - s->last_active_ms >= idle_ms) {
            fp_client_session_close(cl, s, FP_CLOSE_TIMEOUT, 1);
        }
        s = next;
    }
}

void fp_client_on_l_listener(fp_loop *loop, fp_socket_t fd,
                             short revents, void *arg)
{
    client_tunnel *t = (client_tunnel *)arg;
    fp_client *cl;
    int iterations;

    (void)loop;
    (void)fd;
    (void)revents;
    if (t == NULL || t->client == NULL) {
        return;
    }
    cl = t->client;
    if (cl->dead || cl->stopping) {
        return;
    }
    for (iterations = 0; iterations < 32; iterations++) {
        fp_endpoint from;
        fp_socklen_t fromlen = (fp_socklen_t)sizeof(from.ss);
        int n;
        client_session *s;
        char err[192];

        memset(&from, 0, sizeof(from));
        n = fp_udp_recvfrom(t->listen_fd, cl->udp_buf,
                            sizeof(cl->udp_buf), &from.ss, &fromlen);
        if (n < 0) {
            break;
        }
        from.len = fromlen;
        if ((size_t)n > SESSION_MAX_DATAGRAM) {
            fp_client_note_drop(cl, "oversized local datagram");
            continue;
        }
        s = lookup_peer(cl, t, &from);
        if (s == NULL) {
            if (fp_client_session_open_l(cl, t, &from, &s,
                                         err, sizeof(err)) != 0) {
                fp_client_note_drop(cl, "cannot open L session");
                continue;
            }
            FP_LOGD(cl->tag, "new L stream %u for %s:%u",
                    (unsigned)s->stream_id, t->spec.target_host,
                    (unsigned)t->spec.target_port);
        }
        s->last_active_ms = fp_now_ms();
        if (fp_client_send(cl, FP_FRAME_UDP_DATA, s->stream_id,
                           cl->udp_buf, (size_t)n, 0) == 1) {
            fp_client_note_drop(cl, "send queue full");
        }
    }
}

void fp_client_on_session_udp(fp_loop *loop, fp_socket_t fd,
                              short revents, void *arg)
{
    client_session *s = (client_session *)arg;
    fp_client *cl;
    int iterations;

    (void)loop;
    (void)fd;
    (void)revents;
    if (s == NULL || s->client == NULL) {
        return;
    }
    cl = s->client;
    if (cl->dead || cl->stopping || s->dir != FP_DIR_R) {
        return;
    }
    for (iterations = 0; iterations < 32; iterations++) {
        fp_endpoint from;
        fp_socklen_t fromlen = (fp_socklen_t)sizeof(from.ss);
        int n;
        int rc;

        memset(&from, 0, sizeof(from));
        n = fp_udp_recvfrom(s->udp_fd, cl->udp_buf,
                            sizeof(cl->udp_buf), &from.ss, &fromlen);
        if (n < 0) {
            break;
        }
        from.len = fromlen;
        if (n > 0 && !fp_endpoint_equal(&from, &s->target)) {
            fp_client_note_drop(cl, "unexpected UDP source");
            continue;
        }
        if ((size_t)n > SESSION_MAX_DATAGRAM) {
            fp_client_note_drop(cl, "oversized UDP datagram");
            continue;
        }
        s->last_active_ms = fp_now_ms();
        rc = fp_client_send(cl, FP_FRAME_UDP_DATA, s->stream_id,
                            cl->udp_buf, (size_t)n, 0);
        if (rc == 1) {
            fp_client_note_drop(cl, "send queue full");
        } else if (rc < 0) {
            return;
        }
    }
}

void fp_client_tunnels_bind_l(fp_client *cl)
{
    size_t i;

    if (cl == NULL) {
        return;
    }
    for (i = 0; i < cl->ntunnels; i++) {
        client_tunnel *t = &cl->tunnels[i];
        fp_socket_t fd = FP_SOCKET_INVALID;
        char err[192];

        if (!t->ok || t->spec.dir != FP_DIR_L) {
            continue;
        }
        if (fp_udp_bind(t->spec.listen_addr, t->spec.listen_port, 1,
                        &fd, err, sizeof(err)) != 0) {
            t->ok = 0;
            t->status = FP_STATUS_BIND_FAILED;
            snprintf(t->msg, sizeof(t->msg), "local bind failed: %s", err);
            cl->tunnels_failed++;
            FP_LOGW(cl->tag, "L tunnel %s:%u -> %s:%u failed to bind: %s",
                    t->spec.listen_addr, (unsigned)t->spec.listen_port,
                    t->spec.target_host, (unsigned)t->spec.target_port,
                    err);
            continue;
        }
        if (fp_loop_add(cl->loop, fd, FP_POLLIN,
                        fp_client_on_l_listener, t) != 0) {
            fp_socket_close(fd);
            t->ok = 0;
            t->status = FP_STATUS_INTERNAL;
            snprintf(t->msg, sizeof(t->msg), "cannot watch local socket");
            cl->tunnels_failed++;
            FP_LOGW(cl->tag, "cannot watch L listener for %s:%u",
                    t->spec.listen_addr, (unsigned)t->spec.listen_port);
            continue;
        }
        t->listen_fd = fd;
        t->watcher_added = 1;
        FP_LOGI(cl->tag, "L tunnel %s:%u -> %s:%u listening",
                t->spec.listen_addr, (unsigned)t->spec.listen_port,
                t->spec.target_host, (unsigned)t->spec.target_port);
    }
}

void fp_client_tunnels_close_all(fp_client *cl)
{
    size_t i;

    if (cl == NULL) {
        return;
    }
    for (i = 0; i < cl->ntunnels; i++) {
        client_tunnel *t = &cl->tunnels[i];
        if (t->watcher_added) {
            fp_loop_del(cl->loop, t->listen_fd);
            t->watcher_added = 0;
        }
        if (t->listen_fd != FP_SOCKET_INVALID) {
            fp_socket_close(t->listen_fd);
            t->listen_fd = FP_SOCKET_INVALID;
        }
        t->nsessions = 0;
    }
}
