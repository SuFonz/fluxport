/*
 * Client tunnel configuration table.  The table keeps the original file
 * lines so that "config add"/"config remove" preserve comments, settings
 * and ordering.  Only lines whose first word is L or R are tunnels.
 */
#ifndef FP_TUNNEL_TABLE_H
#define FP_TUNNEL_TABLE_H

#include "frame.h"
#include "protocol.h"

#include <stddef.h>

typedef struct fp_tunnel_entry {
    fp_tunnel_spec spec;
    size_t line_index;
} fp_tunnel_entry;

typedef struct fp_tunnel_table {
    fp_tunnel_entry entries[FP_MAX_TUNNELS];
    size_t count;
    char **lines;
    size_t nlines;
} fp_tunnel_table;

void fp_tunnel_table_init(fp_tunnel_table *table);
void fp_tunnel_table_free(fp_tunnel_table *table);

/* missing_ok: a nonexistent file produces an empty table. */
int fp_tunnel_table_load(fp_tunnel_table *table, const char *path,
                         int missing_ok, char *err, size_t errcap);
int fp_tunnel_table_save(const fp_tunnel_table *table, const char *path,
                         char *err, size_t errcap);

/* Append a tunnel line (1-based result in *index when non-NULL). */
int fp_tunnel_table_add(fp_tunnel_table *table, const fp_tunnel_spec *spec,
                        size_t *index, char *err, size_t errcap);

/* Remove tunnel by 1-based index. */
int fp_tunnel_table_remove(fp_tunnel_table *table, size_t index,
                           char *err, size_t errcap);

const char *fp_tunnel_dir_name(uint8_t dir);

#endif /* FP_TUNNEL_TABLE_H */
