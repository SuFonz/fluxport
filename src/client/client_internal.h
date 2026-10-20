#ifndef FP_CLIENT_INTERNAL_H
#define FP_CLIENT_INTERNAL_H

#include "client.h"

#include "addr.h"
#include "bufq.h"
#include "frame.h"
#include "loop.h"
#include "net.h"
#include "protocol.h"

#include <stdint.h>

enum client_state {
    CC_INIT = 0,
    CC_ACK_WAIT,
    CC_RESULT_WAIT,
    CC_RUNNING,
    CC_CLOSING,
    CC_DEAD
};

typedef struct client_session client_session;

typedef struct client_tunnel {
    fp_client *client;
    fp_tunnel_spec spec;
    uint8_t status;
    int ok;
    char msg[FP_MAX_MSG + 1];
    fp_socket_t listen_fd;
    int watcher_added;
    size_t nsessions;
} client_tunnel;

struct client_session {
    fp_client *client;
    client_tunnel *tunnel;
    uint8_t dir;
    uint32_t stream_id;
    fp_endpoint target;
    fp_endpoint peer;
    fp_socket_t udp_fd;
    int owns_fd;
    int watcher_added;
    int64_t last_active_ms;
    client_session *next;
};

struct fp_client {
    fp_client_config cfg;
    client_tunnel *tunnels;
    size_t ntunnels;
    size_t tunnels_failed;

    fp_socket_t fd;
    fp_loop *loop;
    fp_frame_parser parser;
    fp_sendq sendq;
    char tag[32];

    int state;
    int dead;
    int stopping;
    int handshake_done;
    int shutdown_done;
    int fatal_exit;
    int exit_code;

    uint32_t next_l_stream;
    client_session *sessions;
    size_t nsessions;

    int64_t connected_ms;
    int64_t last_recv_ms;
    int64_t last_ping_ms;
    int64_t last_tick_ms;
    int64_t close_deadline_ms;

    uint64_t dropped;
    unsigned long dropped_recent;
    int64_t last_drop_log_ms;

    uint8_t rbuf[65536];
    uint8_t udp_buf[65536];
};

/* client.c */
void fp_client_note_drop(fp_client *cl, const char *what);
int fp_client_send(fp_client *cl, uint8_t type, uint32_t stream_id,
                   const void *payload, size_t len, int critical);
void fp_client_update_events(fp_client *cl);
void fp_client_flush(fp_client *cl);
void fp_client_set_dead(fp_client *cl, int exit_code, const char *reason);
void fp_client_set_closing(fp_client *cl, int64_t grace_ms);

/* forwarder.c */
uint32_t fp_client_alloc_l_stream(fp_client *cl);
client_session *fp_client_session_lookup_id(fp_client *cl, uint32_t sid);
int fp_client_session_open_l(fp_client *cl, client_tunnel *t,
                             const fp_endpoint *peer, client_session **out,
                             char *err, size_t errcap);
int fp_client_session_open_r(fp_client *cl, uint32_t sid,
                             const char *target_text,
                             client_session **out,
                             char *err, size_t errcap);
void fp_client_session_data(fp_client *cl, uint32_t sid,
                            const uint8_t *payload, size_t len);
void fp_client_session_peer_close(fp_client *cl, uint32_t sid, int reason);
void fp_client_session_close(fp_client *cl, client_session *s,
                             int reason, int notify);
void fp_client_session_close_all(fp_client *cl, int notify);
void fp_client_session_tick(fp_client *cl, int64_t now);
void fp_client_tunnels_bind_l(fp_client *cl);
void fp_client_tunnels_close_all(fp_client *cl);
void fp_client_on_l_listener(fp_loop *loop, fp_socket_t fd,
                             short revents, void *arg);
void fp_client_on_session_udp(fp_loop *loop, fp_socket_t fd,
                              short revents, void *arg);

#endif /* FP_CLIENT_INTERNAL_H */
