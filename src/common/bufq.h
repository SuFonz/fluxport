/*
 * Bounded FIFO byte queue used for the control connection's outgoing side.
 * It supports partial writes: the caller consumes bytes after send().
 */
#ifndef FP_BUFQ_H
#define FP_BUFQ_H

#include <stddef.h>
#include <stdint.h>

typedef struct fp_sendq {
    uint8_t *data;
    size_t cap;
    size_t start;
    size_t end;
    size_t limit;
} fp_sendq;

#define FP_SENDQ_OK 0
#define FP_SENDQ_FULL 1
#define FP_SENDQ_ENOMEM (-1)

int fp_sendq_init(fp_sendq *q, size_t limit);
void fp_sendq_free(fp_sendq *q);
void fp_sendq_reset(fp_sendq *q);

size_t fp_sendq_pending(const fp_sendq *q);
size_t fp_sendq_limit(const fp_sendq *q);
int fp_sendq_empty(const fp_sendq *q);

const uint8_t *fp_sendq_data(const fp_sendq *q);
void fp_sendq_consume(fp_sendq *q, size_t n);

/*
 * Append raw bytes.  Returns FP_SENDQ_OK, FP_SENDQ_FULL when the configured
 * limit would be exceeded, or FP_SENDQ_ENOMEM.
 */
int fp_sendq_append(fp_sendq *q, const void *data, size_t len);

/* Append a complete protocol frame.  Same return codes as append. */
int fp_sendq_frame(fp_sendq *q, uint8_t type, uint32_t stream_id,
                   const void *payload, size_t payload_len);

#endif /* FP_BUFQ_H */
