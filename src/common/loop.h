/*
 * Single-threaded event loop with a poll(2)/WSAPoll backend.
 *
 * Callbacks may add, modify or remove watchers, including their own.  A
 * generation counter prevents a removed/reused descriptor from receiving a
 * stale event.  Watchers never own descriptors: callbacks must remove the
 * watcher (or let the owner do it) before closing an fd.
 */
#ifndef FP_LOOP_H
#define FP_LOOP_H

#include "net.h"

typedef struct fp_loop fp_loop;
typedef void (*fp_loop_cb)(fp_loop *loop, fp_socket_t fd, short revents,
                           void *arg);

fp_loop *fp_loop_new(void);
void fp_loop_free(fp_loop *loop);

/* Add an fd.  Returns 0, or -1 on duplicate fd / allocation failure. */
int fp_loop_add(fp_loop *loop, fp_socket_t fd, short events,
                fp_loop_cb cb, void *arg);

/* Change the interest mask.  Returns 0 or -1 when fd is not watched. */
int fp_loop_mod(fp_loop *loop, fp_socket_t fd, short events);

/* Remove an fd.  Returns 0 or -1 when fd is not watched. */
int fp_loop_del(fp_loop *loop, fp_socket_t fd);

/* Run one iteration.  Returns dispatched event count or -1 on poll error. */
int fp_loop_run_once(fp_loop *loop, int timeout_ms);

void fp_loop_request_stop(fp_loop *loop);
int fp_loop_stopped(const fp_loop *loop);

#endif /* FP_LOOP_H */
