#include "server.h"

#include "addr.h"
#include "config.h"
#include "log.h"
#include "net.h"
#include "protocol.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out)
{
    fprintf(out,
        "FluxPort server " FP_VERSION_STRING "\n"
        "Usage: fluxport-server [options]\n"
        "\n"
        "Options:\n"
        "  -c, --config PATH      read settings from PATH\n"
        "  -l, --listen HOST:PORT control listen address (default 0.0.0.0:7000)\n"
        "      --token TOKEN      shared authentication token\n"
        "      --allow-ports POLICY all, none, PORT or LO-HI (for R listeners)\n"
        "      --max-clients N    maximum concurrent clients (default 64)\n"
        "      --keepalive SECONDS heartbeat interval, 0 disables (default 15)\n"
        "      --udp-idle SECONDS UDP session idle timeout (default 60)\n"
        "      --log-level LEVEL  error, warn, info or debug\n"
        "  -v, --verbose          increase log verbosity\n"
        "  -h, --help             show this help\n"
        "  -V, --version          show version\n");
}

static int option_takes_value(const char *arg)
{
    return strcmp(arg, "--config") == 0 ||
           strcmp(arg, "-c") == 0 ||
           strcmp(arg, "--listen") == 0 ||
           strcmp(arg, "-l") == 0 ||
           strcmp(arg, "--token") == 0 ||
           strcmp(arg, "--allow-ports") == 0 ||
           strcmp(arg, "--max-clients") == 0 ||
           strcmp(arg, "--keepalive") == 0 ||
           strcmp(arg, "--udp-idle") == 0 ||
           strcmp(arg, "--log-level") == 0;
}

static int set_long_option(const char *name, const char *value,
                           long min, long max, int *out)
{
    long parsed;
    char err[128];

    if (fp_parse_long(value, min, max, &parsed) != 0) {
        snprintf(err, sizeof(err), "%s must be %ld..%ld", name, min, max);
        FP_LOGE("server", "%s", err);
        return -1;
    }
    *out = (int)parsed;
    return 0;
}

int main(int argc, char **argv)
{
    fp_server_config cfg;
    const char *config_path = NULL;
    const char *value;
    int i;
    int rc = 1;
    fp_server *srv;
    char err[512];

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("fluxport-server %s\n", FP_VERSION_STRING);
            return 0;
        }
    }
    /* Locate --config first so command line options override the file. */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 ||
            strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "fluxport-server: %s requires a path\n",
                        argv[i]);
                return 1;
            }
            config_path = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0 ||
                   strcmp(argv[i], "--verbose") == 0) {
            continue;
        } else if (option_takes_value(argv[i])) {
            if (i + 1 >= argc) {
                fprintf(stderr, "fluxport-server: %s requires a value\n",
                        argv[i]);
                return 1;
            }
            i++;
        }
    }
    fp_server_config_defaults(&cfg);
    if (config_path != NULL) {
        if (fp_server_config_load(config_path, &cfg,
                                  err, sizeof(err)) != 0) {
            FP_LOGE("server", "config: %s", err);
            return 1;
        }
    }
    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--config") == 0 || strcmp(arg, "-c") == 0) {
            i++;
            continue;
        }
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            if (cfg.log_level < FP_LOG_DEBUG) {
                cfg.log_level++;
            }
            continue;
        }
        if (i + 1 >= argc) {
            FP_LOGE("server", "%s requires a value", arg);
            return 1;
        }
        value = argv[++i];
        if (strcmp(arg, "--listen") == 0 || strcmp(arg, "-l") == 0) {
            char host[FP_MAX_HOST + 1];
            uint16_t port;
            if (fp_hostport_parse(value, host, sizeof(host), &port,
                                  err, sizeof(err)) != 0) {
                FP_LOGE("server", "--listen: %s", err);
                return 1;
            }
            snprintf(cfg.listen_host, sizeof(cfg.listen_host), "%s", host);
            cfg.listen_port = port;
        } else if (strcmp(arg, "--token") == 0) {
            if (strlen(value) > FP_MAX_TOKEN) {
                FP_LOGE("server", "--token is too long");
                return 1;
            }
            snprintf(cfg.token, sizeof(cfg.token), "%s", value);
        } else if (strcmp(arg, "--allow-ports") == 0) {
            char perr[160];
            if (fp_parse_port_policy(value, &cfg.port_policy,
                                     &cfg.port_lo, &cfg.port_hi,
                                     perr, sizeof(perr)) != 0) {
                FP_LOGE("server", "--allow-ports: %s", perr);
                return 1;
            }
        } else if (strcmp(arg, "--max-clients") == 0) {
            if (set_long_option("--max-clients", value, 1,
                                FP_MAX_CLIENTS_LIMIT,
                                &cfg.max_clients) != 0) {
                return 1;
            }
        } else if (strcmp(arg, "--keepalive") == 0) {
            if (set_long_option("--keepalive", value, 0,
                                FP_MAX_TIMEOUT_SEC, &cfg.keepalive_s) != 0) {
                return 1;
            }
        } else if (strcmp(arg, "--udp-idle") == 0) {
            if (set_long_option("--udp-idle", value, 1,
                                FP_MAX_UDP_IDLE_SEC, &cfg.udp_idle_s) != 0) {
                return 1;
            }
        } else if (strcmp(arg, "--log-level") == 0) {
            int level = fp_log_level_parse(value);
            if (level < 0) {
                FP_LOGE("server", "--log-level must be error, warn, info "
                        "or debug");
                return 1;
            }
            cfg.log_level = level;
        } else {
            FP_LOGE("server", "unknown option '%s'", arg);
            usage(stderr);
            return 1;
        }
    }
    if (fp_server_config_validate(&cfg, err, sizeof(err)) != 0) {
        FP_LOGE("server", "%s", err);
        return 1;
    }
    fp_log_set_level(cfg.log_level);
    if (fp_net_init() != 0) {
        FP_LOGE("server", "cannot initialize the network stack");
        return 2;
    }
    if (fp_install_signal_handlers() != 0) {
        FP_LOGE("server", "cannot install signal handlers");
        fp_net_cleanup();
        return 2;
    }
    srv = fp_server_new(&cfg, err, sizeof(err));
    if (srv == NULL) {
        FP_LOGE("server", "%s", err);
        fp_net_cleanup();
        return 1;
    }
    if (fp_server_start(srv, err, sizeof(err)) != 0) {
        FP_LOGE("server", "cannot start: %s", err);
        fp_server_free(srv);
        fp_net_cleanup();
        return 2;
    }
    fp_server_run(srv);
    rc = 0;
    fp_server_free(srv);
    fp_net_cleanup();
    return rc;
}
