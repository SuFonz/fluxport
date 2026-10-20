/*
 * Address parsing, formatting and listener/connector helpers.
 * Host/port text uses "host:port" and "[ipv6]:port" forms.
 */
#ifndef FP_ADDR_H
#define FP_ADDR_H

#include "net.h"

#include <stddef.h>
#include <stdint.h>

#define FP_ENDPOINT_TEXT_MAX 512

typedef struct fp_endpoint {
    struct sockaddr_storage ss;
    fp_socklen_t len;
} fp_endpoint;

/* Parse "host:port" / "[ipv6]:port" without resolving.  Returns 0 or -1. */
int fp_hostport_parse(const char *text, char *host, size_t hostcap,
                      uint16_t *port, char *err, size_t errcap);

/* Format host:port, adding brackets for IPv6 literals. */
int fp_hostport_format(const char *host, uint16_t port,
                       char *buf, size_t cap);

/* Resolve a target address (prefers IPv4 when both families exist). */
int fp_endpoint_resolve(const char *host, uint16_t port, int socktype,
                        fp_endpoint *out, char *err, size_t errcap);

/* Numeric "ip:port" formatting. */
int fp_endpoint_format(const fp_endpoint *ep, char *buf, size_t cap);

int fp_endpoint_equal(const fp_endpoint *a, const fp_endpoint *b);

/* True for "", "*", "0.0.0.0", "::" and their expanded equivalents. */
int fp_endpoint_is_wildcard(const char *host);

/* Create a bound, non-blocking UDP socket. */
int fp_udp_bind(const char *host, uint16_t port, int reuseaddr,
                fp_socket_t *fd_out, char *err, size_t errcap);

/* Create a bound, listening, non-blocking TCP socket. */
int fp_tcp_listen(const char *host, uint16_t port, int backlog,
                  fp_socket_t *fd_out, char *err, size_t errcap);

/* Non-blocking connect with an overall timeout. */
int fp_tcp_connect(const char *host, uint16_t port, int timeout_ms,
                   fp_socket_t *fd_out, fp_endpoint *peer,
                   char *err, size_t errcap);

#endif /* FP_ADDR_H */
