#include "loop.h"

#include "log.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

struct fp_watch {
    fp_socket_t fd;
    short events;
    fp_loop_cb cb;
    void *arg;
    uint32_t gen;
    int active;
};

struct fp_loop {
    struct fp_watch *watches;
    size_t nwatches;
    size_t cap;
    struct fp_pollfd *pollfds;
    uint32_t *gens;
    size_t pcap;
    uint32_t next_gen;
    int stop;
};

static int grow_watches(fp_loop *loop, size_t need)
{
    struct fp_watch *grown;
    size_t new_cap;

    if (need <= loop->cap) {
        return 0;
    }
    new_cap = loop->cap == 0 ? 16 : loop->cap;
    while (new_cap < need) {
        new_cap *= 2;
    }
    grown = (struct fp_watch *)realloc(loop->watches,
                                       new_cap * sizeof(*grown));
    if (grown == NULL) {
        return -1;
    }
    loop->watches = grown;
    loop->cap = new_cap;
    return 0;
}

static int grow_pollfds(fp_loop *loop, size_t need)
{
    struct fp_pollfd *new_fds;
    uint32_t *new_gens;
    size_t new_cap;

    if (need <= loop->pcap) {
        return 0;
    }
    new_cap = loop->pcap == 0 ? 16 : loop->pcap;
    while (new_cap < need) {
        new_cap *= 2;
    }
    new_fds = (struct fp_pollfd *)realloc(loop->pollfds,
                                          new_cap * sizeof(*new_fds));
    if (new_fds == NULL) {
        return -1;
    }
    loop->pollfds = new_fds;
    new_gens = (uint32_t *)realloc(loop->gens, new_cap * sizeof(*new_gens));
    if (new_gens == NULL) {
        return -1;
    }
    loop->gens = new_gens;
    loop->pcap = new_cap;
    return 0;
}

fp_loop *fp_loop_new(void)
{
    fp_loop *loop = (fp_loop *)calloc(1, sizeof(*loop));
    if (loop == NULL) {
        return NULL;
    }
    loop->next_gen = 1;
    return loop;
}

void fp_loop_free(fp_loop *loop)
{
    if (loop == NULL) {
        return;
    }
    free(loop->watches);
    free(loop->pollfds);
    free(loop->gens);
    free(loop);
}

static struct fp_watch *find_watch(fp_loop *loop, fp_socket_t fd,
                                   uint32_t gen, int require_gen)
{
    size_t i;
    for (i = 0; i < loop->nwatches; i++) {
        struct fp_watch *w = &loop->watches[i];
        if (!w->active || w->fd != fd) {
            continue;
        }
        if (require_gen && w->gen != gen) {
            continue;
        }
        return w;
    }
    return NULL;
}

int fp_loop_add(fp_loop *loop, fp_socket_t fd, short events,
                fp_loop_cb cb, void *arg)
{
    struct fp_watch *w;
    size_t i;
    size_t slot;

    if (loop == NULL || fd == FP_SOCKET_INVALID || cb == NULL) {
        return -1;
    }
    if (find_watch(loop, fd, 0, 0) != NULL) {
        return -1;
    }
    slot = loop->nwatches;
    for (i = 0; i < loop->nwatches; i++) {
        if (!loop->watches[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == loop->nwatches) {
        if (grow_watches(loop, loop->nwatches + 1) != 0) {
            return -1;
        }
        loop->nwatches++;
    }
    w = &loop->watches[slot];
    memset(w, 0, sizeof(*w));
    w->fd = fd;
    w->events = events;
    w->cb = cb;
    w->arg = arg;
    w->gen = loop->next_gen++;
    if (loop->next_gen == 0) {
        loop->next_gen = 1;
    }
    w->active = 1;
    return 0;
}

int fp_loop_mod(fp_loop *loop, fp_socket_t fd, short events)
{
    struct fp_watch *w;
    if (loop == NULL) {
        return -1;
    }
    w = find_watch(loop, fd, 0, 0);
    if (w == NULL) {
        return -1;
    }
    w->events = events;
    return 0;
}

int fp_loop_del(fp_loop *loop, fp_socket_t fd)
{
    struct fp_watch *w;
    if (loop == NULL) {
        return -1;
    }
    w = find_watch(loop, fd, 0, 0);
    if (w == NULL) {
        return -1;
    }
    w->active = 0;
    w->fd = FP_SOCKET_INVALID;
    w->events = 0;
    return 0;
}

static void compact_watches(fp_loop *loop)
{
    size_t i;
    size_t j = 0;
    for (i = 0; i < loop->nwatches; i++) {
        if (loop->watches[i].active) {
            if (i != j) {
                loop->watches[j] = loop->watches[i];
            }
            j++;
        }
    }
    loop->nwatches = j;
}

int fp_loop_run_once(fp_loop *loop, int timeout_ms)
{
    size_t count = 0;
    size_t i;
    int rc;
    int dispatched = 0;

    if (loop == NULL || loop->stop) {
        return 0;
    }
    if (grow_pollfds(loop, loop->nwatches) != 0) {
        return -1;
    }
    for (i = 0; i < loop->nwatches; i++) {
        if (!loop->watches[i].active) {
            continue;
        }
        loop->pollfds[count].fd = loop->watches[i].fd;
        loop->pollfds[count].events = loop->watches[i].events;
        loop->pollfds[count].revents = 0;
        loop->gens[count] = loop->watches[i].gen;
        count++;
    }
    if (count == 0) {
        fp_sleep_ms(timeout_ms > 0 ? timeout_ms : 100);
        compact_watches(loop);
        return 0;
    }
    rc = fp_poll(loop->pollfds, count, timeout_ms);
    if (rc == FP_IO_AGAIN) {
        return 0;
    }
    if (rc == FP_IO_ERROR) {
        FP_LOGE("loop", "poll failed");
        return -1;
    }
    for (i = 0; i < count; i++) {
        fp_socket_t fd;
        uint32_t gen;
        short revents;
        struct fp_watch *w;

        if (loop->pollfds[i].revents == 0) {
            continue;
        }
        fd = loop->pollfds[i].fd;
        gen = loop->gens[i];
        revents = loop->pollfds[i].revents;
        w = find_watch(loop, fd, gen, 1);
        if (w == NULL) {
            continue;
        }
        w->cb(loop, fd, revents, w->arg);
        dispatched++;
        if (loop->stop) {
            break;
        }
    }
    compact_watches(loop);
    return dispatched;
}

void fp_loop_request_stop(fp_loop *loop)
{
    if (loop != NULL) {
        loop->stop = 1;
    }
}

int fp_loop_stopped(const fp_loop *loop)
{
    return loop != NULL ? loop->stop : 1;
}
