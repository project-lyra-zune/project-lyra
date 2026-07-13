#pragma once

#include "nativeapp_common.h"

// Send `count` bytes, looping over partial sends. 1 = error.
int safe_send(SOCKET client, unsigned char* out, uint32_t count);

// Build a zunecom RESP_ERR protobuf and send it. true = transmission
// failure (caller should drop the connection).
bool send_resp_err(SOCKET client, DWORD code, const char* fmt, ...);

// Hash of (addr, ifindex) pairs from GetIpAddrTable. Used by Server() to
// detect interface set changes and rebind. 0 on iphlpapi failure.
DWORD compute_iface_signature();

// 1 = readable, 0 = timeout, SOCKET_ERROR = failure. CE 6 silently
// no-ops SO_RCVTIMEO so select() is the only reliable timeout primitive.
int wait_for_readable(SOCKET s, int timeout_ms);

int wait_for_writable(SOCKET s, int timeout_ms);

// Tracks residual bytes already pulled into inbuf by the outer recv().
// Each request part consumes from the residual first, falling back to
// recv() once dry.
struct recv_stream {
	SOCKET client;
	unsigned char* inbuf;
	int residual_offset;
	int residual_len;
};

// Pull `wanted` bytes into dst, draining the residual first. Returns
// wanted on success, 0xFFFFFFFFu on error / peer close / timeout.
u32 stream_consume(struct recv_stream* s, void* dst, u32 wanted);
