/*
 * FluxPort frame codec and incremental receive parser.
 *
 * The parser turns a byte stream into frames and rejects absurd lengths
 * before allocating or waiting for them.  Payload pointers returned by
 * fp_parser_next() stay valid only until the next push/next call.
 */
#ifndef FP_FRAME_H
#define FP_FRAME_H

#include "protocol.h"

#include <stddef.h>
#include <stdint.h>

#define FP_PARSER_BUF_LIMIT (1024u * 1024u)

typedef struct fp_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
    size_t limit;
} fp_buf;

int fp_buf_init(fp_buf *buf);
void fp_buf_free(fp_buf *buf);
void fp_buf_reset(fp_buf *buf);
int fp_buf_append(fp_buf *buf, const void *data, size_t len);
int fp_buf_append_u8(fp_buf *buf, uint8_t value);
int fp_buf_append_u16(fp_buf *buf, uint16_t value);
int fp_buf_append_u32(fp_buf *buf, uint32_t value);

typedef struct fp_frame {
    uint8_t type;
    uint32_t stream_id;
    const uint8_t *payload;
    size_t len;
} fp_frame;

typedef struct fp_frame_parser {
    uint8_t *data;
    size_t start;
    size_t end;
    size_t cap;
    char error[192];
} fp_frame_parser;

void fp_parser_init(fp_frame_parser *p);
void fp_parser_free(fp_frame_parser *p);
void fp_parser_reset(fp_frame_parser *p);
const char *fp_parser_error(const fp_frame_parser *p);

/* Append raw bytes.  Returns 0 or -1 when the parser hit its size limit. */
int fp_parser_push(fp_frame_parser *p, const void *data, size_t len);

/* Returns 1 when a frame was produced, 0 when more bytes are needed, -1 on error. */
int fp_parser_next(fp_frame_parser *p, fp_frame *out);

/* Payload structures used by the handshake and tunnel setup. */
typedef struct fp_hello {
    uint16_t version;
    char token[FP_MAX_TOKEN + 1];
    char name[FP_MAX_CLIENT_NAME + 1];
} fp_hello;

typedef struct fp_hello_ack {
    uint16_t version;
    uint8_t status;
    char msg[FP_MAX_MSG + 1];
} fp_hello_ack;

typedef struct fp_tunnel_spec {
    uint8_t dir;
    char listen_addr[FP_MAX_HOST + 1];
    uint16_t listen_port;
    char target_host[FP_MAX_HOST + 1];
    uint16_t target_port;
} fp_tunnel_spec;

typedef struct fp_tunnel_result {
    uint16_t index;
    uint8_t status;
    char msg[FP_MAX_MSG + 1];
} fp_tunnel_result;

/* HELLO / HELLO_ACK */
int fp_build_hello(fp_buf *out, const char *token, const char *name);
int fp_decode_hello(const uint8_t *payload, size_t len,
                    fp_hello *out, char *err, size_t errcap);
int fp_build_hello_ack(fp_buf *out, uint8_t status, const char *msg);
int fp_decode_hello_ack(const uint8_t *payload, size_t len,
                        fp_hello_ack *out, char *err, size_t errcap);

/* TUNNEL_ADD / TUNNEL_RESULT */
int fp_build_tunnel_add(fp_buf *out, const fp_tunnel_spec *specs, size_t count);
int fp_decode_tunnel_add(const uint8_t *payload, size_t len,
                         fp_tunnel_spec *out, size_t max_count,
                         size_t *count, char *err, size_t errcap);
int fp_build_tunnel_result(fp_buf *out, const fp_tunnel_result *results,
                           size_t count);
int fp_decode_tunnel_result(const uint8_t *payload, size_t len,
                            fp_tunnel_result *out, size_t max_count,
                            size_t *count, char *err, size_t errcap);

/* UDP_OPEN / UDP_CLOSE / GOAWAY */
int fp_build_open(fp_buf *out, const char *target);
int fp_decode_open(const uint8_t *payload, size_t len,
                   char *target, size_t target_cap,
                   char *err, size_t errcap);
int fp_build_close(fp_buf *out, int reason);
int fp_decode_close(const uint8_t *payload, size_t len,
                    int *reason, char *err, size_t errcap);
int fp_build_goaway(fp_buf *out, const char *reason);
int fp_decode_goaway(const uint8_t *payload, size_t len,
                     char *reason, size_t reason_cap,
                     char *err, size_t errcap);

#endif /* FP_FRAME_H */
