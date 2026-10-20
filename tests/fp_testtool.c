/*
 * Small test peer for the FluxPort test suite.  It intentionally uses the
 * public wire helpers so malformed and normal peers can be scripted without
 * pulling in Python or netcat.
 */
#include "addr.h"
#include "frame.h"
#include "net.h"
#include "protocol.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOOL_TIMEOUT_MS 5000

static int parse_port(const char *text, uint16_t *port)
{
    long value;

    if (fp_parse_long(text, 1, 65535, &value) != 0) {
        return -1;
    }
    *port = (uint16_t)value;
    return 0;
}

static fp_socket_t connect_tcp(const char *host, uint16_t port)
{
    fp_socket_t fd = FP_SOCKET_INVALID;
    fp_endpoint peer;
    char err[192];

    if (fp_tcp_connect(host, port, TOOL_TIMEOUT_MS,
                       &fd, &peer, err, sizeof(err)) != 0) {
        fprintf(stderr, "testtool: connect: %s\n", err);
        return FP_SOCKET_INVALID;
    }
    return fd;
}

static int send_all(fp_socket_t fd, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t off = 0;
    int64_t deadline = fp_now_ms() + TOOL_TIMEOUT_MS;

    while (off < len) {
        int rc = fp_tcp_send(fd, p + off, len - off);
        if (rc > 0) {
            off += (size_t)rc;
            continue;
        }
        if (rc == FP_IO_AGAIN) {
            if (fp_now_ms() > deadline) {
                return -1;
            }
            fp_sleep_ms(3);
            continue;
        }
        return -1;
    }
    return 0;
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static int send_frame(fp_socket_t fd, uint8_t type, uint32_t stream_id,
                      const void *payload, size_t len)
{
    uint8_t header[9];

    if (len > FP_MAX_PAYLOAD) {
        return -1;
    }
    put_u32(header, (uint32_t)len);
    header[4] = type;
    put_u32(header + 5, stream_id);
    if (send_all(fd, header, sizeof(header)) != 0) {
        return -1;
    }
    if (len > 0 && send_all(fd, payload, len) != 0) {
        return -1;
    }
    return 0;
}

static int send_ack(fp_socket_t fd, uint16_t version, uint8_t status,
                    const char *msg)
{
    fp_buf payload;
    size_t msg_len = msg != NULL ? strlen(msg) : 0;
    int rc;

    if (msg_len > FP_MAX_MSG) {
        msg_len = FP_MAX_MSG;
    }
    fp_buf_init(&payload);
    rc = fp_buf_append_u16(&payload, version);
    if (rc == 0) {
        rc = fp_buf_append_u8(&payload, status);
    }
    if (rc == 0) {
        rc = fp_buf_append(&payload, msg, msg_len);
    }
    if (rc == 0) {
        rc = send_frame(fd, FP_FRAME_HELLO_ACK, 0,
                        payload.data, payload.len);
    } else {
        rc = -1;
    }
    fp_buf_free(&payload);
    return rc;
}

static int recv_frame(fp_socket_t fd, fp_frame_parser *parser,
                      fp_frame *frame, int timeout_ms)
{
    uint8_t buf[8192];
    int64_t deadline = fp_now_ms() + timeout_ms;

    for (;;) {
        int parsed = fp_parser_next(parser, frame);
        if (parsed == 1) {
            return 1;
        }
        if (parsed < 0) {
            return -1;
        }
        if (fp_now_ms() >= deadline) {
            return 0;
        }
        {
            int n = fp_tcp_recv(fd, buf, sizeof(buf));
            if (n > 0) {
                if (fp_parser_push(parser, buf, (size_t)n) != 0) {
                    return -1;
                }
                continue;
            }
            if (n == 0) {
                return -2;
            }
            if (n == FP_IO_AGAIN) {
                fp_sleep_ms(5);
                continue;
            }
            return -1;
        }
    }
}

static fp_socket_t accept_one(fp_socket_t listen_fd, int timeout_ms)
{
    int64_t deadline = fp_now_ms() + timeout_ms;

    for (;;) {
        struct sockaddr_storage ss;
        fp_socklen_t len = (fp_socklen_t)sizeof(ss);
        fp_socket_t fd;

        memset(&ss, 0, sizeof(ss));
        fd = accept(listen_fd, (struct sockaddr *)&ss, &len);
        if (fd != FP_SOCKET_INVALID) {
            return fd;
        }
        if (fp_now_ms() >= deadline) {
            return FP_SOCKET_INVALID;
        }
        fp_sleep_ms(5);
    }
}

static int cmd_echo(const char *host, uint16_t port, const char *prefix)
{
    fp_socket_t fd = FP_SOCKET_INVALID;
    char err[192];
    uint8_t *buf;
    size_t prefix_len = prefix != NULL ? strlen(prefix) : 0;

    if (fp_net_init() != 0) {
        fprintf(stderr, "testtool: cannot initialize network\n");
        return 1;
    }
    if (fp_udp_bind(host, port, 1, &fd, err, sizeof(err)) != 0) {
        fprintf(stderr, "testtool: echo bind: %s\n", err);
        fp_net_cleanup();
        return 1;
    }
    if (fp_install_signal_handlers() != 0) {
        fp_socket_close(fd);
        fp_net_cleanup();
        return 1;
    }
    buf = (uint8_t *)malloc(65536 + prefix_len);
    if (buf == NULL) {
        fp_socket_close(fd);
        fp_net_cleanup();
        return 1;
    }
    printf("READY\n");
    fflush(stdout);
    while (!fp_stop_requested()) {
        struct sockaddr_storage from;
        fp_socklen_t fromlen = (fp_socklen_t)sizeof(from);
        int n = fp_udp_recvfrom(fd, buf + prefix_len, 65536,
                                &from, &fromlen);

        if (n < 0) {
            if (n == FP_IO_AGAIN) {
                fp_sleep_ms(2);
                continue;
            }
            break;
        }
        memcpy(buf, prefix != NULL ? prefix : "", prefix_len);
        (void)fp_udp_sendto(fd, buf, prefix_len + (size_t)n,
                            (const struct sockaddr *)&from, fromlen);
    }
    free(buf);
    fp_socket_close(fd);
    fp_net_cleanup();
    return 0;
}

static int cmd_probe(const char *host, uint16_t port, const char *message,
                     int timeout_ms)
{
    fp_endpoint target;
    fp_socket_t fd = FP_SOCKET_INVALID;
    char err[192];
    uint8_t buf[65536];
    int rc;
    int64_t deadline;

    if (fp_net_init() != 0) {
        fprintf(stderr, "testtool: cannot initialize network\n");
        return 1;
    }
    if (fp_endpoint_resolve(host, port, SOCK_DGRAM,
                            &target, err, sizeof(err)) != 0) {
        fprintf(stderr, "testtool: resolve: %s\n", err);
        fp_net_cleanup();
        return 1;
    }
    fd = fp_socket_open(target.ss.ss_family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == FP_SOCKET_INVALID) {
        fprintf(stderr, "testtool: cannot create UDP socket\n");
        fp_net_cleanup();
        return 1;
    }
    rc = fp_udp_sendto(fd, message, strlen(message),
                       (const struct sockaddr *)&target.ss, target.len);
    if (rc < 0) {
        fprintf(stderr, "testtool: send failed\n");
        fp_socket_close(fd);
        fp_net_cleanup();
        return 1;
    }
    deadline = fp_now_ms() + timeout_ms;
    while (fp_now_ms() < deadline) {
        struct fp_pollfd pfd;
        struct sockaddr_storage from;
        fp_socklen_t fromlen = (fp_socklen_t)sizeof(from);

        pfd.fd = fd;
        pfd.events = FP_POLLIN;
        pfd.revents = 0;
        rc = fp_poll(&pfd, 1, 100);
        if (rc > 0 && (pfd.revents & FP_POLLIN) != 0) {
            int n = fp_udp_recvfrom(fd, buf, sizeof(buf), &from, &fromlen);
            if (n >= 0) {
                fwrite(buf, 1, (size_t)n, stdout);
                fputc('\n', stdout);
                fflush(stdout);
                fp_socket_close(fd);
                fp_net_cleanup();
                return 0;
            }
        }
    }
    fprintf(stderr, "testtool: probe timed out\n");
    fp_socket_close(fd);
    fp_net_cleanup();
    return 1;
}

static int fake_handshake_result(fp_socket_t fd, fp_frame_parser *parser,
                                 uint8_t status)
{
    fp_frame frame;
    fp_tunnel_spec *specs;
    fp_tunnel_result *results;
    size_t count = 0;
    size_t i;
    char err[192];
    fp_buf payload;
    int rc;

    rc = recv_frame(fd, parser, &frame, TOOL_TIMEOUT_MS);
    if (rc != 1 || frame.type != FP_FRAME_TUNNEL_ADD) {
        return -1;
    }
    specs = (fp_tunnel_spec *)calloc(FP_MAX_TUNNELS, sizeof(*specs));
    if (specs == NULL) {
        return -1;
    }
    if (fp_decode_tunnel_add(frame.payload, frame.len, specs,
                             FP_MAX_TUNNELS, &count,
                             err, sizeof(err)) != 0) {
        free(specs);
        return -1;
    }
    free(specs);
    results = (fp_tunnel_result *)calloc(count > 0 ? count : 1,
                                         sizeof(*results));
    if (results == NULL) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        results[i].index = (uint16_t)i;
        results[i].status = status;
        snprintf(results[i].msg, sizeof(results[i].msg), "%s",
                 status == FP_STATUS_OK ? "ok" : "denied");
    }
    fp_buf_init(&payload);
    rc = fp_build_tunnel_result(&payload, results, count);
    free(results);
    if (rc == 0) {
        rc = send_frame(fd, FP_FRAME_TUNNEL_RESULT, 0,
                        payload.data, payload.len);
    }
    fp_buf_free(&payload);
    return rc;
}

static int cmd_fake_server(uint16_t port, const char *mode)
{
    fp_socket_t listen_fd = FP_SOCKET_INVALID;
    char err[192];
    fp_socket_t cfd;
    fp_frame_parser parser;
    fp_frame frame;
    int rc = 1;

    if (fp_net_init() != 0) {
        fprintf(stderr, "testtool: cannot initialize network\n");
        return 1;
    }
    if (fp_tcp_listen("127.0.0.1", port, 4,
                      &listen_fd, err, sizeof(err)) != 0) {
        fprintf(stderr, "testtool: fake server listen: %s\n", err);
        fp_net_cleanup();
        return 1;
    }
    printf("READY\n");
    fflush(stdout);
    cfd = accept_one(listen_fd, 10000);
    fp_socket_close(listen_fd);
    if (cfd == FP_SOCKET_INVALID) {
        fprintf(stderr, "testtool: fake server accept timed out\n");
        fp_net_cleanup();
        return 1;
    }
    if (strcmp(mode, "bad-len") == 0) {
        uint8_t bad[9];
        put_u32(bad, 100000);
        bad[4] = FP_FRAME_HELLO;
        put_u32(bad + 5, 0);
        (void)send_all((fp_socket_t)cfd, bad, sizeof(bad));
        fp_sleep_ms(100);
        fp_socket_close((fp_socket_t)cfd);
        fp_net_cleanup();
        return 0;
    }
    if (strcmp(mode, "half") == 0) {
        uint8_t bad[12];
        put_u32(bad, 64);
        bad[4] = FP_FRAME_HELLO;
        put_u32(bad + 5, 0);
        memset(bad + 9, 'x', 3);
        (void)send_all((fp_socket_t)cfd, bad, sizeof(bad));
        fp_sleep_ms(100);
        fp_socket_close((fp_socket_t)cfd);
        fp_net_cleanup();
        return 0;
    }
    fp_parser_init(&parser);
    rc = recv_frame((fp_socket_t)cfd, &parser, &frame, TOOL_TIMEOUT_MS);
    if (rc != 1 || frame.type != FP_FRAME_HELLO) {
        fprintf(stderr, "testtool: fake server expected HELLO\n");
        goto done;
    }
    if (strcmp(mode, "bad-token") == 0) {
        (void)send_ack((fp_socket_t)cfd, FP_PROTO_VERSION,
                       FP_STATUS_BAD_TOKEN, "bad token");
        fp_sleep_ms(200);
        rc = 0;
        goto done;
    }
    if (strcmp(mode, "bad-version") == 0) {
        (void)send_ack((fp_socket_t)cfd, (uint16_t)(FP_PROTO_VERSION + 1),
                       FP_STATUS_OK, "wrong version");
        fp_sleep_ms(200);
        rc = 0;
        goto done;
    }
    if (send_ack((fp_socket_t)cfd, FP_PROTO_VERSION,
                 FP_STATUS_OK, "welcome") != 0) {
        goto done;
    }
    {
        uint8_t result_status = strcmp(mode, "partial-goaway") == 0 ?
                                FP_STATUS_PORT_DENIED : FP_STATUS_OK;
        if (fake_handshake_result((fp_socket_t)cfd, &parser,
                                  result_status) != 0) {
            fprintf(stderr, "testtool: fake server tunnel setup failed\n");
            goto done;
        }
    }
    if (strcmp(mode, "goaway") == 0 ||
        strcmp(mode, "partial-goaway") == 0) {
        fp_buf goodbye;
        fp_buf_init(&goodbye);
        if (fp_build_goaway(&goodbye, "test done") == 0) {
            (void)send_frame((fp_socket_t)cfd, FP_FRAME_GOAWAY, 0,
                             goodbye.data, goodbye.len);
        }
        fp_buf_free(&goodbye);
        fp_sleep_ms(300);
        rc = 0;
        goto done;
    }
    if (strcmp(mode, "silent") == 0) {
        int64_t deadline = fp_now_ms() + 6000;
        while (fp_now_ms() < deadline) {
            int got = recv_frame((fp_socket_t)cfd, &parser, &frame, 1000);
            if (got == -2) {
                rc = 0;
                break;
            }
            if (got < 0) {
                break;
            }
            /* Ignore PING and any other frame: the client must give up. */
        }
        goto done;
    }
    fprintf(stderr, "testtool: unknown fake server mode '%s'\n", mode);

done:
    fp_parser_free(&parser);
    fp_socket_close((fp_socket_t)cfd);
    fp_net_cleanup();
    return rc;
}

static int cmd_malicious(const char *host, uint16_t port, const char *mode)
{
    fp_socket_t cfd;
    fp_frame_parser parser;
    fp_frame frame;
    int rc = 1;

    if (fp_net_init() != 0) {
        fprintf(stderr, "testtool: cannot initialize network\n");
        return 1;
    }
    cfd = connect_tcp(host, port);
    if (cfd == FP_SOCKET_INVALID) {
        fp_net_cleanup();
        return 1;
    }
    if (strcmp(mode, "huge") == 0 || strcmp(mode, "oversized") == 0) {
        uint8_t bad[9];
        put_u32(bad, 100000);
        bad[4] = FP_FRAME_HELLO;
        put_u32(bad + 5, 0);
        (void)send_all((fp_socket_t)cfd, bad, sizeof(bad));
        fp_sleep_ms(200);
        fp_socket_close((fp_socket_t)cfd);
        fp_net_cleanup();
        return 0;
    }
    if (strcmp(mode, "half") == 0) {
        uint8_t bad[12];
        put_u32(bad, 64);
        bad[4] = FP_FRAME_HELLO;
        put_u32(bad + 5, 0);
        memset(bad + 9, 'x', 3);
        (void)send_all((fp_socket_t)cfd, bad, sizeof(bad));
        fp_sleep_ms(100);
        fp_socket_close((fp_socket_t)cfd);
        fp_net_cleanup();
        return 0;
    }
    if (strcmp(mode, "silent-client") != 0) {
        fprintf(stderr, "testtool: unknown malicious mode '%s'\n", mode);
        fp_socket_close((fp_socket_t)cfd);
        fp_net_cleanup();
        return 1;
    }
    fp_parser_init(&parser);
    {
        fp_buf hello;
        fp_buf_init(&hello);
        if (fp_build_hello(&hello, "", "silent-test") != 0 ||
            send_frame((fp_socket_t)cfd, FP_FRAME_HELLO, 0,
                       hello.data, hello.len) != 0) {
            fp_buf_free(&hello);
            goto done;
        }
        fp_buf_free(&hello);
    }
    rc = recv_frame((fp_socket_t)cfd, &parser, &frame, TOOL_TIMEOUT_MS);
    if (rc != 1 || frame.type != FP_FRAME_HELLO_ACK) {
        fprintf(stderr, "testtool: silent client expected HELLO_ACK\n");
        goto done;
    }
    {
        fp_buf add;
        fp_buf_init(&add);
        if (fp_build_tunnel_add(&add, NULL, 0) != 0 ||
            send_frame((fp_socket_t)cfd, FP_FRAME_TUNNEL_ADD, 0,
                       add.data, add.len) != 0) {
            fp_buf_free(&add);
            goto done;
        }
        fp_buf_free(&add);
    }
    rc = recv_frame((fp_socket_t)cfd, &parser, &frame, TOOL_TIMEOUT_MS);
    if (rc != 1 || frame.type != FP_FRAME_TUNNEL_RESULT) {
        fprintf(stderr, "testtool: silent client expected TUNNEL_RESULT\n");
        goto done;
    }
    for (;;) {
        int got = recv_frame((fp_socket_t)cfd, &parser, &frame, 6000);
        if (got == -2) {
            rc = 0;
            break;
        }
        if (got <= 0) {
            break;
        }
        /* Deliberately never send PONG. */
    }

done:
    fp_parser_free(&parser);
    fp_socket_close((fp_socket_t)cfd);
    fp_net_cleanup();
    return rc;
}

static void usage(void)
{
    fprintf(stderr,
        "Usage:\n"
        "  fp_testtool echo <host> <port> [prefix]\n"
        "  fp_testtool probe <host> <port> <message> [timeout_ms]\n"
        "  fp_testtool fake-server <port> <mode>\n"
        "  fp_testtool malicious <host> <port> <mode>\n"
        "Modes: bad-token, bad-version, bad-len, half, goaway, silent,\n"
        "       partial-goaway\n"
        "       malicious: huge, oversized, half, silent-client\n");
}

int main(int argc, char **argv)
{
    uint16_t port;

    if (argc < 2) {
        usage();
        return 1;
    }
    if (strcmp(argv[1], "echo") == 0) {
        if (argc < 4 || parse_port(argv[3], &port) != 0) {
            usage();
            return 1;
        }
        return cmd_echo(argv[2], port, argc > 4 ? argv[4] : "echo:");
    }
    if (strcmp(argv[1], "probe") == 0) {
        int timeout_ms = 2000;
        if (argc < 5 || parse_port(argv[3], &port) != 0) {
            usage();
            return 1;
        }
        if (argc > 5) {
            long value;
            if (fp_parse_long(argv[5], 100, 60000, &value) != 0) {
                usage();
                return 1;
            }
            timeout_ms = (int)value;
        }
        return cmd_probe(argv[2], port, argv[4], timeout_ms);
    }
    if (strcmp(argv[1], "fake-server") == 0) {
        if (argc < 4 || parse_port(argv[2], &port) != 0) {
            usage();
            return 1;
        }
        return cmd_fake_server(port, argv[3]);
    }
    if (strcmp(argv[1], "malicious") == 0) {
        if (argc < 5 || parse_port(argv[3], &port) != 0) {
            usage();
            return 1;
        }
        return cmd_malicious(argv[2], port, argv[4]);
    }
    usage();
    return 1;
}
