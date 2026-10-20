/*
 * Configuration file and command line value parsing for both programs.
 *
 * Files are line oriented: "key value" settings, blank lines and lines
 * starting with '#' are ignored.  Client files additionally contain tunnel
 * lines: "L listen_host:port target_host:port" and the R equivalent.
 */
#ifndef FP_CONFIG_H
#define FP_CONFIG_H

#include "frame.h"
#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

#define FP_CONFIG_PATH_MAX 1024
#define FP_DEFAULT_SERVER_PORT 7000u
#define FP_DEFAULT_KEEPALIVE 15
#define FP_DEFAULT_UDP_IDLE 60
#define FP_DEFAULT_CONNECT_TIMEOUT 10
#define FP_DEFAULT_MAX_CLIENTS 64
#define FP_HANDSHAKE_TIMEOUT_MS 10000
#define FP_MAX_CLIENTS_LIMIT 1024
#define FP_MAX_TIMEOUT_SEC 3600
#define FP_MAX_UDP_IDLE_SEC 86400

enum fp_port_policy {
    FP_PORTS_ALL = 0,
    FP_PORTS_RANGE = 1,
    FP_PORTS_NONE = 2
};

typedef struct fp_server_config {
    char listen_host[FP_MAX_HOST + 1];
    uint16_t listen_port;
    char token[FP_MAX_TOKEN + 1];
    int port_policy;
    uint16_t port_lo;
    uint16_t port_hi;
    int max_clients;
    int keepalive_s;
    int udp_idle_s;
    int log_level;
} fp_server_config;

typedef struct fp_client_config {
    char server_host[FP_MAX_HOST + 1];
    uint16_t server_port;
    char token[FP_MAX_TOKEN + 1];
    char name[FP_MAX_CLIENT_NAME + 1];
    int strict;
    int connect_timeout_s;
    int keepalive_s;
    int udp_idle_s;
    int log_level;
} fp_client_config;

void fp_server_config_defaults(fp_server_config *cfg);
void fp_client_config_defaults(fp_client_config *cfg);

/* Load a required file; returns 0 or -1 with a path:line error message. */
int fp_server_config_load(const char *path, fp_server_config *cfg,
                          char *err, size_t errcap);
int fp_client_config_load(const char *path, fp_client_config *cfg,
                          char *err, size_t errcap);

int fp_server_config_validate(const fp_server_config *cfg,
                              char *err, size_t errcap);
int fp_client_config_validate(const fp_client_config *cfg,
                              int require_server,
                              char *err, size_t errcap);

/* Parse "all", "none", "5353" or "10000-20000" into policy fields. */
int fp_parse_port_policy(const char *text, int *policy,
                         uint16_t *lo, uint16_t *hi,
                         char *err, size_t errcap);

int fp_server_config_port_allowed(const fp_server_config *cfg, uint16_t port);

#endif /* FP_CONFIG_H */
