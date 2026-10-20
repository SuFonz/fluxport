#include "bufq.h"

#include "protocol.h"

#include <stdlib.h>
#include <string.h>

int fp_sendq_init(fp_sendq *q, size_t limit)
{
    if (q == NULL || limit == 0) {
        return -1;
    }
    memset(q, 0, sizeof(*q));
    q->limit = limit;
    return 0;
}

void fp_sendq_free(fp_sendq *q)
{
    if (q == NULL) {
        return;
    }
    free(q->data);
    memset(q, 0, sizeof(*q));
}

void fp_sendq_reset(fp_sendq *q)
{
    if (q == NULL) {
        return;
    }
    q->start = 0;
    q->end = 0;
}

size_t fp_sendq_pending(const fp_sendq *q)
{
    if (q == NULL) {
        return 0;
    }
    return q->end - q->start;
}

size_t fp_sendq_limit(const fp_sendq *q)
{
    return q != NULL ? q->limit : 0;
}

int fp_sendq_empty(const fp_sendq *q)
{
    return fp_sendq_pending(q) == 0;
}

const uint8_t *fp_sendq_data(const fp_sendq *q)
{
    return q != NULL ? q->data + q->start : NULL;
}

void fp_sendq_consume(fp_sendq *q, size_t n)
{
    if (q == NULL) {
        return;
    }
    if (n >= q->end - q->start) {
        q->start = 0;
        q->end = 0;
        return;
    }
    q->start += n;
}

static void compact(fp_sendq *q)
{
    size_t pending;
    if (q->start == 0) {
        return;
    }
    pending = q->end - q->start;
    if (pending > 0) {
        memmove(q->data, q->data + q->start, pending);
    }
    q->start = 0;
    q->end = pending;
}

static int reserve(fp_sendq *q, size_t extra)
{
    size_t pending;
    size_t need;
    size_t new_cap;
    uint8_t *grown;

    if (q == NULL) {
        return FP_SENDQ_ENOMEM;
    }
    if (extra > q->limit) {
        return FP_SENDQ_FULL;
    }
    compact(q);
    pending = q->end;
    if (pending > q->limit || extra > q->limit - pending) {
        return FP_SENDQ_FULL;
    }
    need = pending + extra;
    if (need <= q->cap) {
        return FP_SENDQ_OK;
    }
    new_cap = q->cap == 0 ? 4096 : q->cap;
    while (new_cap < need) {
        if (new_cap > q->limit / 2) {
            new_cap = q->limit;
            break;
        }
        new_cap *= 2;
    }
    if (new_cap < need) {
        new_cap = need;
    }
    grown = (uint8_t *)realloc(q->data, new_cap);
    if (grown == NULL) {
        return FP_SENDQ_ENOMEM;
    }
    q->data = grown;
    q->cap = new_cap;
    return FP_SENDQ_OK;
}

int fp_sendq_append(fp_sendq *q, const void *data, size_t len)
{
    int rc;
    if (len == 0) {
        return FP_SENDQ_OK;
    }
    rc = reserve(q, len);
    if (rc != FP_SENDQ_OK) {
        return rc;
    }
    memcpy(q->data + q->end, data, len);
    q->end += len;
    return FP_SENDQ_OK;
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

int fp_sendq_frame(fp_sendq *q, uint8_t type, uint32_t stream_id,
                   const void *payload, size_t payload_len)
{
    int rc;
    uint8_t *p;

    if (q == NULL || payload_len > FP_MAX_PAYLOAD) {
        return FP_SENDQ_ENOMEM;
    }
    rc = reserve(q, FP_FRAME_HEADER_SIZE + payload_len);
    if (rc != FP_SENDQ_OK) {
        return rc;
    }
    p = q->data + q->end;
    put_u32(p, (uint32_t)payload_len);
    p[4] = type;
    put_u32(p + 5, stream_id);
    if (payload_len > 0) {
        memcpy(p + FP_FRAME_HEADER_SIZE, payload, payload_len);
    }
    q->end += FP_FRAME_HEADER_SIZE + payload_len;
    return FP_SENDQ_OK;
}
