#include "net_helpers.h"

int safe_send(SOCKET client, unsigned char* out, uint32_t count) {
	uint32_t off = 0;
	int rem = count;
	while(true) {
		int send_res = send(client,(char*)&out[off], rem, 0);
		if(send_res == SOCKET_ERROR) {
			return 1;
		}
		rem -= send_res;
		off += send_res;
		if(rem <= 0) {
			return 0;
		}
		Sleep(100);
	}
}

bool send_resp_err(SOCKET client, DWORD code, const char* fmt, ...) {
	unsigned char* out = (unsigned char*)calloc(OBUFSZ, 1);
	if (out == NULL) {
		return true;
	}
	char msgbuf[128];
	memset(msgbuf, 0, sizeof(msgbuf));
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf(msgbuf, sizeof(msgbuf) - 1, fmt, ap);
	va_end(ap);

	zunecom_CommandResp local = zunecom_CommandResp_init_zero;
	local.cmd = zunecom_CommandResp_ResType_RESP_ERR;
	local.which_payload = zunecom_CommandResp_err_tag;
	local.payload.err.code = code;
	strncpy(local.payload.err.msg, msgbuf, sizeof(local.payload.err.msg) - 1);
	local.payload.err.msg[sizeof(local.payload.err.msg) - 1] = 0;

	pb_ostream_t ostream = pb_ostream_from_buffer(out, OBUFSZ);
	bool failed = true;
	if (pb_encode(&ostream, zunecom_CommandResp_fields, &local)) {
		failed = safe_send(client, out, ostream.bytes_written) != 0;
	}
	free(out);
	return failed;
}

DWORD compute_iface_signature() {
	DWORD sig = 0;
	DWORD size = 0;
	MIB_IPADDRTABLE* table = NULL;

	if (GetIpAddrTable(NULL, &size, 0) != ERROR_INSUFFICIENT_BUFFER) return 0;
	table = (MIB_IPADDRTABLE*)malloc(size);
	if (!table) return 0;

	if (GetIpAddrTable(table, &size, 0) == NO_ERROR) {
		for (DWORD i = 0; i < table->dwNumEntries; i++) {
			// Golden-ratio multiplier for uniform-ish mixing of the
			// (addr, ifindex) pairs.
			sig = (sig * 0x9E3779B9u) ^ table->table[i].dwAddr;
			sig = (sig * 0x9E3779B9u) ^ table->table[i].dwIndex;
		}
	}
	free(table);
	return sig;
}

int wait_for_readable(SOCKET s, int timeout_ms) {
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(s, &rfds);
	struct timeval tv;
	tv.tv_sec  = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	return select(0, &rfds, NULL, NULL, &tv);
}

u32 stream_consume(struct recv_stream* s, void* dst, u32 wanted) {
	u32 got = 0;
	while (got < wanted) {
		int from_residual = s->residual_len - s->residual_offset;
		if (from_residual > 0) {
			u32 take = wanted - got;
			if ((int)take > from_residual) take = (u32)from_residual;
			memcpy((unsigned char*)dst + got, s->inbuf + s->residual_offset, take);
			s->residual_offset += (int)take;
			got += take;
			continue;
		}
		int sel = wait_for_readable(s->client, 30000);
		if (sel <= 0) return 0xFFFFFFFFu;
		int r = recv(s->client, (char*)dst + got, (int)(wanted - got), 0);
		if (r <= 0) return 0xFFFFFFFFu;
		got += (u32)r;
	}
	return got;
}
