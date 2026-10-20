#include "client.h"

#include "addr.h"
#include "config.h"
#include "log.h"
#include "net.h"
#include "protocol.h"
#include "tunnel_table.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#define CLIENT_VERSION_TEXT "fluxport-client " FP_VERSION_STRING

enum client_command {
    CMD_RUN = 0,
    CMD_ADD,
    CMD_LIST,
    CMD_REMOVE
};

typedef struct cli_options {
    const char *config_path;
    int config_explicit;

    char server_host[FP_MAX_HOST + 1];
    uint16_t server_port;
    int have_server;

    const char *token;
    const char *name;
    int strict;
    int connect_timeout;
    int keepalive;
    int udp_idle;
    int log_level;
    int verbose;

    fp_tunnel_spec *tunnels;
    size_t ntunnels;
} cli_options;

static void usage(FILE *out)
{
    fprintf(out,
        "Usage:\n"
        "  fluxport-client -c <config> [run] [options]\n"
        "  fluxport-client -c <config> add -L <laddr:lport> <rhost:rport>\n"
        "  fluxport-client -c <config> add -R <raddr:rport> <lhost:lport>\n"
        "  fluxport-client -c <config> list\n"
        "  fluxport-client -c <config> remove <index|prefix>\n"
        "\n"
        "Run options:\n"
        "  -c, --config PATH        configuration file\n"
        "  -s, --server HOST:PORT   server address\n"
        "      --token TOKEN        authentication token\n"
        "      --name NAME          client name shown by the server\n"
        "      --strict             abort if any tunnel fails to set up\n"
        "      --connect-timeout S  TCP connect timeout (default %d)\n"
        "      --keepalive S        heartbeat interval, 0 disables (default %d)\n"
        "      --udp-idle S         UDP session idle timeout (default %d)\n"
        "      --log-level LEVEL    error, warn, info or debug\n"
        "  -v, --verbose            increase log verbosity\n"
        "  -L, --local LISTEN TARGET\n"
        "                           add an L tunnel for this run (LISTEN=TARGET also works)\n"
        "  -R, --remote LISTEN TARGET\n"
        "                           add an R tunnel for this run (LISTEN=TARGET also works)\n"
        "  -h, --help               show this help\n"
        "  -V, --version            show version\n"
        "\n"
        "Tunnels:\n"
        "  L: client listens on LISTEN and forwards UDP payloads to TARGET\n"
        "  R: server listens on LISTEN and forwards UDP payloads to TARGET\n"
        "  TARGET must be a concrete host:port (not 0.0.0.0 or *).\n",
        FP_DEFAULT_CONNECT_TIMEOUT, FP_DEFAULT_KEEPALIVE, FP_DEFAULT_UDP_IDLE);
}

static int parse_int_option(const char *name, const char *text,
                            long min, long max, int *out)
{
    long value;

    if (fp_parse_long(text, min, max, &value) != 0) {
        fprintf(stderr, "fluxport-client: %s expects a number in [%ld, %ld]\n",
                name, min, max);
        return -1;
    }
    *out = (int)value;
    return 0;
}

static int split_pair(const char *text, char *listen_text,
                      size_t listen_cap, char *target_text,
                      size_t target_cap)
{
    const char *eq;
    size_t left_len;

    eq = strchr(text, '=');
    if (eq == NULL) {
        fprintf(stderr, "fluxport-client: expected LISTEN=TARGET, got '%s'\n",
                text);
        return -1;
    }
    left_len = (size_t)(eq - text);
    if (left_len == 0 || left_len >= listen_cap ||
        strlen(eq + 1) >= target_cap) {
        fprintf(stderr, "fluxport-client: tunnel address is too long\n");
        return -1;
    }
    memcpy(listen_text, text, left_len);
    listen_text[left_len] = '\0';
    memcpy(target_text, eq + 1, strlen(eq + 1) + 1);
    return 0;
}

static int make_tunnel_spec(uint8_t dir, const char *listen_text,
                            const char *target_text, fp_tunnel_spec *spec,
                            char *err, size_t errcap)
{
    char perr[192];

    memset(spec, 0, sizeof(*spec));
    spec->dir = dir;
    if (fp_hostport_parse(listen_text, spec->listen_addr,
                          sizeof(spec->listen_addr), &spec->listen_port,
                          perr, sizeof(perr)) != 0) {
        snprintf(err, errcap, "invalid listen address '%s': %s",
                 listen_text, perr);
        return -1;
    }
    if (fp_hostport_parse(target_text, spec->target_host,
                          sizeof(spec->target_host), &spec->target_port,
                          perr, sizeof(perr)) != 0) {
        snprintf(err, errcap, "invalid target address '%s': %s",
                 target_text, perr);
        return -1;
    }
    if (fp_endpoint_is_wildcard(spec->target_host)) {
        snprintf(err, errcap, "target address must be concrete");
        return -1;
    }
    return 0;
}

static int add_cli_tunnel(cli_options *opts, uint8_t dir,
                          const char *listen_text, const char *target_text)
{
    char err[256];
    fp_tunnel_spec spec;

    if (opts->ntunnels >= FP_MAX_TUNNELS) {
        fprintf(stderr, "fluxport-client: too many tunnels (max %d)\n",
                FP_MAX_TUNNELS);
        return -1;
    }
    if (make_tunnel_spec(dir, listen_text, target_text, &spec,
                         err, sizeof(err)) != 0) {
        fprintf(stderr, "fluxport-client: %s\n", err);
        return -1;
    }
    opts->tunnels[opts->ntunnels++] = spec;
    return 0;
}

static int add_cli_tunnel_token(cli_options *opts, uint8_t dir,
                                const char *token)
{
    char listen_text[FP_MAX_ADDR_TEXT];
    char target_text[FP_MAX_ADDR_TEXT];

    if (split_pair(token, listen_text, sizeof(listen_text),
                   target_text, sizeof(target_text)) != 0) {
        return -1;
    }
    return add_cli_tunnel(opts, dir, listen_text, target_text);
}

static void format_tunnel(const fp_tunnel_spec *s, char *buf, size_t cap)
{
    char listen_text[FP_ENDPOINT_TEXT_MAX];
    char target_text[FP_ENDPOINT_TEXT_MAX];

    if (fp_hostport_format(s->listen_addr, s->listen_port,
                           listen_text, sizeof(listen_text)) != 0) {
        snprintf(listen_text, sizeof(listen_text), "%s:%u",
                 s->listen_addr, (unsigned)s->listen_port);
    }
    if (fp_hostport_format(s->target_host, s->target_port,
                           target_text, sizeof(target_text)) != 0) {
        snprintf(target_text, sizeof(target_text), "%s:%u",
                 s->target_host, (unsigned)s->target_port);
    }
    snprintf(buf, cap, "%s %s %s", fp_tunnel_dir_name(s->dir),
             listen_text, target_text);
}

static int tunnel_matches_prefix(const fp_tunnel_spec *s, const char *prefix)
{
    char line[FP_MAX_ADDR_TEXT * 2 + 8];
    char listen_text[FP_ENDPOINT_TEXT_MAX];
    char target_text[FP_ENDPOINT_TEXT_MAX];
    size_t plen = strlen(prefix);

    format_tunnel(s, line, sizeof(line));
    if (strncmp(line, prefix, plen) == 0) {
        return 1;
    }
    if (fp_hostport_format(s->listen_addr, s->listen_port,
                           listen_text, sizeof(listen_text)) == 0 &&
        strncmp(listen_text, prefix, plen) == 0) {
        return 1;
    }
    if (fp_hostport_format(s->target_host, s->target_port,
                           target_text, sizeof(target_text)) == 0 &&
        strncmp(target_text, prefix, plen) == 0) {
        return 1;
    }
    return 0;
}

static int default_config_path(char *buf, size_t cap)
{
    const char *base = NULL;
    int n;

#ifdef _WIN32
    base = getenv("APPDATA");
    if (base != NULL && base[0] != '\0') {
        n = snprintf(buf, cap, "%s\\fluxport\\client.conf", base);
        return (n > 0 && (size_t)n < cap) ? 0 : -1;
    }
#endif
    base = getenv("XDG_CONFIG_HOME");
    if (base != NULL && base[0] != '\0') {
        n = snprintf(buf, cap, "%s/fluxport/client.conf", base);
        return (n > 0 && (size_t)n < cap) ? 0 : -1;
    }
    base = getenv("HOME");
#ifdef _WIN32
    if (base == NULL || base[0] == '\0') {
        base = getenv("USERPROFILE");
    }
#endif
    if (base == NULL || base[0] == '\0') {
        return -1;
    }
    n = snprintf(buf, cap, "%s/.config/fluxport/client.conf", base);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int path_is_readable(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL) {
        return 0;
    }
    fclose(f);
    return 1;
}

static int cmd_add(const cli_options *opts)
{
    fp_tunnel_table table;
    char err[256];
    size_t index = 0;
    char line[FP_MAX_ADDR_TEXT * 2 + 8];

    if (opts->config_path == NULL) {
        fprintf(stderr, "fluxport-client: -c/--config is required for add\n");
        return 1;
    }
    if (opts->ntunnels != 1) {
        fprintf(stderr, "fluxport-client: add expects exactly one -L or -R "
                "tunnel\n");
        return 1;
    }
    fp_tunnel_table_init(&table);
    if (fp_tunnel_table_load(&table, opts->config_path, 1,
                             err, sizeof(err)) != 0) {
        fprintf(stderr, "fluxport-client: %s\n", err);
        fp_tunnel_table_free(&table);
        return 1;
    }
    if (fp_tunnel_table_add(&table, &opts->tunnels[0], &index,
                            err, sizeof(err)) != 0) {
        fprintf(stderr, "fluxport-client: %s\n", err);
        fp_tunnel_table_free(&table);
        return 1;
    }
    if (fp_tunnel_table_save(&table, opts->config_path,
                             err, sizeof(err)) != 0) {
        fprintf(stderr, "fluxport-client: %s\n", err);
        fp_tunnel_table_free(&table);
        return 1;
    }
    format_tunnel(&opts->tunnels[0], line, sizeof(line));
    printf("added tunnel #%lu: %s\n", (unsigned long)index, line);
    fp_tunnel_table_free(&table);
    return 0;
}

static int cmd_list(const cli_options *opts)
{
    fp_tunnel_table table;
    char err[256];
    size_t i;

    if (opts->config_path == NULL) {
        fprintf(stderr, "fluxport-client: -c/--config is required for list\n");
        return 1;
    }
    fp_tunnel_table_init(&table);
    if (fp_tunnel_table_load(&table, opts->config_path, 1,
                             err, sizeof(err)) != 0) {
        fprintf(stderr, "fluxport-client: %s\n", err);
        fp_tunnel_table_free(&table);
        return 1;
    }
    if (table.count == 0) {
        printf("no tunnels configured in %s\n", opts->config_path);
    }
    for (i = 0; i < table.count; i++) {
        char line[FP_MAX_ADDR_TEXT * 2 + 8];
        format_tunnel(&table.entries[i].spec, line, sizeof(line));
        printf("%lu. %s\n", (unsigned long)i + 1, line);
    }
    fp_tunnel_table_free(&table);
    return 0;
}

static int cmd_remove(const cli_options *opts, const char *selector)
{
    fp_tunnel_table table;
    char err[256];
    size_t i;
    size_t matches = 0;
    size_t match_index = 0;
    long numeric;
    int is_numeric;

    if (opts->config_path == NULL) {
        fprintf(stderr, "fluxport-client: -c/--config is required for remove\n");
        return 1;
    }
    if (selector == NULL || selector[0] == '\0') {
        fprintf(stderr, "fluxport-client: remove needs an index or prefix\n");
        return 1;
    }
    fp_tunnel_table_init(&table);
    if (fp_tunnel_table_load(&table, opts->config_path, 1,
                             err, sizeof(err)) != 0) {
        fprintf(stderr, "fluxport-client: %s\n", err);
        fp_tunnel_table_free(&table);
        return 1;
    }
    if (table.count == 0) {
        fprintf(stderr, "fluxport-client: no tunnels configured\n");
        fp_tunnel_table_free(&table);
        return 1;
    }
    is_numeric = 1;
    for (i = 0; selector[i] != '\0'; i++) {
        if (selector[i] < '0' || selector[i] > '9') {
            is_numeric = 0;
            break;
        }
    }
    if (is_numeric) {
        if (fp_parse_long(selector, 1, (long)table.count, &numeric) != 0) {
            fprintf(stderr, "fluxport-client: tunnel index must be between 1 "
                    "and %lu\n", (unsigned long)table.count);
            fp_tunnel_table_free(&table);
            return 1;
        }
        match_index = (size_t)numeric;
        matches = 1;
    } else {
        for (i = 0; i < table.count; i++) {
            if (tunnel_matches_prefix(&table.entries[i].spec, selector)) {
                matches++;
                match_index = i + 1;
            }
        }
        if (matches == 0) {
            fprintf(stderr, "fluxport-client: no tunnel matches '%s'\n",
                    selector);
            fp_tunnel_table_free(&table);
            return 1;
        }
        if (matches > 1) {
            fprintf(stderr, "fluxport-client: prefix '%s' is ambiguous "
                    "(%lu tunnels match)\n", selector,
                    (unsigned long)matches);
            fp_tunnel_table_free(&table);
            return 1;
        }
    }
    {
        char line[FP_MAX_ADDR_TEXT * 2 + 8];
        format_tunnel(&table.entries[match_index - 1].spec, line,
                      sizeof(line));
        if (fp_tunnel_table_remove(&table, match_index,
                                   err, sizeof(err)) != 0) {
            fprintf(stderr, "fluxport-client: %s\n", err);
            fp_tunnel_table_free(&table);
            return 1;
        }
        if (fp_tunnel_table_save(&table, opts->config_path,
                                 err, sizeof(err)) != 0) {
            fprintf(stderr, "fluxport-client: %s\n", err);
            fp_tunnel_table_free(&table);
            return 1;
        }
        printf("removed tunnel #%lu: %s\n",
               (unsigned long)match_index, line);
    }
    fp_tunnel_table_free(&table);
    return 0;
}

static int cmd_run(const cli_options *opts)
{
    fp_client_config cfg;
    fp_tunnel_table table;
    fp_tunnel_spec *specs = NULL;
    fp_client *client;
    char err[256];
    char default_path[FP_CONFIG_PATH_MAX];
    const char *config_path = opts->config_path;
    int log_level;
    int rc;
    size_t i;

    fp_client_config_defaults(&cfg);
    if (config_path == NULL) {
        if (default_config_path(default_path, sizeof(default_path)) == 0 &&
            path_is_readable(default_path)) {
            config_path = default_path;
        }
    }
    if (config_path != NULL) {
        if (fp_client_config_load(config_path, &cfg,
                                  err, sizeof(err)) != 0) {
            fprintf(stderr, "fluxport-client: %s\n", err);
            return 1;
        }
    }
    if (opts->have_server) {
        snprintf(cfg.server_host, sizeof(cfg.server_host), "%s",
                 opts->server_host);
        cfg.server_port = opts->server_port;
    }
    if (opts->token != NULL) {
        if (strlen(opts->token) > FP_MAX_TOKEN) {
            fprintf(stderr, "fluxport-client: token is too long\n");
            return 1;
        }
        snprintf(cfg.token, sizeof(cfg.token), "%s", opts->token);
    }
    if (opts->name != NULL) {
        if (strlen(opts->name) > FP_MAX_CLIENT_NAME) {
            fprintf(stderr, "fluxport-client: name is too long\n");
            return 1;
        }
        snprintf(cfg.name, sizeof(cfg.name), "%s", opts->name);
    }
    if (opts->strict >= 0) {
        cfg.strict = opts->strict;
    }
    if (opts->connect_timeout > 0) {
        cfg.connect_timeout_s = opts->connect_timeout;
    }
    if (opts->keepalive >= 0) {
        cfg.keepalive_s = opts->keepalive;
    }
    if (opts->udp_idle > 0) {
        cfg.udp_idle_s = opts->udp_idle;
    }
    log_level = opts->log_level >= 0 ? opts->log_level : cfg.log_level;
    log_level += opts->verbose;
    if (log_level > FP_LOG_DEBUG) {
        log_level = FP_LOG_DEBUG;
    }
    if (log_level < FP_LOG_ERROR) {
        log_level = FP_LOG_ERROR;
    }
    cfg.log_level = log_level;
    fp_log_set_level(log_level);
    if (fp_client_config_validate(&cfg, 1, err, sizeof(err)) != 0) {
        fprintf(stderr, "fluxport-client: %s\n", err);
        return 1;
    }

    fp_tunnel_table_init(&table);
    if (config_path != NULL &&
        fp_tunnel_table_load(&table, config_path, 0,
                             err, sizeof(err)) != 0) {
        fprintf(stderr, "fluxport-client: %s\n", err);
        fp_tunnel_table_free(&table);
        return 1;
    }
    for (i = 0; i < opts->ntunnels; i++) {
        size_t index = 0;
        if (fp_tunnel_table_add(&table, &opts->tunnels[i], &index,
                                err, sizeof(err)) != 0) {
            fprintf(stderr, "fluxport-client: %s\n", err);
            fp_tunnel_table_free(&table);
            return 1;
        }
    }
    specs = (fp_tunnel_spec *)calloc(table.count > 0 ? table.count : 1,
                                     sizeof(*specs));
    if (specs == NULL) {
        fprintf(stderr, "fluxport-client: out of memory\n");
        fp_tunnel_table_free(&table);
        return 1;
    }
    for (i = 0; i < table.count; i++) {
        specs[i] = table.entries[i].spec;
    }
    client = fp_client_new(&cfg, specs, table.count, err, sizeof(err));
    free(specs);
    fp_tunnel_table_free(&table);
    if (client == NULL) {
        fprintf(stderr, "fluxport-client: %s\n", err);
        return 1;
    }
    if (fp_net_init() != 0) {
        fprintf(stderr, "fluxport-client: cannot initialize network\n");
        fp_client_free(client);
        return 1;
    }
    if (fp_install_signal_handlers() != 0) {
        fprintf(stderr, "fluxport-client: cannot install signal handlers\n");
        fp_net_cleanup();
        fp_client_free(client);
        return 1;
    }
    rc = fp_client_start(client, err, sizeof(err));
    if (rc != 0) {
        if (err[0] != '\0') {
            fprintf(stderr, "fluxport-client: %s\n", err);
        }
        fp_client_free(client);
        fp_net_cleanup();
        return rc;
    }
    rc = fp_client_run(client);
    fp_client_free(client);
    fp_net_cleanup();
    return rc;
}

int main(int argc, char **argv)
{
    cli_options opts;
    enum client_command command = CMD_RUN;
    int command_seen = 0;
    const char *remove_selector = NULL;
    int i;
    int rc;

    memset(&opts, 0, sizeof(opts));
    opts.strict = -1;
    opts.connect_timeout = -1;
    opts.keepalive = -1;
    opts.udp_idle = -1;
    opts.log_level = -1;
    opts.tunnels = (fp_tunnel_spec *)calloc(FP_MAX_TUNNELS,
                                            sizeof(*opts.tunnels));
    if (opts.tunnels == NULL) {
        fprintf(stderr, "fluxport-client: out of memory\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout);
            free(opts.tunnels);
            return 0;
        }
        if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
            printf("%s\n", CLIENT_VERSION_TEXT);
            free(opts.tunnels);
            return 0;
        }
        if (strcmp(arg, "-c") == 0 || strcmp(arg, "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "fluxport-client: %s needs a path\n", arg);
                goto fail;
            }
            opts.config_path = argv[++i];
            opts.config_explicit = 1;
            continue;
        }
        if (strcmp(arg, "-s") == 0 || strcmp(arg, "--server") == 0) {
            char perr[192];
            if (i + 1 >= argc) {
                fprintf(stderr, "fluxport-client: %s needs HOST:PORT\n", arg);
                goto fail;
            }
            if (fp_hostport_parse(argv[++i], opts.server_host,
                                  sizeof(opts.server_host),
                                  &opts.server_port, perr,
                                  sizeof(perr)) != 0) {
                fprintf(stderr, "fluxport-client: %s\n", perr);
                goto fail;
            }
            opts.have_server = 1;
            continue;
        }
        if (strcmp(arg, "--token") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "fluxport-client: --token needs a value\n");
                goto fail;
            }
            opts.token = argv[++i];
            continue;
        }
        if (strcmp(arg, "--name") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "fluxport-client: --name needs a value\n");
                goto fail;
            }
            opts.name = argv[++i];
            continue;
        }
        if (strcmp(arg, "--strict") == 0) {
            opts.strict = 1;
            continue;
        }
        if (strcmp(arg, "--no-strict") == 0) {
            opts.strict = 0;
            continue;
        }
        if (strcmp(arg, "--connect-timeout") == 0) {
            if (i + 1 >= argc ||
                parse_int_option("--connect-timeout", argv[++i], 1, 300,
                                 &opts.connect_timeout) != 0) {
                goto fail;
            }
            continue;
        }
        if (strcmp(arg, "--keepalive") == 0) {
            if (i + 1 >= argc ||
                parse_int_option("--keepalive", argv[++i], 0,
                                 FP_MAX_TIMEOUT_SEC, &opts.keepalive) != 0) {
                goto fail;
            }
            continue;
        }
        if (strcmp(arg, "--udp-idle") == 0) {
            if (i + 1 >= argc ||
                parse_int_option("--udp-idle", argv[++i], 1,
                                 FP_MAX_UDP_IDLE_SEC, &opts.udp_idle) != 0) {
                goto fail;
            }
            continue;
        }
        if (strcmp(arg, "--log-level") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "fluxport-client: --log-level needs a value\n");
                goto fail;
            }
            opts.log_level = fp_log_level_parse(argv[++i]);
            if (opts.log_level < 0) {
                fprintf(stderr, "fluxport-client: unknown log level '%s'\n",
                        argv[i]);
                goto fail;
            }
            continue;
        }
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            if (opts.verbose < 8) {
                opts.verbose++;
            }
            continue;
        }
        if (strcmp(arg, "-L") == 0 || strcmp(arg, "--local") == 0 ||
            strcmp(arg, "-R") == 0 || strcmp(arg, "--remote") == 0) {
            uint8_t dir = (arg[1] == 'R' || strcmp(arg, "--remote") == 0) ?
                          FP_DIR_R : FP_DIR_L;
            const char *listen_text;
            const char *target_text;

            if (i + 1 >= argc) {
                fprintf(stderr, "fluxport-client: %s needs LISTEN and TARGET\n",
                        arg);
                goto fail;
            }
            if (strchr(argv[i + 1], '=') != NULL) {
                if (add_cli_tunnel_token(&opts, dir, argv[++i]) != 0) {
                    goto fail;
                }
            } else {
                if (i + 2 >= argc) {
                    fprintf(stderr, "fluxport-client: %s needs LISTEN and "
                            "TARGET\n", arg);
                    goto fail;
                }
                listen_text = argv[++i];
                target_text = argv[++i];
                if (add_cli_tunnel(&opts, dir, listen_text,
                                   target_text) != 0) {
                    goto fail;
                }
            }
            continue;
        }
        if (strcmp(arg, "run") == 0) {
            if (command_seen) {
                fprintf(stderr, "fluxport-client: duplicate command '%s'\n",
                        arg);
                goto fail;
            }
            command = CMD_RUN;
            command_seen = 1;
            continue;
        }
        if (strcmp(arg, "add") == 0) {
            if (command_seen) {
                fprintf(stderr, "fluxport-client: duplicate command '%s'\n",
                        arg);
                goto fail;
            }
            command = CMD_ADD;
            command_seen = 1;
            continue;
        }
        if (strcmp(arg, "list") == 0) {
            if (command_seen) {
                fprintf(stderr, "fluxport-client: duplicate command '%s'\n",
                        arg);
                goto fail;
            }
            command = CMD_LIST;
            command_seen = 1;
            continue;
        }
        if (strcmp(arg, "remove") == 0) {
            if (command_seen) {
                fprintf(stderr, "fluxport-client: duplicate command '%s'\n",
                        arg);
                goto fail;
            }
            command = CMD_REMOVE;
            command_seen = 1;
            continue;
        }
        if (command == CMD_REMOVE && remove_selector == NULL &&
            arg[0] != '-') {
            remove_selector = arg;
            continue;
        }
        fprintf(stderr, "fluxport-client: unexpected argument '%s'\n", arg);
        goto fail;
    }

    if (command == CMD_ADD) {
        rc = cmd_add(&opts);
    } else if (command == CMD_LIST) {
        if (opts.ntunnels > 0) {
            fprintf(stderr, "fluxport-client: list does not take tunnels\n");
            goto fail;
        }
        rc = cmd_list(&opts);
    } else if (command == CMD_REMOVE) {
        if (opts.ntunnels > 0) {
            fprintf(stderr, "fluxport-client: remove does not take tunnels\n");
            goto fail;
        }
        rc = cmd_remove(&opts, remove_selector);
    } else {
        rc = cmd_run(&opts);
    }
    free(opts.tunnels);
    return rc;

fail:
    free(opts.tunnels);
    return 1;
}
