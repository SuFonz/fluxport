#ifndef FP_SERVER_H
#define FP_SERVER_H

#include "config.h"

#include <stddef.h>

typedef struct fp_server fp_server;

fp_server *fp_server_new(const fp_server_config *cfg,
                         char *err, size_t errcap);
int fp_server_start(fp_server *srv, char *err, size_t errcap);
void fp_server_run(fp_server *srv);
void fp_server_stop(fp_server *srv);
void fp_server_free(fp_server *srv);

#endif /* FP_SERVER_H */
