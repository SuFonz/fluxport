#include "tunnel_table.h"

#include "addr.h"
#include "config.h"
#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fp_tunnel_table_init(fp_tunnel_table *table)
{
    if (table != NULL) {
        memset(table, 0, sizeof(*table));
    }
}

void fp_tunnel_table_free(fp_tunnel_table *table)
{
    size_t i;
    if (table == NULL) {
        return;
    }
    for (i = 0; i < table->nlines; i++) {
        free(table->lines[i]);
    }
    free(table->lines);
    memset(table, 0, sizeof(*table));
}

const char *fp_tunnel_dir_name(uint8_t dir)
{
    if (dir == FP_DIR_L) {
        return "L";
    }
    if (dir == FP_DIR_R) {
        return "R";
    }
    return "?";
}

static void terr(char *err, size_t cap, const char *path, size_t line,
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

static char *next_token(char **cursor)
{
    char *p = *cursor;
    char *start;

    while (*p != '\0' && fp_isspace_c(*p)) {
        p++;
    }
    if (*p == '\0') {
        *cursor = p;
        return NULL;
    }
    start = p;
    while (*p != '\0' && !fp_isspace_c(*p)) {
        p++;
    }
    if (*p != '\0') {
        *p = '\0';
        p++;
    }
    *cursor = p;
    return start;
}

static int same_dir_key(const char *key)
{
    return strcmp(key, "L") == 0 || strcmp(key, "l") == 0 ||
           strcmp(key, "R") == 0 || strcmp(key, "r") == 0;
}

/*
 * Returns 1 for a parsed tunnel, 0 for a non-tunnel line, -1 for a bad
 * tunnel line (detail filled).
 */
static int parse_tunnel_line(const char *line, fp_tunnel_spec *out,
                             char *detail, size_t detail_cap)
{
    char *copy;
    char *cursor;
    char *key;
    char *listen_text;
    char *target_text;
    char *extra;
    char listen_host[FP_MAX_HOST + 1];
    char target_host[FP_MAX_HOST + 1];
    char perr[128];
    uint16_t listen_port;
    uint16_t target_port;
    int rc = 0;

    if (detail != NULL && detail_cap > 0) {
        detail[0] = '\0';
    }
    copy = fp_strdup(line);
    if (copy == NULL) {
        if (detail != NULL && detail_cap > 0) {
            snprintf(detail, detail_cap, "out of memory");
        }
        return -1;
    }
    cursor = fp_trim(copy);
    key = next_token(&cursor);
    if (key == NULL || !same_dir_key(key)) {
        free(copy);
        return 0;
    }
    listen_text = next_token(&cursor);
    target_text = next_token(&cursor);
    extra = next_token(&cursor);
    if (listen_text == NULL || target_text == NULL || extra != NULL) {
        if (detail != NULL && detail_cap > 0) {
            snprintf(detail, detail_cap,
                     "expected '%c listen_host:port target_host:port'",
                     key[0]);
        }
        free(copy);
        return -1;
    }
    if (fp_hostport_parse(listen_text, listen_host, sizeof(listen_host),
                          &listen_port, perr, sizeof(perr)) != 0) {
        if (detail != NULL && detail_cap > 0) {
            snprintf(detail, detail_cap, "invalid listen address: %s", perr);
        }
        free(copy);
        return -1;
    }
    if (fp_hostport_parse(target_text, target_host, sizeof(target_host),
                          &target_port, perr, sizeof(perr)) != 0) {
        if (detail != NULL && detail_cap > 0) {
            snprintf(detail, detail_cap, "invalid target address: %s", perr);
        }
        free(copy);
        return -1;
    }
    if (fp_endpoint_is_wildcard(target_host)) {
        if (detail != NULL && detail_cap > 0) {
            snprintf(detail, detail_cap, "target address must be concrete");
        }
        free(copy);
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->dir = (key[0] == 'L' || key[0] == 'l') ? FP_DIR_L : FP_DIR_R;
    snprintf(out->listen_addr, sizeof(out->listen_addr), "%s", listen_host);
    out->listen_port = listen_port;
    snprintf(out->target_host, sizeof(out->target_host), "%s", target_host);
    out->target_port = target_port;
    rc = 1;
    free(copy);
    return rc;
}

int fp_tunnel_table_load(fp_tunnel_table *table, const char *path,
                         int missing_ok, char *err, size_t errcap)
{
    FILE *probe;
    char *text = NULL;
    char **raw_lines = NULL;
    size_t nraw = 0;
    size_t i;
    int rc = -1;

    if (table == NULL || path == NULL) {
        return -1;
    }
    fp_tunnel_table_init(table);
    probe = fopen(path, "rb");
    if (probe == NULL) {
        if (missing_ok && errno == ENOENT) {
            return 0;
        }
        terr(err, errcap, path, 0, "cannot open: %s", strerror(errno));
        return -1;
    }
    fclose(probe);
    if (fp_read_file(path, &text, NULL, err, errcap) != 0) {
        return -1;
    }
    if (fp_split_lines_inplace(text, &raw_lines, &nraw) != 0) {
        terr(err, errcap, path, 0, "out of memory");
        free(text);
        return -1;
    }
    for (i = 0; i < nraw; i++) {
        fp_tunnel_spec spec;
        char detail[192];
        int parsed = parse_tunnel_line(raw_lines[i], &spec,
                                       detail, sizeof(detail));
        if (parsed < 0) {
            terr(err, errcap, path, i + 1, "%s", detail);
            goto done;
        }
        if (parsed == 0) {
            continue;
        }
        if (table->count >= FP_MAX_TUNNELS) {
            terr(err, errcap, path, i + 1,
                 "too many tunnels (max %u)", FP_MAX_TUNNELS);
            goto done;
        }
        table->entries[table->count].spec = spec;
        table->entries[table->count].line_index = i;
        table->count++;
    }
    if (nraw > 0) {
        table->lines = (char **)calloc(nraw, sizeof(char *));
        if (table->lines == NULL) {
            terr(err, errcap, path, 0, "out of memory");
            goto done;
        }
        for (i = 0; i < nraw; i++) {
            table->lines[i] = fp_strdup(raw_lines[i]);
            if (table->lines[i] == NULL) {
                terr(err, errcap, path, 0, "out of memory");
                goto done;
            }
        }
    }
    table->nlines = nraw;
    rc = 0;

done:
    fp_free_lines(raw_lines, nraw);
    free(text);
    if (rc != 0) {
        fp_tunnel_table_free(table);
    }
    return rc;
}

int fp_tunnel_table_save(const fp_tunnel_table *table, const char *path,
                         char *err, size_t errcap)
{
    size_t total = 0;
    size_t i;
    char *buf;
    char *p;
    int rc;

    if (table == NULL || path == NULL) {
        return -1;
    }
    for (i = 0; i < table->nlines; i++) {
        size_t len = strlen(table->lines[i]);
        if (len > SIZE_MAX - total - 1) {
            terr(err, errcap, path, 0, "file is too large");
            return -1;
        }
        total += len + 1;
    }
    if (fp_mkdir_parent(path) != 0) {
        terr(err, errcap, path, 0, "cannot create parent directory");
        return -1;
    }
    buf = (char *)malloc(total + 1);
    if (buf == NULL) {
        terr(err, errcap, path, 0, "out of memory");
        return -1;
    }
    p = buf;
    for (i = 0; i < table->nlines; i++) {
        size_t len = strlen(table->lines[i]);
        memcpy(p, table->lines[i], len);
        p += len;
        *p++ = '\n';
    }
    *p = '\0';
    rc = fp_write_file_atomic(path, buf, total, err, errcap);
    free(buf);
    return rc;
}

static int same_spec(const fp_tunnel_spec *a, const fp_tunnel_spec *b)
{
    return a->dir == b->dir &&
           a->listen_port == b->listen_port &&
           a->target_port == b->target_port &&
           strcmp(a->listen_addr, b->listen_addr) == 0 &&
           strcmp(a->target_host, b->target_host) == 0;
}

int fp_tunnel_table_add(fp_tunnel_table *table, const fp_tunnel_spec *spec,
                        size_t *index, char *err, size_t errcap)
{
    char listen_text[FP_ENDPOINT_TEXT_MAX];
    char target_text[FP_ENDPOINT_TEXT_MAX];
    char line[FP_ENDPOINT_TEXT_MAX * 2 + 8];
    char *new_line;
    char **grown;
    size_t n;
    int nlen;

    if (table == NULL || spec == NULL) {
        return -1;
    }
    if (spec->dir != FP_DIR_L && spec->dir != FP_DIR_R) {
        terr(err, errcap, NULL, 0, "direction must be L or R");
        return -1;
    }
    if (spec->listen_addr[0] == '\0' || spec->target_host[0] == '\0' ||
        spec->listen_port == 0 || spec->target_port == 0) {
        terr(err, errcap, NULL, 0, "invalid tunnel parameters");
        return -1;
    }
    if (fp_endpoint_is_wildcard(spec->target_host)) {
        terr(err, errcap, NULL, 0, "target address must be concrete");
        return -1;
    }
    if (table->count >= FP_MAX_TUNNELS) {
        terr(err, errcap, NULL, 0, "too many tunnels (max %u)",
             FP_MAX_TUNNELS);
        return -1;
    }
    for (n = 0; n < table->count; n++) {
        if (same_spec(&table->entries[n].spec, spec)) {
            terr(err, errcap, NULL, 0, "this tunnel already exists");
            return -1;
        }
    }
    if (fp_hostport_format(spec->listen_addr, spec->listen_port,
                           listen_text, sizeof(listen_text)) != 0 ||
        fp_hostport_format(spec->target_host, spec->target_port,
                           target_text, sizeof(target_text)) != 0) {
        terr(err, errcap, NULL, 0, "tunnel address is too long");
        return -1;
    }
    nlen = snprintf(line, sizeof(line), "%s %s %s",
                    fp_tunnel_dir_name(spec->dir), listen_text, target_text);
    if (nlen < 0 || (size_t)nlen >= sizeof(line)) {
        terr(err, errcap, NULL, 0, "tunnel line is too long");
        return -1;
    }
    new_line = fp_strdup(line);
    if (new_line == NULL) {
        terr(err, errcap, NULL, 0, "out of memory");
        return -1;
    }
    grown = (char **)realloc(table->lines,
                             (table->nlines + 1) * sizeof(char *));
    if (grown == NULL) {
        free(new_line);
        terr(err, errcap, NULL, 0, "out of memory");
        return -1;
    }
    table->lines = grown;
    table->lines[table->nlines] = new_line;
    table->entries[table->count].spec = *spec;
    table->entries[table->count].line_index = table->nlines;
    table->count++;
    table->nlines++;
    if (index != NULL) {
        *index = table->count;
    }
    return 0;
}

int fp_tunnel_table_remove(fp_tunnel_table *table, size_t index,
                           char *err, size_t errcap)
{
    size_t entry_index;
    size_t removed_line;
    size_t i;

    if (table == NULL || index == 0 || index > table->count) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "tunnel index must be 1..%lu",
                     (unsigned long)(table != NULL ? table->count : 0));
        }
        return -1;
    }
    entry_index = index - 1;
    removed_line = table->entries[entry_index].line_index;
    if (removed_line >= table->nlines) {
        if (err != NULL && errcap > 0) {
            snprintf(err, errcap, "internal table error");
        }
        return -1;
    }
    free(table->lines[removed_line]);
    if (removed_line + 1 < table->nlines) {
        memmove(&table->lines[removed_line], &table->lines[removed_line + 1],
                (table->nlines - removed_line - 1) * sizeof(char *));
    }
    table->nlines--;
    for (i = 0; i < table->count; i++) {
        if (i != entry_index && table->entries[i].line_index > removed_line) {
            table->entries[i].line_index--;
        }
    }
    if (entry_index + 1 < table->count) {
        memmove(&table->entries[entry_index], &table->entries[entry_index + 1],
                (table->count - entry_index - 1) * sizeof(fp_tunnel_entry));
    }
    table->count--;
    memset(&table->entries[table->count], 0, sizeof(fp_tunnel_entry));
    return 0;
}
