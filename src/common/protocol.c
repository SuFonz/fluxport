#include "protocol.h"

const char *fp_frame_type_name(uint8_t type)
{
    switch (type) {
    case FP_FRAME_HELLO: return "HELLO";
    case FP_FRAME_HELLO_ACK: return "HELLO_ACK";
    case FP_FRAME_TUNNEL_ADD: return "TUNNEL_ADD";
    case FP_FRAME_TUNNEL_RESULT: return "TUNNEL_RESULT";
    case FP_FRAME_UDP_OPEN: return "UDP_OPEN";
    case FP_FRAME_UDP_DATA: return "UDP_DATA";
    case FP_FRAME_UDP_CLOSE: return "UDP_CLOSE";
    case FP_FRAME_PING: return "PING";
    case FP_FRAME_PONG: return "PONG";
    case FP_FRAME_GOAWAY: return "GOAWAY";
    default: return "UNKNOWN";
    }
}

const char *fp_status_text(uint8_t status)
{
    switch (status) {
    case FP_STATUS_OK: return "ok";
    case FP_STATUS_BAD_VERSION: return "protocol version mismatch";
    case FP_STATUS_BAD_TOKEN: return "bad token";
    case FP_STATUS_TOO_MANY_CLIENTS: return "too many clients";
    case FP_STATUS_MALFORMED: return "malformed request";
    case FP_STATUS_PORT_DENIED: return "port not allowed";
    case FP_STATUS_BIND_FAILED: return "bind failed";
    case FP_STATUS_BAD_TUNNEL: return "invalid tunnel";
    case FP_STATUS_DUPLICATE: return "duplicate tunnel";
    case FP_STATUS_INTERNAL: return "internal error";
    default: return "unknown status";
    }
}
