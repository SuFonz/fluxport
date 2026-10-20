#ifndef FP_SERVER_INTERNAL_H
#define FP_SERVER_INTERNAL_H

#include "addr.h"
#include "bufq.h"
#include "config.h"
#include "frame.h"
#include "loop.h"
#include "net.h"
#include "server.h"

#include <stdint.h>

typedef struct server_client server_client;
typedef struct server_tunnel server_tunnel;
typedef struct server_session server_session;

typedef enum server_client_state {
    SC_HANDSHAKE = 0,
    SC_WAIT_TUNNELS,
    SC_READY,
    SC_CLOSING,
    SC_DEAD
} server_client_state;

struct server_tunnel {
    server_tunnel *next;
    server_client *client;
    fp_tunnel_spec spec;
    fp_socket_t listen_fd;
    int watcher_added;
    size_t nsessions;
    uint16_t index;
};

struct server_session {
    server_session *next;
    server_client *client;
    server_tunnel *tunnel;
    uint8_t dir;
    uint32_t stream_id;
    fp_socket_t udp_fd;
    int owns_fd;
    int watcher_added;
    fp_endpoint target; /* L: destination */
    fp_endpoint peer;   /* R: application source; L: destination */
    int64_t last_active_ms;
};

struct server_client {
    server_client *next;
    fp_server *srv;
    uint64_t id;
    char tag[40];
    fp_socket_t fd;
    fp_endpoint peer;
    char peer_text[FP_ENDPOINT_TEXT_MAX];
    server_client_state state;
    fp_frame_parser parser;
    fp_sendq sendq;
    int64_t connected_ms;
    int64_t last_recv_ms;
    int64_t last_ping_ms;
    int64_t close_deadline_ms;
    char name[FP_MAX_CLIENT_NAME + 1];
    server_tunnel *tunnels;
    size_t ntunnels;
    server_session *sessions;
    size_t nsessions;
    uint32_t next_r_stream;
    size_t dropped;
    size_t dropped_recent;
    int64_t last_drop_log_ms;
    int dead;
};

struct fp_server {
    fp_server_config cfg;
    fp_loop *loop;
    fp_socket_t listen_fd;
    int listener_watched;
    server_client *clients;
    size_t nclients;
    uint64_t next_client_id;
    size_t nsessions;
    int64_t last_tick_ms;
    int stopping;
    uint8_t rbuf[65536];
    uint8_t udp_buf[65536];
};

/* server.c entry points used by tunnel/session modules. */
int fp_server_client_send(server_client *c, uint8_t type, uint32_t stream_id,
                          const void *payload, size_t len, int critical);
void fp_server_client_set_dead(server_client *c, const char *reason);
void fp_server_client_set_closing(server_client *c, int64_t grace_ms);
void fp_server_client_update_events(server_client *c);
void fp_server_client_flush(server_client *c);
void fp_server_note_drop(server_client *c, const char *what);

/* tunnel.c */
int fp_server_tunnel_add(server_client *c, const fp_tunnel_spec *spec,
                         uint16_t index, fp_tunnel_result *result);
void fp_server_tunnel_close(server_tunnel *t, int notify);
void fp_server_tunnel_close_all(server_client *c, int notify);
server_tunnel *fp_server_tunnel_find_l(server_client *c,
                                       const char *host, uint16_t port);

/* session.c */
int fp_server_session_open_l(server_client *c, uint32_t stream_id,
                             const char *target_text,
                             char *err, size_t errcap);
int fp_server_session_open_r(server_client *c, server_tunnel *t,
                             const fp_endpoint *peer, uint32_t *stream_id,
                             server_session **out, char *err, size_t errcap);
server_session *fp_server_session_lookup_id(server_client *c,
                                            uint32_t stream_id);
void fp_server_session_close(server_client *c, server_session *s,
                             int reason, int notify);
void fp_server_session_close_all(server_client *c, int notify);
void fp_server_session_close_tunnel(server_tunnel *t, int notify);
void fp_server_session_client_data(server_client *c, uint32_t stream_id,
                                   const uint8_t *payload, size_t len);
void fp_server_session_client_close(server_client *c, uint32_t stream_id,
                                    int reason);
void fp_server_session_tick(fp_server *srv, int64_t now);
uint32_t fp_server_alloc_r_stream(server_client *c);
void fp_server_session_on_r_udp(fp_loop *loop, fp_socket_t fd,
                                short revents, void *arg);
void fp_server_session_on_l_udp(fp_loop *loop, fp_socket_t fd,
                                short revents, void *arg);

#endif /* FP_SERVER_INTERNAL_H */
