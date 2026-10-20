#include "config.h"

#include "addr.h"
#include "log.h"
#include "util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int eqi(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void cfg_err(char *err, size_t cap, const char *path, size_t line,
                    const char *fmt, ...)
{
    va_list ap;
    int used = 0;

    if (err == NULL || cap == 0) {
        return;
    }
    if (path != NULL) {
        if (line > 0) {
            used = snprintf(err, cap, "%s:%lu: ", path, (unsigned long)line);
        } else {
            used = snprintf(err, cap, "%s: ", path);
        }
    }
    if (used < 0) {
        used = 0;
    }
    if ((size_t)used >= cap) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err + used, cap - (size_t)used, fmt, ap);
    va_end(ap);
}

/*
 * Split a trimmed setting line into key and rest.  Returns 0 for blank and
 * comment lines, 1 for a setting, -1 when only a key is present.
 */
static int split_setting(char *line, char **key, char **value)
{
    char *s = fp_trim(line);
    char *p;

    *key = NULL;
    *value = NULL;
    if (*s == '\0' || *s == '#') {
        return 0;
    }
    p = s;
    while (*p != '\0' && !fp_isspace_c(*p)) {
        p++;
    }
    *key = s;
    if (*p == '\0') {
        *value = p;
        return -1;
    }
    *p = '\0';
    p++;
    *value = fp_trim(p);
    {
        char *hash = strchr(*value, '#');
        if (hash != NULL &&
            (hash == *value || fp_isspace_c(hash[-1]))) {
            *hash = '\0';
            *value = fp_trim(*value);
        }
    }
    return 1;
}

static int parse_bool(const char *text, int *out)
{
    if (eqi(text, "on") || eqi(text, "true") || eqi(text, "yes") ||
        strcmp(text, "1") == 0) {
        *out = 1;
        return 0;
    }
    if (eqi(text, "off") || eqi(text, "false") || eqi(text, "no") ||
        strcmp(text, "0") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

void fp_server_config_defaults(fp_server_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    fp_strcpy_bounded(cfg->listen_host, sizeof(cfg->listen_host), "0.0.0.0");
    cfg->listen_port = (uint16_t)FP_DEFAULT_SERVER_PORT;
    cfg->port_policy = FP_PORTS_ALL;
    cfg->max_clients = FP_DEFAULT_MAX_CLIENTS;
    cfg->keepalive_s = FP_DEFAULT_KEEPALIVE;
    cfg->udp_idle_s = FP_DEFAULT_UDP_IDLE;
    cfg->log_level = FP_LOG_INFO;
}

void fp_client_config_defaults(fp_client_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    fp_strcpy_bounded(cfg->name, sizeof(cfg->name), "fluxport-client");
    cfg->strict = 0;
    cfg->connect_timeout_s = FP_DEFAULT_CONNECT_TIMEOUT;
    cfg->keepalive_s = FP_DEFAULT_KEEPALIVE;
    cfg->udp_idle_s = FP_DEFAULT_UDP_IDLE;
    cfg->log_level = FP_LOG_INFO;
}

int fp_parse_port_policy(const char *text, int *policy,
                         uint16_t *lo, uint16_t *hi,
                         char *err, size_t errcap)
{
    char buf[64];
    char *dash;
    long first;
    long second;

    if (text == NULL || policy == NULL || lo == NULL || hi == NULL) {
        return -1;
    }
    if (strlen(text) >= sizeof(buf)) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "port policy is too long");
        }
        return -1;
    }
    snprintf(buf, sizeof(buf), "%s", text);
    if (eqi(buf, "all") || eqi(buf, "any") || strcmp(buf, "*") == 0) {
        *policy = FP_PORTS_ALL;
        *lo = 0;
        *hi = 0;
        return 0;
    }
    if (eqi(buf, "none") || eqi(buf, "off")) {
        *policy = FP_PORTS_NONE;
        *lo = 0;
        *hi = 0;
        return 0;
    }
    dash = strchr(buf, '-');
    if (dash != NULL) {
        if (strchr(dash + 1, '-') != NULL) {
            goto invalid;
        }
        *dash = '\0';
        if (fp_parse_long(buf, 1, 65535, &first) != 0 ||
            fp_parse_long(dash + 1, 1, 65535, &second) != 0 ||
            first > second) {
            goto invalid;
        }
        *policy = FP_PORTS_RANGE;
        *lo = (uint16_t)first;
        *hi = (uint16_t)second;
        return 0;
    }
    if (fp_parse_long(buf, 0, 65535, &first) != 0) {
        goto invalid;
    }
    if (first == 0) {
        *policy = FP_PORTS_NONE;
        *lo = 0;
        *hi = 0;
        return 0;
    }
    *policy = FP_PORTS_RANGE;
    *lo = (uint16_t)first;
    *hi = (uint16_t)first;
    return 0;

invalid:
    if (err != NULL && errcap > 0) {
        snprintf(err, errcap,
                 "invalid port policy '%s' (use all, none, PORT or LO-HI)",
                 text);
    }
    return -1;
}

int fp_server_config_load(const char *path, fp_server_config *cfg,
                          char *err, size_t errcap)
{
    char *text = NULL;
    char **lines = NULL;
    size_t nlines = 0;
    size_t i;
    int rc = -1;

    if (path == NULL || cfg == NULL) {
        return -1;
    }
    fp_server_config_defaults(cfg);
    if (fp_read_file(path, &text, NULL, err, errcap) != 0) {
        return -1;
    }
    if (fp_split_lines_inplace(text, &lines, &nlines) != 0) {
        cfg_err(err, errcap, path, 0, "out of memory");
        free(text);
        return -1;
    }
    for (i = 0; i < nlines; i++) {
        char *key;
        char *value;
        int kind = split_setting(lines[i], &key, &value);
        if (kind == 0) {
            continue;
        }
        if (kind < 0) {
            cfg_err(err, errcap, path, i + 1, "missing value for '%s'", key);
            goto done;
        }
        if (strcmp(key, "listen") == 0) {
            char host[FP_MAX_HOST + 1];
            char perr[128];
            uint16_t port;
            if (fp_hostport_parse(value, host, sizeof(host), &port,
                                  perr, sizeof(perr)) != 0) {
                cfg_err(err, errcap, path, i + 1, "%s", perr);
                goto done;
            }
            snprintf(cfg->listen_host, sizeof(cfg->listen_host), "%s", host);
            cfg->listen_port = port;
        } else if (strcmp(key, "token") == 0) {
            if (strlen(value) > FP_MAX_TOKEN) {
                cfg_err(err, errcap, path, i + 1, "token is too long");
                goto done;
            }
            snprintf(cfg->token, sizeof(cfg->token), "%s", value);
        } else if (strcmp(key, "allow_ports") == 0) {
            char perr[160];
            if (fp_parse_port_policy(value, &cfg->port_policy,
                                     &cfg->port_lo, &cfg->port_hi,
                                     perr, sizeof(perr)) != 0) {
                cfg_err(err, errcap, path, i + 1, "%s", perr);
                goto done;
            }
        } else if (strcmp(key, "max_clients") == 0) {
            long n;
            if (fp_parse_long(value, 1, FP_MAX_CLIENTS_LIMIT, &n) != 0) {
                cfg_err(err, errcap, path, i + 1,
                        "max_clients must be 1..%d", FP_MAX_CLIENTS_LIMIT);
                goto done;
            }
            cfg->max_clients = (int)n;
        } else if (strcmp(key, "keepalive") == 0) {
            long n;
            if (fp_parse_long(value, 0, FP_MAX_TIMEOUT_SEC, &n) != 0) {
                cfg_err(err, errcap, path, i + 1,
                        "keepalive must be 0..%d seconds", FP_MAX_TIMEOUT_SEC);
                goto done;
            }
            cfg->keepalive_s = (int)n;
        } else if (strcmp(key, "udp_idle") == 0 ||
                   strcmp(key, "udp_timeout") == 0) {
            long n;
            if (fp_parse_long(value, 1, FP_MAX_UDP_IDLE_SEC, &n) != 0) {
                cfg_err(err, errcap, path, i + 1,
                        "udp_idle must be 1..%d seconds", FP_MAX_UDP_IDLE_SEC);
                goto done;
            }
            cfg->udp_idle_s = (int)n;
        } else if (strcmp(key, "log_level") == 0) {
            int level = fp_log_level_parse(value);
            if (level < 0) {
                cfg_err(err, errcap, path, i + 1,
                        "log_level must be error, warn, info or debug");
                goto done;
            }
            cfg->log_level = level;
        } else {
            cfg_err(err, errcap, path, i + 1, "unknown setting '%s'", key);
            goto done;
        }
    }
    rc = 0;

done:
    fp_free_lines(lines, nlines);
    free(text);
    return rc;
}

int fp_client_config_load(const char *path, fp_client_config *cfg,
                          char *err, size_t errcap)
{
    char *text = NULL;
    char **lines = NULL;
    size_t nlines = 0;
    size_t i;
    int rc = -1;

    if (path == NULL || cfg == NULL) {
        return -1;
    }
    fp_client_config_defaults(cfg);
    if (fp_read_file(path, &text, NULL, err, errcap) != 0) {
        return -1;
    }
    if (fp_split_lines_inplace(text, &lines, &nlines) != 0) {
        cfg_err(err, errcap, path, 0, "out of memory");
        free(text);
        return -1;
    }
    for (i = 0; i < nlines; i++) {
        char *key;
        char *value;
        int kind = split_setting(lines[i], &key, &value);
        if (kind == 0) {
            continue;
        }
        if (kind < 0) {
            cfg_err(err, errcap, path, i + 1, "missing value for '%s'", key);
            goto done;
        }
        if (strcmp(key, "L") == 0 || strcmp(key, "R") == 0) {
            /* Tunnel lines are owned and validated by fp_tunnel_table. */
            continue;
        }
        if (strcmp(key, "server") == 0) {
            char host[FP_MAX_HOST + 1];
            char perr[128];
            uint16_t port;
            if (fp_hostport_parse(value, host, sizeof(host), &port,
                                  perr, sizeof(perr)) != 0) {
                cfg_err(err, errcap, path, i + 1, "%s", perr);
                goto done;
            }
            snprintf(cfg->server_host, sizeof(cfg->server_host), "%s", host);
            cfg->server_port = port;
        } else if (strcmp(key, "token") == 0) {
            if (strlen(value) > FP_MAX_TOKEN) {
                cfg_err(err, errcap, path, i + 1, "token is too long");
                goto done;
            }
            snprintf(cfg->token, sizeof(cfg->token), "%s", value);
        } else if (strcmp(key, "name") == 0) {
            if (strlen(value) > FP_MAX_CLIENT_NAME) {
                cfg_err(err, errcap, path, i + 1, "name is too long");
                goto done;
            }
            snprintf(cfg->name, sizeof(cfg->name), "%s", value);
        } else if (strcmp(key, "strict") == 0) {
            int on;
            if (parse_bool(value, &on) != 0) {
                cfg_err(err, errcap, path, i + 1, "strict must be on or off");
                goto done;
            }
            cfg->strict = on;
        } else if (strcmp(key, "connect_timeout") == 0) {
            long n;
            if (fp_parse_long(value, 1, 300, &n) != 0) {
                cfg_err(err, errcap, path, i + 1,
                        "connect_timeout must be 1..300 seconds");
                goto done;
            }
            cfg->connect_timeout_s = (int)n;
        } else if (strcmp(key, "keepalive") == 0) {
            long n;
            if (fp_parse_long(value, 0, FP_MAX_TIMEOUT_SEC, &n) != 0) {
                cfg_err(err, errcap, path, i + 1,
                        "keepalive must be 0..%d seconds", FP_MAX_TIMEOUT_SEC);
                goto done;
            }
            cfg->keepalive_s = (int)n;
        } else if (strcmp(key, "udp_idle") == 0 ||
                   strcmp(key, "udp_timeout") == 0) {
            long n;
            if (fp_parse_long(value, 1, FP_MAX_UDP_IDLE_SEC, &n) != 0) {
                cfg_err(err, errcap, path, i + 1,
                        "udp_idle must be 1..%d seconds", FP_MAX_UDP_IDLE_SEC);
                goto done;
            }
            cfg->udp_idle_s = (int)n;
        } else if (strcmp(key, "log_level") == 0) {
            int level = fp_log_level_parse(value);
            if (level < 0) {
                cfg_err(err, errcap, path, i + 1,
                        "log_level must be error, warn, info or debug");
                goto done;
            }
            cfg->log_level = level;
        } else {
            cfg_err(err, errcap, path, i + 1, "unknown setting '%s'", key);
            goto done;
        }
    }
    rc = 0;

done:
    fp_free_lines(lines, nlines);
    free(text);
    return rc;
}

int fp_server_config_validate(const fp_server_config *cfg,
                              char *err, size_t errcap)
{
    if (cfg->listen_host[0] == '\0') {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "listen host is empty");
        }
        return -1;
    }
    if (cfg->listen_port == 0) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "listen port must not be 0");
        }
        return -1;
    }
    if (strlen(cfg->token) > FP_MAX_TOKEN) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "token is too long");
        }
        return -1;
    }
    if (cfg->max_clients < 1 || cfg->max_clients > FP_MAX_CLIENTS_LIMIT) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "invalid max_clients");
        }
        return -1;
    }
    if (cfg->keepalive_s < 0 || cfg->keepalive_s > FP_MAX_TIMEOUT_SEC) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "invalid keepalive");
        }
        return -1;
    }
    if (cfg->udp_idle_s < 1 || cfg->udp_idle_s > FP_MAX_UDP_IDLE_SEC) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "invalid udp_idle");
        }
        return -1;
    }
    if (cfg->port_policy != FP_PORTS_ALL &&
        cfg->port_policy != FP_PORTS_NONE &&
        cfg->port_policy != FP_PORTS_RANGE) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "invalid port policy");
        }
        return -1;
    }
    return 0;
}

int fp_client_config_validate(const fp_client_config *cfg,
                              int require_server,
                              char *err, size_t errcap)
{
    if (require_server) {
        if (cfg->server_host[0] == '\0') {
            if (err != NULL && errcap > 0) {
                snprintf(err, errcap,
                         "no server configured (use --server or a config file)");
            }
            return -1;
        }
        if (cfg->server_port == 0) {
            if (err != NULL && errcap > 0) {
                snprintf(err, errcap, "invalid server port");
            }
            return -1;
        }
        if (fp_endpoint_is_wildcard(cfg->server_host)) {
            if (err != NULL && errcap > 0) {
                snprintf(err, errcap, "server address must not be a wildcard");
            }
            return -1;
        }
    }
    if (strlen(cfg->token) > FP_MAX_TOKEN) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "token is too long");
        }
        return -1;
    }
    if (strlen(cfg->name) > FP_MAX_CLIENT_NAME) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "client name is too long");
        }
        return -1;
    }
    if (cfg->connect_timeout_s < 1 || cfg->connect_timeout_s > 300) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "invalid connect timeout");
        }
        return -1;
    }
    if (cfg->keepalive_s < 0 || cfg->keepalive_s > FP_MAX_TIMEOUT_SEC) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "invalid keepalive");
        }
        return -1;
    }
    if (cfg->udp_idle_s < 1 || cfg->udp_idle_s > FP_MAX_UDP_IDLE_SEC) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "invalid udp_idle");
        }
        return -1;
    }
    return 0;
}

int fp_server_config_port_allowed(const fp_server_config *cfg, uint16_t port)
{
    if (cfg == NULL) {
        return 0;
    }
    switch (cfg->port_policy) {
    case FP_PORTS_ALL:
        return 1;
    case FP_PORTS_NONE:
        return 0;
    case FP_PORTS_RANGE:
        return port >= cfg->port_lo && port <= cfg->port_hi;
    default:
        return 0;
    }
}
