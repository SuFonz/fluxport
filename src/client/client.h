/*
 * FluxPort client: one control connection, a persisted tunnel table and
 * per-source UDP sessions.
 */
#ifndef FP_CLIENT_H
#define FP_CLIENT_H

#include "config.h"
#include "frame.h"

#include <stddef.h>

typedef struct fp_client fp_client;

/*
 * Create a client.  The tunnel specs are copied.  Validation errors are
 * reported through err and NULL is returned.
 */
fp_client *fp_client_new(const fp_client_config *cfg,
                         const fp_tunnel_spec *tunnels, size_t ntunnels,
                         char *err, size_t errcap);

/*
 * Connect to the server, authenticate, submit the tunnel table and bind
 * L listeners.  Returns 0 on success, 2 on connect/auth failure and 3
 * when --strict aborts after a partial tunnel failure.
 */
int fp_client_start(fp_client *cl, char *err, size_t errcap);

/* Run the forwarding loop.  Returns the process exit code. */
int fp_client_run(fp_client *cl);

void fp_client_stop(fp_client *cl);
void fp_client_free(fp_client *cl);

#endif /* FP_CLIENT_H */
