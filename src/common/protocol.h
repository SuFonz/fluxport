/*
 * FluxPort wire protocol constants.
 *
 * All multi-byte integers on the wire are big endian.  A frame header is
 * nine bytes: payload length (u32), type (u8), stream id (u32).
 */
#ifndef FP_PROTOCOL_H
#define FP_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define FP_PROTO_VERSION 1u
#define FP_VERSION_STRING "1.0.0"

#define FP_FRAME_HEADER_SIZE 9u
#define FP_MAX_PAYLOAD 65535u
#define FP_MAX_FRAME (FP_FRAME_HEADER_SIZE + FP_MAX_PAYLOAD)

#define FP_MAX_TOKEN 512u
#define FP_MAX_CLIENT_NAME 127u
#define FP_MAX_TUNNELS 256u
#define FP_MAX_HOST 255u
#define FP_MAX_ADDR_TEXT 512u
#define FP_MAX_MSG 511u
#define FP_MAX_SESSIONS 4096u

/* Per control connection outgoing queue limit (bytes). */
#define FP_SENDQ_LIMIT (8u * 1024u * 1024u)

/*
 * Stream id namespace.  The listening side allocates the id, and bit 31
 * records which side listens: 0 for L tunnels (client listens), 1 for R
 * tunnels (server listens).  This keeps the two allocators collision free.
 */
#define FP_STREAM_R_BIT 0x80000000u
#define FP_STREAM_ID_MASK 0x7FFFFFFFu

/* Frame types. */
enum fp_frame_type {
    FP_FRAME_HELLO = 0x01,
    FP_FRAME_HELLO_ACK = 0x02,
    FP_FRAME_TUNNEL_ADD = 0x03,
    FP_FRAME_TUNNEL_RESULT = 0x04,
    FP_FRAME_UDP_OPEN = 0x10,
    FP_FRAME_UDP_DATA = 0x11,
    FP_FRAME_UDP_CLOSE = 0x12,
    FP_FRAME_PING = 0x20,
    FP_FRAME_PONG = 0x21,
    FP_FRAME_GOAWAY = 0x30
};

/* Tunnel direction: who owns the UDP listener. */
enum fp_tunnel_dir {
    FP_DIR_L = 0,
    FP_DIR_R = 1
};

/*
 * Status values used by HELLO_ACK and TUNNEL_RESULT.  Values 0..4 are
 * handshake statuses; 5 and above describe tunnel setup results.
 */
enum fp_status {
    FP_STATUS_OK = 0,
    FP_STATUS_BAD_VERSION = 1,
    FP_STATUS_BAD_TOKEN = 2,
    FP_STATUS_TOO_MANY_CLIENTS = 3,
    FP_STATUS_MALFORMED = 4,
    FP_STATUS_PORT_DENIED = 5,
    FP_STATUS_BIND_FAILED = 6,
    FP_STATUS_BAD_TUNNEL = 7,
    FP_STATUS_DUPLICATE = 8,
    FP_STATUS_INTERNAL = 9
};

/* Optional one-byte payload of UDP_CLOSE.  An empty payload means NORMAL. */
enum fp_close_reason {
    FP_CLOSE_NORMAL = 0,
    FP_CLOSE_TIMEOUT = 1,
    FP_CLOSE_ERROR = 2
};

const char *fp_frame_type_name(uint8_t type);
const char *fp_status_text(uint8_t status);

#endif /* FP_PROTOCOL_H */
