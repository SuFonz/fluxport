#include "addr.h"

#include "protocol.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#endif

static const char *gai_text(int rc)
{
#ifdef _WIN32
    return gai_strerrorA(rc);
#else
    return gai_strerror(rc);
#endif
}

static int host_char_ok(unsigned char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) {
        return 1;
    }
    switch (c) {
    case '.': case '-': case '_': case ':': case '%': case '*':
        return 1;
    default:
        return 0;
    }
}

int fp_hostport_parse(const char *text, char *host, size_t hostcap,
                      uint16_t *port, char *err, size_t errcap)
{
    const char *host_start;
    size_t host_len;
    const char *port_start;
    size_t i;
    long port_value;

    if (err != NULL && errcap > 0) {
        err[0] = '\0';
    }
    if (text == NULL) {
        goto invalid;
    }
    if (strlen(text) >= FP_ENDPOINT_TEXT_MAX) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "address is too long");
        }
        return -1;
    }
    if (text[0] == '[') {
        const char *close = strchr(text, ']');
        if (close == NULL || close[1] != ':' || close[2] == '\0') {
            goto invalid;
        }
        host_start = text + 1;
        host_len = (size_t)(close - host_start);
        port_start = close + 2;
    } else {
        const char *colon = strrchr(text, ':');
        const char *scan;
        if (colon == NULL || colon == text || colon[1] == '\0') {
            goto invalid;
        }
        for (scan = text; scan < colon; scan++) {
            if (*scan == ':') {
                if (err != NULL && errcap > 0) {
                    snprintf(err, errcap,
                             "IPv6 addresses must be written as [addr]:port");
                }
                return -1;
            }
        }
        host_start = text;
        host_len = (size_t)(colon - text);
        port_start = colon + 1;
    }
    if (host_len == 0 || host_len > FP_MAX_HOST) {
        goto invalid;
    }
    for (i = 0; i < host_len; i++) {
        if (!host_char_ok((unsigned char)host_start[i])) {
            goto invalid;
        }
    }
    if (fp_parse_long(port_start, 1, 65535, &port_value) != 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "invalid port '%s' (expected 1..65535)",
                     port_start);
        }
        return -1;
    }
    if (host_len + 1 > hostcap) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "host name is too long");
        }
        return -1;
    }
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    *port = (uint16_t)port_value;
    return 0;

invalid:
    if (err != NULL && errcap > 0) {
        snprintf(err, errcap, "invalid address '%s' (expected host:port)",
                 text != NULL ? text : "(null)");
    }
    return -1;
}

int fp_hostport_format(const char *host, uint16_t port,
                       char *buf, size_t cap)
{
    int n;
    int needs_brackets;

    if (host == NULL || buf == NULL || cap == 0) {
        return -1;
    }
    needs_brackets = strchr(host, ':') != NULL;
    if (needs_brackets) {
        n = snprintf(buf, cap, "[%s]:%u", host, (unsigned)port);
    } else {
        n = snprintf(buf, cap, "%s:%u", host, (unsigned)port);
    }
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    return 0;
}

int fp_endpoint_is_wildcard(const char *host)
{
    if (host == NULL || host[0] == '\0' || strcmp(host, "*") == 0) {
        return 1;
    }
    if (strcmp(host, "0.0.0.0") == 0) {
        return 1;
    }
    if (strcmp(host, "::") == 0 ||
        strcmp(host, "0:0:0:0:0:0:0:0") == 0) {
        return 1;
    }
    return 0;
}

int fp_endpoint_resolve(const char *host, uint16_t port, int socktype,
                        fp_endpoint *out, char *err, size_t errcap)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    struct addrinfo *chosen = NULL;
    char portbuf[8];
    int rc;

    if (err != NULL && errcap > 0) {
        err[0] = '\0';
    }
    if (host == NULL || host[0] == '\0' || out == NULL) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "empty host");
        }
        return -1;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socktype;
    hints.ai_protocol = socktype == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP;
    snprintf(portbuf, sizeof(portbuf), "%u", (unsigned)port);
    rc = getaddrinfo(host, portbuf, &hints, &res);
    if (rc != 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "cannot resolve %s: %s", host, gai_text(rc));
        }
        return -1;
    }
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            chosen = ai;
            break;
        }
    }
    if (chosen == NULL) {
        for (ai = res; ai != NULL; ai = ai->ai_next) {
            if (ai->ai_family == AF_INET || ai->ai_family == AF_INET6) {
                chosen = ai;
                break;
            }
        }
    }
    if (chosen == NULL || chosen->ai_addrlen > sizeof(out->ss)) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "no usable address for %s", host);
        }
        freeaddrinfo(res);
        return -1;
    }
    memset(out, 0, sizeof(*out));
    memcpy(&out->ss, chosen->ai_addr, (size_t)chosen->ai_addrlen);
    out->len = (fp_socklen_t)chosen->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

int fp_endpoint_format(const fp_endpoint *ep, char *buf, size_t cap)
{
    char host[NI_MAXHOST];
    char serv[NI_MAXSERV];
    int rc;
    int n;

    if (ep == NULL || buf == NULL || cap == 0 || ep->len == 0) {
        return -1;
    }
    rc = getnameinfo((const struct sockaddr *)&ep->ss, ep->len,
                     host, (fp_socklen_t)sizeof(host),
                     serv, (fp_socklen_t)sizeof(serv),
                     NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc != 0) {
        snprintf(buf, cap, "?");
        return -1;
    }
    if (strchr(host, ':') != NULL) {
        n = snprintf(buf, cap, "[%s]:%s", host, serv);
    } else {
        n = snprintf(buf, cap, "%s:%s", host, serv);
    }
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    return 0;
}

int fp_endpoint_equal(const fp_endpoint *a, const fp_endpoint *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }
    if (a->ss.ss_family != b->ss.ss_family) {
        return 0;
    }
    if (a->ss.ss_family == AF_INET) {
        const struct sockaddr_in *sa = (const struct sockaddr_in *)&a->ss;
        const struct sockaddr_in *sb = (const struct sockaddr_in *)&b->ss;
        return sa->sin_port == sb->sin_port &&
               sa->sin_addr.s_addr == sb->sin_addr.s_addr;
    }
    if (a->ss.ss_family == AF_INET6) {
        const struct sockaddr_in6 *sa = (const struct sockaddr_in6 *)&a->ss;
        const struct sockaddr_in6 *sb = (const struct sockaddr_in6 *)&b->ss;
        return sa->sin6_port == sb->sin6_port &&
               sa->sin6_scope_id == sb->sin6_scope_id &&
               memcmp(&sa->sin6_addr, &sb->sin6_addr,
                      sizeof(sa->sin6_addr)) == 0;
    }
    return 0;
}

static int bind_udp_attempt(const char *host, uint16_t port, int reuseaddr,
                            fp_socket_t *fd_out, int *last_err,
                            char *err, size_t errcap)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    char portbuf[8];
    char endpoint[FP_ENDPOINT_TEXT_MAX];
    int rc;
    int wildcard = fp_endpoint_is_wildcard(host);
    const char *node = (!wildcard || (host != NULL && host[0] != '\0' &&
                                      strcmp(host, "*") != 0))
                           ? host
                           : NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_PASSIVE;
    snprintf(portbuf, sizeof(portbuf), "%u", (unsigned)port);
    if (fp_hostport_format(host != NULL ? host : "*", port,
                           endpoint, sizeof(endpoint)) != 0) {
        snprintf(endpoint, sizeof(endpoint), "%s:%u",
                 host != NULL ? host : "*", (unsigned)port);
    }
    rc = getaddrinfo(node, portbuf, &hints, &res);
    if (rc != 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "cannot resolve bind address %s: %s",
                     endpoint, gai_text(rc));
        }
        return -1;
    }
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fp_socket_t fd;
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) {
            continue;
        }
        fd = fp_socket_open(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == FP_SOCKET_INVALID) {
            *last_err = fp_net_last_error();
            continue;
        }
        if (reuseaddr) {
            (void)fp_net_set_reuseaddr(fd);
        }
        if (ai->ai_family == AF_INET6 && wildcard) {
            (void)fp_net_set_v6only(fd, 0);
        }
        if (bind(fd, ai->ai_addr, (fp_socklen_t)ai->ai_addrlen) == 0) {
            *fd_out = fd;
            freeaddrinfo(res);
            return 0;
        }
        *last_err = fp_net_last_error();
        fp_socket_close(fd);
    }
    freeaddrinfo(res);
    if (err != NULL && errcap > 0) {
        char text[256];
        fp_net_error_text(*last_err, text, sizeof(text));
        snprintf(err, errcap, "cannot bind %s: %s", endpoint, text);
    }
    return -1;
}

int fp_udp_bind(const char *host, uint16_t port, int reuseaddr,
                fp_socket_t *fd_out, char *err, size_t errcap)
{
    int last_err = 0;
    if (fd_out == NULL) {
        return -1;
    }
    *fd_out = FP_SOCKET_INVALID;
    return bind_udp_attempt(host, port, reuseaddr, fd_out, &last_err,
                            err, errcap);
}

int fp_tcp_listen(const char *host, uint16_t port, int backlog,
                  fp_socket_t *fd_out, char *err, size_t errcap)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    char portbuf[8];
    char endpoint[FP_ENDPOINT_TEXT_MAX];
    int rc;
    int last_err = 0;
    int wildcard = fp_endpoint_is_wildcard(host);
    const char *node = (!wildcard || (host != NULL && host[0] != '\0' &&
                                      strcmp(host, "*") != 0))
                           ? host
                           : NULL;

    if (fd_out == NULL) {
        return -1;
    }
    *fd_out = FP_SOCKET_INVALID;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    snprintf(portbuf, sizeof(portbuf), "%u", (unsigned)port);
    if (fp_hostport_format(host != NULL ? host : "*", port,
                           endpoint, sizeof(endpoint)) != 0) {
        snprintf(endpoint, sizeof(endpoint), "%s:%u",
                 host != NULL ? host : "*", (unsigned)port);
    }
    rc = getaddrinfo(node, portbuf, &hints, &res);
    if (rc != 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "cannot resolve listen address %s: %s",
                     endpoint, gai_text(rc));
        }
        return -1;
    }
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fp_socket_t fd;
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) {
            continue;
        }
        fd = fp_socket_open(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == FP_SOCKET_INVALID) {
            last_err = fp_net_last_error();
            continue;
        }
        (void)fp_net_set_reuseaddr(fd);
        if (ai->ai_family == AF_INET6 && wildcard) {
            (void)fp_net_set_v6only(fd, 0);
        }
        if (bind(fd, ai->ai_addr, (fp_socklen_t)ai->ai_addrlen) == 0 &&
            listen(fd, backlog) == 0) {
            *fd_out = fd;
            freeaddrinfo(res);
            return 0;
        }
        last_err = fp_net_last_error();
        fp_socket_close(fd);
    }
    freeaddrinfo(res);
    if (err != NULL && errcap > 0) {
        char text[256];
        fp_net_error_text(last_err, text, sizeof(text));
        snprintf(err, errcap, "cannot listen on %s: %s", endpoint, text);
    }
    return -1;
}

int fp_tcp_connect(const char *host, uint16_t port, int timeout_ms,
                   fp_socket_t *fd_out, fp_endpoint *peer,
                   char *err, size_t errcap)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *ai;
    char portbuf[8];
    char endpoint[FP_ENDPOINT_TEXT_MAX];
    int rc;
    int last_err = 0;
    int timed_out = 0;
    int64_t deadline;

    if (fd_out == NULL) {
        return -1;
    }
    *fd_out = FP_SOCKET_INVALID;
    if (err != NULL && errcap > 0) {
        err[0] = '\0';
    }
    if (timeout_ms <= 0) {
        timeout_ms = 10000;
    }
    deadline = fp_now_ms() + timeout_ms;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    snprintf(portbuf, sizeof(portbuf), "%u", (unsigned)port);
    if (fp_hostport_format(host, port, endpoint, sizeof(endpoint)) != 0) {
        snprintf(endpoint, sizeof(endpoint), "%s:%u", host, (unsigned)port);
    }
    rc = getaddrinfo(host, portbuf, &hints, &res);
    if (rc != 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "cannot resolve %s: %s", endpoint,
                     gai_text(rc));
        }
        return -1;
    }
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fp_socket_t fd;
        int crc;

        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) {
            continue;
        }
        fd = fp_socket_open(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == FP_SOCKET_INVALID) {
            last_err = fp_net_last_error();
            continue;
        }
        crc = fp_net_connect(fd, ai->ai_addr, (fp_socklen_t)ai->ai_addrlen);
        if (crc == FP_IO_AGAIN) {
            for (;;) {
                struct fp_pollfd pfd;
                int64_t remain = deadline - fp_now_ms();
                int prc;
                if (remain <= 0) {
                    timed_out = 1;
                    crc = FP_IO_ERROR;
                    break;
                }
                pfd.fd = fd;
                pfd.events = FP_POLLOUT;
                pfd.revents = 0;
                prc = fp_poll(&pfd, 1, (int)remain);
                if (prc == FP_IO_AGAIN) {
                    continue;
                }
                if (prc <= 0) {
                    crc = FP_IO_ERROR;
                    break;
                }
                {
                    int so_err = fp_net_so_error(fd);
                    if (so_err != 0) {
                        last_err = so_err > 0 ? so_err : fp_net_last_error();
                        crc = FP_IO_ERROR;
                    } else {
                        crc = 0;
                    }
                }
                break;
            }
        } else if (crc == FP_IO_ERROR) {
            last_err = fp_net_last_error();
        }
        if (crc == 0) {
            (void)fp_net_set_nodelay(fd);
            if (peer != NULL) {
                memset(peer, 0, sizeof(*peer));
                if (ai->ai_addrlen <= sizeof(peer->ss)) {
                    memcpy(&peer->ss, ai->ai_addr, (size_t)ai->ai_addrlen);
                    peer->len = (fp_socklen_t)ai->ai_addrlen;
                }
            }
            *fd_out = fd;
            freeaddrinfo(res);
            return 0;
        }
        fp_socket_close(fd);
        if (timed_out) {
            break;
        }
    }
    freeaddrinfo(res);
    if (err != NULL && errcap > 0) {
        if (timed_out) {
            snprintf(err, errcap, "connect to %s timed out after %d ms",
                     endpoint, timeout_ms);
        } else {
            char text[256];
            fp_net_error_text(last_err, text, sizeof(text));
            snprintf(err, errcap, "cannot connect to %s: %s", endpoint, text);
        }
    }
    return -1;
}
