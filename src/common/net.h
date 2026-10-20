/*
 * Thin portability layer over BSD/Winsock sockets and poll(2)/WSAPoll.
 * Every socket created through this module is non-blocking.
 */
#ifndef FP_NET_H
#define FP_NET_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET fp_socket_t;
typedef int fp_socklen_t;
#define FP_SOCKET_INVALID INVALID_SOCKET
#else
#include <sys/socket.h>
#include <sys/types.h>
typedef int fp_socket_t;
typedef socklen_t fp_socklen_t;
#define FP_SOCKET_INVALID (-1)
#endif

/* I/O result codes shared by all wrappers. */
#define FP_IO_AGAIN (-2)
#define FP_IO_ERROR (-1)

/* poll event bits (independent of the platform's constants). */
#define FP_POLLIN 0x0001
#define FP_POLLOUT 0x0002
#define FP_POLLERR 0x0004
#define FP_POLLHUP 0x0008
#define FP_POLLNVAL 0x0010

struct fp_pollfd {
    fp_socket_t fd;
    short events;
    short revents;
};

/* Must be called once before any other network helper. */
int fp_net_init(void);
void fp_net_cleanup(void);

int fp_set_nonblocking(fp_socket_t fd);
int fp_net_set_reuseaddr(fp_socket_t fd);
int fp_net_set_nodelay(fp_socket_t fd);
int fp_net_set_v6only(fp_socket_t fd, int on);

/* Create a non-blocking socket, or FP_SOCKET_INVALID. */
fp_socket_t fp_socket_open(int family, int type, int protocol);
void fp_socket_close(fp_socket_t fd);

int fp_net_last_error(void);
/* Human readable text for a native socket error code. */
void fp_net_error_text(int err, char *buf, size_t cap);

/*
 * Start a non-blocking connect.  Returns 0 (connected), FP_IO_AGAIN
 * (in progress) or FP_IO_ERROR.
 */
int fp_net_connect(fp_socket_t fd, const struct sockaddr *addr,
                   fp_socklen_t len);
/* Pending SO_ERROR value, or -1 if getsockopt failed. */
int fp_net_so_error(fp_socket_t fd);

int fp_tcp_recv(fp_socket_t fd, void *buf, size_t len);
int fp_tcp_send(fp_socket_t fd, const void *buf, size_t len);
int fp_udp_recvfrom(fp_socket_t fd, void *buf, size_t len,
                    struct sockaddr_storage *from, fp_socklen_t *fromlen);
int fp_udp_sendto(fp_socket_t fd, const void *buf, size_t len,
                  const struct sockaddr *to, fp_socklen_t tolen);

/*
 * poll wrapper.  Returns the number of ready descriptors, 0 on timeout,
 * FP_IO_AGAIN when interrupted, FP_IO_ERROR on failure.
 */
int fp_poll(struct fp_pollfd *fds, size_t nfds, int timeout_ms);

#endif /* FP_NET_H */
