#include "frame.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Payload buffer                                                      */
/* ------------------------------------------------------------------ */

int fp_buf_init(fp_buf *buf)
{
    if (buf == NULL) {
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    buf->limit = FP_MAX_PAYLOAD;
    return 0;
}

void fp_buf_free(fp_buf *buf)
{
    if (buf == NULL) {
        return;
    }
    free(buf->data);
    memset(buf, 0, sizeof(*buf));
}

void fp_buf_reset(fp_buf *buf)
{
    if (buf != NULL) {
        buf->len = 0;
    }
}

static int buf_reserve(fp_buf *buf, size_t extra)
{
    size_t need;
    size_t new_cap;
    uint8_t *grown;

    if (buf == NULL || extra > buf->limit - buf->len) {
        return -1;
    }
    need = buf->len + extra;
    if (need <= buf->cap) {
        return 0;
    }
    new_cap = buf->cap == 0 ? 256 : buf->cap;
    while (new_cap < need) {
        if (new_cap > buf->limit / 2) {
            new_cap = buf->limit;
            break;
        }
        new_cap *= 2;
    }
    if (new_cap < need) {
        return -1;
    }
    grown = (uint8_t *)realloc(buf->data, new_cap);
    if (grown == NULL) {
        return -1;
    }
    buf->data = grown;
    buf->cap = new_cap;
    return 0;
}

int fp_buf_append(fp_buf *buf, const void *data, size_t len)
{
    if (len == 0) {
        return 0;
    }
    if (data == NULL || buf_reserve(buf, len) != 0) {
        return -1;
    }
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    return 0;
}

int fp_buf_append_u8(fp_buf *buf, uint8_t value)
{
    return fp_buf_append(buf, &value, 1);
}

int fp_buf_append_u16(fp_buf *buf, uint16_t value)
{
    uint8_t tmp[2];
    tmp[0] = (uint8_t)(value >> 8);
    tmp[1] = (uint8_t)value;
    return fp_buf_append(buf, tmp, sizeof(tmp));
}

int fp_buf_append_u32(fp_buf *buf, uint32_t value)
{
    uint8_t tmp[4];
    tmp[0] = (uint8_t)(value >> 24);
    tmp[1] = (uint8_t)(value >> 16);
    tmp[2] = (uint8_t)(value >> 8);
    tmp[3] = (uint8_t)value;
    return fp_buf_append(buf, tmp, sizeof(tmp));
}

/* ------------------------------------------------------------------ */
/* Reader                                                              */
/* ------------------------------------------------------------------ */

typedef struct reader {
    const uint8_t *data;
    size_t len;
    size_t off;
    int err;
} reader;

static void rd_init(reader *r, const uint8_t *data, size_t len)
{
    r->data = data;
    r->len = len;
    r->off = 0;
    r->err = 0;
}

static uint8_t rd_u8(reader *r)
{
    if (r->err || r->off + 1 > r->len) {
        r->err = 1;
        return 0;
    }
    return r->data[r->off++];
}

static uint16_t rd_u16(reader *r)
{
    uint16_t value;
    if (r->err || r->off + 2 > r->len) {
        r->err = 1;
        return 0;
    }
    value = (uint16_t)(((uint16_t)r->data[r->off] << 8) |
                       (uint16_t)r->data[r->off + 1]);
    r->off += 2;
    return value;
}

static const uint8_t *rd_bytes(reader *r, size_t n)
{
    const uint8_t *p;
    if (r->err || n > r->len - r->off) {
        r->err = 1;
        return NULL;
    }
    p = r->data + r->off;
    r->off += n;
    return p;
}

static int rd_string(reader *r, size_t n, char *out, size_t cap)
{
    const uint8_t *p;
    if (n >= cap) {
        r->err = 1;
        return -1;
    }
    p = rd_bytes(r, n);
    if (p == NULL) {
        return -1;
    }
    if (n > 0 && memchr(p, 0, n) != NULL) {
        r->err = 1;
        return -1;
    }
    if (n > 0) {
        memcpy(out, p, n);
    }
    out[n] = '\0';
    return 0;
}

static void set_err(char *err, size_t errcap, const char *fmt, ...)
{
    va_list ap;
    if (err == NULL || errcap == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, errcap, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

void fp_parser_init(fp_frame_parser *p)
{
    if (p != NULL) {
        memset(p, 0, sizeof(*p));
    }
}

void fp_parser_free(fp_frame_parser *p)
{
    if (p == NULL) {
        return;
    }
    free(p->data);
    memset(p, 0, sizeof(*p));
}

void fp_parser_reset(fp_frame_parser *p)
{
    if (p == NULL) {
        return;
    }
    p->start = 0;
    p->end = 0;
    p->error[0] = '\0';
}

const char *fp_parser_error(const fp_frame_parser *p)
{
    if (p == NULL || p->error[0] == '\0') {
        return "";
    }
    return p->error;
}

static int parser_fail(fp_frame_parser *p, const char *fmt, ...)
{
    va_list ap;
    if (p->error[0] == '\0') {
        va_start(ap, fmt);
        vsnprintf(p->error, sizeof(p->error), fmt, ap);
        va_end(ap);
    }
    return -1;
}

int fp_parser_push(fp_frame_parser *p, const void *data, size_t len)
{
    size_t pending;
    size_t need;
    size_t new_cap;
    uint8_t *grown;

    if (p == NULL) {
        return -1;
    }
    if (p->error[0] != '\0') {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (p->start == p->end) {
        p->start = 0;
        p->end = 0;
    } else if (p->start > 0) {
        pending = p->end - p->start;
        memmove(p->data, p->data + p->start, pending);
        p->start = 0;
        p->end = pending;
    }
    pending = p->end;
    if (len > FP_PARSER_BUF_LIMIT - pending) {
        return parser_fail(p, "receive buffer limit exceeded");
    }
    need = pending + len;
    if (need > p->cap) {
        new_cap = p->cap == 0 ? 8192 : p->cap;
        while (new_cap < need) {
            if (new_cap > FP_PARSER_BUF_LIMIT / 2) {
                new_cap = FP_PARSER_BUF_LIMIT;
                break;
            }
            new_cap *= 2;
        }
        grown = (uint8_t *)realloc(p->data, new_cap);
        if (grown == NULL) {
            return parser_fail(p, "out of memory");
        }
        p->data = grown;
        p->cap = new_cap;
    }
    memcpy(p->data + p->end, data, len);
    p->end += len;
    return 0;
}

int fp_parser_next(fp_frame_parser *p, fp_frame *out)
{
    size_t avail;
    uint32_t payload_len;

    if (p == NULL || out == NULL) {
        return -1;
    }
    if (p->error[0] != '\0') {
        return -1;
    }
    avail = p->end - p->start;
    if (avail < FP_FRAME_HEADER_SIZE) {
        return 0;
    }
    payload_len = ((uint32_t)p->data[p->start] << 24) |
                  ((uint32_t)p->data[p->start + 1] << 16) |
                  ((uint32_t)p->data[p->start + 2] << 8) |
                  (uint32_t)p->data[p->start + 3];
    if (payload_len > FP_MAX_PAYLOAD) {
        return parser_fail(p, "frame length %lu exceeds limit %u",
                           (unsigned long)payload_len, FP_MAX_PAYLOAD);
    }
    if (avail < FP_FRAME_HEADER_SIZE + (size_t)payload_len) {
        return 0;
    }
    out->type = p->data[p->start + 4];
    out->stream_id = ((uint32_t)p->data[p->start + 5] << 24) |
                     ((uint32_t)p->data[p->start + 6] << 16) |
                     ((uint32_t)p->data[p->start + 7] << 8) |
                     (uint32_t)p->data[p->start + 8];
    out->payload = p->data + p->start + FP_FRAME_HEADER_SIZE;
    out->len = payload_len;
    p->start += FP_FRAME_HEADER_SIZE + (size_t)payload_len;
    if (p->start == p->end) {
        p->start = 0;
        p->end = 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* HELLO                                                               */
/* ------------------------------------------------------------------ */

int fp_build_hello(fp_buf *out, const char *token, const char *name)
{
    size_t token_len = token != NULL ? strlen(token) : 0;
    size_t name_len = name != NULL ? strlen(name) : 0;

    if (token_len > FP_MAX_TOKEN || name_len > FP_MAX_CLIENT_NAME) {
        return -1;
    }
    if (memchr(token != NULL ? token : "", '\0', token_len) != NULL ||
        memchr(name != NULL ? name : "", '\0', name_len) != NULL) {
        return -1;
    }
    if (fp_buf_append_u16(out, FP_PROTO_VERSION) != 0 ||
        fp_buf_append_u16(out, (uint16_t)token_len) != 0 ||
        fp_buf_append(out, token, token_len) != 0 ||
        fp_buf_append(out, name, name_len) != 0) {
        return -1;
    }
    return 0;
}

int fp_decode_hello(const uint8_t *payload, size_t len,
                    fp_hello *out, char *err, size_t errcap)
{
    reader r;
    uint16_t token_len;
    size_t name_len;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    rd_init(&r, payload, len);
    out->version = rd_u16(&r);
    token_len = rd_u16(&r);
    if (r.err) {
        set_err(err, errcap, "truncated HELLO");
        return -1;
    }
    if (token_len > FP_MAX_TOKEN) {
        set_err(err, errcap, "token too long (%u bytes)", (unsigned)token_len);
        return -1;
    }
    if (rd_string(&r, token_len, out->token, sizeof(out->token)) != 0) {
        set_err(err, errcap, "invalid token encoding");
        return -1;
    }
    name_len = len - r.off;
    if (name_len > FP_MAX_CLIENT_NAME) {
        set_err(err, errcap, "client name too long");
        return -1;
    }
    if (rd_string(&r, name_len, out->name, sizeof(out->name)) != 0) {
        set_err(err, errcap, "invalid client name encoding");
        return -1;
    }
    return 0;
}

int fp_build_hello_ack(fp_buf *out, uint8_t status, const char *msg)
{
    size_t msg_len = msg != NULL ? strlen(msg) : 0;
    if (msg_len > FP_MAX_MSG) {
        return -1;
    }
    if (fp_buf_append_u16(out, FP_PROTO_VERSION) != 0 ||
        fp_buf_append_u8(out, status) != 0 ||
        fp_buf_append(out, msg, msg_len) != 0) {
        return -1;
    }
    return 0;
}

int fp_decode_hello_ack(const uint8_t *payload, size_t len,
                        fp_hello_ack *out, char *err, size_t errcap)
{
    reader r;
    size_t msg_len;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    rd_init(&r, payload, len);
    out->version = rd_u16(&r);
    out->status = rd_u8(&r);
    if (r.err) {
        set_err(err, errcap, "truncated HELLO_ACK");
        return -1;
    }
    msg_len = len - r.off;
    if (msg_len > FP_MAX_MSG) {
        set_err(err, errcap, "HELLO_ACK message too long");
        return -1;
    }
    if (rd_string(&r, msg_len, out->msg, sizeof(out->msg)) != 0) {
        set_err(err, errcap, "invalid HELLO_ACK message");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* TUNNEL_ADD / RESULT                                                 */
/* ------------------------------------------------------------------ */

static int valid_tunnel_spec(const fp_tunnel_spec *spec)
{
    size_t listen_len = strlen(spec->listen_addr);
    size_t target_len = strlen(spec->target_host);
    if (spec->dir != FP_DIR_L && spec->dir != FP_DIR_R) {
        return 0;
    }
    if (listen_len == 0 || listen_len > FP_MAX_HOST ||
        target_len == 0 || target_len > FP_MAX_HOST) {
        return 0;
    }
    if (spec->listen_port == 0 || spec->target_port == 0) {
        return 0;
    }
    return 1;
}

int fp_build_tunnel_add(fp_buf *out, const fp_tunnel_spec *specs, size_t count)
{
    size_t i;

    if (count > FP_MAX_TUNNELS || (count > 0 && specs == NULL)) {
        return -1;
    }
    if (fp_buf_append_u16(out, (uint16_t)count) != 0) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        const fp_tunnel_spec *s = &specs[i];
        size_t listen_len;
        size_t target_len;

        if (!valid_tunnel_spec(s)) {
            return -1;
        }
        listen_len = strlen(s->listen_addr);
        target_len = strlen(s->target_host);
        if (fp_buf_append_u8(out, s->dir) != 0 ||
            fp_buf_append_u16(out, (uint16_t)listen_len) != 0 ||
            fp_buf_append(out, s->listen_addr, listen_len) != 0 ||
            fp_buf_append_u16(out, s->listen_port) != 0 ||
            fp_buf_append_u16(out, (uint16_t)target_len) != 0 ||
            fp_buf_append(out, s->target_host, target_len) != 0 ||
            fp_buf_append_u16(out, s->target_port) != 0) {
            return -1;
        }
    }
    return 0;
}

int fp_decode_tunnel_add(const uint8_t *payload, size_t len,
                         fp_tunnel_spec *out, size_t max_count,
                         size_t *count, char *err, size_t errcap)
{
    reader r;
    uint16_t n;
    uint16_t i;

    if (count != NULL) {
        *count = 0;
    }
    rd_init(&r, payload, len);
    n = rd_u16(&r);
    if (r.err) {
        set_err(err, errcap, "truncated TUNNEL_ADD");
        return -1;
    }
    if (n > FP_MAX_TUNNELS) {
        set_err(err, errcap, "too many tunnels (%u, max %u)",
                (unsigned)n, FP_MAX_TUNNELS);
        return -1;
    }
    if (n > max_count || (n > 0 && out == NULL)) {
        set_err(err, errcap, "result array too small");
        return -1;
    }
    for (i = 0; i < n; i++) {
        fp_tunnel_spec *s = &out[i];
        uint16_t addr_len;
        uint16_t host_len;

        memset(s, 0, sizeof(*s));
        s->dir = rd_u8(&r);
        addr_len = rd_u16(&r);
        if (addr_len == 0 || addr_len > FP_MAX_HOST ||
            rd_string(&r, addr_len, s->listen_addr,
                      sizeof(s->listen_addr)) != 0) {
            set_err(err, errcap, "entry %u: invalid listen address",
                    (unsigned)i);
            return -1;
        }
        s->listen_port = rd_u16(&r);
        host_len = rd_u16(&r);
        if (host_len == 0 || host_len > FP_MAX_HOST ||
            rd_string(&r, host_len, s->target_host,
                      sizeof(s->target_host)) != 0) {
            set_err(err, errcap, "entry %u: invalid target host",
                    (unsigned)i);
            return -1;
        }
        s->target_port = rd_u16(&r);
        if (r.err) {
            set_err(err, errcap, "entry %u: truncated", (unsigned)i);
            return -1;
        }
        if (!valid_tunnel_spec(s)) {
            set_err(err, errcap, "entry %u: invalid parameters",
                    (unsigned)i);
            return -1;
        }
    }
    if (r.off != len) {
        set_err(err, errcap, "trailing bytes in TUNNEL_ADD");
        return -1;
    }
    if (count != NULL) {
        *count = n;
    }
    return 0;
}

int fp_build_tunnel_result(fp_buf *out, const fp_tunnel_result *results,
                           size_t count)
{
    size_t i;

    if (count > FP_MAX_TUNNELS || (count > 0 && results == NULL)) {
        return -1;
    }
    if (fp_buf_append_u16(out, (uint16_t)count) != 0) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        size_t msg_len = strlen(results[i].msg);
        if (msg_len > FP_MAX_MSG) {
            return -1;
        }
        if (fp_buf_append_u16(out, results[i].index) != 0 ||
            fp_buf_append_u8(out, results[i].status) != 0 ||
            fp_buf_append_u16(out, (uint16_t)msg_len) != 0 ||
            fp_buf_append(out, results[i].msg, msg_len) != 0) {
            return -1;
        }
    }
    return 0;
}

int fp_decode_tunnel_result(const uint8_t *payload, size_t len,
                            fp_tunnel_result *out, size_t max_count,
                            size_t *count, char *err, size_t errcap)
{
    reader r;
    uint16_t n;
    uint16_t i;

    if (count != NULL) {
        *count = 0;
    }
    rd_init(&r, payload, len);
    n = rd_u16(&r);
    if (r.err) {
        set_err(err, errcap, "truncated TUNNEL_RESULT");
        return -1;
    }
    if (n > max_count || (n > 0 && out == NULL)) {
        set_err(err, errcap, "result count %u exceeds expected", (unsigned)n);
        return -1;
    }
    for (i = 0; i < n; i++) {
        fp_tunnel_result *res = &out[i];
        uint16_t msg_len;

        memset(res, 0, sizeof(*res));
        res->index = rd_u16(&r);
        res->status = rd_u8(&r);
        msg_len = rd_u16(&r);
        if (msg_len > FP_MAX_MSG || r.err) {
            set_err(err, errcap, "entry %u: invalid message length",
                    (unsigned)i);
            return -1;
        }
        if (rd_string(&r, msg_len, res->msg, sizeof(res->msg)) != 0) {
            set_err(err, errcap, "entry %u: invalid message", (unsigned)i);
            return -1;
        }
    }
    if (r.off != len) {
        set_err(err, errcap, "trailing bytes in TUNNEL_RESULT");
        return -1;
    }
    if (count != NULL) {
        *count = n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* OPEN / CLOSE / GOAWAY                                               */
/* ------------------------------------------------------------------ */

int fp_build_open(fp_buf *out, const char *target)
{
    size_t len;
    if (target == NULL) {
        return -1;
    }
    len = strlen(target);
    if (len == 0 || len >= FP_MAX_ADDR_TEXT) {
        return -1;
    }
    return fp_buf_append(out, target, len);
}

int fp_decode_open(const uint8_t *payload, size_t len,
                   char *target, size_t target_cap,
                   char *err, size_t errcap)
{
    reader r;
    if (target == NULL || target_cap == 0) {
        return -1;
    }
    if (len == 0 || len >= target_cap || len > FP_MAX_ADDR_TEXT) {
        set_err(err, errcap, "invalid UDP_OPEN target length %lu",
                (unsigned long)len);
        return -1;
    }
    rd_init(&r, payload, len);
    if (rd_string(&r, len, target, target_cap) != 0) {
        set_err(err, errcap, "invalid UDP_OPEN target");
        return -1;
    }
    return 0;
}

int fp_build_close(fp_buf *out, int reason)
{
    if (reason < 0) {
        return 0;
    }
    if (reason > 255) {
        return -1;
    }
    return fp_buf_append_u8(out, (uint8_t)reason);
}

int fp_decode_close(const uint8_t *payload, size_t len,
                    int *reason, char *err, size_t errcap)
{
    if (reason == NULL) {
        return -1;
    }
    if (len == 0) {
        *reason = FP_CLOSE_NORMAL;
        return 0;
    }
    if (len != 1) {
        set_err(err, errcap, "invalid UDP_CLOSE payload length %lu",
                (unsigned long)len);
        return -1;
    }
    *reason = payload[0];
    return 0;
}

int fp_build_goaway(fp_buf *out, const char *reason)
{
    size_t len;
    if (reason == NULL) {
        reason = "";
    }
    len = strlen(reason);
    if (len > FP_MAX_MSG) {
        return -1;
    }
    return fp_buf_append(out, reason, len);
}

int fp_decode_goaway(const uint8_t *payload, size_t len,
                     char *reason, size_t reason_cap,
                     char *err, size_t errcap)
{
    reader r;
    if (reason == NULL || reason_cap == 0) {
        return -1;
    }
    if (len >= reason_cap || len > FP_MAX_MSG) {
        set_err(err, errcap, "invalid GOAWAY reason length %lu",
                (unsigned long)len);
        return -1;
    }
    rd_init(&r, payload, len);
    if (rd_string(&r, len, reason, reason_cap) != 0) {
        set_err(err, errcap, "invalid GOAWAY reason");
        return -1;
    }
    return 0;
}
