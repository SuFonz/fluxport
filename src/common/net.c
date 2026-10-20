#include "net.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

#ifdef _WIN32
static int g_wsa_ready = 0;
#endif

int fp_net_init(void)
{
#ifdef _WIN32
    WSADATA wsa;
    int rc;
    if (g_wsa_ready) {
        return 0;
    }
    rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        return -1;
    }
    g_wsa_ready = 1;
    return 0;
#else
    signal(SIGPIPE, SIG_IGN);
    return 0;
#endif
}

void fp_net_cleanup(void)
{
#ifdef _WIN32
    if (g_wsa_ready) {
        WSACleanup();
        g_wsa_ready = 0;
    }
#endif
}

int fp_net_last_error(void)
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

void fp_net_error_text(int err, char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return;
    }
#ifdef _WIN32
    {
        DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
        DWORD len = FormatMessageA(flags, NULL, (DWORD)err, 0, buf, (DWORD)cap, NULL);
        size_t i;
        if (len == 0) {
            snprintf(buf, cap, "winsock error %d", err);
            return;
        }
        for (i = 0; i < len && buf[i] != '\0'; i++) {
            if (buf[i] == '\r' || buf[i] == '\n') {
                buf[i] = '\0';
                break;
            }
        }
    }
#else
    const char *text = strerror(err);
    snprintf(buf, cap, "%s", text != NULL ? text : "unknown error");
#endif
}

int fp_set_nonblocking(fp_socket_t fd)
{
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
    return 0;
#endif
}

int fp_net_set_reuseaddr(fp_socket_t fd)
{
    int on = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                      (const char *)&on, (fp_socklen_t)sizeof(on)) == 0 ? 0 : -1;
}

int fp_net_set_nodelay(fp_socket_t fd)
{
    int on = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                      (const char *)&on, (fp_socklen_t)sizeof(on)) == 0 ? 0 : -1;
}

int fp_net_set_v6only(fp_socket_t fd, int on)
{
    return setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
                      (const char *)&on, (fp_socklen_t)sizeof(on)) == 0 ? 0 : -1;
}

fp_socket_t fp_socket_open(int family, int type, int protocol)
{
    fp_socket_t fd;
#ifdef _WIN32
    fd = socket(family, type, protocol);
    if (fd == INVALID_SOCKET) {
        return FP_SOCKET_INVALID;
    }
#else
    fd = socket(family, type, protocol);
    if (fd < 0) {
        return FP_SOCKET_INVALID;
    }
#endif
    if (fp_set_nonblocking(fd) != 0) {
        fp_socket_close(fd);
        return FP_SOCKET_INVALID;
    }
    return fd;
}

void fp_socket_close(fp_socket_t fd)
{
    if (fd == FP_SOCKET_INVALID) {
        return;
    }
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

int fp_net_connect(fp_socket_t fd, const struct sockaddr *addr,
                   fp_socklen_t len)
{
    int rc = connect(fd, addr, len);
    if (rc == 0) {
        return 0;
    }
#ifdef _WIN32
    {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS ||
            err == WSAEINTR) {
            return FP_IO_AGAIN;
        }
        return FP_IO_ERROR;
    }
#else
    if (errno == EINPROGRESS || errno == EINTR) {
        return FP_IO_AGAIN;
    }
    return FP_IO_ERROR;
#endif
}

int fp_net_so_error(fp_socket_t fd)
{
    int err = 0;
    fp_socklen_t len = (fp_socklen_t)sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0) {
        return -1;
    }
    return err;
}

int fp_tcp_recv(fp_socket_t fd, void *buf, size_t len)
{
    int rc;
    if (len > (size_t)INT_MAX) {
        len = (size_t)INT_MAX;
    }
    rc = (int)recv(fd, (char *)buf, (int)len, 0);
    if (rc > 0) {
        return rc;
    }
    if (rc == 0) {
        return 0;
    }
#ifdef _WIN32
    {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAEINTR) {
            return FP_IO_AGAIN;
        }
    }
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return FP_IO_AGAIN;
    }
#endif
    return FP_IO_ERROR;
}

int fp_tcp_send(fp_socket_t fd, const void *buf, size_t len)
{
    int rc;
    int flags = 0;
    if (len > (size_t)INT_MAX) {
        len = (size_t)INT_MAX;
    }
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    rc = (int)send(fd, (const char *)buf, (int)len, flags);
    if (rc >= 0) {
        return rc;
    }
#ifdef _WIN32
    {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAEINTR) {
            return FP_IO_AGAIN;
        }
    }
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return FP_IO_AGAIN;
    }
#endif
    return FP_IO_ERROR;
}

int fp_udp_recvfrom(fp_socket_t fd, void *buf, size_t len,
                    struct sockaddr_storage *from, fp_socklen_t *fromlen)
{
    int rc;
    if (len > (size_t)INT_MAX) {
        len = (size_t)INT_MAX;
    }
    rc = (int)recvfrom(fd, (char *)buf, (int)len, 0,
                       (struct sockaddr *)from, fromlen);
    if (rc >= 0) {
        return rc;
    }
#ifdef _WIN32
    {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAEINTR) {
            return FP_IO_AGAIN;
        }
    }
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return FP_IO_AGAIN;
    }
#endif
    return FP_IO_ERROR;
}

int fp_udp_sendto(fp_socket_t fd, const void *buf, size_t len,
                  const struct sockaddr *to, fp_socklen_t tolen)
{
    int rc;
    if (len > (size_t)INT_MAX) {
        len = (size_t)INT_MAX;
    }
    rc = (int)sendto(fd, (const char *)buf, (int)len, 0, to, tolen);
    if (rc >= 0) {
        return rc;
    }
#ifdef _WIN32
    {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAEINTR || err == WSAENOBUFS) {
            return FP_IO_AGAIN;
        }
    }
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
        errno == ENOBUFS) {
        return FP_IO_AGAIN;
    }
#endif
    return FP_IO_ERROR;
}

#ifdef _WIN32
static short win_from_poll(short native)
{
    short out = 0;
    if (native & POLLRDNORM) {
        out |= FP_POLLIN;
    }
    if (native & POLLWRNORM) {
        out |= FP_POLLOUT;
    }
    if (native & POLLERR) {
        out |= FP_POLLERR;
    }
    if (native & POLLHUP) {
        out |= FP_POLLHUP;
    }
    if (native & POLLNVAL) {
        out |= FP_POLLNVAL;
    }
    return out;
}

static short win_to_poll(short events)
{
    short out = 0;
    if (events & FP_POLLIN) {
        out |= POLLRDNORM;
    }
    if (events & FP_POLLOUT) {
        out |= POLLWRNORM;
    }
    return out;
}
#endif

int fp_poll(struct fp_pollfd *fds, size_t nfds, int timeout_ms)
{
#ifdef _WIN32
    WSAPOLLFD *native;
    size_t i;
    int rc;

    if (nfds > 0) {
        native = (WSAPOLLFD *)calloc(nfds, sizeof(*native));
        if (native == NULL) {
            return FP_IO_ERROR;
        }
        for (i = 0; i < nfds; i++) {
            native[i].fd = fds[i].fd;
            native[i].events = win_to_poll(fds[i].events);
        }
    } else {
        native = NULL;
    }
    rc = WSAPoll(native, (ULONG)nfds, timeout_ms);
    if (rc > 0) {
        for (i = 0; i < nfds; i++) {
            fds[i].revents = win_from_poll(native[i].revents);
        }
    } else if (rc == SOCKET_ERROR) {
        int err = WSAGetLastError();
        free(native);
        if (err == WSAEINTR) {
            return FP_IO_AGAIN;
        }
        return FP_IO_ERROR;
    }
    free(native);
    return rc;
#else
    struct pollfd *native;
    size_t i;
    int rc;

    if (nfds > 0) {
        native = (struct pollfd *)calloc(nfds, sizeof(*native));
        if (native == NULL) {
            return FP_IO_ERROR;
        }
        for (i = 0; i < nfds; i++) {
            native[i].fd = fds[i].fd;
            if (fds[i].events & FP_POLLIN) {
                native[i].events |= POLLIN;
            }
            if (fds[i].events & FP_POLLOUT) {
                native[i].events |= POLLOUT;
            }
        }
    } else {
        native = NULL;
    }
    rc = poll(native, (nfds_t)nfds, timeout_ms);
    if (rc > 0) {
        for (i = 0; i < nfds; i++) {
            short rev = native[i].revents;
            short out = 0;
            if (rev & POLLIN) {
                out |= FP_POLLIN;
            }
            if (rev & POLLOUT) {
                out |= FP_POLLOUT;
            }
            if (rev & POLLERR) {
                out |= FP_POLLERR;
            }
            if (rev & POLLHUP) {
                out |= FP_POLLHUP;
            }
            if (rev & POLLNVAL) {
                out |= FP_POLLNVAL;
            }
            fds[i].revents = out;
        }
    } else if (rc < 0) {
        int err = errno;
        free(native);
        if (err == EINTR) {
            return FP_IO_AGAIN;
        }
        return FP_IO_ERROR;
    }
    free(native);
    return rc;
#endif
}
